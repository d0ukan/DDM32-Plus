/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus round 3 "misc" UI flows (tests/plus/test_ui_misc.c): upstream's UI simulator (tests/host/sim/ui_sim.c) as
 * it is, built in place of it, with a few of its services taken over for the test: the radio requests tuning makes
 * recorded in order (then handed to the simulator's own, so its view follows), a radio that receives AM (Tools >
 * Airfields and Air band are shown), the position aprs_position_now gives, and the battery voltage in the view. */
#define radio_req_vfo sim_base_vfo
#define radio_req_vfo_set sim_base_vfo_set
#define radio_req_vfo_tx sim_base_vfo_tx
#define radio_req_bandwidth sim_base_bandwidth
#define radio_req_tone sim_base_tone
#define sim_reset sim_base_reset	/* refpack_sim.inc: every reset gets the map pack */
#include "sim/ui_sim.c"
#undef sim_reset
#include "refpack_sim.inc"
#undef radio_req_vfo
#undef radio_req_vfo_set
#undef radio_req_vfo_tx
#undef radio_req_bandwidth
#undef radio_req_tone
#include "ui_sim_misc.h"

misc_sim_t misc_sim;

static bool am_rx(uint32_t hz, radio_bw_t bw)
{
	(void)hz;
	(void)bw;
	return true;
}
static hal_radio_t misc_radio = { .rx_am = am_rx };

void misc_sim_setup(void)
{
	memset(&misc_sim, 0, sizeof misc_sim);
	board.caps |= CAP_AM_RX;	/* after sim_reset (it sets the caps of the key map) */
	board.radio = &misc_radio;
}

void misc_sim_battery_mv(uint16_t mv)
{
	view.battery_mv = mv;
	changed();
}

void misc_sim_state(radio_state_t st)
{
	view.state = st;
	changed();
}

/* The position APRS uses (Tools > Airfields' order) */
bool aprs_position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps)
{
	*err_m = 0;
	*dmr_gps = false;
	if (!misc_sim.pos)
		return false;
	*lat = misc_sim.lat_e7;
	*lon = misc_sim.lon_e7;
	return true;
}

static void req(const char *what, long v)
{
	size_t n = strlen(misc_sim.req);
	snprintf(misc_sim.req + n, sizeof misc_sim.req - n, "%s%s:%ld", n ? " " : "", what, v);
}

void radio_req_vfo(bool on)
{
	req("vfo", on);
	sim_base_vfo(on);
}

void radio_req_vfo_set(uint32_t hz)
{
	req("vfo_set", (long)hz);
	sim_base_vfo_set(hz);
}

void radio_req_vfo_tx(uint32_t hz)
{
	req("vfo_tx", (long)hz);
	sim_base_vfo_tx(hz);
}

void radio_req_bandwidth(radio_bw_t bw)
{
	req("bw", bw == RADIO_BW_12K5 ? 12 : 25);
	sim_base_bandwidth(bw);
}

void radio_req_tone(uint8_t which, subtone_t tone)
{
	req(which == RADIO_TONE_TX ? "tone_tx" : "tone_rx", tone.type == TONE_NONE ? 0 : (long)tone.value);
	sim_base_tone(which, tone);
}

/* Not in the simulator (its radio has no mode request): the view follows */
void radio_req_mode(uint8_t mode)
{
	req(mode == RADIO_MODE_DMR ? "mode_dmr" : "mode_fm", 1);
	view.ch.mode = mode;
	changed();
}
