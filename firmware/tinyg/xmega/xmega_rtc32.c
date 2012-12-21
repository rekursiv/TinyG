/*
 * xmega_rtc32.c - real-time counter/clock
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

#include <avr/io.h>
#include <avr/interrupt.h>
//#include <avr/sysclk.h>

#include <util/delay.h>	

#include "../tinyg.h"
#include "../gpio.h"
#include "xmega_rtc32.h"



// adapted from Atmel ASF code:
// src\asf\xmega\drivers\rtc32\rtc32.c
void rtc_init()
{
	
//	sysclk_enable_module(SYSCLK_PORT_GEN, SYSCLK_RTC);   // ??

	// Enable access to VBAT
//	VBAT.CTRL |= VBAT_ACCEN_bm;
//	VBAT.CTRL |= VBAT_RESET_bm;
//	VBAT.CTRL |= VBAT_XOSCFDEN_bm;
	
	/* This delay is needed to give the voltage in the backup system some
	* time to stabilize before we turn on the oscillator. If we do not
	* have this delay we may get a failure detection.
	*/
//	_delay_us(200);
	
//	VBAT.CTRL |= CLK_RTCSRC_RCOSC_gc;    /* 1kHz from internal 32kHz RC oscillator */
//	while (!(VBAT.STATUS & VBAT_XOSCRDY_bm));


	OSC.CTRL |= OSC_RC32KEN_bm;							// Turn on internal 32kHz.
	while ((OSC.STATUS & OSC_RC32KRDY_bm) == 0);		// Wait for 32kHz oscillator to stabilize.
	while (RTC32.SYNCCTRL & RTC32_SYNCBUSY_bm);			// Wait until RTC is not busy

	CLK.RTCCTRL = CLK_RTCSRC_RCOSC_gc | CLK_RTCEN_bm;	// Set internal 32kHz osc as RTC clock source
	while (RTC32.SYNCCTRL & RTC32_SYNCBUSY_bm);			// Wait until RTC is not busy

	
	// Disable the RTC32 module before setting it up
	RTC32.CTRL = 0;

	while (RTC32.SYNCCTRL & RTC32_SYNCBUSY_bm);

	RTC32.PER = RTC_PERIOD-1;								// overflow period
	RTC32.CNT = 0;
	RTC32.COMP = RTC_PERIOD-1;

	while (RTC32.SYNCCTRL & RTC32_SYNCBUSY_bm);

	RTC32.INTCTRL = RTC_COMPINTLVL;						// interrupt on compare
	RTC32.CTRL = RTC32_ENABLE_bm;

	// Make sure it's sync'ed before return
	while (RTC32.SYNCCTRL & RTC32_SYNCBUSY_bm);
	
	rtc.clock_ticks = 0;								//  default RTC clock counter
}


/* 
 * rtc ISR 
 *
 * This used to have application-specific clocks and timers in it but that approach
 * was abandoned because I decided it was better to just provide callbacks to the 
 * relevant code modules to perform those functions.
 *
 * It is the responsibility of the callback code to ensure atomicity and volatiles
 * are observed correctly as the callback will be run at the interrupt level.
 *
 * Here's the code in case the main loop (non-interrupt) function needs to 
 * create a critical region for variables set or used by the callback:
 *
 *		#include "gpio.h"
 *		#include "xmega_rtc32.h"
 *
 *		RTC.INTCTRL = RTC_OVFINTLVL_OFF_gc;	// disable interrupt
 * 		blah blah blah critical region
 *		RTC.INTCTRL = RTC_OVFINTLVL_LO_gc;	// enable interrupt
 */

ISR(RTC32_COMP_vect)
{
	// callbacks to whatever you need to happen on each RTC tick go here:
	gpio_switch_timer_callback();		// switch debouncing

	// here's the default RTC timer clock
	++rtc.clock_ticks;					// increment real time clock (unused)
}

void rtc_reset_rtc_clock()
{
//	RTC.INTCTRL = RTC_OVFINTLVL_OFF_gc;	// disable interrupt
	rtc.clock_ticks = 0;
//	RTC.INTCTRL = RTC_OVFINTLVL_LO_gc;	// enable interrupt
}
