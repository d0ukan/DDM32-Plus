/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 air items' UI flows: what tests/plus/ui_sim_air.c adds to upstream's simulator */
#ifndef UI_SIM_AIR_H
#define UI_SIM_AIR_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/radio.h"

typedef struct {
	bool pos;			/* aprs_position_now has a position */
	int32_t lat_e7, lon_e7;
	/* the air watch's requests (CONFIG_AIR_WATCH): VFO frequencies asked for, ends */
	int vfo_calls, end_calls;
	uint32_t hz[6];
	uint8_t atis;			/* the VFOs asked for as an ATIS (UI_AIR_ATIS) */
	uint32_t arg[6];		/* each VFO's last request bits (DDM32 Plus auto scan: UI_AIR_*) */
	uint16_t tone[6];		/* ... and access tone (Hz x 10, 0 none) */
	int replaced, refused;		/* refreshes (UI_AIR_REPLACE) taken, refused */
	int refuse;			/* the next this many refreshes refused anyway (the ring held by the time the radio core
				 * read them, after the UI saw it quiet) */
	uint8_t gps;			/* ui_svc_position: AIR_GPS_* */
} air_sim_t;
enum { AIR_GPS_DEFAULT, AIR_GPS_OFF, AIR_GPS_NOFIX, AIR_GPS_FIX };	/* DEFAULT: the simulator's own (a fix) */

extern air_sim_t air_sim;

void air_sim_setup(void);			/* after sim_reset: AM receive */
void air_sim_state(radio_state_t st);		/* the radio's state (RS_TX: transmitting) */
void air_sim_session(void);			/* DOTA / cross-band ends the air watch in the radio core */
void air_sim_phase(uint8_t phase);		/* the scan's phase (SCAN_PH_HOLD: the ring holds a signal) */
uint32_t air_sim_vfo_hz(uint8_t idx);		/* VFO idx's frequency as the simulated radio keeps it */

#endif
