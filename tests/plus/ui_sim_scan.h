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

#endif
