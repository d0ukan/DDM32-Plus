/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus map navigation UI flows (tests/plus/test_ui_mapnav.c): what the test sets in the simulator
 * (tests/plus/ui_sim_mapnav.c wraps upstream's tests/host/sim/ui_sim.c) and what it records. */
#ifndef UI_SIM_MAPNAV_H
#define UI_SIM_MAPNAV_H

#include <stdbool.h>
#include <stdint.h>
#include "aprs/aprs.h"

typedef struct {
	bool gps;			/* the position below instead of the simulator's own (Boston) */
	bool fix;
	int32_t lat_e7, lon_e7;
	bool stations;			/* the APRS stations below instead of the simulator's two */
	uint8_t nst;
	aprs_station_t st[APRS_STATIONS];
	char req[512];			/* the radio requests in order: "vfo_set:145600000 mode:fm bw:25 ..." */
	char msg_to[16], msg_text[72];	/* the APRS message handed to the engine */
	int messages;
} mapnav_sim_t;

extern mapnav_sim_t mapnav_sim;
void mapnav_sim_clear(void);
void mapnav_sim_mode(uint8_t mode);	/* the channel's mode in the simulator's view (RADIO_MODE_FM / _DMR) */
/* The map's cheap frames (docs/mappack-design.md T10): a radio view change only (the RSSI, a new view serial); the
 * data chip's reads counted from now on; how many since; a hash of the panel pixels of viewport rows y .. y + h */
void mapnav_sim_radio_update(int rssi_dbm);
void mapnav_sim_count_reads(void);
uint32_t mapnav_sim_reads(void);
uint32_t mapnav_sim_rows_hash(int y, int h);

#endif
