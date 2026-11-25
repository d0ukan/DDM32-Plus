/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 15 scan job UI flows (tests/plus/test_ui_scan.c): what the test sets in the simulator
 * (tests/plus/ui_sim_scan.c wraps upstream's tests/host/sim/ui_sim.c) and what it records. */
#ifndef UI_SIM_SCAN_H
#define UI_SIM_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/p25search.h"
#include "radio/dmrsearch.h"

typedef struct {
	p25s_view_t p25;		/* what the P25 search shows */
	dmrs_view_t dmr;		/* and the DMR search */
	char req[512];			/* search requests in order: "p25s:433000-434500/12500", "p25s:stop" */
	int requests;
} sc_sim_t;

extern sc_sim_t sc_sim;
void sc_sim_clear(void);
uint32_t sc_sim_freq(void);		/* the view's RX frequency */
void sc_sim_changed(void);		/* the view changed: the screen on top draws again */
/* searchlist: the drawn text that contains want (false: none), its top and bottom, and whether it sits on the picked
 * row's highlight (the focus colour at its first pixel or at the row's right end) */
bool sc_sim_text(const char *want, int *y0, int *y1, bool *focus);
bool sc_sim_text_is(const char *want);	/* a drawn text that is exactly want */
int sc_sim_hint_y(void);		/* the hint line's top: the list ends above it */

#endif
