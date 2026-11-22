/* SPDX-License-Identifier: MIT */
#ifndef HRC_NATIVE_PORT_H
#define HRC_NATIVE_PORT_H
#include "hrc_native_chip.h"
/* Derivative-only reservation: APRS is disabled, leaving timer 4 available.
 * Keep upstream's PCM refill on timer 6 and the board's PA delay on timer 3.
 * DDM32 Plus builds APRS, whose AFSK transmitter owns timer 4 (TIMER_AFSK):
 * its build defines HN_GUARD_ON_TIMER5, and the guard takes timer 5
 * (TIMER_BOARD, a board's own one-shot, which only the DB25-D board uses;
 * the DM-32 board has none). */
#ifdef HN_GUARD_ON_TIMER5
enum { HN_GUARD_TIMER = 5 };
#else
enum { HN_GUARD_TIMER = 4 };
#endif
int hn_port_on(hn_chip *, nt_test *);
void hn_port_quiet(void);
void hn_port_restore(void);
#if defined(CONFIG_RF_FIXES) && CONFIG_RF_FIXES
/* DDM32 Plus (hn_watch): the modem system source unmasked again while the session's handlers are in */
void hn_port_kick(void);
#endif
#endif
