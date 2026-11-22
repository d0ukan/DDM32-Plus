// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus Satellites' auto track (CONFIG_SATELLITES, docs/plus.md "Satellites: ISS notifications and auto track"):
// the radio core's part. Included by core/radio/radio_sm.c before radio_sm_request (tools/plus_features.py
// prepare_sat_auto); sat_held is declared near the top of radio_sm.c (persist and radio_sm_tx_check read it).
//
// RREQ_SAT_HOLD arg 1 (radio_req_sat_hold(true), from the satellites' service at a pass's rise): when the radio is free
// (idle, no PTT, monitor, scan or watch, search, trunk following, DOTA, cross-band, APRS sending, hotspot, a C..F VFO
// in use), the user's position and VFOs are noted where a power off notes them (persist), and from then on nothing is
// noted (persist returns at once) and TX is refused (receive only) while the service moves the VFO with the Doppler.
// The service waits for radio_sat_held() before it moves anything, so a refusal leaves everything as it was.
// RREQ_SAT_HOLD arg 0: the VFO in use and the position come back from what was noted, as the radio's start takes
// them (radio_sm_init), and the radio is tuned there: on the channel, or in the VFO with its frequency, mode, width,
// tones and talk target. A power off while it holds comes back as before the hold too. RAM: the flag.
// An emergency (the key, the lone worker, SOS) ends the hold at once, before the emergency is taken (emergency_tick:
// the user's channel back, then the emergency on it: a DMR channel transmits, another fails so SOS / the lone worker
// fall back), and a hold left without its track ends by itself (sat_hold_watch).

#include "radio/sat.h"	// sat_auto_on (the hold's owner, sat_svc.c), SAT_TX_*

bool radio_sat_held(void)
{
	return sat_held;
}

// Free for an auto track: nothing the user started, nothing on air, nothing another feature holds
static bool sat_hold_free(const radio_sm_t *sm)
{
	if (sm->ptt || sm->hotspot || sm->view.state != RS_IDLE || sm->view.monitor || sm->power_off_req || dota_on() ||
	    xband_on())
		return false;
#if CONFIG_SCAN
	if (scan_on(&sm->scan))
		return false;		// a scan, dual watch or multi-watch
#endif
#if CONFIG_AIR_WATCH
	if (sm->air)
		return false;
#endif
#if CONFIG_VFO_MODES && CONFIG_MULTI_WATCH
	if (sm->vfox_at >= 2)
		return false;		// a ring letter's own VFO (RAM only): it couldn't be put back
#endif
#if CONFIG_DMR_SEARCH
	if (dmrs_on())
		return false;
#endif
#if CONFIG_DMR_TRUNK
	if (dmrtk_on())
		return false;
#endif
#if CONFIG_P25_TRUNK
	if (p25tk_on())
		return false;
#endif
#if CONFIG_P25_SEARCH
	if (p25s_on())
		return false;
#endif
#if CONFIG_CW_BEACON
	if (beacon.on || beacon.keying)
		return false;
#endif
#if CONFIG_APRS
	if (sm->aprs_phase != APRS_RF_OFF)
		return false;
#endif
	return true;
}

// The hold's end: the VFO in use and the position as noted at its start (radio_sm_init's way)
static void __attribute__((noinline)) sat_hold_end(radio_sm_t *sm, uint32_t now_ms)
{
	sat_held = false;
	const settings_t *st = settings_begin();	// read only (settings_end below finds no change)
#if CONFIG_DUAL_WATCH
	uint8_t idx = sm->vfo_idx;			// the VFO in use, noted in its own fields
#else
	uint8_t idx = 0;
#endif
	vfo_defaults(&sm->vfo_ch);
	sm->vfo_seeded = false;
	sm->vfo_talk_id = 0;
	sm->vfo_talk_private = false;
	restore_vfo(sm, st, idx);
#if CONFIG_VFO_MODES
	if (sm->vfo_seeded)
		vfox_load(st, idx, &sm->vfo_ch);
#endif
	bool vfo = st->vfo;
	uint16_t zi = st->zone_index, number = st->channel, pos = st->zone_pos;
	settings_end(now_ms);
	select_position(sm, zi, number, pos);
	if (!sm->have_channels || (vfo && VFO_AVAILABLE)) {
		if (VFO_AVAILABLE)
			enter_vfo(sm, now_ms);
	} else {
		tune(sm, now_ms);
	}
	changed(sm);
}

// A hold whose track is gone ends by itself (holehunt B6): its end request lost in a full queue (or the track over
// before its hold was taken) left the radio receive only, saving nothing, until a power cycle. Held for 2 s with no auto
// or held track running (sat_auto_on, the service in this task): the user's state back as at a normal end. From
// radio_sm_poll. RAM: one word
static void sat_hold_watch(radio_sm_t *sm, uint32_t now_ms)
{
	static uint32_t orphan_ms;	// since when held without a track (0: not)
	if (!sat_held || sat_auto_on()) {
		orphan_ms = 0;
	} else if (orphan_ms == 0) {
		orphan_ms = now_ms | 1u;
	} else if (now_ms - orphan_ms >= 2000u) {
		sat_hold_end(sm, now_ms);
	}
}

// The service's word on the air (sat.h SAT_TX_*; radio_task.c): PTT, or the radio's own transmission. An APRS frame
// counts from its wait for a clear channel on (holehunt B8): a Doppler step then (a VFO request) would abort it, so the
// tracking waits for the frame as it waits for any TX
uint8_t radio_sat_txk(const radio_sm_t *sm)
{
	if (sm->ptt)
		return SAT_TX_PTT;
#if CONFIG_APRS
	if (sm->aprs_phase != APRS_RF_OFF)
		return SAT_TX_OWN;
#endif
	return sm->view.state == RS_TX ? SAT_TX_OWN : SAT_TX_NONE;
}

static void sat_hold_request(radio_sm_t *sm, bool on, uint32_t now_ms)
{
	if (!on) {
		if (sat_held)
			sat_hold_end(sm, now_ms);
		return;
	}
	if (sat_held || !sat_hold_free(sm))
		return;
	persist(sm, now_ms);	// the user's state, as a power off notes it
	sat_held = true;	// then nothing more is noted until the end
}
