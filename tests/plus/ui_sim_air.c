/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 air items (tests/plus/test_ui_air.c): upstream's UI simulator (tests/host/sim/ui_sim.c) as it is,
 * built in place of it, with what Tools > Airfields needs from the radio: one that receives AM (the screen is shown and
 * tunes) and the position aprs_position_now gives; with CONFIG_AIR_WATCH the radio core's air watch
 * (radio_airwatch.inc) on the simulator's VFOs: the first frequency keeps the VFOs and the position (a running watch
 * stops on its selected channel), each VFO takes its frequency as a fresh FM VFO, the end stops the ring and brings
 * the kept ones back. */
#define sim_reset sim_base_reset	/* refpack_sim.inc: every reset gets the map pack */
#include "sim/ui_sim.c"
#undef sim_reset
#include "refpack_sim.inc"
#include "ui_sim_air.h"

air_sim_t air_sim;

static bool air_am_rx(uint32_t hz, radio_bw_t bw)
{
	(void)hz;
	(void)bw;
	return true;
}
static hal_radio_t air_radio = { .rx_am = air_am_rx };

void air_sim_setup(void)
{
	memset(&air_sim, 0, sizeof air_sim);
	board.caps |= CAP_AM_RX;	/* after sim_reset (it sets the caps of the key map) */
	board.radio = &air_radio;
}

void air_sim_state(radio_state_t st)
{
	view.state = st;
	changed();
}

#if CONFIG_AIR_WATCH
static struct {
	bool kept;
	sim_vfo_t cur, other, store[6];
	channel_t vx[6];
	bool vx_ok[6], vfo;
	uint8_t lastab, vfo_index;
	uint16_t zone, pos;
} air_kept;

/* VFO idx's frequency (the one in use: the view's when on view) */
uint32_t air_sim_vfo_hz(uint8_t idx)
{
	channel_t c;
	if (idx == view.vfo_index && view.vfo)
		return view.ch.rx_hz;
	ui_svc_vfo_get(idx, &c);
	return c.rx_hz;
}

void ui_svc_air_vfo(uint8_t idx, uint32_t hz)
{
	air_sim.vfo_calls++;
	bool atis = (idx & UI_AIR_ATIS) != 0;
	idx = (uint8_t)(idx & ~UI_AIR_ATIS);
	if (idx >= 6)
		return;
	air_sim.hz[idx] = hz;
	air_sim.atis = (uint8_t)(atis ? air_sim.atis | 1u << idx : air_sim.atis & ~(1u << idx));
	if (!air_kept.kept) {
		if (view.scan == 3) {
			sim_tune(sim_sel);	/* the radio core: a watch stops on its selected channel */
			view.scan = 0;
		}
		if (view.vfo) {		/* the VFO on view: its changes are its own (sim_vfo_use's way) */
			vfo_rx_hz = view.ch.rx_hz;
			vfo_tx_hz = view.ch.tx_hz;
			vfo_mode = view.ch.mode;
		}
		air_kept.kept = true;
		sim_vfo_get(&air_kept.cur);
		air_kept.other = vfo_other;
		memcpy(air_kept.store, sim_store, sizeof sim_store);
		memcpy(air_kept.vx, sim_vx, sizeof sim_vx);
		memcpy(air_kept.vx_ok, sim_vx_ok, sizeof sim_vx_ok);
		air_kept.lastab = sim_lastab;
		air_kept.vfo = view.vfo;
		air_kept.vfo_index = view.vfo_index;
		air_kept.zone = view.zone_index;
		air_kept.pos = view.zone_pos;
	}
	sim_vfo_t f = { RADIO_MODE_FM, 1, 1, RADIO_BW_12K5, hz, hz, 0, { TONE_NONE, 0 }, { TONE_NONE, 0 }, false };
	sim_vx_ok[idx] = false;
	if (idx == view.vfo_index) {
		vfo_mode = f.mode;
		vfo_cc = f.cc;
		vfo_ts = f.ts;
		vfo_bw = f.bw;
		vfo_rx_hz = vfo_tx_hz = hz;
		vfo_talk = 0;
		vfo_rx_tone = vfo_tx_tone = f.rx_tone;
		vfo_talk_private = false;
		if (view.vfo)
			sim_vfo(true);
	} else if (idx >= 2 || (view.vfo_index >= 2 && idx == sim_lastab)) {
		sim_store[idx] = f;
	} else {
		vfo_other = f;
	}
	changed();
}

void ui_svc_air_end(void)
{
	air_sim.end_calls++;
	if (!air_kept.kept)
		return;
	air_kept.kept = false;
	view.scan = 0;
	memset(sim_keep_ok, 0, sizeof sim_keep_ok);
	vfo_mode = air_kept.cur.mode;
	vfo_cc = air_kept.cur.cc;
	vfo_ts = air_kept.cur.ts;
	vfo_bw = air_kept.cur.bw;
	vfo_rx_hz = air_kept.cur.rx_hz;
	vfo_tx_hz = air_kept.cur.tx_hz;
	vfo_talk = air_kept.cur.talk;
	vfo_rx_tone = air_kept.cur.rx_tone;
	vfo_tx_tone = air_kept.cur.tx_tone;
	vfo_talk_private = air_kept.cur.talk_private;
	vfo_other = air_kept.other;
	memcpy(sim_store, air_kept.store, sizeof sim_store);
	memcpy(sim_vx, air_kept.vx, sizeof sim_vx);
	memcpy(sim_vx_ok, air_kept.vx_ok, sizeof sim_vx_ok);
	sim_lastab = air_kept.lastab;
	view.vfo_index = air_kept.vfo_index;
	if (air_kept.vfo) {
		sim_vfo(true);
	} else {
		view.vfo = false;
		view.zone_index = air_kept.zone;
		view.zone_pos = air_kept.pos;
		load_channel();
	}
	changed();
}
#endif

/* The position APRS uses (Tools > Airfields' order) */
bool aprs_position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps)
{
	*err_m = 0;
	*dmr_gps = false;
	if (!air_sim.pos)
		return false;
	*lat = air_sim.lat_e7;
	*lon = air_sim.lon_e7;
	return true;
}
