/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 air items (tests/plus/test_ui_air.c): upstream's UI simulator (tests/host/sim/ui_sim.c) as it is,
 * built in place of it, with what Tools > Airfields needs from the radio: one that receives AM (the screen is shown and
 * tunes) and the position aprs_position_now gives; with CONFIG_AIR_WATCH the radio core's air watch
 * (radio_airwatch.inc) on the simulator's VFOs: the first frequency keeps the VFOs and the position (a running watch
 * stops on its selected channel), each VFO takes its frequency as a fresh FM VFO, the end stops the ring and brings
 * the kept ones back. */
#define sim_reset sim_base_reset	/* refpack_sim.inc: every reset gets the map pack */
#define ui_svc_position sim_base_position	/* the test's GPS position (air_sim.gps) */
#include "sim/ui_sim.c"
#undef sim_reset
#undef ui_svc_position
#include "refpack_sim.inc"
#include "ui_sim_air.h"
#include "radio/scan.h"
#include "radio/tones.h"

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

void air_sim_phase(uint8_t phase)
{
	view.scan_phase = phase;
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

void ui_svc_air_vfo(uint32_t arg, uint32_t hz)
{
	air_sim.vfo_calls++;
	bool atis = (arg & UI_AIR_ATIS) != 0;
	uint8_t idx = (uint8_t)(arg & 7u);
	if (idx >= 6)
		return;
	air_sim.arg[idx] = arg;
	/* DDM32 Plus auto scan: a refresh is refused under a call, its wait and a transmission, and after the end, as the
	 * radio core does (radio_airwatch.inc) */
	if ((arg & UI_AIR_REPLACE) != 0 && (!air_kept.kept || view.state == RS_TX ||
					      (view.scan == 3 && (view.scan_phase == SCAN_PH_HOLD ||
								  view.scan_phase == SCAN_PH_RESUME)))) {
		air_sim.refused++;
		return;
	}
	air_sim.replaced += (arg & UI_AIR_REPLACE) != 0;
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
	if ((arg & (UI_AIR_FM | UI_AIR_DMR)) != 0) {	/* a repeater letter as listed (radio_airwatch.inc air_vfo_fill) */
		uint8_t p = (uint8_t)((arg >> UI_AIR_P_SHIFT) & 0x3Fu);
		f.tx_hz = (uint32_t)((int32_t)hz + ((int32_t)arg >> UI_AIR_OFF_SHIFT) * 1250);
		if ((arg & UI_AIR_DMR) != 0) {
			f.mode = RADIO_MODE_DMR;
			f.cc = (uint8_t)(p & 0x0Fu);
			f.ts = (uint8_t)((p >> 4) == 2u ? 2u : 1u);
		} else {
			f.bw = (arg & UI_AIR_NARROW) != 0 ? RADIO_BW_12K5 : RADIO_BW_25K;
			if (p != 0 && p <= TONES_CTCSS_COUNT)
				f.tx_tone = (subtone_t){ TONE_CTCSS, tones_ctcss[p - 1u] };
		}
	}
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

/* DOTA or cross-band takes the radio during the air watch (radio_airwatch.inc air_session_end): the radio core ends it
 * itself and counts the stop (view.dual_stops); the UI's own end that follows is dropped (not counted here) */
void air_sim_session(void)
{
	if (!air_kept.kept)
		return;
	ui_svc_air_end();
	air_sim.end_calls--;
	view.dual_stops++;
	changed();
}
#endif

#if CONFIG_REPEATERS
/* Tools > Repeaters' tune (ui_rpt.c, linked beside the auto scan): not used by these flows */
void radio_req_mode(uint8_t mode) { (void)mode; }
void radio_req_dmr_color_code(uint8_t cc) { (void)cc; }
void radio_req_dmr_timeslot(uint8_t ts) { (void)ts; }
#endif

/* The GPS (the auto scan): off (no GPS), no fix yet, or a fix at air_sim.lat_e7 / lon_e7 */
bool ui_svc_position(ui_position_t *out)
{
	if (air_sim.gps == AIR_GPS_DEFAULT)
		return sim_base_position(out);
	memset(out, 0, sizeof *out);
	if (air_sim.gps == AIR_GPS_OFF)
		return false;
	out->fix = air_sim.gps == AIR_GPS_FIX;
	out->lat_e7 = out->fix ? air_sim.lat_e7 : 0;
	out->lon_e7 = out->fix ? air_sim.lon_e7 : 0;
	out->sats = out->fix ? 9 : 0;
	return true;
}

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
