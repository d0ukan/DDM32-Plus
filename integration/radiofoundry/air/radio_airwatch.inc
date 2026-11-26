// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus air watch (CONFIG_AIR_WATCH, item 44b, docs/plus.md "Round 14: air band and spectrum"): the radio core's
// part. Included by core/radio/radio_sm.c before radio_sm_request (tools/plus_air.py). Tools > Airfields' "Watch all"
// gives the ring letters' own VFOs (A..F: VFOs 0..5) an airfield's frequencies (RREQ_AIR_VFO), then starts the ring as
// multi-watch does (RREQ_MWATCH): the letters are FM VFOs in the air band, so each is received in AM
// (radio_am_channel) with the AM squelch on its signal strength, and none transmits (TXDENY_AM).
//
// The user's own state is kept where a power cycle keeps it: the first RREQ_AIR_VFO stops a scan or the user's watch
// and notes the position and VFOs A / B in the settings (persist), and nothing more is noted while the air watch runs
// (persist returns at once). RREQ_AIR_END brings VFOs A / B and the position back from there, as the radio's start
// does (radio_sm_init); C..F's VFOs are emptied (RAM only, as after a power off). A power off during the air watch
// therefore comes back as before it.
//
// An ATIS (RREQ_AIR_VFO's idx | RADIO_AIR_ATIS; the UI puts it last) is watched too, but its recording never stops:
// held as any signal, it would keep the one receiver there for good. So its letter holds the ring AIR_ATIS_HOLD_MS at
// most (from the first poll of the hold, through the wait and its looks), then the ring goes on and every ATIS letter
// is passed over (nothing heard there: the ring's quiet stop) for AIR_ATIS_COOL_MS. RAM: the flag, the ATIS bits and
// two times (12 B in radio_sm_t).
//
// DDM32 Plus auto scan (CONFIG_AUTO_SCAN, docs/autoscan-design.md): the same ring for the nearest repeaters and
// airfields. RREQ_AIR_VFO's arg carries the letter's kind (RADIO_AIR_FM / _DMR: a repeater, as Tools > Repeaters'
// tune sets the VFO: RX, mode, FM bandwidth and access tone as the TX tone, DMR colour code and slot, TX = RX + the
// listed offset; a DMR letter talks to the user's DMR VFO talk target as it was at the start). Nothing in it refuses
// a transmission the radio's own checks allow (an input outside the bands is refused by the band check as on any VFO).
// RADIO_AIR_REPLACE marks a refresh: refused while the ring holds a signal or waits after one, and while transmitting,
// so a letter is never swapped under a call (the UI asks again). PTT while the ring holds or waits on a letter talks
// on that letter (air_talk_back: the one the user hears), else on the selected one as multi-watch does. DOTA and
// cross-band end the air watch before they take the radio (air_session_end), and a scan the user starts ends it first
// (air_scan_first): their own stops left sm->air set before (review M3, m9).
enum { AIR_ATIS_HOLD_MS = 5000, AIR_ATIS_COOL_MS = 60000 };
enum { AIR_K_AM, AIR_K_FM, AIR_K_DMR };

// A VFO for letter request arg on hz: a fresh VFO (FM, so AM in the air band), simplex; a repeater's as listed (tone10:
// an FM letter's access tone, Hz x 10 as the list has it, 0 none: as Tools > Repeaters' tune sets it)
static void air_vfo_fill(channel_t *c, uint32_t arg, uint32_t hz, uint16_t tone10)
{
	vfo_defaults(c);
	c->rx_hz = c->tx_hz = hz;
	uint8_t kind = (uint8_t)((arg >> RADIO_AIR_KIND_SHIFT) & 3u), p = (uint8_t)((arg >> RADIO_AIR_P_SHIFT) & 0x3Fu);
	if (kind == AIR_K_AM)
		return;
	int32_t off = (int32_t)arg >> RADIO_AIR_OFF_SHIFT;	// 1.25 kHz steps, signed
	c->tx_hz = (uint32_t)((int32_t)hz + off * 1250);
	if (kind == AIR_K_DMR) {
		c->mode = RADIO_MODE_DMR;
		c->color_code = (uint8_t)(p & 0x0Fu);
		c->timeslot = (uint8_t)((p >> 4) == 2u ? 2u : 1u);
		return;
	}
	c->mode = RADIO_MODE_FM;
	c->bw = (arg & RADIO_AIR_NARROW) != 0 ? RADIO_BW_12K5 : RADIO_BW_25K;
	if (tone10 != 0)
		c->tx_tone = (subtone_t){ TONE_CTCSS, tone10 };	// the access tone; the RX tone stays off
}

// The ring holds a signal or waits after one (a letter the user hears)
static bool air_held(const radio_sm_t *sm)
{
	return dual_running(sm) && (sm->scan.phase == SCAN_PH_HOLD || sm->scan.phase == SCAN_PH_RESUME);
}

// RREQ_AIR_VFO: VFO idx (0..5) takes hz. What the receiver is tuned to stays (the ring that follows tunes), unless
// that VFO is on view: it is tuned again
static void __attribute__((noinline)) air_vfo(radio_sm_t *sm, uint32_t arg, uint32_t hz, uint16_t tone10,
					      uint32_t now_ms)
{
	bool atis = (arg & RADIO_AIR_ATIS) != 0;
	uint8_t idx = (uint8_t)(arg & 7u), kind = (uint8_t)((arg >> RADIO_AIR_KIND_SHIFT) & 3u);
	if (idx >= RADIO_DUAL_SLOTS || kind > AIR_K_DMR || radio_am_band(hz) != (kind == AIR_K_AM) ||
	    !board_in_band(hz, false))
		return;
	if ((arg & RADIO_AIR_REPLACE) != 0 &&
	    (!sm->air || air_held(sm) || (dual_running(sm) && sm->scan.phase == SCAN_PH_PROBE)))
		return;		// a refresh: never under a call or a signal being looked at (the UI asks again), never after the end
	if (!sm->air) {
		if (scan_on(&sm->scan))
			scan_stop(sm, true, now_ms);	// a scan or the user's watch: back where it started (dual
							// watch: on the selected one), noted as a power off would
		persist(sm, now_ms);			// the position and VFOs A / B, as a power off keeps them
		dual_keep_drop(sm);
		sm->air = true;
		sm->air_atis = 0;
		sm->air_held_ms = 0;
		sm->air_cool_ms = now_ms;		// nothing passed over yet
		sm->air_talk = sm->vfo_talk_id | (sm->vfo_talk_private ? 0x80000000u : 0u);	// the user's DMR target
	}
	if (atis)
		sm->air_atis |= (uint8_t)(1u << idx);
	else
		sm->air_atis &= (uint8_t)~(1u << idx);
	sm->dual_keep[idx].valid = false;		// a letter that changed: its changes for now went with it
	dual_busy_set(sm, (uint8_t)(sm->view.dual_busy & ~(1u << idx)));
	// A DMR letter: the user's DMR VFO target at the start, TG 9 when it had none
	uint32_t talk = kind != AIR_K_DMR ? 0u : (sm->air_talk & 0x7FFFFFFFu) != 0 ? sm->air_talk : 9u;
	channel_t c;
	if (idx == vfox_in_use(sm)) {
		air_vfo_fill(&sm->vfo_ch, arg, hz, tone10);
		sm->vfo_seeded = true;
		sm->vfo_talk_id = talk & 0x7FFFFFFFu;
		sm->vfo_talk_private = (talk & 0x80000000u) != 0;
		vfox_retune(sm, now_ms);		// on view: tuned again (else only kept)
		return;
	}
	air_vfo_fill(&c, arg, hz, tone10);
	if (idx >= 2) {
		vfox_slot_put(&sm->vfox_x[idx - 2u], &c, talk, true);
	} else if (sm->vfox_at >= 2 && idx == sm->vfo_idx) {
		vfox_slot_put(&sm->vfox_park, &c, talk, true);	// A / B waiting while a C..F VFO is in use
	} else {
		sm->vfo_other = (radio_vfo_t){ c, true, talk & 0x7FFFFFFFu, (talk & 0x80000000u) != 0 };
	}
	changed(sm);
}

// RREQ_AIR_END: the ring stops; VFOs A / B and the position as the settings keep them (radio_sm_init's way), C..F's
// VFOs empty. Nothing without an air watch
static void __attribute__((noinline)) air_end(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->air)
		return;
	sm->air = false;
	sm->air_atis = 0;
	if (scan_on(&sm->scan))
		scan_stop(sm, false, now_ms);
	dual_keep_drop(sm);
	vfox_unpark(sm);				// a C..F VFO in use goes: vfo_ch is VFO vfo_idx again
	memset(sm->vfox_x, 0, sizeof sm->vfox_x);	// C..F: empty, as after a power off
	const settings_t *st = settings_begin();	// read only (settings_end below finds no change)
	vfo_defaults(&sm->vfo_ch);			// VFO 1 first, then moved to vfo_other, as radio_sm_init
	sm->vfo_seeded = false;
	sm->vfo_talk_id = 0;
	sm->vfo_talk_private = false;
	restore_vfo(sm, st, 1);
	if (sm->vfo_seeded)
		vfox_load(st, 1, &sm->vfo_ch);
	sm->vfo_other = (radio_vfo_t){ sm->vfo_ch, sm->vfo_seeded, sm->vfo_talk_id, sm->vfo_talk_private };
	vfo_defaults(&sm->vfo_ch);
	sm->vfo_seeded = false;
	sm->vfo_talk_id = 0;
	sm->vfo_talk_private = false;
	restore_vfo(sm, st, 0);
	if (sm->vfo_seeded)
		vfox_load(st, 0, &sm->vfo_ch);
	bool vfo = st->vfo;
	uint8_t idx = st->vfo_index;
	uint16_t zi = st->zone_index, number = st->channel, pos = st->zone_pos;
	settings_end(now_ms);
	sm->vfo_idx = 0;
	sm->view.vfo_index = 0;
	vfo_use(sm, idx);
	select_position(sm, zi, number, pos);
	if (!sm->have_channels || (vfo && VFO_AVAILABLE)) {
		if (VFO_AVAILABLE)
			enter_vfo(sm, now_ms);
	} else {
		tune(sm, now_ms);
	}
	changed(sm);
}

// DOTA / cross-band take the radio (dota_take, xb_take, before they tune): the air watch ends first as the user's stop
// would, and the view counts the stop so the UI ends its side (its RREQ_AIR_END would be dropped during the session)
static void air_session_end(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->air)
		return;
	air_end(sm, now_ms);
	sm->view.dual_stops++;
}

// A scan of the user's asked for (RREQ_SCAN: a zone, the VFO, dual watch) during the air watch: it ends first, so the
// UI's RREQ_AIR_END that follows doesn't stop the new scan
static void air_scan_first(radio_sm_t *sm, uint8_t what, uint32_t now_ms)
{
	if (sm->air && (what == RSCAN_ZONE || what == RSCAN_VFO || what == RSCAN_DUAL ||
			(what == RSCAN_TOGGLE && !scan_on(&sm->scan))))
		air_end(sm, now_ms);
}

// PTT (ptt_down, before dual_talk): while the ring holds a signal or waits after one, the letter heard is the one PTT
// talks on (during a look, the one the look goes back to); the UI follows the radio's selection (mw_tick: "Talking on")
static void air_talk_back(radio_sm_t *sm)
{
	if (!sm->air || !air_held(sm))
		return;
	uint8_t k = sm->dual_peek ? dual_back(sm) : sm->dual_at;
	if (k < dual_count(sm)) {
		sm->dual_sel = k;
		sm->view.dual_sel = k;
	}
}

// Every scan poll (scan_service, before the scan engine): the ring on an ATIS letter of the air watch. Passed over
// while it cools (nothing heard: the ring's quiet stop); held AIR_ATIS_HOLD_MS (the hold and the wait after it, looks
// and all): true, the ring goes on and every ATIS letter cools. Any other stop, or a new one, starts the time over
static bool air_atis_poll(radio_sm_t *sm, bool *active, bool *carrier, bool *sync, uint32_t now_ms)
{
	const scan_t *sc = &sm->scan;
	if (!sm->air || sm->air_atis == 0)
		return false;
	const radio_dual_ref_t *r = &sm->dual[sm->dual_at];
	bool atis = dual_running(sm) && r->vfo && r->pos < RADIO_DUAL_SLOTS && ((sm->air_atis >> r->pos) & 1u) != 0;
	bool held = sc->phase == SCAN_PH_HOLD || sc->phase == SCAN_PH_RESUME;
	if (!atis || !held)
		sm->air_held_ms = 0;
	if (!atis)
		return false;
	if ((int32_t)(sm->air_cool_ms - now_ms) > 0) {
		*active = *carrier = *sync = false;	// cooling: passed over
		return false;
	}
	if (!held)
		return false;
	if (sm->air_held_ms == 0) {
		sm->air_held_ms = now_ms | 1u;		// 0 is "not held"
		return false;
	}
	if (now_ms - sm->air_held_ms < AIR_ATIS_HOLD_MS)
		return false;
	sm->air_held_ms = 0;
	sm->air_cool_ms = now_ms + AIR_ATIS_COOL_MS;
	return true;
}

// The requests (radio_req_air_vfo / radio_req_air_end): refused, as RREQ_MWATCH is, while transmitting or in hotspot
// mode
static void air_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	if (sm->view.state == RS_TX || sm->hotspot || sm->ptt)
		return;
	if (req->type == RREQ_AIR_VFO) {
		uint32_t arg = (uint32_t)req->arg, hz = req->arg2;
		uint16_t tone10 = 0;
		if ((arg & (RADIO_AIR_FM | RADIO_AIR_DMR)) != 0) {	// a repeater letter: radio_air_arg2's packing
			hz = (req->arg2 & 0xFFFFFu) * 1250u;
			tone10 = (uint16_t)(req->arg2 >> 20);
		}
		air_vfo(sm, arg, hz, tone10, now_ms);
	}
	else
		air_end(sm, now_ms);
}
