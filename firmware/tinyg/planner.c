/*
 * planner.c - cartesian trajectory planning and motion execution
 * Part of TinyG project
 *
 * Copyright (c) 2010 - 2012 Alden S. Hart Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * See the GNU General Public License for details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with TinyG  If not, see <http://www.gnu.org/licenses/>.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* --- Planner Notes ----
 *
 *	The planner works below the canonical machine and above the motor mapping 
 *	and stepper execution layers. A rudimentary multitasking capability is 
 *	implemented for long-running commands such as lines, arcs, and dwells. 
 *	These functions are coded as non-blocking continuations - which are simple 
 *	state machines that are re-entered multiple times until a particular 
 *	operation is complete. These functions have 2 parts - the initial call, 
 *	which sets up the local context, and callbacks (continuations) that are 
 *	called from the main loop (in controller.c).
 *
 *	One important concept is isolation of the three layers of the data model - 
 *	the Gcode model (gm), planner model (bf queue & mm), and runtime model (mr).
 *	These are designated as "model", "planner" and "runtime" in function names.
 *
 *	The Gcode model is owned by the canonical machine and should only be accessed
 *	by cm_xxxx() functions. Data from the Gcode model is transferred to the planner
 *	by the mp_xxx() functions called by the canonical machine. 
 *
 *	The planner should only use data in the planner model. When a move (block) 
 *	is ready for execution the planner data is transferred to the runtime model, 
 *	which should also be isolated.
 *
 *	Lower-level models should never use data from upper-level models as the data 
 *	may have changed and lead to unpredictable results.
 */

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>				// precursor for xio.h
#include <avr/pgmspace.h>		// precursor for xio.h
#include <string.h>

#include "tinyg.h"
#include "config.h"
#include "controller.h"
#include "gcode_parser.h"
#include "canonical_machine.h"
#include "arc.h"
#include "planner.h"
#include "spindle.h"
#include "kinematics.h"
#include "stepper.h"
#include "report.h"
#include "util.h"
#include "test.h"
#include "xio/xio.h"			// supports trap and debug statements

// All the enums that equal zero must be zero. Don't change this

enum mpBufferState {			// bf->buffer_state values 
	MP_BUFFER_EMPTY = 0,		// struct is available for use (MUST BE 0)
	MP_BUFFER_LOADING,			// being written ("checked out")
	MP_BUFFER_QUEUED,			// in queue
	MP_BUFFER_PENDING,			// marked as the next buffer to run
	MP_BUFFER_RUNNING			// current running buffer
};

struct mpBuffer {				// See Planning Velocity Notes for variable usage
	struct mpBuffer *pv;		// static pointer to previous buffer
	struct mpBuffer *nx;		// static pointer to next buffer
	uint8_t (*bf_func)(struct mpBuffer *bf); // callback to buffer exec function - passes *bf, returns uint8_t
	cm_exec cm_func;			// callback to canonical machine execution function
	uint32_t linenum;			// runtime line number; or line index if not numbered
	uint32_t lineindex;			// runtime autoincremented line index
	uint8_t buffer_state;		// used to manage queueing/dequeueing
	uint8_t move_type;			// used to dispatch to run routine
	uint8_t move_code;			// byte that can be used by used exec functions
	uint8_t move_state;			// move state machine sequence
	uint8_t replannable;		// TRUE if move can be replanned

	double target[AXES];		// target position in floating point
	double unit[AXES];			// unit vector for axis scaling & planning
	double work_offset[AXES];	// offset from the work coordinate system (for reporting only)

	double time;				// line, helix or dwell time in minutes
	double min_time;			// minimum time for the move - for rate override replanning
	double head_length;
	double body_length;
	double tail_length;
	double length;				// total length of line or helix in mm
								// *** SEE NOTES ON THESE VARIABLES, in aline() ***
	double entry_velocity;		// entry velocity requested for the move
	double cruise_velocity;		// cruise velocity requested & achieved
	double exit_velocity;		// exit velocity requested for the move

	double entry_vmax;			// max junction velocity at entry of this move
	double cruise_vmax;			// max cruise velocity requested for move
	double exit_vmax;			// max exit velocity possible (redundant)
	double delta_vmax;			// max velocity difference for this move
	double braking_velocity;	// current value for braking velocity

	double jerk;				// maximum linear jerk term for this move
#ifdef __PLAN_R2
	double recip_half_jerk;		// used by planning
	double half_jerk;			// used by planning
#else
	double recip_jerk;			// 1/Jm used for planning (compute-once)
	double cbrt_jerk;			// cube root of Jm used for planning (compute-once)
#endif
};
typedef struct mpBuffer mpBuf;

#define spindle_speed time		// alias spindle_speed to the time variable
#define int_val move_code		// alias the uint8_t to the move_code
#define dbl_val time			// alias the double to the time variable

struct mpBufferPool {			// ring buffer for sub-moves
	uint8_t buffers_available;	// running count of available buffers
	struct mpBuffer *w;			// get_write_buffer pointer
	struct mpBuffer *q;			// queue_write_buffer pointer
	struct mpBuffer *r;			// get/end_run_buffer pointer
	struct mpBuffer bf[PLANNER_BUFFER_POOL_SIZE];// buffer storage
};

struct mpMoveMasterSingleton {	// common variables for planning (move master)
	uint32_t lineindex;			// runtime line index of BF being planned
	double position[AXES];		// final move position for planning purposes
	double ms_in_queue;			// total ms of movement & dwell in planner queue
	double prev_jerk;			// jerk values cached from previous move
	double prev_recip_jerk;
	double prev_cbrt_jerk;
#ifdef __UNIT_TEST_PLANNER
	double test_case;
	double test_velocity;
	double a_unit[AXES];
	double b_unit[AXES];
#endif
};

struct mpMoveRuntimeSingleton {	// persistent runtime variables
//	uint8_t (*run_move)(struct mpMoveRuntimeSingleton *m); // currently running move - left in for reference
	uint32_t linenum;			// runtime line/block number of BF being executed
	uint32_t lineindex;			// runtime line index of BF being executed
	uint8_t move_state;			// state of the overall move
	uint8_t section_state;		// state within a move section

	double endpoint[AXES];		// final target for bf (used to correct rounding errors)
	double position[AXES];		// current move position
	double target[AXES];		// target move position
	double unit[AXES];			// unit vector for axis scaling & planning
	double work_offset[AXES];	// offset from the work coordinate system (for reporting only)

	double head_length;			// copies of bf variables of same name
	double body_length;
	double tail_length;
	double entry_velocity;
	double cruise_velocity;
	double exit_velocity;

	double length;				// length of line in mm
	double move_time;			// total running time (derived)
	double midpoint_velocity;	// velocity at accel/decel midpoint
	double jerk;				// max linear jerk

	double segments;			// number of segments in arc or blend
	uint32_t segment_count;		// count of running segments
	double segment_move_time;	// actual time increment per aline segment
	double microseconds;		// line or segment time in microseconds
	double segment_length;		// computed length for aline segment
	double segment_velocity;	// computed velocity for aline segment

#ifdef __EXEC_R2
	double forward_diff_1;      // forward difference level 1 (Acceleration)
	double forward_diff_2;      // forward difference level 2 (Jerk - constant)
#else
	double accel_time;			// total pseudo-time for acceleration calculation
	double elapsed_accel_time;	// current running time for accel calculation
	double midpoint_acceleration;//acceleration at the midpoint
	double jerk_div2;			// max linear jerk divided by 2
	double segment_accel_time;	// time increment for accel computation purposes
#endif
};

static struct mpBufferPool mb;			// move buffer queue
static struct mpMoveMasterSingleton mm;	// static context for planning
static struct mpMoveRuntimeSingleton mr;// static context for runtime

/*
 * Local Scope Data and Functions
 */
#define _bump(a) ((a<PLANNER_BUFFER_POOL_SIZE-1)?(a+1):0) // buffer incr & wrap

//static mpBuf * _get_prev_buffer(const mpBuf *bf);	// use the defines below instead
//static mpBuf * _get_next_buffer(const mpBuf *bf);
#define _get_prev_buffer(b) ((mpBuf *)(b->pv))
#define _get_next_buffer(b) ((mpBuf *)(b->nx))

// aline planner routines / feedhold planning
static void _plan_block_list(mpBuf *bf, uint8_t *mr_flag);
static void _calculate_trapezoid(mpBuf *bf);
static double _get_target_length(const double Vi, const double Vt, const mpBuf *bf);
static double _get_target_velocity(const double Vi, const double L, const mpBuf *bf);
static double _get_intersection_distance(const double Vi_squared, const double Vt_squared, const double L, const mpBuf *bf);
static double _get_junction_vmax(const double a_unit[], const double b_unit[]);
static void _reset_replannable_list(void);

#ifndef __JUNCTION_VMAX_R2
static double _get_junction_deviation(const double a_unit[], const double b_unit[]);
#endif

// execute routines (NB: These are all called from the LO interrupt)
static uint8_t _exec_dwell(mpBuf *bf);
static uint8_t _exec_command(mpBuf *bf);
static uint8_t _exec_aline(mpBuf *bf);
static uint8_t _exec_aline_head(void);
static uint8_t _exec_aline_body(void);
static uint8_t _exec_aline_tail(void);
static uint8_t _exec_aline_segment(uint8_t correction_flag);
static void _init_forward_diffs(double t0, double t2);
static double _compute_next_segment_velocity(void);

// planning buffer routines
static void _init_buffers(void);
static void _clear_buffer(mpBuf *bf); 
static void _copy_buffer(mpBuf *bf, const mpBuf *bp);
static void _queue_write_buffer(const uint8_t move_type);
static void _free_run_buffer(void);
static mpBuf * _get_write_buffer(void); 
static mpBuf * _get_run_buffer(void);
static mpBuf * _get_first_buffer(void);
static mpBuf * _get_last_buffer(void);

#ifdef __DEBUG
static uint8_t _get_buffer_index(mpBuf *bf); 
static void _dump_plan_buffer(mpBuf *bf);
#endif

/* 
 * mp_init()
 *
 * The memset does:
 *	- clears all values
 *	- sets buffer states to MP_EMPTY
 *	- sets other states to their zero values - which is typically OFF
 */

void mp_init()
{
// You can assume all memory has been zeroed by a hard reset. If not, use this code:
//	memset(&mr, 0, sizeof(mr));	// clear all values, pointers and status
//	memset(&mm, 0, sizeof(mm));	// clear all values, pointers and status

	_init_buffers();
}
 
/* 
 * mp_isbusy() - return TRUE if motion control busy (i.e. robot is moving)
 *
 *	Use this function to sync to the queue. If you wait until it returns
 *	FALSE you know the queue is empty and the motors have stopped.
 */

uint8_t mp_isbusy()
{
	if ((st_isbusy() == true) || (mr.move_state > MOVE_STATE_NEW)) {
		return (true);
	}
	return (false);
}

/* 
 * mp_zero_segment_velocity() - correct velocity in last segment for reporting purposes
 */
void mp_zero_segment_velocity() 
{ 
	mr.segment_velocity = 0;
}

/* 
 * mp_flush_planner() - flush all moves in the planner and all arcs
 *
 *	Does not affect the move currently running in mr.
 *	This function is designed to be called during a hold to reset the planner
 *	and is also useful for jogs and other console-driven commands
 */

void mp_flush_planner()
{
	ar_abort_arc();
	_init_buffers();
	cm.motion_state = MOTION_STOP;
//	cm_exec_program_stop();
}

/*
 * mp_set_plan_position() 	- sets planning position (for G92)
 * mp_get_plan_position() 	- returns planning position
 * mp_set_axis_position() 	- sets both planning and runtime positions (for G2/G3)
 * mp_set_plan_lineindex()	- set line index in MM struct
 *
 * mp_get_runtime_work_position() - returns current axis position in work coordinates
 *									that were in effect at move planning time
 *
 * mp_get_runtime_machine_position() - returns current axis position in machine coordinates
 * mp_get_runtime_velocity()	- returns current velocity (aggregate)
 * mp_get_runtime_linenum()		- returns currently executing line number
 * mp_get_runtime_lineindex()	- returns currently executing line index
 *
 * 	Keeping track of position is complicated by the fact that moves can
 *	require multiple reference frames. The scheme to keep this straight is:
 *
 *	 - mm.position	- start and end position for planning
 *	 - mr.position	- current position of runtime segment
 *	 - mr.target	- target position of runtime segment
 *	 - mr.endpoint	- final target position of runtime segment
 *
 *	Note that the positions are set immediately when they are computed and 
 *	are not an accurate representation of the tool position. In reality 
 *	the motors will still be processing the action and the real tool 
 *	position is still close to the starting point.
 */

double *mp_get_plan_position(double position[])
{
	copy_axis_vector(position, mm.position);	
	return (position);
}

void mp_set_plan_position(const double position[])
{
	copy_axis_vector(mm.position, position);
}

void mp_set_plan_lineindex(uint32_t lineindex)
{
	mm.lineindex = lineindex;
	mr.lineindex = lineindex;
}

void mp_set_axes_position(const double position[])
{
	copy_axis_vector(mm.position, position);
	copy_axis_vector(mr.position, position);
}

void mp_set_axis_position(uint8_t axis, const double position)
{
	mm.position[axis] = position;
	mr.position[axis] = position;
}

double mp_get_runtime_work_position(uint8_t axis) { 
	return (mr.position[axis] - mr.work_offset[axis]);
}

double mp_get_runtime_machine_position(uint8_t axis) { 
	return (mr.position[axis]);
}

double mp_get_runtime_velocity(void) { return (mr.segment_velocity);}
double mp_get_runtime_linenum(void) { return (mr.linenum);}
double mp_get_runtime_lineindex(void) { return (mr.lineindex);}


/*************************************************************************/
/* mp_exec_move() - execute runtime functions to prep move for steppers
 *
 *	Dequeues the buffer queue and executes the move continuations.
 *	Manages run buffers and other details
 */

uint8_t mp_exec_move() 
{
	mpBuf *bf;

	if ((bf = _get_run_buffer()) == NULL) return (TG_NOOP);		// NULL means nothing's running

	if (cm.cycle_state == CYCLE_OFF) cm_cycle_start();			// cycle state management

	if ((cm.motion_state == MOTION_STOP) && (bf->move_type == MOVE_TYPE_ALINE)) {
		cm.motion_state = MOTION_RUN;							// auto state-change
	}
	if (bf->bf_func != NULL) {		// run the callback - should be set up
		return (bf->bf_func(bf));
	}
	return (TG_INTERNAL_ERROR);		// never supposed to get here
}

/************************************************************************************
 * mp_queue_command() - queue a synchronous Mcode, program control, or other command
 *
 *	How this works:
 *	  - The command is called by the Gcode interpreter (cm_<command>, e.g. an M code)
 *	  - cm_ function calls mp_queue_command which puts it in the planning queue.
 *		This involves setting some parameters and registering a callback to the 
 *		execution function in the canonical machine
 *	  - the planning queue gets to the function and calls _exec_command()
 *	  - ...which passes the saved parameters to the callback function
 *	  - To finish up _exec_command() needs to run a null pre and free the planner buffer
 *
 *	Doing it this way instead of synchronizing on queue empty simplifies the
 *	handling of feedholds, feed overrides, buffer flushes, and thread blocking,
 *	and makes keeping the queue full much easier - therefore avoiding Q starvation
 */

void mp_queue_command(void(*cm_exec)(uint8_t, double), uint8_t i, double f)
{
	mpBuf *bf;

	// this error is not reported as buffer availability was checked upstream in the controller
	if ((bf = _get_write_buffer()) == NULL) return;

	bf->move_type = MOVE_TYPE_COMMAND;
	bf->bf_func = _exec_command;		// callback to planner queue exec function
	bf->cm_func = cm_exec;				// callback to canonical machine exec function
	bf->int_val = i;
	bf->dbl_val = f;
	_queue_write_buffer(MOVE_TYPE_COMMAND);
	return;
}

static uint8_t _exec_command(mpBuf *bf)
{
	bf->cm_func(bf->int_val, bf->dbl_val);
	st_prep_null();			// Must call a null prep to keep the loader happy. 
	_free_run_buffer();
	return (TG_OK);
}

/*************************************************************************
 * mp_dwell() 	 - queue a dwell
 * _exec_dwell() - dwell continuation
 *
 * Dwells are performed by passing a dwell move to the stepper drivers.
 * When the stepper driver sees a dwell it times the swell on a separate 
 * timer than the stepper pulse timer.
 */

uint8_t mp_dwell(double seconds) 
{
	mpBuf *bf; 

	if ((bf = _get_write_buffer()) == NULL) {	// get write buffer or fail
		return (TG_BUFFER_FULL_FATAL);		  	// (not supposed to fail)
	}
	bf->bf_func = _exec_dwell;					// register the callback to the exec function
	bf->time = seconds;						  	// in seconds, not minutes
	_queue_write_buffer(MOVE_TYPE_DWELL);
	return (TG_OK);
}

static uint8_t _exec_dwell(mpBuf *bf)	// NEW
{
	st_prep_dwell((uint32_t)(bf->time * 1000000));// convert seconds to uSec
	_free_run_buffer();
	return (TG_OK);
}

/*************************************************************************
 * The non-acceleration line is left in for illustration purposes
 * mp_line() 	- queue a linear move (simple version - no accel/decel)
 * _exec_line() - run a line to generate and load a linear move
 *
 * Compute and queue a line segment to the move buffer.
 * Executes linear motion in absolute millimeter coordinates. 
 * Feed rate has already been converted to time (minutes).
 * Zero length lines are skipped at this level. 
 * 
 * The run_line routine is a continuation and can be called multiple times 
 * until it can successfully load the line into the move buffer.
 */
/*
uint8_t mp_line(const double target[], const double minutes)
{
	mpBuf *bf;
	double length = get_axis_vector_length(target, mr.position);

	if (minutes < EPSILON) { return (TG_ZERO_LENGTH_MOVE);}
	if (length < EPSILON) { return (TG_ZERO_LENGTH_MOVE);}
	if ((bf = _get_write_buffer()) == NULL) { return (TG_BUFFER_FULL_FATAL);} // never supposed to fail

	bf->bf_func = _exec_line;					// register the callback to the exec function
	bf->time = minutes;
	bf->length = length;
	copy_axis_vector(bf->target, target);		// target to bf_target
	bf->cruise_vmax = bf->length / bf->time;	// for yuks
	_queue_write_buffer(MOVE_TYPE_LINE);
	copy_axis_vector(mm.position, bf->target);	// update planning position
	return(TG_OK);
}

static uint8_t _exec_line(mpBuf *bf) 
{
	uint8_t i;
	double travel[AXES];
	double steps[MOTORS];

	for (i=0; i < AXES; i++) {
		travel[i] = bf->target[i] - mr.position[i];
	}
	mr.microseconds = uSec(bf->time);
	(void)ik_kinematics(travel, steps, mr.microseconds);
	if (st_prep_line(steps, mr.microseconds) == TG_OK) {
		copy_axis_vector(mr.position, bf->target);	// update runtime position
	}
	_free_run_buffer();
	return (TG_OK);
}
*/

/**************************************************************************
 * mp_aline() - plan a line with acceleration / deceleration
 *
 *	This function uses constant jerk motion equations to plan acceleration 
 *	and deceleration. The jerk is the rate of change of acceleration; it's
 *	the 1st derivative of acceleration, and the 3rd derivative of position. 
 *	Jerk is a measure of impact to the machine. Controlling jerk smoothes 
 *	transitions between moves and allows for faster feeds while controlling 
 *	machine oscillations and other undesirable side-effects.
 *
 *	A detailed explanation of how this module works can be found on the wiki:
 *  http://www.synthetos.com/wiki/index.php?title=Projects:TinyG-Developer-Info:#Acceleration_Planning
 *
 * 	Note: All math is done in absolute coordinates using "double precision" 
 *	floating point (even though AVRgcc does this as single precision)
 *
 *	Note: Returning a status that is not TG_OK means the endpoint is NOT
 *	advanced. So lines that are too short to move will accumulate and get 
 *	executed once the accumlated error exceeds the minimums 
 */

uint8_t mp_aline(const double target[], const double minutes, const double work_offset[], const double min_time)
{
	mpBuf *bf; 						// current move pointer
	double exact_stop = 0;
	double junction_velocity;

	// trap error conditions
	if (minutes < EPSILON) { return (TG_ZERO_LENGTH_MOVE);}

	double length = get_axis_vector_length(target, mm.position);
	if (length < EPSILON) { return (TG_ZERO_LENGTH_MOVE);}

	// get a cleared buffer and setup move variables
	if ((bf = _get_write_buffer()) == NULL) { return (TG_BUFFER_FULL_FATAL);} // never supposed to fail

	bf->bf_func = _exec_aline;					// register the callback to the exec function
	bf->linenum = cm_get_model_linenum();		// block being planned
	bf->time = minutes;
	bf->min_time = min_time;
	bf->length = length;
	copy_axis_vector(bf->target, target); 		// set target for runtime
	copy_axis_vector(bf->work_offset, work_offset);// propagate offset

	// Set unit vector and jerk terms - this is all done together for efficiency 
	// Ordinarily FP tests are to EPSILON but in this case they actually are zero
	double jerk_squared = 0;
	double diff = target[X] - mm.position[X];
	if (fp_NOT_ZERO(diff)) { 
		bf->unit[X] = diff / length;
		jerk_squared = square(bf->unit[X] * cfg.a[X].jerk_max);
	}
	if (fp_NOT_ZERO(diff = target[Y] - mm.position[Y])) { 
		bf->unit[Y] = diff / length;
		jerk_squared += square(bf->unit[Y] * cfg.a[Y].jerk_max);
	}
	if (fp_NOT_ZERO(diff = target[Z] - mm.position[Z])) { 
		bf->unit[Z] = diff / length;
		jerk_squared += square(bf->unit[Z] * cfg.a[Z].jerk_max);
	}
	if (fp_NOT_ZERO(diff = target[A] - mm.position[A])) { 
		bf->unit[A] = diff / length;
		jerk_squared += square(bf->unit[A] * cfg.a[A].jerk_max);
	}
	if (fp_NOT_ZERO(diff = target[B] - mm.position[B])) { 
		bf->unit[B] = diff / length;
		jerk_squared += square(bf->unit[B] * cfg.a[B].jerk_max);
	}
	if (fp_NOT_ZERO(diff = target[C] - mm.position[C])) { 
		bf->unit[C] = diff / length;
		jerk_squared += square(bf->unit[C] * cfg.a[C].jerk_max);
	}
	bf->jerk = sqrt(jerk_squared);

#ifdef __PLAN_R2
	bf->recip_half_jerk = 2/bf->jerk;			// used by planning
	bf->half_jerk = bf->jerk/2;				    // used by planning
#else
	if (fabs(bf->jerk - mm.prev_jerk) < JERK_MATCH_PRECISION) {	// can we re-use jerk terms?
		bf->cbrt_jerk = mm.prev_cbrt_jerk;
		bf->recip_jerk = mm.prev_recip_jerk;
	} else {
		bf->cbrt_jerk = cbrt(bf->jerk);
		bf->recip_jerk = 1/bf->jerk;			
		mm.prev_jerk = bf->jerk;
		mm.prev_cbrt_jerk = bf->cbrt_jerk;
		mm.prev_recip_jerk = bf->recip_jerk;
	}
#endif

	// finish up the current block variables
	if (cm_get_path_control() != PATH_EXACT_STOP) { // exact stop cases already zeroed
		bf->replannable = true;
		exact_stop = 12345678;					// an arbitrarily large floating point number
	}
	bf->cruise_vmax = bf->length / bf->time;	// target velocity requested
	junction_velocity = _get_junction_vmax(bf->pv->unit, bf->unit);
	bf->entry_vmax = min3(bf->cruise_vmax, junction_velocity, exact_stop);
	bf->delta_vmax = _get_target_velocity(0, bf->length, bf);
	bf->exit_vmax = min3(bf->cruise_vmax, (bf->entry_vmax + bf->delta_vmax), exact_stop);
	bf->braking_velocity = bf->delta_vmax;

	uint8_t mr_flag = false;
	_plan_block_list(bf, &mr_flag);				// replan block list and commit current block
	copy_axis_vector(mm.position, bf->target);	// update planning position
	_queue_write_buffer(MOVE_TYPE_ALINE);
	return (TG_OK);
}

/***** ALINE HELPERS *****
 * _plan_block_list()
 * _calculate_trapezoid()
 * _get_target_length()
 * _get_target_velocity()
 * _get_junction_vmax()
 * _get_junction_deviation()
 * _reset_replannable_list()
 */

/* _plan_block_list() - plans the entire block list
 *
 *	Plans all blocks between and including the first block and the block provided (bf).
 *	Sets entry, exit and cruise v's from vmax's then calls trapezoid generation. 
 *
 *	Variables that must be provided in the mpBuffers that will be processed:
 *
 *	  bf (function arg)		- end of block list (last block in time)
 *	  bf->replannable		- start of block list set by last FALSE value [Note 1]
 *	  bf->move_type			- typically ALINE. Other move_types should be set to 
 *							  length=0, entry_vmax=0 and exit_vmax=0 and are treated
 *							  as a momentary hold (plan to zero and from zero).
 *
 *	  bf->length			- provides block length
 *	  bf->entry_vmax		- used during forward planning to set entry velocity
 *	  bf->cruise_vmax		- used during forward planning to set cruise velocity
 *	  bf->exit_vmax			- used during forward planning to set exit velocity
 *	  bf->delta_vmax		- used during forward planning to set exit velocity
 *
 *	  bf->recip_jerk		- used during trapezoid generation
 *	  bf->cbrt_jerk			- used during trapezoid generation
 *
 *	Variables that will be set during processing:
 *
 *	  bf->replannable		- set if the block becomes optimally planned
 *
 *	  bf->braking_velocity	- set during backward planning
 *	  bf->entry_velocity	- set during forward planning
 *	  bf->cruise_velocity	- set during forward planning
 *	  bf->exit_velocity		- set during forward planning
 *
 *	  bf->head_length		- set during trapezoid generation
 *	  bf->body_length		- set during trapezoid generation
 *	  bf->tail_length		- set during trapezoid generation
 *
 *	Variables that are ignored but here's what you would expect them to be:
 *	  bf->move_state		- NEW for all blocks but the earliest
 *	  bf->target[]			- block target position
 *	  bf->unit[]			- block unit vector
 *	  bf->time				- gets set later
 *	  bf->jerk				- source of the other jerk variables. Used in mr.
 */
/* Notes:
 *	[1]	Whether or not a block is planned is controlled by the bf->replannable 
 *		setting (set TRUE if it should be). Replan flags are checked during the 
 *		backwards pass and prune the replan list to include only the the latest 
 *		blocks that require planning
 *
 *		In normal operation the first block (currently running block) is not 
 *		replanned, but may be for feedholds and feed overrides. In these cases 
 *		the prep routines modify the contents of the mr buffer and re-shuffle 
 *		the block list, re-enlisting the current bf buffer with new parameters.
 *		These routines also set all blocks in the list to be replannable so the 
 *		list can be recomputed regardless of exact stops and previous replanning 
 *		optimizations.
 */
static void _plan_block_list(mpBuf *bf, uint8_t *mr_flag)
{
	mpBuf *bp = bf;

	// Backward planning pass. Find beginning of the list and update the braking velocities.
	// At the end *bp points to the first buffer before the list.
	while ((bp = _get_prev_buffer(bp)) != bf) {
		if (bp->replannable == false) { break; }
		bp->braking_velocity = min(bp->nx->entry_vmax, bp->nx->braking_velocity) + bp->delta_vmax;
	}

	// forward planning pass - recomputes trapezoids in the list.
	while ((bp = _get_next_buffer(bp)) != bf) {
		if ((bp->pv == bf) || (*mr_flag == true))  {
			bp->entry_velocity = bp->entry_vmax;		// first block in the list
			*mr_flag = false;
		} else {
			bp->entry_velocity = bp->pv->exit_velocity;	// other blocks in the list
		}
		bp->cruise_velocity = bp->cruise_vmax;
		bp->exit_velocity = min4(bp->exit_vmax, bp->nx->braking_velocity, bp->nx->entry_vmax,
								(bp->entry_velocity + bp->delta_vmax));
		_calculate_trapezoid(bp);

		// test for optimally planned trapezoids - only need to check various exit conditions
		if ((bp->exit_velocity == bp->exit_vmax) || (bp->exit_velocity == bp->nx->entry_vmax) || 
		   ((bp->pv->replannable == false) && (bp->exit_velocity == bp->entry_velocity + bp->delta_vmax))) {
			bp->replannable = false;
		}
	}
	// finish up the last block move
	bp->entry_velocity = bp->pv->exit_velocity;
	bp->cruise_velocity = bp->cruise_vmax;
	bp->exit_velocity = 0;
	_calculate_trapezoid(bp);
}

/*
 *	_reset_replannable_list() - resets all blocks in the planning list to be replannable
 */	
void _reset_replannable_list()
{
	mpBuf *bf = _get_first_buffer();
	if (bf == NULL) { return;}
	mpBuf *bp = bf;
	do {
		bp->replannable = true;
	} while (((bp = _get_next_buffer(bp)) != bf) && (bp->move_state != MOVE_STATE_OFF));
}

#ifdef __PLAN_R2

/*
 * _calculate_trapezoid() - calculate trapezoid parameters
 *
 *	This rather brute-force function sets section lengths and velocities based 
 *	on the line length and velocities requested. It modifies the bf buffer and 
 *	returns accurate head_length, body_length and tail_length, and accurate or 
 *	reasonably approximate velocities. We care about accuracy on lengths, less 
 *	so for velocity (as long as velocity err's on the side of too slow). We need 
 *	the velocities to be set even for zero-length sections so we can compute 
 *	entry and exits for adjacent sections.
 *
 *	Inputs used are:
 *	  bf->length			- actual block length (must remain accurate)
 *	  bf->entry_velocity	- requested Ve
 *	  bf->cruise_velocity	- requested Vt
 *	  bf->exit_velocity		- requested Vx
 *	  bf->cruise_vmax		- used in some comparisons
 *
 *	Variables set may include the velocities above (not the vmax), and:
 *	  bf->head_length		- bf->length allocated to head
 *	  bf->body_length		- bf->length allocated to body
 *	  bf->tail_length		- bf->length allocated to tail
 *
 *	Note: The following condition must be met on entry: Ve <= Vt >= Vx 
 *
 *	Classes of moves:
 *	  Maximum-Fit - The trapezoid can accommodate its maximum velocity values for
 *		the given length (entry_vmax, cruise_vmax, exit_vmax). But the trapezoid 
 *		generator actally doesn't know about the max's and only processes requested 
 *		values.
 *
 *	  Requested-Fit - The move has sufficient length to achieve the target (set) 
 *		cruising velocity. It will accommodate the acceleration / deceleration 
 *		profile and in the distance given (length)
 *
 *	  Rate-Limited-Fit - The move does not have sufficient length to achieve target 
 *		cruising velocity - the target velocity will be lower than the requested 
 *		velocity. The entry and exit velocities are satisfied. 
 *
 *	  Degraded-Fit - The move does not have sufficient length to transition from
 *		the entry velocity to the exit velocity in the available length. These 
 *		velocities are not negotiable, so a degraded solution is found.
 *
 *	  No-Fit - The move cannot be executed as the planned execution time is less
 *		than the minimum segment interpolation time of the runtime execution module.
 *
 *	Various cases handled;
 *	  No-Fit cases - the line is too short to plan
 *		No fit
 *
 *	  Degraded fit cases - line is too short to satisfy both Ve and Vx
 *	    H"	Ve<Vx		Ve is degraded (velocity step). Vx is met
 *	  	T"	Ve>Vx		Ve is degraded (velocity step). Vx is met
 *	  	B	<short>		line is very short but drawable; is treated as a body only
 *
 *	  Rate-Limited cases - Ve and Vx can be satisfied but Vt cannot
 *	  	HT	(Ve=Vx)<Vt	symmetric case. Split the length and compute Vt.
 *	  	HT'	(Ve!=Vx)<Vt	asymmetric case. Find H and T by successive approximation.
 *		HBT'			Lb < min body length - treated as an HT case
 *		H'				Lb < min body length - reduce J to fit H to length
 *		T'				Lb < min body length - reduce J to fit T to length
 *
 *	  Requested-Fit cases
 *	  	HBT	Ve<Vt>Vx	sufficient length exists for all part (corner case: HBT')
 *	  	HB	Ve<Vt=Vx	head accelerates to cruise - exits at full speed (corner case: H')
 *	  	BT	Ve=Vt>Vx	enter at full speed and decelerate (corner case: T')
 *	  	HT	Ve & Vx		perfect fit HT (very rare)
 *	  	H	Ve<Vx		perfect fit H (common, results from planning)
 *	  	T	Ve>Vx		perfect fit T (common, results from planning)
 *	  	B	Ve=Vt=Vx	Velocities tested to tolerance
 *
 *	The order of the cases/tests in the code is pretty important
 */

// The minimum lengths are dynamic, and depend on the velocity
// These expressions evaluate to the minimum lengths for the current velocity settings
// Note: The head and tail lengths are 2 minimum segments, the body is 1 min segment
#define MIN_HEAD_LENGTH (MIN_SEGMENT_TIME * (bf->cruise_velocity + bf->entry_velocity))
#define MIN_TAIL_LENGTH (MIN_SEGMENT_TIME * (bf->cruise_velocity + bf->exit_velocity))
#define MIN_BODY_LENGTH (MIN_SEGMENT_TIME * bf->cruise_velocity)

static void _calculate_trapezoid(mpBuf *bf) 
{
	bf->head_length = 0;		// inialize the lengths
	bf->body_length = 0;
	bf->tail_length = 0;
	
	// precomputed squares of velocities (skip cruise_velocity for now)
	double entry_velocity_squared  = square(bf->entry_velocity);
	double exit_velocity_squared   = square(bf->exit_velocity);

	// Combined short cases:
	//	- H and T requested-fit cases (exact fit cases, to within TRAPEZOID_LENGTH_FIT_TOLERANCE)
	//	- H" and T" degraded-fit cases
	//	- H' and T' requested-fit cases where the body residual is less than MIN_BODY_LENGTH
	//	- no-fit case
	// Also converts 2 segment heads and tails that would be too short to a body-only move (1 segment)
	double minimum_length = _get_target_length(entry_velocity_squared, exit_velocity_squared, bf);
	if (bf->length <= (minimum_length + MIN_BODY_LENGTH)) {	// Head & tail cases
		if (bf->entry_velocity > bf->exit_velocity)	{		// Tail cases
			if (bf->length < (minimum_length - TRAPEZOID_LENGTH_FIT_TOLERANCE)) { 	// T" (degraded case)
				bf->entry_velocity = _get_target_velocity(exit_velocity_squared, bf->length, bf);
			}
			bf->cruise_velocity = bf->entry_velocity;
			if (bf->length >= MIN_TAIL_LENGTH) {			// run this as a 2+ segment tail
				bf->tail_length = bf->length;
			} else if (bf->length > MIN_BODY_LENGTH) {		// run this as a 1 segment body
				bf->body_length = bf->length;
			} else {
				bf->move_state = MOVE_STATE_SKIP;			// tell runtime to skip the block
			}
			return;
		}
		if (bf->entry_velocity < bf->exit_velocity)	{		// Head cases
			if (bf->length < (minimum_length - TRAPEZOID_LENGTH_FIT_TOLERANCE)) { 	// H" (degraded case)
				bf->exit_velocity = _get_target_velocity(entry_velocity_squared, bf->length, bf);
			}
			bf->cruise_velocity = bf->exit_velocity;
			if (bf->length >= MIN_HEAD_LENGTH) {			// run this as a 2+ segment head
				bf->head_length = bf->length;
			} else if (bf->length > MIN_BODY_LENGTH) {		// run this as a 1 segment body
				bf->body_length = bf->length;
			} else {
				bf->move_state = MOVE_STATE_SKIP;			// tell runtime to skip the block
			}
			return;
		}
	}

	// Compute the optimal head and tail lengths and see if you can satify the move with these
	double cruise_velocity_squared = square(bf->cruise_velocity);
	bf->head_length = _get_target_length(entry_velocity_squared, cruise_velocity_squared, bf);
	bf->tail_length = _get_target_length(exit_velocity_squared, cruise_velocity_squared, bf);

	// Do target velocity cases
	if (bf->length >= (bf->head_length + bf->tail_length)) {
		bf->body_length = bf->length - bf->head_length - bf->tail_length;
		// If a non-zero body is < minimum length distribute it to the head and/or tail
		// This will generate small (acceptable) velocity errors in runtime execution
		// but preserve correct distance, which is more important.
		if (bf->body_length < MIN_BODY_LENGTH) {
			bf->head_length += bf->body_length/2;
			bf->tail_length += bf->body_length/2;
			bf->body_length = 0;
		}
	} else { // Do reduced velocity cases
		bf->head_length = _get_intersection_distance(entry_velocity_squared, exit_velocity_squared, bf->length, bf);
		bf->cruise_velocity = min(bf->cruise_vmax, _get_target_velocity(entry_velocity_squared, bf->head_length, bf));
		bf->tail_length = bf->length - bf->head_length;		
		if (bf->head_length < MIN_HEAD_LENGTH) {	// adjust if head_length < minimum
			bf->tail_length = bf->length;			// ... to be all tail
			bf->head_length = 0;
		}
		if (bf->tail_length < MIN_TAIL_LENGTH) {	// adjust if tail_length < minimum
			bf->head_length = bf->length;			//... to be all head
			bf->tail_length = 0;
		}
	}
}

/*
	// Compute the optimal head and tail lengths
	double cruise_velocity_squared = square(bf->cruise_velocity);// now pre-compute the cruise velocity square
	bf->head_length = _get_target_length(entry_velocity_squared, cruise_velocity_squared, bf);
	bf->tail_length = _get_target_length(exit_velocity_squared, cruise_velocity_squared, bf);
	if (bf->head_length < MIN_HEAD_LENGTH) { bf->head_length = 0;}
	if (bf->tail_length < MIN_TAIL_LENGTH) { bf->tail_length = 0;}

	// Rate-limited HT and HT' cases
	if (bf->length < (bf->head_length + bf->tail_length)) { // it's rate limited

		// Rate-limited HT case (symmetric case)
		if (fabs(bf->entry_velocity - bf->exit_velocity) < TRAPEZOID_VELOCITY_TOLERANCE) {
			bf->head_length = bf->length/2;
			bf->tail_length = bf->head_length;
			bf->cruise_velocity = min(bf->cruise_vmax, _get_target_velocity(entry_velocity_squared, bf->head_length, bf));
			return;
		}
		bf->head_length = _get_intersection_distance(entry_velocity_squared, exit_velocity_squared, bf->length, bf);
		bf->cruise_velocity = min(bf->cruise_vmax, _get_target_velocity(entry_velocity_squared, bf->head_length, bf));
		bf->tail_length = bf->length - bf->head_length;
		// Adjust the head and tail lengths for corner cases (lengths < minimums)
		if (bf->head_length < MIN_HEAD_LENGTH) {
			bf->tail_length = bf->length;			// adjust the move to be all tail...
			bf->head_length = 0;
		}
		if (bf->tail_length < MIN_TAIL_LENGTH) {
			bf->head_length = bf->length;			//...or all head
			bf->tail_length = 0;
		}
		return;
	}

	// Requested-fit cases: remaining of: HBT, HB, BT, BT, H, T, B, cases
	bf->body_length = bf->length - bf->head_length - bf->tail_length;

	// If a non-zero body is < minimum length distribute it to the head and/or tail
	// This will generate small (acceptable) velocity errors in runtime execution
	// but preserve correct distance, which is more important.
	if ((bf->body_length < MIN_BODY_LENGTH) && (bf->body_length > EPSILON)) {
		if (bf->head_length > EPSILON) {
			if (bf->tail_length > EPSILON) {			// HBT reduces to HT
				bf->head_length += bf->body_length/2;
				bf->tail_length += bf->body_length/2;
			} else {									// HB reduces to H
				bf->head_length += bf->body_length;
			}
		} else {										// BT reduces to T
			bf->tail_length += bf->body_length;
		}
		bf->body_length = 0;

	// If the body is a standalone make the cruise velocity match the entry velocity 
	// This removes a potential velocity discontinuity at the expense of top speed
	} else if ((bf->head_length < EPSILON) && (bf->tail_length < EPSILON)) {
		bf->cruise_velocity = bf->entry_velocity;
	}
}
*/

#else

/*
 * _calculate_trapezoid() - calculate trapezoid parameters
 *
 *	This rather brute-force function sets section lengths and velocities based 
 *	on the line length and velocities requested. It modifies the bf buffer and 
 *	returns accurate head_length, body_length and tail_length, and accurate or 
 *	reasonably approximate velocities. We care about accuracy on lengths, less 
 *	so for velocity (as long as velocity err's on the side of too slow). We need 
 *	the velocities to be set even for zero-length sections so we can compute 
 *	entry and exits for adjacent sections.
 *
 *	Inputs used are:
 *	  bf->length			- actual block length (must remain accurate)
 *	  bf->entry_velocity	- requested Ve
 *	  bf->cruise_velocity	- requested Vt
 *	  bf->exit_velocity		- requested Vx
 *	  bf->cruise_vmax		- used in some comparisons
 *
 *	Variables set may include the velocities above (not the vmax), and:
 *	  bf->head_length		- bf->length allocated to head
 *	  bf->body_length		- bf->length allocated to body
 *	  bf->tail_length		- bf->length allocated to tail
 *
 *	Note: The following condition must be met on entry: Ve <= Vt >= Vx 
 *
 *	Classes of moves:
 *	  Maximum-Fit - The trapezoid can accommodate its maximum velocity values for
 *		the given length (entry_vmax, cruise_vmax, exit_vmax). But the trapezoid 
 *		generator actally doesn't know about the max's and only processes requested 
 *		values.
 *
 *	  Requested-Fit - The move has sufficient length to achieve the target ("set") 
 *		cruising velocity. It will accommodate the acceleration / deceleration 
 *		profile and in the distance given (length)
 *
 *	  Rate-Limited-Fit - The move does not have sufficient length to achieve target 
 *		cruising velocity - the target velocity will be lower than the requested 
 *		velocity. The entry and exit velocities are satisfied. 
 *
 *	  Degraded-Fit - The move does not have sufficient length to transition from
 *		the entry velocity to the exit velocity in the available length. These 
 *		velocities are not negotiable, so a degraded solution is found.
 *
 *	  No-Fit - The move cannot be executed as the planned execution time is less
 *		than the minimum segment interpolation time of the runtime execution module.
 *
 *	Various cases handled;
 *	  No-Fit cases - the line is too short to plan
 *		No fit
 *
 *	  Degraded fit cases - line is too short to satisfy both Ve and Vx
 *	    H"	Ve<Vx		Ve is degraded (velocity step). Vx is met
 *	  	T"	Ve>Vx		Ve is degraded (velocity step). Vx is met
 *	  	B	<short>		line is very short but drawable; is treated as a body only
 *
 *	  Rate-Limited cases - Ve and Vx can be satisfied but Vt cannot
 *	  	HT	(Ve=Vx)<Vt	symmetric case. Split the length and compute Vt.
 *	  	HT'	(Ve!=Vx)<Vt	asymmetric case. Find H and T by successive approximation.
 *		HBT'			Lb < min body length - treated as an HT case
 *		H'				Lb < min body length - reduce J to fit H to length
 *		T'				Lb < min body length - reduce J to fit T to length
 *
 *	  Requested-Fit cases
 *	  	HBT	Ve<Vt>Vx	sufficient length exists for all part (corner case: HBT')
 *	  	HB	Ve<Vt=Vx	head accelerates to cruise - exits at full speed (corner case: H')
 *	  	BT	Ve=Vt>Vx	enter at full speed and decelerate (corner case: T')
 *	  	HT	Ve & Vx		perfect fit HT (very rare)
 *	  	H	Ve<Vx		perfect fit H (common, results from planning)
 *	  	T	Ve>Vx		perfect fit T (common, results from planning)
 *	  	B	Ve=Vt=Vx	Velocities tested to tolerance
 *
 *	The order of the cases/tests in the code is pretty important
 */

// The minimum lengths are dynamic, and depend on the velocity
// These expressions evaluate to the minimum lengths for the current velocity settings
// Note: The head and tail lengths are 2 minimum segments, the body is 1 min segment
#define MIN_HEAD_LENGTH (MIN_SEGMENT_TIME * (bf->cruise_velocity + bf->entry_velocity))
#define MIN_TAIL_LENGTH (MIN_SEGMENT_TIME * (bf->cruise_velocity + bf->exit_velocity))
#define MIN_BODY_LENGTH (MIN_SEGMENT_TIME * bf->cruise_velocity)

static void _calculate_trapezoid(mpBuf *bf) 
{
	bf->head_length = 0;		// inialize the lengths
	bf->body_length = 0;
	bf->tail_length = 0;

	// Combined short cases:
	//	- H and T requested-fit cases (exact fit cases, to within TRAPEZOID_LENGTH_FIT_TOLERANCE)
	//	- H" and T" degraded-fit cases
	//	- H' and T' requested-fit cases where the body residual is less than MIN_BODY_LENGTH
	//	- no-fit case
	// Also converts 2 segment heads and tails that would be too short to a body-only move (1 segment)
	double minimum_length = _get_target_length(bf->entry_velocity, bf->exit_velocity, bf);
	if (bf->length <= (minimum_length + MIN_BODY_LENGTH)) {	// Head & tail cases
		if (bf->entry_velocity > bf->exit_velocity)	{		// Tail cases
			if (bf->length < (minimum_length - TRAPEZOID_LENGTH_FIT_TOLERANCE)) { 	// T" (degraded case)
				bf->entry_velocity = _get_target_velocity(bf->exit_velocity, bf->length, bf);
			}
			bf->cruise_velocity = bf->entry_velocity;
			if (bf->length >= MIN_TAIL_LENGTH) {			// run this as a 2+ segment tail
				bf->tail_length = bf->length;
			} else if (bf->length > MIN_BODY_LENGTH) {		// run this as a 1 segment body
				bf->body_length = bf->length;
			} else {
				bf->move_state = MOVE_STATE_SKIP;			// tell runtime to skip the block
			}
			return;
		}
		if (bf->entry_velocity < bf->exit_velocity)	{		// Head cases
			if (bf->length < (minimum_length - TRAPEZOID_LENGTH_FIT_TOLERANCE)) { 	// H" (degraded case)
				bf->exit_velocity = _get_target_velocity(bf->entry_velocity, bf->length, bf);
			}
			bf->cruise_velocity = bf->exit_velocity;
			if (bf->length >= MIN_HEAD_LENGTH) {			// run this as a 2+ segment head
				bf->head_length = bf->length;
			} else if (bf->length > MIN_BODY_LENGTH) {		// run this as a 1 segment body
				bf->body_length = bf->length;
			} else {
				bf->move_state = MOVE_STATE_SKIP;			// tell runtime to skip the block
			}
			return;
		}
	}
	// Set head and tail lengths
	bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
	bf->tail_length = _get_target_length(bf->exit_velocity, bf->cruise_velocity, bf);
	if (bf->head_length < MIN_HEAD_LENGTH) { bf->head_length = 0;}
	if (bf->tail_length < MIN_TAIL_LENGTH) { bf->tail_length = 0;}

	// Rate-limited HT and HT' cases
	if (bf->length < (bf->head_length + bf->tail_length)) { // it's rate limited

		// Rate-limited HT case (symmetric case)
		if (fabs(bf->entry_velocity - bf->exit_velocity) < TRAPEZOID_VELOCITY_TOLERANCE) {
			bf->head_length = bf->length/2;
			bf->tail_length = bf->head_length;
			bf->cruise_velocity = min(bf->cruise_vmax, _get_target_velocity(bf->entry_velocity, bf->head_length, bf));
			return;
		}

		// Rate-limited HT' case (asymmetric) - this is relatively expensive but it's not called very often
		double computed_velocity = bf->cruise_vmax;
		uint8_t i=0;
		do {
			bf->cruise_velocity = computed_velocity;	// initialize from previous iteration 
			bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
			bf->tail_length = _get_target_length(bf->exit_velocity, bf->cruise_velocity, bf);
			if (bf->head_length > bf->tail_length) {
				bf->head_length = (bf->head_length / (bf->head_length + bf->tail_length)) * bf->length;
				computed_velocity = _get_target_velocity(bf->entry_velocity, bf->head_length, bf);
			} else {
				bf->tail_length = (bf->tail_length / (bf->head_length + bf->tail_length)) * bf->length;
				computed_velocity = _get_target_velocity(bf->exit_velocity, bf->tail_length, bf);
			}
			if (++i > TRAPEZOID_ITERATION_MAX) { fprintf_P(stderr,PSTR("_calculate_trapezoid() failed to converge"));}
		} while ((fabs(bf->cruise_velocity - computed_velocity) / computed_velocity) > TRAPEZOID_ITERATION_ERROR_PERCENT);
		bf->cruise_velocity = computed_velocity;
		bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
		bf->tail_length = bf->length - bf->head_length;
		if (bf->head_length < MIN_HEAD_LENGTH) {
			bf->tail_length = bf->length;			// adjust the move to be all tail...
			bf->head_length = 0;					// adjust the jerk to fit to the adjusted length
		}
		if (bf->tail_length < MIN_TAIL_LENGTH) {
			bf->head_length = bf->length;			//...or all head
			bf->tail_length = 0;
		}
		return;
	}

	// Requested-fit cases: remaining of: HBT, HB, BT, BT, H, T, B, cases
	bf->body_length = bf->length - bf->head_length - bf->tail_length;

	// If a non-zero body is < minimum length distribute it to the head and/or tail
	// This will generate small (acceptable) velocity errors in runtime execution
	// but preserve correct distance, which is more important.
	if ((bf->body_length < MIN_BODY_LENGTH) && (bf->body_length > EPSILON)) {
		if (bf->head_length > EPSILON) {
			if (bf->tail_length > EPSILON) {			// HBT reduces to HT
				bf->head_length += bf->body_length/2;
				bf->tail_length += bf->body_length/2;
			} else {									// HB reduces to H
				bf->head_length += bf->body_length;
			}
		} else {										// BT reduces to T
			bf->tail_length += bf->body_length;
		}
		bf->body_length = 0;

	// If the body is a standalone make the cruise velocity match the entry velocity 
	// This removes a potential velocity discontinuity at the expense of top speed
	} else if ((bf->head_length < EPSILON) && (bf->tail_length < EPSILON)) {
		bf->cruise_velocity = bf->entry_velocity;
	}
}
#endif // ifdef __PLAN_R2 

/*	
 * _get_target_length()		- derive accel/decel length from delta V and jerk
 * _get_target_velocity()	- derive velocity achievable from delta V and length
 *
 *	This set of functions returns the fourth thing knowing the other three.
 *	
 *	_get_target_length() is a convenient function for determining the 
 *	optimal_length (L) of a line given the inital velocity (Vi), 
 *	target velocity (Vt) and maximum jerk (Jm).
 *
 *	The length (distance) equation uses a linear acceleration ramp of rate Ar.
 *	This works because the distance covered by the convex AND concave sections 
 *	combined adds up to the same as a linear acceleration period.

 * 	  Jm = the given maximum jerk
 *	  T  = time of the entire move
 *	  T  = 2*sqrt((Vt-Vi)/Jm)
 *	  As = The acceleration at inflection point between convex and concave portions of the S-curve.
 *	  As = (Jm*T)/2
 *    Ar = ramp acceleration
 *	  Ar = As/2 = (Jm*T)/4
 *	
 *	Assumes Vt, Vi and L are positive or zero
 *	Cannot assume Vt>=Vi due to rounding errors and use of PLANNER_VELOCITY_TOLERANCE
 *	necessitating the introduction of fabs()
 *
 *	Linear acceleration formula solved for length:
 *
 *	 a)  L = (Vt^2 - Vi^2)/(2 Ar)
 *	 b)  L = (Vt^2 - Vi^2)/(2 (Jm*T)/4) = (Vt^2 - Vi^2)/((Jm*T)/2)
 *	 c)  L = (Vt^2 - Vi^2)/(Jm/2)     (We can safely assume we need the entire movement, and treat T as 1)
 *
 * 	_get_target_velocity() is a convenient function for determining Vt target  
 *	velocity for a given the initial velocity (Vi), length (L), and maximum jerk (Jm).
 *	Equation d) is b) solved for Vt. e) Returns Vt^2, which can delay the sqrt() until needed.
 *
 *	 d)	Vt = sqrt(L*(Jm/2)+Vi^2)
 *	 e)	Vt^2 = L*(Jm/2)+Vi^2
 *
 *	When using linear acceleration, we can easily compute what distance (D) to start decelerating
 *	to Vt at total distance L, after accelerating from Vi without even stopping at a plateau.
 *	This is case HT' described above.
 *
 *	 f) solve Sqrt[2 (j/2) D + v^2] = Sqrt[2 (j/2) (L-D) + V^2] for D
 *	 g) D = (L j - Vi^2 + Vt^2)/(2j)
 *
 *	NOTE that many operations require Vi^2, Vt^2, and Jm/2. 
 *	These should be precomputed and stored when possible.
 *
 *  FYI: Here's an expression that returns the jerk for a given deltaV and L:
 * 	 return(cube(deltaV / (pow(L, 0.66666666))));
 */
#ifdef __PLAN_R2
static double _get_target_length(const double Vi_squared, const double Vt_squared, const mpBuf *bf)
{
	return fabs((Vt_squared - Vi_squared) * bf->recip_half_jerk);
}

static double _get_target_velocity(const double Vi_squared, const double L, const mpBuf *bf)
{
	return sqrt(L * bf->half_jerk + Vi_squared);
}

static double _get_intersection_distance(const double Vi_squared, const double Vt_squared, const double L, const mpBuf *bf)
{
	return (L * bf->jerk - Vi_squared + Vt_squared)/(2 * bf->jerk);
}
#else

/*	
 * _get_target_length()		- derive accel/decel length from delta V and jerk
 * _get_target_velocity()	- derive velocity achievable from delta V and length
 *
 *	This set of functions returns the fourth thing knowing the other three.
 *	
 *	_get_target_length() is a convenient function for determining the 
 *	optimal_length (L) of a line given the inital velocity (Vi), 
 *	target velocity (Vt) and maximum jerk (Jm).
 *
 *	The length (distance) equation is derived from: 
 *
 *	 a)	L = (Vt-Vi) * T - (Ar*T^2)/2	... which becomes b) with substitutions for Ar and T
 *	 b) L = (Vt-Vi) * 2*sqrt((Vt-Vi)/Jm) - (2*sqrt((Vt-Vi)/Jm) * (Vt-Vi))/2
 *	 c)	L = (Vt-Vi)^(3/2) / sqrt(Jm)	...is an alternate form of b) (see Wolfram Alpha)
 *	 c')L = (Vt-Vi) * sqrt((Vt-Vi)/Jm) ... second alternate form; requires Vt >= Vi
 *
 *	 Notes: Ar = (Jm*T)/4					Ar is ramp acceleration
 *			T  = 2*sqrt((Vt-Vi)/Jm)			T is time
 *			Assumes Vt, Vi and L are positive or zero
 *			Cannot assume Vt>=Vi due to rounding errors and use of PLANNER_VELOCITY_TOLERANCE
 *			  necessitating the introduction of fabs()
 *
 * 	_get_target_velocity() is a convenient function for determining Vt target 
 *	velocity for a given the initial velocity (Vi), length (L), and maximum jerk (Jm).
 *	Equation d) is b) solved for Vt. Equation e) is c) solved for Vt. Use e) (obviously)
 *
 *	 d)	Vt = (sqrt(L)*(L/sqrt(1/Jm))^(1/6)+(1/Jm)^(1/4)*Vi)/(1/Jm)^(1/4)
 *	 e)	Vt = L^(2/3) * Jm^(1/3) + Vi
 *
 *  FYI: Here's an expression that returns the jerk for a given deltaV and L:
 * 	return(cube(deltaV / (pow(L, 0.66666666))));
 */
static double _get_target_length(const double Vi, const double Vt, const mpBuf *bf)
{
	return (fabs(Vi-Vt) * sqrt(fabs(Vi-Vt) * bf->recip_jerk));
}

static double _get_target_velocity(const double Vi, const double L, const mpBuf *bf)
{
	return (pow(L, 0.66666666) * bf->cbrt_jerk + Vi);
}
#endif

/*
 * _get_junction_vmax() - Chamnit's algorithm - simple
 *
 *  Computes the maximum allowable junction speed by finding the velocity
 *	that will yield the centripetal acceleration in the corner_acceleration 
 *	value. The value of delta sets the effective radius of curvature.
 *	Here's Chamnit's (Sungeun K. Jeon's) explanation of what's going on:
 *
 *	"First let's assume that at a junction we only look a centripetal 
 *	acceleration to simply things. At a junction of two lines, let's place 
 *	a circle such that both lines are tangent to the circle. The circular 
 *	segment joining the lines represents the path for constant centripetal 
 *	acceleration. This creates a deviation from the path (let's call this 
 *	delta), which is the distance from the junction to the edge of the 
 *	circular segment. Delta needs to be defined, so let's replace the 
 *	term max_jerk with max_junction_deviation( or delta). This indirectly 
 *	sets the radius of the circle, and hence limits the velocity by the 
 *	centripetal acceleration. Think of the this as widening the race track.
 *	If a race car is driving on a track only as wide as a car, it'll have 
 *	to slow down a lot to turn corners. If we widen the track a bit, the 
 *	car can start to use the track to go into the turn. The wider it is, 
 *	the faster through the corner it can go.
 *
 *	If you do the geometry in terms of the known variables, you get:
 *	sin(theta/2) = R/(R+delta)  Re-arranging in terms of circle radius (R)
 *	R = delta*sin(theta/2)/(1-sin(theta/2). Theta is the angle between 
 *	line segments given by: cos(theta) = dot(a,b)/(norm(a)*norm(b)). 
 *	Most of these calculations are already done in the planner. To remove 
 *	the acos() and sin() computations, use the trig half angle identity: 
 *	sin(theta/2) = +/- sqrt((1-cos(theta))/2). For our applications, this 
 *	should always be positive. Now just plug and chug the equations into 
 *	the centripetal acceleration equation: v_c = sqrt(a_max*R). You'll see
 *	that there are only two sqrt computations and no sine/cosines."
 *
 *	How to compute the radius using brute-force trig:
 *	double theta = acos(costheta);
 *	double radius = delta * sin(theta/2)/(1-sin(theta/2));
 */
/*  This version function extends Chamnit's algorithm by computing a value
 *	for delta that takes the contributions of the individual axes in the 
 *	move into account. It allows the radius of curvature to vary by axis.
 *	This is necessary to support axes that have different dynamics; such 
 *	as a Z axis that doesn't move as fast as X and Y (such as a screw driven 
 *	Z axis on machine with a belt driven XY - like a Shapeoko), or rotary 
 *	axes ABC that have completely different dynamics than their linear 
 *	counterparts.
 *
 *	The function takes the absolute values of the sum of the unit vector
 *	components as a measure of contribution to the move, then scales the 
 *	delta values from the non-zero axes into a composite delta to be used
 *	for the move. Shown for an XY vector:
 *
 *	 U[i]	Unit sum of i'th axis	fabs(unit_a[i]) + fabs(unit_b[i])
 *	 Usum	Length of sums			Ux + Uy
 *	 d		Delta of sums			(Dx*Ux+DY*UY)/Usum
 */
static double _get_junction_vmax(const double a_unit[], const double b_unit[])
{
	double costheta = - (a_unit[X] * b_unit[X]) - (a_unit[Y] * b_unit[Y]) 
					  - (a_unit[Z] * b_unit[Z]) - (a_unit[A] * b_unit[A]) 
					  - (a_unit[B] * b_unit[B]) - (a_unit[C] * b_unit[C]);

	if (costheta < -0.99) { return (10000000); } 		// straight line cases
	if (costheta > 0.99)  { return (0); } 				// reversal cases

	// Fuse the junction deviations into a vector sum
	double a_delta = square(a_unit[X] * cfg.a[X].junction_dev);
	a_delta += square(a_unit[Y] * cfg.a[Y].junction_dev);
	a_delta += square(a_unit[Z] * cfg.a[Z].junction_dev);
	a_delta += square(a_unit[A] * cfg.a[A].junction_dev);
	a_delta += square(a_unit[B] * cfg.a[B].junction_dev);
	a_delta += square(a_unit[C] * cfg.a[C].junction_dev);

	double b_delta = square(b_unit[X] * cfg.a[X].junction_dev);
	b_delta += square(b_unit[Y] * cfg.a[Y].junction_dev);
	b_delta += square(b_unit[Z] * cfg.a[Z].junction_dev);
	b_delta += square(b_unit[A] * cfg.a[A].junction_dev);
	b_delta += square(b_unit[B] * cfg.a[B].junction_dev);
	b_delta += square(b_unit[C] * cfg.a[C].junction_dev);

	double delta = (sqrt(a_delta) + sqrt(b_delta))/2;
	double sintheta_over2 = sqrt((1 - costheta)/2);
	double radius = delta * sintheta_over2 / (1-sintheta_over2);
	return(sqrt(radius * cfg.junction_acceleration));
}

/*************************************************************************
 * feedholds - functions for performing holds
 *
 * mp_plan_hold_callback() - replan block list to execute hold
 * mp_end_hold_callback() - remove the hold and restart block list
 *
 *	Feedhold is executed as cm.hold_state transitions executed inside 
 *	_exec_aline() and main loop callbacks to these functions:
 *	mp_plan_hold_callback() and mp_end_hold_callback().
 */
/*	Holds work like this:
 * 
 * 	  - Hold is asserted by calling cm_feedhold() (usually invoked via a ! char)
 *		If hold_state is OFF and motion_state is RUNning it sets 
 *		hold_state to SYNC and motion_state to HOLD.
 *
 *	  - Hold state == SYNC tells the aline exec routine to execute the next aline 
 *		segment then set hold_state to PLAN. This gives the planner sufficient 
 *		time to replan the block list for the hold before the next aline 
 *		segment needs to be processed.
 *
 *	  - Hold state == PLAN tells the planner to replan the mr buffer, the current
 *		run buffer (bf), and any subsequent bf buffers as necessary to execute a
 *		hold. Hold planning replans the planner buffer queue down to zero and then
 *		back up from zero. Hold state is set to DECEL when planning is complete.
 *
 *	  - Hold state == DECEL persists until the aline execution gets runs to 
 *		zero velocity, at which point hold state transitions to HOLD.
 *
 *	  - Hold state == HOLD persists until the cycle is restarted. A cycle start 
 *		is an asynchronous event that sets the cycle_start_flag TRUE. It can 
 *		occur any time after the hold is requested - either before or after 
 *		motion stops.
 *
 *	  - mp_end_hold_callback() will execute once the hold state == HOLD and 
 *		cycle_start_flag == TRUE. This sets the hold state to OFF which enables
 *		_exec_aline() to continue processing. Move execution begins with the 
 *		first buffer after the hold.
 *
 *	Terms used:
 *	 - mr is the runtime buffer. It was initially loaded from the bf buffer
 *	 - bp+0 is the "companion" bf buffer to the mr buffer.
 *	 - bp+1 is the bf buffer following bp+0. This runs through bp+N
 *	 - bp (by itself) just refers to the current buffer being adjusted / replanned
 *
 *	Details: Planning re-uses bp+0 as an "extra" buffer. Normally bp+0 is returned 
 *		to the buffer pool as it is redundant once mr is loaded. Use the extra 
 *		buffer to split the move in two where the hold decelerates to zero. Use 
 *		one buffer to go to zero, the other to replan up from zero. All buffers past
 *		that point are unaffected other than that they need to be replanned for velocity.  
 *
 *	Note: There are multiple opportunities for more efficient organization of 
 *		  code in this module, but the code is so complicated I just left it
 *		  organized for clarity and hoped for the best from compiler optimization. 
 */

uint8_t mp_plan_hold_callback()
{
	if (cm.hold_state != FEEDHOLD_PLAN) { return (TG_NOOP);}	// not planning a feedhold

//	printf("Enter feedhold [%d]\n", cm.hold_state); 	//+++++++++++++++++++++++

	mpBuf *bp; 					// working buffer pointer
	if ((bp = _get_run_buffer()) == NULL) { return (TG_NOOP);}	// Oops! nothing's running

	uint8_t mr_flag = true;		// used to tell replan to account for mr buffer Vx
	double mr_available_length; // available length left in mr buffer for deceleration
	double braking_velocity;	// velocity left to shed to brake to zero
	double braking_length;		// distance required to brake to zero from braking_velocity

	// examine and process mr buffer
	mr_available_length = get_axis_vector_length(mr.endpoint, mr.position);

/*	mr_available_length = 
		(sqrt(square(mr.endpoint[X] - mr.position[X]) +
			  square(mr.endpoint[Y] - mr.position[Y]) +
			  square(mr.endpoint[Z] - mr.position[Z]) +
			  square(mr.endpoint[A] - mr.position[A]) +
			  square(mr.endpoint[B] - mr.position[B]) +
			  square(mr.endpoint[C] - mr.position[C])));
*/
//	braking_velocity = mr.segment_velocity;
	braking_velocity = _compute_next_segment_velocity();

#ifdef __PLAN_R2
	braking_length = _get_target_length(square(braking_velocity), 0, bp); // bp is OK to use here
#else
	braking_length = _get_target_length(braking_velocity, 0, bp); // bp is OK to use here
#endif
	
	// Hack to prevent Case 2 moves for perfect-fit decels. Happens in homing situations
	// The real fix: The braking velocity cannot simply be the mr.segment_velocity as this
	// is the velocity of the last segment, not the one that's going to be executed next.
	// The braking_velocity needs to be the velocity of the next segment that has not yet 
	// been computed. In the eman time, this hack will work. 
	if ((braking_length > mr_available_length) && (bp->exit_velocity < EPSILON)) {
		braking_length = mr_available_length;
	}

//	printf("end:%f pos:%f avail:%f brake:%f\n", mr.endpoint[X], mr.position[X], mr_available_length, braking_length);	//+++++++++++++++++++++++++++

	// Case 1: deceleration fits entirely in mr
	if (braking_length <= mr_available_length) {
//		printf("Feedhold case 1 [%d]\n", cm.hold_state); //+++++++++++++++++++++++
		// set mr to a tail to perform the deceleration
		mr.exit_velocity = 0;
		mr.tail_length = braking_length;
		mr.cruise_velocity = braking_velocity;
		mr.move_state = MOVE_STATE_TAIL;
		mr.section_state = MOVE_STATE_NEW;

		// re-use bp+0 to be the hold point and to draw the remaining length
		bp->length = mr_available_length - braking_length;
		bp->delta_vmax = _get_target_velocity(0, bp->length, bp);
		bp->entry_vmax = 0;						// set bp+0 as hold point
		bp->move_state = MOVE_STATE_NEW;		// tell _exec to re-use the bf buffer

		_reset_replannable_list();				// make it replan all the blocks
		_plan_block_list(_get_last_buffer(), &mr_flag);
		cm.hold_state = FEEDHOLD_DECEL;			// set state to decelerate and exit
		return (TG_OK);
	}

	// Case 2: deceleration exceeds available length in mr buffer
	// First, replan mr to minimum (but non-zero) exit velocity

//	printf("Feedhold case 2 [%d]\n", cm.hold_state); 			  //+++++++++++++++++++++
//	mp_dump_runtime_state();	// turn on __DEBUG if you need this +++++++++++++++++++++
//	_dump_plan_buffer(bp); 		// turn on __DEBUG if you need this +++++++++++++++++++++
//	_dump_plan_buffer(bp->nx);	// turn on __DEBUG if you need this +++++++++++++++++++++

	mr.move_state = MOVE_STATE_TAIL;
	mr.section_state = MOVE_STATE_NEW;
	mr.tail_length = mr_available_length;
	mr.cruise_velocity = braking_velocity;
	mr.exit_velocity = braking_velocity - _get_target_velocity(0, mr_available_length, bp);	

	// Find the point where deceleration reaches zero. This could span multiple buffers.
	braking_velocity = mr.exit_velocity;		// adjust braking velocity downward
	bp->move_state = MOVE_STATE_NEW;			// tell _exec to re-use buffer
	for (uint8_t i=0; i<PLANNER_BUFFER_POOL_SIZE; i++) {// a safety to avoid wraparound
		_copy_buffer(bp, bp->nx);				// copy bp+1 into bp+0 (and onward...)
		if (bp->move_type != MOVE_TYPE_ALINE) {	// skip any non-move buffers
			bp = _get_next_buffer(bp);			// point to next buffer
			continue;
		}
		bp->entry_vmax = braking_velocity;		// velocity we need to shed
#ifdef __PLAN_R2
		braking_length = _get_target_length(square(braking_velocity), 0, bp);
#else
		braking_length = _get_target_length(braking_velocity, 0, bp);
#endif
		if (braking_length > bp->length) {		// decel does not fit in bp buffer
			bp->exit_vmax = braking_velocity - _get_target_velocity(0, bp->length, bp);
			braking_velocity = bp->exit_vmax;	// braking velocity for next buffer
			bp = _get_next_buffer(bp);			// point to next buffer
			continue;
		}
		break;
	}
	// Deceleration now fits in the current bp buffer
	// Plan the first buffer of the pair as the decel, the second as the accel
	bp->length = braking_length;
	bp->exit_vmax = 0;

	bp = _get_next_buffer(bp);					// point to the acceleration buffer
	bp->entry_vmax = 0;
	bp->length -= braking_length;				// the buffers were identical (and hence their lengths)
	bp->delta_vmax = _get_target_velocity(0, bp->length, bp);
	bp->exit_vmax = bp->delta_vmax;

	_reset_replannable_list();					// make it replan all the blocks
	_plan_block_list(_get_last_buffer(), &mr_flag);
	cm.hold_state = FEEDHOLD_DECEL;				// set state to decelerate and exit
	return (TG_OK);
}

double _compute_next_segment_velocity()
{
	if (mr.move_state == MOVE_STATE_BODY) {
		return (mr.segment_velocity);
	}
	return (mr.segment_velocity + mr.forward_diff_1);
}

/* 
 * mp_end_hold_callback() - callback from main loop to end a feedhold
 *
 * 	This function is a callback that is called from the controller. To end a 
 *	hold do not call mp_end_feedhold() directly, instead call cm_cycle_start().
 */

uint8_t mp_end_hold_callback()
{
	mpBuf *bf;
	if ((cm.hold_state == FEEDHOLD_HOLD) && (cm.cycle_start_flag == true)) { 
		cm.cycle_start_flag = false;
		cm.hold_state = FEEDHOLD_OFF;
		if ((bf = _get_run_buffer()) == NULL) {	// NULL means nothing's running
			cm.motion_state = MOTION_STOP;
			return (TG_NOOP);
		}
		cm.motion_state = MOTION_RUN;
		st_request_exec_move();					// restart the steppers
	}
	return (TG_OK);
}

/*************************************************************************/
/**** ALINE EXECUTION ROUTINES *******************************************/
/*************************************************************************
 * ---> Everything here fires from LO interrupt and must be interrupt safe
 *
 *  _exec_aline()			- acceleration line main routine
 *	_exec_aline_head()		- helper for acceleration section
 *	_exec_aline_body()		- helper for cruise section
 *	_exec_aline_tail()		- helper for deceleration section
 *	_exec_aline_segment()	- helper for running a segment
 *
 *	Returns:
 *	 TG_OK		move is done
 *	 TG_EAGAIN	move is not finished - has more segments to run
 *	 TG_NOOP	cause no operation from the steppers - do not load the move
 *	 TG_xxxxx	fatal error. Ends the move and frees the bf buffer
 *	
 *	This routine is called from the (LO) interrupt level. The interrupt 
 *	sequencing relies on the behaviors of the routines being exactly correct.
 *	Each call to _exec_aline() must execute and prep *one and only one* 
 *	segment. If the segment is the not the last segment in the bf buffer the 
 *	_aline() must return TG_EAGAIN. If it's the last segment it must return 
 *	TG_OK. If it encounters a fatal error that would terminate the move it 
 *	should return a valid error code. Failure to obey this will introduce 
 *	subtle and very difficult to diagnose bugs (trust me on this).
 *
 *	Note 1 Returning TG_OK ends the move and frees the bf buffer. 
 *		   Returning TG_OK at this point does NOT advance position meaning any
 *		   position error will be compensated by the next move.
 *
 *	Note 2 Solves a potential race condition where the current move ends but the 
 * 		   new move has not started because the previous move is still being run 
 *		   by the steppers. Planning can overwrite the new move.
 */
/* OPERATION:
 *	Aline generates jerk-controlled S-curves as per Ed Red's course notes:
 *	  http://www.et.byu.edu/~ered/ME537/Notes/Ch5.pdf
 *	  http://www.scribd.com/doc/63521608/Ed-Red-Ch5-537-Jerk-Equations
 *
 *	A full trapezoid is divided into 5 periods Periods 1 and 2 are the 
 *	first and second halves of the acceleration ramp (the concave and convex 
 *	parts of the S curve in the "head"). Periods 3 and 4 are the first 
 *	and second parts of the deceleration ramp (the tail). There is also 
 *	a period for the constant-velocity plateau of the trapezoid (the body).
 *	There are various degraded trapezoids possible, including 2 section 
 *	combinations (head and tail; head and body; body and tail), and single 
 *	sections - any one of the three.
 *
 *	The equations that govern the acceleration and deceleration ramps are:
 *
 *	  Period 1	  V = Vi + Jm*(T^2)/2
 *	  Period 2	  V = Vh + As*T - Jm*(T^2)/2
 *	  Period 3	  V = Vi - Jm*(T^2)/2
 *	  Period 4	  V = Vh + As*T + Jm*(T^2)/2
 *
 * 	These routines play some games with the acceleration and move timing 
 *	to make sure this actually all works out. move_time is the actual time of the 
 *	move, accel_time is the time valaue needed to compute the velocity - which 
 *	takes the initial velocity into account (move_time does not need to).
 */
/* --- State transitions - hierarchical state machine ---
 *
 *	bf->move_state transitions:
 *	 from _NEW to _RUN on first call (sub_state set to _OFF)
 *	 from _RUN to _OFF on final call
 * 	 or just remains _OFF
 *
 *	mr.move_state transitions on first call from _OFF to one of _HEAD, _BODY, _TAIL
 *	Within each section state may be 
 *	 _NEW - trigger initialization
 *	 _RUN1 - run the first part
 *	 _RUN2 - run the second part 
 */
static uint8_t _exec_aline(mpBuf *bf)
{
	uint8_t status = TG_OK;

	if (bf->move_state == MOVE_STATE_OFF) { return (TG_NOOP);} 
	if (mr.move_state == MOVE_STATE_OFF) {
		if (cm.hold_state == FEEDHOLD_HOLD) { return (TG_NOOP);}// stops here if holding

		// initialization to process the new incoming bf buffer
		bf->replannable = false;
		if (bf->length < EPSILON) {
			mr.move_state = MOVE_STATE_OFF;			// reset mr buffer
			mr.section_state = MOVE_STATE_OFF;
			bf->nx->replannable = false;			// prevent overplanning (Note 2)
			st_prep_null();							// call this to leep the loader happy
			_free_run_buffer();
			return (TG_NOOP);
		}
		bf->move_state = MOVE_STATE_RUN;
		mr.move_state = MOVE_STATE_HEAD;
		mr.section_state = MOVE_STATE_NEW;
		mr.linenum = bf->linenum;
		mr.lineindex = bf->lineindex;
		mr.jerk = bf->jerk;
#ifndef __EXEC_R2
		mr.jerk_div2 = bf->jerk/2;
#endif
		mr.head_length = bf->head_length;
		mr.body_length = bf->body_length;
		mr.tail_length = bf->tail_length;
		mr.entry_velocity = bf->entry_velocity;
		mr.cruise_velocity = bf->cruise_velocity;
		mr.exit_velocity = bf->exit_velocity;
		copy_axis_vector(mr.unit, bf->unit);
		copy_axis_vector(mr.endpoint, bf->target);	// save the final target of the move
		copy_axis_vector(mr.work_offset, bf->work_offset);// propagate offset
	}
	// NB: from this point on the contents of the bf buffer do not affect execution


	//**** main dispatcher to process segments ***
	switch (mr.move_state) {
		case (MOVE_STATE_HEAD): { status = _exec_aline_head(); break;}
		case (MOVE_STATE_BODY): { status = _exec_aline_body(); break;}
		case (MOVE_STATE_TAIL): { status = _exec_aline_tail(); break;}
		case (MOVE_STATE_SKIP): { status = TG_OK; break;}
	}

	// feed hold post-processing
	if (cm.hold_state == FEEDHOLD_SYNC) { 
		cm.hold_state = FEEDHOLD_PLAN;
	}

	// initiate the hold - look for the end of the decel move
	if ((cm.hold_state == FEEDHOLD_DECEL) && (status == TG_OK)) {
		cm.hold_state = FEEDHOLD_HOLD;
		rpt_request_status_report();
	}

	// There are 3 things that can happen here depending on return conditions:
	//	  status	 bf->move_state	 Description
	//    ---------	 --------------	 ----------------------------------------
	//	  TG_EAGAIN	 <don't care>	 mr buffer has more segments to run
	//	  TG_OK		 MOVE_STATE_RUN	 mr and bf buffers are done
	//	  TG_OK		 MOVE_STATE_NEW	 mr done; bf must be run again (it's been reused)

	if (status == TG_EAGAIN) { 
		rpt_decr_status_report(); 				// continue running mr buffer
	} else {
		mr.move_state = MOVE_STATE_OFF;			// reset mr buffer
		mr.section_state = MOVE_STATE_OFF;
		bf->nx->replannable = false;			// prevent overplanning (Note 2)
		if (bf->move_state == MOVE_STATE_RUN) {
			_free_run_buffer();					// free bf if it's actually done
		}
	}
	return (status);
}

/* Forward difference math explained:
 * 	We're using two quadratic curve end-to-end, forming the concave and convex 
 *	section of the s-curve.
 *	For each half, we have three points:
 * 	T[0] is the start point, or the entro or middle of the "s". This will be one of:
 *  	entry_velocity (acceleration concave),
 * 		cruise_velocity (deceleration concave), or
 * 		midpoint_velocity (convex)
 *	T[1] is the "control point" set to T[0] for concave sections, and T[2] for convex
 *	T[2] is the end point of the quadratic, which will be the midpoint or endpoint of the s.
 *
 *  TODO MATH EXPLANATION
 *  
 *  A = T[0] - 2*T[1] + T[2]
 *  B = 2 * (T[1] - T[0])
 *  C = T[0]
 *  h = (1/mr.segments)

 *  forward_diff_1 = Ah^2+Bh = (T[0] - 2*T[1] + T[2])h*h + (2 * (T[1] - T[0]))h
 *  forward_diff_2 = 2Ah^2 = 2*(T[0] - 2*T[1] + T[2])h*h
 */

// NOTE: t1 will always be == t0, so we don't pass it
static void _init_forward_diffs(double t0, double t2)
{
#ifdef __EXEC_R2
	double H_squared = square(1/mr.segments);
	// A = T[0] - 2*T[1] + T[2], if T[0] == T[1], then it becomes - T[0] + T[2]
	double AH_squared = (t2 - t0) * H_squared;
	
	// Ah²+Bh, and B=2 * (T[1] - T[0]), if T[0] == T[1], then it becomes simply Ah^2
	mr.forward_diff_1 = AH_squared;
	mr.forward_diff_2 = 2*AH_squared;
	mr.segment_velocity = t0;
#endif
}

/*
 * _exec_aline_head()
 */
static uint8_t _exec_aline_head()
#ifdef __EXEC_R2
{
	if (mr.section_state == MOVE_STATE_NEW) {	// initialize the move singleton (mr)
		if (mr.head_length < EPSILON) { 
			mr.move_state = MOVE_STATE_BODY;
			return(_exec_aline_body());			// skip ahead to the body generator
		}
		mr.midpoint_velocity = (mr.entry_velocity + mr.cruise_velocity) / 2;
		mr.move_time = mr.head_length / mr.midpoint_velocity;	// time for entire accel region
		mr.segments = ceil(uSec(mr.move_time) / (2 * cfg.estd_segment_usec)); // # of segments in *each half*
		mr.segment_move_time = mr.move_time / (2 * mr.segments);
		mr.segment_count = (uint32_t)mr.segments;
		if ((mr.microseconds = uSec(mr.segment_move_time)) < MIN_SEGMENT_USEC) {
			return(TG_GCODE_BLOCK_SKIPPED);		// exit without advancing position
		}
		_init_forward_diffs(mr.entry_velocity, mr.midpoint_velocity);
		mr.section_state = MOVE_STATE_RUN1;
	}
	if (mr.section_state == MOVE_STATE_RUN1) {	// concave part of accel curve (period 1)
		mr.segment_velocity += mr.forward_diff_1;
		if (_exec_aline_segment(false) == TG_COMPLETE) { // set up for second half
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = MOVE_STATE_RUN2;

			// Here's a trick: The second half of the S starts at the end of the first,
			//  And the only thing that changes is the sign of mr.forward_diff_2
			mr.forward_diff_2 = -mr.forward_diff_2;
		} else {
			mr.forward_diff_1 += mr.forward_diff_2;
		}
		return(TG_EAGAIN);
	}
	if (mr.section_state == MOVE_STATE_RUN2) {	// convex part of accel curve (period 2)
		mr.segment_velocity += mr.forward_diff_1;
		mr.forward_diff_1 += mr.forward_diff_2;
		if (_exec_aline_segment(false) == TG_COMPLETE) {
			if ((mr.body_length < EPSILON) && (mr.tail_length < EPSILON)) { return(TG_OK);}	// end the move
			mr.move_state = MOVE_STATE_BODY;
			mr.section_state = MOVE_STATE_NEW;
		}
	}
	return(TG_EAGAIN);
}
#else
{
	if (mr.section_state == MOVE_STATE_NEW) {	// initialize the move singleton (mr)
		if (mr.head_length < EPSILON) { 
			mr.move_state = MOVE_STATE_BODY;
			return(_exec_aline_body());			// skip ahead to the body generator
		}
		mr.midpoint_velocity = (mr.entry_velocity + mr.cruise_velocity) / 2;
		mr.move_time = mr.head_length / mr.midpoint_velocity;	// time for entire accel region
		mr.accel_time = 2 * sqrt((mr.cruise_velocity - mr.entry_velocity) / mr.jerk);
		mr.midpoint_acceleration = 2 * (mr.cruise_velocity - mr.entry_velocity) / mr.accel_time;
		mr.segments = ceil(uSec(mr.move_time) / (2 * cfg.estd_segment_usec)); // # of segments in *each half*
		mr.segment_move_time = mr.move_time / (2 * mr.segments);
		mr.segment_accel_time = mr.accel_time / (2 * mr.segments);// time to advance for each segment
		mr.elapsed_accel_time = mr.segment_accel_time / 2;	// elapsed time starting point (offset)
		mr.segment_count = (uint32_t)mr.segments;
		if ((mr.microseconds = uSec(mr.segment_move_time)) < MIN_SEGMENT_USEC) {
			return(TG_GCODE_BLOCK_SKIPPED);		// exit without advancing position
		}
		mr.section_state = MOVE_STATE_RUN1;
	}
	if (mr.section_state == MOVE_STATE_RUN1) {	// concave part of accel curve (period 1)
		mr.segment_velocity = mr.entry_velocity + (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(false) == TG_COMPLETE) { 	  	// set up for second half
			mr.elapsed_accel_time = mr.segment_accel_time / 2;	// start time from midpoint of segment
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = MOVE_STATE_RUN2;
		}
		return(TG_EAGAIN);
	}
	if (mr.section_state == MOVE_STATE_RUN2) {	// convex part of accel curve (period 2)
		mr.segment_velocity = mr.midpoint_velocity + (mr.elapsed_accel_time * mr.midpoint_acceleration) -
							 (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(false) == TG_COMPLETE) {
			if ((mr.body_length < EPSILON) && (mr.tail_length < EPSILON)) { return(TG_OK);}	// end the move
			mr.move_state = MOVE_STATE_BODY;
			mr.section_state = MOVE_STATE_NEW;
		}
	}
	return(TG_EAGAIN);
}
#endif

/*
 * _exec_aline_body()
 *
 *	The body is broken into little segments even though it is a straight line so that 
 *	feedholds can happen in the middle of a line with a minimum of latency
 */
static uint8_t _exec_aline_body()
{
	if (mr.section_state == MOVE_STATE_NEW) {
		if (mr.body_length < EPSILON) {
			mr.move_state = MOVE_STATE_TAIL;
			return(_exec_aline_tail());			// skip ahead to tail periods
		}
		mr.move_time = mr.body_length / mr.cruise_velocity;
		mr.segments = ceil(uSec(mr.move_time) / cfg.estd_segment_usec);
		mr.segment_move_time = mr.move_time / mr.segments;
		mr.segment_velocity = mr.cruise_velocity;
		mr.segment_count = (uint32_t)mr.segments;
		if ((mr.microseconds = uSec(mr.segment_move_time)) < MIN_SEGMENT_USEC) {
			return(TG_GCODE_BLOCK_SKIPPED);		// exit without advancing position
		}
		
		mr.section_state = MOVE_STATE_RUN;
	}
	if (mr.section_state == MOVE_STATE_RUN) {				// stright part (period 3)
		if (_exec_aline_segment(false) == TG_COMPLETE) {
			if (mr.tail_length < EPSILON) { return(TG_OK);}	// end the move
			mr.move_state = MOVE_STATE_TAIL;
			mr.section_state = MOVE_STATE_NEW;
		}
	}
	return(TG_EAGAIN);
}

/*
 * _exec_aline_tail()
 */
static uint8_t _exec_aline_tail()
#ifdef __EXEC_R2
{
	if (mr.section_state == MOVE_STATE_NEW) {
		if (mr.tail_length < EPSILON) { return(TG_OK);}		// end the move
		mr.midpoint_velocity = (mr.cruise_velocity + mr.exit_velocity) / 2;
		mr.move_time = mr.tail_length / mr.midpoint_velocity;
		mr.segments = ceil(uSec(mr.move_time) / (2 * cfg.estd_segment_usec));// # of segments in *each half*
		mr.segment_move_time = mr.move_time / (2 * mr.segments);// time to advance for each segment
		mr.segment_count = (uint32_t)mr.segments;
		if ((mr.microseconds = uSec(mr.segment_move_time)) < MIN_SEGMENT_USEC) {
			return(TG_GCODE_BLOCK_SKIPPED);					// exit without advancing position
		}
		_init_forward_diffs(mr.cruise_velocity, mr.midpoint_velocity);
		mr.section_state = MOVE_STATE_RUN1;
	}
	if (mr.section_state == MOVE_STATE_RUN1) {				// convex part (period 4)
		mr.segment_velocity += mr.forward_diff_1;
		if (_exec_aline_segment(false) == TG_COMPLETE) { 	  	// set up for second half
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = MOVE_STATE_RUN2;

			// Here's a trick: The second half of the S starts at the end of the first,
			//  And the only thing that changes is the sign of mr.forward_diff_2
			mr.forward_diff_2 = -mr.forward_diff_2;
		} else {
			mr.forward_diff_1 += mr.forward_diff_2;
		}
		return(TG_EAGAIN);
	}
	if (mr.section_state == MOVE_STATE_RUN2) {				// concave part (period 5)
		mr.segment_velocity += mr.forward_diff_1;
		mr.forward_diff_1 += mr.forward_diff_2;
		if (_exec_aline_segment(true) == TG_COMPLETE) { return (TG_OK);}	// end the move
	}
	return(TG_EAGAIN);
}
#else
{
	if (mr.section_state == MOVE_STATE_NEW) {
		if (mr.tail_length < EPSILON) { return(TG_OK);}		// end the move
		mr.midpoint_velocity = (mr.cruise_velocity + mr.exit_velocity) / 2;
		mr.move_time = mr.tail_length / mr.midpoint_velocity;
		mr.accel_time = 2 * sqrt((mr.cruise_velocity - mr.exit_velocity) / mr.jerk);
		mr.midpoint_acceleration = 2 * (mr.cruise_velocity - mr.exit_velocity) / mr.accel_time;
		mr.segments = ceil(uSec(mr.move_time) / (2 * cfg.estd_segment_usec));// # of segments in *each half*
		mr.segment_move_time = mr.move_time / (2 * mr.segments);// time to advance for each segment
		mr.segment_accel_time = mr.accel_time / (2 * mr.segments);// time to advance for each segment
		mr.elapsed_accel_time = mr.segment_accel_time / 2; //compute time from midpoint of segment
		mr.segment_count = (uint32_t)mr.segments;
		if ((mr.microseconds = uSec(mr.segment_move_time)) < MIN_SEGMENT_USEC) {
			return(TG_GCODE_BLOCK_SKIPPED);					// exit without advancing position
		}
		mr.section_state = MOVE_STATE_RUN1;
	}
	if (mr.section_state == MOVE_STATE_RUN1) {				// convex part (period 4)
		mr.segment_velocity = mr.cruise_velocity - (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(false) == TG_COMPLETE) { 	  	// set up for second half
			mr.elapsed_accel_time = mr.segment_accel_time / 2;	// start time from midpoint of segment
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = MOVE_STATE_RUN2;
		}
		return(TG_EAGAIN);
	}
	if (mr.section_state == MOVE_STATE_RUN2) {				// concave part (period 5)
		mr.segment_velocity = mr.midpoint_velocity - 
							 (mr.elapsed_accel_time * mr.midpoint_acceleration) +
							 (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(true) == TG_COMPLETE) { return (TG_OK);}	// end the move
	}
	return(TG_EAGAIN);
}

#endif
/*
 * _exec_aline_segment() - segment runner helper
 */
static uint8_t _exec_aline_segment(uint8_t correction_flag)
{
	double travel[AXES];
	double steps[MOTORS];

	// Multiply computed length by the unit vector to get the contribution for
	// each axis. Set the target in absolute coords and compute relative steps.

	if ((correction_flag == true) && (mr.segment_count == 1) && 
		(cm.motion_state == MOTION_RUN) && (cm.cycle_state == CYCLE_STARTED)) {
		mr.target[X] = mr.endpoint[X];	// rounding error correction for last segment
		mr.target[Y] = mr.endpoint[Y];
		mr.target[Z] = mr.endpoint[Z];
		mr.target[A] = mr.endpoint[A];
		mr.target[B] = mr.endpoint[B];
		mr.target[C] = mr.endpoint[C];
	} else {
		double intermediate = mr.segment_velocity * mr.segment_move_time;
		mr.target[X] = mr.position[X] + (mr.unit[X] * intermediate);
		mr.target[Y] = mr.position[Y] + (mr.unit[Y] * intermediate);
		mr.target[Z] = mr.position[Z] + (mr.unit[Z] * intermediate);
		mr.target[A] = mr.position[A] + (mr.unit[A] * intermediate);
		mr.target[B] = mr.position[B] + (mr.unit[B] * intermediate);
		mr.target[C] = mr.position[C] + (mr.unit[C] * intermediate);
	}
	travel[X] = mr.target[X] - mr.position[X];
	travel[Y] = mr.target[Y] - mr.position[Y];
	travel[Z] = mr.target[Z] - mr.position[Z];
	travel[A] = mr.target[A] - mr.position[A];
	travel[B] = mr.target[B] - mr.position[B];
	travel[C] = mr.target[C] - mr.position[C];

/* The above is a re-arranged and loop unrolled version of this:
	for (uint8_t i=0; i < AXES; i++) {	// don't do the error correction if you are going into a hold
		if ((correction_flag == true) && (mr.segment_count == 1) && 
			(cm.motion_state == MOTION_RUN) && (cm.cycle_state == CYCLE_STARTED)) {
			mr.target[i] = mr.endpoint[i];	// rounding error correction for last segment
		} else {
			mr.target[i] = mr.position[i] + (mr.unit[i] * mr.segment_velocity * mr.segment_move_time);
		}
		travel[i] = mr.target[i] - mr.position[i];
	}
*/
	// prep the segment for the steppers and adjust the variables for the next iteration
	(void)ik_kinematics(travel, steps, mr.microseconds);
	if (st_prep_line(steps, mr.microseconds) == TG_OK) {
		copy_axis_vector(mr.position, mr.target); 	// update runtime position	
/*  TRY THIS
		mr.position[X] = mr.target[X];
		mr.position[Y] = mr.target[Y];
		mr.position[Z] = mr.target[Z];
		mr.position[A] = mr.target[A];
		mr.position[B] = mr.target[B];
		mr.position[C] = mr.target[C];	
*/	
	}
#ifndef __EXEC_R2
	mr.elapsed_accel_time += mr.segment_accel_time; // NB: ignored if running the body
#endif
	if (--mr.segment_count == 0) {
		return (TG_COMPLETE);	// this section has run all its segments
	}
	return (TG_EAGAIN);			// this section still has more segments to run
}

/**** PLANNER BUFFERS - FUNCTIONS *********************************************
 *
 * Planner buffers are used to queue and operate on Gcode blocks. Each buffer 
 * contains one Gcode block which may be a move, and M code, or other command 
 * that must be executed synchronously with movement.
 *
 * Buffers are in a circularly linked list managed by a WRITE pointer and a RUN pointer.
 * New blocks are populated by (1) getting a write buffer, (2) populating the buffer,
 * then (3) placing it in the queue (queue write buffer). If an exception occurs
 * during population you can unget the write buffer before queuing it, which returns
 * it to the pool of available buffers.
 *
 * The RUN buffer is the buffer currently executing. It may be retrieved once for 
 * simple commands, or multiple times for long-running commands like moves. When 
 * the command is complete the run buffer is returned to the pool by freeing it.
 * 
 * Notes:
 *	The write buffer pointer only moves forward on _queue_write_buffer, and
 *	the read buffer pointer only moves forward on free_read calls.
 *	(test, get and unget have no effect)
 * 
 * mp_get_planner_buffers_available()   Returns # of available planner buffers
 *
 * _init_buffers()			Initializes or resets buffers
 *
 * _get_write_buffer()		Get pointer to next available write buffer
 *							Returns pointer or NULL if no buffer available.
 *
 * _unget_write_buffer()	Free write buffer if you decide not to queue it.
 *
 * _queue_write_buffer()	Commit the next write buffer to the queue
 *							Advances write pointer & changes buffer state
 *
 * _get_run_buffer()		Get pointer to the next or current run buffer
 *							Returns a new run buffer if prev buf was ENDed
 *							Returns same buf if called again before ENDing
 *							Returns NULL if no buffer available
 *							The behavior supports continuations (iteration)
 *
 * _free_run_buffer()		Release the run buffer & return to buffer pool.
 *
 * _get_prev_buffer(bf)		Returns pointer to prev buffer in linked list
 * _get_next_buffer(bf)		Returns pointer to next buffer in linked list 
 * _get_first_buffer(bf)	Returns pointer to first buffer, i.e. the running block
 * _get_last_buffer(bf)		Returns pointer to last buffer, i.e. last block (zero)
 * _clear_buffer(bf)		Zeroes the contents of the buffer
 * _copy_buffer(bf,bp)		Copies the contents of bp into bf - preserves links
 */

uint8_t mp_get_planner_buffers_available(void) { return (mb.buffers_available);}

static void _init_buffers(void)
{
	mpBuf *pv;
	uint8_t i;

	memset(&mb, 0, sizeof(mb));		// clear all values, pointers and status
	mb.w = &mb.bf[0];				// init write and read buffer pointers
	mb.q = &mb.bf[0];
	mb.r = &mb.bf[0];
	pv = &mb.bf[PLANNER_BUFFER_POOL_SIZE-1];
	for (i=0; i < PLANNER_BUFFER_POOL_SIZE; i++) { // setup ring pointers
		mb.bf[i].nx = &mb.bf[_bump(i)];
		mb.bf[i].pv = pv;
		pv = &mb.bf[i];
	}
	mb.buffers_available = PLANNER_BUFFER_POOL_SIZE;
}

static mpBuf * _get_write_buffer() 				// get & clear a buffer
{
	if (mb.w->buffer_state == MP_BUFFER_EMPTY) {
		mpBuf *w = mb.w;
		mpBuf *nx = mb.w->nx;					// save pointers
		mpBuf *pv = mb.w->pv;
		memset(mb.w, 0, sizeof(mpBuf));
		w->nx = nx;								// restore pointers
		w->pv = pv;
		w->buffer_state = MP_BUFFER_LOADING;
		w->lineindex = (++mm.lineindex);		// increment line index and store in buffer
		mb.buffers_available--;
		mb.w = w->nx;
		return (w);
	}
	return (NULL);
}
/* NOT USED
static void _unget_write_buffer()
{
	mb.w = mb.w->pv;							// queued --> write
	mb.w->buffer_state = MP_BUFFER_EMPTY; 		// not loading anymore
	mb.buffers_available++;
}
*/
static void _queue_write_buffer(const uint8_t move_type)
{
	mb.q->move_type = move_type;
	mb.q->move_state = MOVE_STATE_NEW;
	mb.q->buffer_state = MP_BUFFER_QUEUED;
	mb.q = mb.q->nx;							// advance the queued buffer pointer
	st_request_exec_move();						// request a move exec if not busy
}

static mpBuf * _get_run_buffer() 
{
	// condition: fresh buffer; becomes running if queued or pending
	if ((mb.r->buffer_state == MP_BUFFER_QUEUED) || 
		(mb.r->buffer_state == MP_BUFFER_PENDING)) {
		 mb.r->buffer_state = MP_BUFFER_RUNNING;
	}
	// condition: asking for the same run buffer for the Nth time
	if (mb.r->buffer_state == MP_BUFFER_RUNNING) {	// return same buffer
		return (mb.r);
	}
	return (NULL);								// condition: no queued buffers. fail it.
}

static void _free_run_buffer()					// EMPTY current run buf & adv to next
{
	_clear_buffer(mb.r);						// clear it out (& reset replannable)
	mb.r->buffer_state = MP_BUFFER_EMPTY;
	mb.r = mb.r->nx;							 // advance to next run buffer
	if (mb.r->buffer_state == MP_BUFFER_QUEUED) {// only if queued...
		mb.r->buffer_state = MP_BUFFER_PENDING;  // pend next buffer
	}
	if (mb.w == mb.r) cm_cycle_end();			// end the cycle if the queue empties
	mb.buffers_available++;
	rpt_request_queue_report();
}

static mpBuf * _get_first_buffer(void)
{
	return(_get_run_buffer());	// returns buffer or NULL if nothing's running
}

static mpBuf * _get_last_buffer(void)
{
	mpBuf *bf = _get_run_buffer();
	mpBuf *bp = bf;

	if (bf == NULL) { return(NULL);}

	do {
		if ((bp->nx->move_state == MOVE_STATE_OFF) || (bp->nx == bf)) { 
			return (bp); 
		}
	} while ((bp = _get_next_buffer(bp)) != bf);
	return (bp);
}

// Use the macro instead
//static mpBuf * _get_prev_buffer(const mpBuf *bf) { return (bf->pv);}
//static mpBuf * _get_next_buffer(const mpBuf *bf) { return (bf->nx);}

static void _clear_buffer(mpBuf *bf) 
{
	mpBuf *nx = bf->nx;	// save pointers
	mpBuf *pv = bf->pv;
	memset(bf, 0, sizeof(mpBuf));
	bf->nx = nx;					// restore pointers
	bf->pv = pv;
}

static void _copy_buffer(mpBuf *bf, const mpBuf *bp)
{
	mpBuf *nx = bf->nx;	// save pointers
	mpBuf *pv = bf->pv;
 	memcpy(bf, bp, sizeof(mpBuf));
	bf->nx = nx;					// restore pointers
	bf->pv = pv;
}

#ifdef __DEBUG	// currently this routine is only used by debug routines
static uint8_t _get_buffer_index(mpBuf *bf) 
{
	mpBuf *b = bf;		// temp buffer pointer

	for (uint8_t i=0; i < PLANNER_BUFFER_POOL_SIZE; i++) {
		if (b->pv > b) {
			return (i);
		}
		b = b->pv;
	}
	return (PLANNER_BUFFER_POOL_SIZE);	// should never happen
}
#endif

//####################################################################################
//##### UNIT TESTS AND DEBUG CODE ####################################################
//####################################################################################

/****** DEBUG Code ******	(see beginning of file for static function prototypes) */

#ifdef __DEBUG
void mp_dump_running_plan_buffer() { _dump_plan_buffer(mb.r);}
void mp_dump_plan_buffer_by_index(uint8_t index) { _dump_plan_buffer(&mb.bf[index]);	}

static void _dump_plan_buffer(mpBuf *bf)
{
	fprintf_P(stderr, PSTR("***Runtime Buffer[%d] bstate:%d  mtype:%d  mstate:%d  replan:%d\n"),
			_get_buffer_index(bf),
			bf->buffer_state,
			bf->move_type,
			bf->move_state,
			bf->replannable);

	print_scalar(PSTR("line number:     "), bf->linenum);
	print_scalar(PSTR("line index:      "), bf->lineindex);
	print_vector(PSTR("position:        "), mm.position, AXES);
	print_vector(PSTR("target:          "), bf->target, AXES);
	print_vector(PSTR("unit:            "), bf->unit, AXES);
	print_scalar(PSTR("jerk:            "), bf->jerk);
	print_scalar(PSTR("time:            "), bf->time);
	print_scalar(PSTR("length:          "), bf->length);
	print_scalar(PSTR("head_length:     "), bf->head_length);
	print_scalar(PSTR("body_length:     "), bf->body_length);
	print_scalar(PSTR("tail_length:     "), bf->tail_length);
	print_scalar(PSTR("entry_velocity:  "), bf->entry_velocity);
	print_scalar(PSTR("cruise_velocity: "), bf->cruise_velocity);
	print_scalar(PSTR("exit_velocity:   "), bf->exit_velocity);
	print_scalar(PSTR("exit_vmax:       "), bf->exit_vmax);
	print_scalar(PSTR("entry_vmax:      "), bf->entry_vmax);
	print_scalar(PSTR("cruise_vmax:     "), bf->cruise_vmax);
	print_scalar(PSTR("delta_vmax:      "), bf->delta_vmax);
	print_scalar(PSTR("braking_velocity:"), bf->braking_velocity);
}

void mp_dump_runtime_state(void)
{
	fprintf_P(stderr, PSTR("***Runtime Singleton (mr)\n"));
	print_scalar(PSTR("line number:       "), mr.linenum);
	print_scalar(PSTR("line index:        "), mr.lineindex);
	print_vector(PSTR("position:          "), mr.position, AXES);
	print_vector(PSTR("target:            "), mr.target, AXES);
	print_scalar(PSTR("length:            "), mr.length);

	print_scalar(PSTR("move_time:         "), mr.move_time);
//	print_scalar(PSTR("accel_time;        "), mr.accel_time);
//	print_scalar(PSTR("elapsed_accel_time:"), mr.elapsed_accel_time);
	print_scalar(PSTR("midpoint_velocity: "), mr.midpoint_velocity);
//	print_scalar(PSTR("midpoint_accel:    "), mr.midpoint_acceleration);
//	print_scalar(PSTR("jerk_div2:         "), mr.jerk_div2);

	print_scalar(PSTR("segments:          "), mr.segments);
	print_scalar(PSTR("segment_count:     "), mr.segment_count);
	print_scalar(PSTR("segment_move_time: "), mr.segment_move_time);
//	print_scalar(PSTR("segment_accel_time:"), mr.segment_accel_time);
	print_scalar(PSTR("microseconds:      "), mr.microseconds);
	print_scalar(PSTR("segment_length:	  "), mr.segment_length);
	print_scalar(PSTR("segment_velocity:  "), mr.segment_velocity);
}
#endif // __DEBUG

/****** UNIT TESTS ******/

#ifdef __UNIT_TEST_PLANNER

//#define JERK_TEST_VALUE (double)50000000	// set this to the value in the profile you are running
#define JERK_TEST_VALUE (double)100000000	// set this to the value in the profile you are running

static void _test_calculate_trapezoid(void);
static void _test_get_junction_vmax(void);
static void _test_trapezoid(double length, double Ve, double Vt, double Vx, mpBuf *bf);
static void _make_unit_vector(double unit[], double x, double y, double z, double a, double b, double c);
//static void _set_jerk(const double jerk, mpBuf *bf);

void mp_unit_tests()
{
	_test_calculate_trapezoid();
//	_test_get_junction_vmax();
}

static void _test_trapezoid(double length, double Ve, double Vt, double Vx, mpBuf *bf)
{
	bf->length = length;
	bf->entry_velocity = Ve;
	bf->cruise_velocity = Vt;
	bf->exit_velocity = Vx;
	bf->cruise_vmax = Vt;
	bf->jerk = JERK_TEST_VALUE;
#ifdef __PLAN_R2
	TODO_make_this_work();
#else
	bf->recip_jerk = 1/bf->jerk;
	bf->cbrt_jerk = cbrt(bf->jerk);
#endif
	_calculate_trapezoid(bf);
}

static void _test_calculate_trapezoid()
{
	mpBuf *bf = _get_write_buffer();

// these tests are calibrated the following parameters:
//	jerk_max 				50 000 000		(all axes)
//	jerk_corner_offset		   		 0.1	(all exes)
//	jerk_corner_acceleration   200 000		(global)

/*
// no-fit cases: line below minimum velocity or length
//				   	L	 Ve  	Vt		Vx
	_test_trapezoid(1.0, 0,		0.001,	0,	bf);
	_test_trapezoid(0.0, 0,		100,	0,	bf);
	_test_trapezoid(0.01, 0,	100,	0,	bf);

// requested-fit cases
//				   	L  	 Ve  	Vt		Vx
	_test_trapezoid(0.8, 400,	400, 	0, 	 bf);
	_test_trapezoid(0.8, 600,	600, 	200, bf);
	_test_trapezoid(0.8, 0,		400, 	400, bf);
	_test_trapezoid(0.8, 200,	600, 	600, bf);

// HBT - 3 section cases
//				   	L    Ve  	Vt		Vx
	_test_trapezoid(0.8, 0,		190, 	0, bf);
	_test_trapezoid(2.0, 200,	400, 	0, bf);

// 2 section cases (HT)
//				   	L   Ve  	Vt		Vx
	_test_trapezoid(0.8, 0,		200, 	0, bf);		// requested fit HT case (exact fit)
	_test_trapezoid(0.8, 0,		400, 	0, bf);		// symmetric rate-limited HT case
	_test_trapezoid(0.8, 200,	400, 	0, bf);		// asymmetric rate-limited HT case
	_test_trapezoid(2.0, 400,	400, 	0, bf);
	_test_trapezoid(0.8, 0,		400, 	200,bf);

// 1 section cases (H,B and T)
//				   	L	 Ve  	Vt		Vx
	_test_trapezoid(1.0, 800,	800, 	800,bf);	// B case
	_test_trapezoid(0.8, 0,		400, 	0, bf);		// B case
	_test_trapezoid(0.8, 200,	400, 	0, bf);
	_test_trapezoid(2.0, 400,	400, 	0, bf);
	_test_trapezoid(0.8, 0,		400, 	200,bf);
*/
// test cases drawn from Mudflap
//				   	L		Ve  	  Vt		Vx
//	_test_trapezoid(0.6604, 000.000,  800.000,  000.000, bf);	// line 50
//	_test_trapezoid(0.8443, 000.000,  805.855,  000.000, bf);	// line 55
	_test_trapezoid(0.8443, 000.000,  805.855,  393.806, bf);	// line 55'
	_test_trapezoid(0.7890, 393.805,  955.829,  000.000, bf);	// line 60
	_test_trapezoid(0.7890, 393.806,  955.829,  390.294, bf);	// line 60'
	_test_trapezoid(0.9002, 390.294,  833.884,  000.000, bf);	// line 65

	_test_trapezoid(0.9002, 390.294,  833.884,  455.925, bf);	// line 65'
	_test_trapezoid(0.9002, 390.294,  833.884,  806.895, bf);	// line 65"
	_test_trapezoid(0.9735, 455.925,  806.895,  000.000, bf);	// line 70
	_test_trapezoid(0.9735, 455.925,  806.895,  462.101, bf);	// line 70'

	_test_trapezoid(0.9735, 806.895,  806.895,  802.363, bf);	// line 70"

	_test_trapezoid(0.9935, 462.101,  802.363,  000.000, bf);	// line 75
	_test_trapezoid(0.9935, 462.101,  802.363,  000.000, bf);	// line 75'
	_test_trapezoid(0.9935, 802.363,  802.363,  477.729, bf);	// line 75"
	_test_trapezoid(0.9935, 802.363,  802.363,  802.363, bf);	// line 75"
	_test_trapezoid(1.0441, 477.729,  843.274,  000.000, bf);	// line 80
	_test_trapezoid(1.0441, 802.363,  843.274,  388.515, bf);	// line 80'
	_test_trapezoid(1.0441, 802.363,  843.274,  803.990, bf);	// line 80"
	_test_trapezoid(0.7658, 388.515,  803.990,  000.000, bf);	// line 85
	_test_trapezoid(0.7658, 803.990,  803.990,  733.618, bf);	// line 85'
	_test_trapezoid(0.7658, 803.990,  803.990,  802.363, bf);	// line 85"
	_test_trapezoid(1.9870, 733.618,  802.363,  000.000, bf);	// line 90
	_test_trapezoid(1.9870, 802.363,  802.363,  727.371, bf);	// line 90'
	_test_trapezoid(1.9870, 802.363,  802.363,  802.363, bf);	// line 90'
	_test_trapezoid(1.9617, 727.371,  802.425,  000.000, bf);	// line 95
	_test_trapezoid(1.9617, 727.371,  802.425,  000.000, bf);	// line 95'
	_test_trapezoid(1.9617, 802.363,  802.425,  641.920, bf);	// line 95"
	_test_trapezoid(1.9617, 802.363,  802.425,  802.425, bf);	// line 95"'
	_test_trapezoid(1.6264, 641.920,  826.209,  000.000, bf);	// line 100
	_test_trapezoid(1.6264, 802.425,  826.209,  266.384, bf);	// line 100'
	_test_trapezoid(1.6264, 802.425,  826.209,  658.149, bf);	// line 100"
	_test_trapezoid(1.6264, 802.425,  826.209,  679.360, bf);	// line 100"'
	_test_trapezoid(0.4348, 266.384,  805.517,  000.000, bf);	// line 105
	_test_trapezoid(0.4348, 658.149,  805.517,  391.765, bf);	// line 105'
	_test_trapezoid(0.4348, 679.360,  805.517,  412.976, bf);	// line 105"
	_test_trapezoid(0.7754, 391.765,  939.343,  000.000, bf);	// line 110
	_test_trapezoid(0.7754, 412.976,  939.343,  376.765, bf);	// line 110'
	_test_trapezoid(0.7754, 802.425,  826.209,  679.360, bf);	// line 110"
	_test_trapezoid(0.7754, 412.976,  939.343,  804.740, bf);	// line 110"'
	_test_trapezoid(0.7313, 376.765,  853.107,  000.000, bf);	// line 115
	_test_trapezoid(0.7313, 804.740,  853.107,  437.724, bf);	// line 115'
	_test_trapezoid(0.7313, 804.740,  853.107,  683.099, bf);	// line 115"
	_test_trapezoid(0.7313, 804.740,  853.107,  801.234, bf);	// line 115"'
	_test_trapezoid(0.9158, 437.724,  801.233,  000.000, bf);	// line 120
	_test_trapezoid(0.9158, 683.099,  801.233,  245.375, bf);	// line 120'
	_test_trapezoid(0.9158, 801.233,  801.233,  617.229, bf);	// line 120"
	_test_trapezoid(0.3843, 245.375,  807.080,  000.000, bf);	// line 125
	_test_trapezoid(0.3843, 617.229,  807.080,  371.854, bf);	// line 125'  6,382,804 cycles



	_test_trapezoid(0.8, 0,	400, 400, bf);


// test cases drawn from braid_600mm					 		// expected results
//				   	L   	Ve  		Vt		Vx
	_test_trapezoid(0.327,	000.000,	600,	000.000, bf); // Ve=0 	   	Vc=110.155
	_test_trapezoid(0.327,	000.000,	600,	174.538, bf); // Ve=0, 	   	Vc=174.744	Vx=174.537
	_test_trapezoid(0.327,	174.873,	600,	173.867, bf); // Ve=174.873	Vc=185.356	Vx=173.867
	_test_trapezoid(0.327,	173.593,	600,	000.000, bf); // Ve=174.873	Vc=185.356	Vx=173.867
	_test_trapezoid(0.327,	347.082,	600,	173.214, bf); // Ve=174.873	Vc=185.356	Vx=173.867

}

static void _make_unit_vector(double unit[], double x, double y, double z, double a, double b, double c)
{
	double length = sqrt(x*x + y*y + z*z + a*a + b*b + c*c);
	unit[X] = x/length;
	unit[Y] = y/length;
	unit[Z] = z/length;
	unit[A] = a/length;
	unit[B] = b/length;
	unit[C] = c/length;
}

static void _test_get_junction_vmax()
{
//	cfg.a[X].jerk_max = JERK_TEST_VALUE;
//	cfg.a[Y].jerk_max = JERK_TEST_VALUE;
//	cfg.a[Z].jerk_max = JERK_TEST_VALUE;
//	cfg.a[A].jerk_max = JERK_TEST_VALUE;
//	cfg.a[B].jerk_max = JERK_TEST_VALUE;
//	cfg.a[C].jerk_max = JERK_TEST_VALUE;
//	mm.jerk_transition_size = 0.5;
//	mm.jerk_limit_max = 184.2;

	mm.test_case = 1;				// straight line along X axis
	_make_unit_vector(mm.a_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 2;				// angled straight line
	_make_unit_vector(mm.a_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 3;				// 5 degree bend
	_make_unit_vector(mm.a_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.9962, 0.0872, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 4;				// 30 degrees
	_make_unit_vector(mm.a_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.8660, 0.5000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 5;				// 45 degrees
	_make_unit_vector(mm.a_unit, 0.8660,	0.5000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.2588,	0.9659, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 6;				// 60 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.5000,	0.8660, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 7;				// 90 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.0000,	1.0000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 8;				// 90 degrees rotated 45 degrees
	_make_unit_vector(mm.a_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.7071, 0.7071, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 9;				// 120 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.5000,	0.8660, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 10;				// 150 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.8660,	0.5000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 11;				// 180 degrees
	_make_unit_vector(mm.a_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.7071,-0.7071, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);
}

#endif // __UNIT_TEST_PLANNER
