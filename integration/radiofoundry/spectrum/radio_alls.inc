// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus all digital search, the radio core's side (radio/dmrsearch.h CONFIG_ALL_SEARCH, docs/plus.md "All digital
// search"). Included by core/radio/radio_sm.c before dmrs_service (tools/plus_alldigital.py), which hands this kind to
// alls_service. The DMR search's engine and hooks do the rest: tune() ends it, radio_sm_tx_check denies (receive
// only), PTT and any other request end it, dmr_configure hears everything while dmrs_on, no APRS job meanwhile.
//
// The sweep runs on the FM path. A hit is probed mode by mode in the engine's order (P25 > DMR > YSF > NXDN), each
// with the receiver it needs:
//   P25, YSF  the native C4FM receiver (native_lab.c takes the frequency: while it owns the modem the radio task does
//             not poll this; it gives it back with dmrs_regained and this sets up what comes next)
//   DMR       the board in DMR mode, the call stack hearing every color code, slot and talkgroup (dmrs_heard_read)
//   NXDN      the FM path, the software 4FSK modem (radio_sm_poll's NXDN block listens on dmrs_nxdn_hz)
// Nothing here keys anything.

static uint8_t alls_epoch_seen;
static void scan_stop(radio_sm_t *sm, bool cancel, uint32_t now_ms);	// below (the scan)

// The search's own request (a new range or its stop) doesn't end it; the other kind's does (that one starts anew)
static bool dmrs_own_request(const radio_req_t *req)
{
	return dmrs_kind() == DMRS_KIND_ALL ? req->type == RREQ_ALL_SEARCH : req->type == RREQ_DMR_SEARCH;
}

// The receiver set up for what the search does now: the sweep's step or a mode's probe on the frequency held
static void __attribute__((noinline)) alls_setup(radio_sm_t *sm, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	uint32_t hz = dmrs_at_hz();
	end_reception(sm, now_ms);
	if (dmrs_phase() != DMRS_SWEEP && dmrs_mode() == DMRS_MODE_DMR && board.dmr != NULL) {
		if (sm->board_mode != RADIO_MODE_DMR) {
			sm->board_mode = RADIO_MODE_DMR;
			board.radio->set_mode(RADIO_MODE_DMR);
		}
		dmrs_tune(sm, hz);
		if (!sm->dmr_running) {
			dmr_configure(sm);	// dmrs_on: every color code, slot and talkgroup, receive only
			sm->dmr_running = dmr_call_start();
			sm->view.dmr.active = sm->dmr_running;
			sm->dmr_serial = dmr_call_view()->serial - 1u;
		}
	} else {
		// The FM path: the sweep, the native receiver's modes, NXDN (no DMR modem: its probe hears nothing)
		if (sm->board_mode == RADIO_MODE_DMR)
			dmr_end(sm);
		if (sm->board_mode != RADIO_MODE_FM) {
			sm->board_mode = RADIO_MODE_FM;
			board.radio->set_mode(RADIO_MODE_FM);
		}
		board.radio->rx(hz, RADIO_BW_12K5, &none);
	}
	dmrs_retuned(now_ms);
	sm->audio_out = sm->green_out = -1;	// outputs() sets the speaker and the LEDs again
}

// What the probe of the mode held now heard (the radio core's modes; the native receiver reports its own)
static void alls_heard(dmrs_heard_t *h, uint32_t now_ms)
{
	if (dmrs_mode() == DMRS_MODE_DMR) {
		dmrs_heard_read(h);
		return;
	}
	memset(h, 0, sizeof *h);
	h->cc = DMRS_CC_NONE;
#if CONFIG_NXDN
	if (dmrs_mode() == DMRS_MODE_NXDN) {
		radio_nxdn_caller_t c;
		radio_nxdn_caller(&c);
		if (radio_nxdn_heard(now_ms))
			h->bursts = DMRS_MIN_BURSTS;	// frames with a good LICH in the last 300 ms
		if (c.active) {
			h->cc = c.ran;
			h->call = c.playing;
			h->other = c.encrypted;
			h->group = c.group;
			h->dst = c.destination;
			h->src = c.source;
		}
	}
#endif
	(void)now_ms;
}

// Every poll while the all digital search runs and the radio core has the receiver
static void __attribute__((noinline)) alls_service(radio_sm_t *sm, uint32_t now_ms, uint32_t *shown_ms)
{
	if (dmrs_epoch() != alls_epoch_seen) {
		alls_epoch_seen = dmrs_epoch();	// the native receiver gave the frequency back: what comes next
		alls_setup(sm, now_ms);
	} else if (dmrs_phase() == DMRS_SWEEP) {
		static const subtone_t none = { TONE_NONE, 0 };
		radio_status_t st;
		bool hit;
		board.radio->status(&st);
		uint32_t hz = dmrs_sweep(st.rssi_dbm, now_ms, &hit);
		if (hit)
			alls_setup(sm, now_ms);	// the first mode's receiver (P25: the native receiver takes it from here)
		else if (hz != 0)
			board.radio->rx(hz, RADIO_BW_12K5, &none);
	} else {
		// P25 / YSF before the native receiver has taken the frequency (it lets go of the last receive first): nothing
		// heard, so the mode's window still runs out if it never does
		dmrs_heard_t h;
		alls_heard(&h, now_ms);
#if CONFIG_SEARCH_SURE
		if (dmrs_mode() == DMRS_MODE_DMR || dmrs_mode() == DMRS_MODE_NXDN) {
			radio_status_t st;
			board.radio->status(&st);
			h.rssi_dbm = st.rssi_dbm;	// the level on the radio core's receiver (radio/searchsure.h)
		}
#endif
		if (dmrs_probe(&h, now_ms) != 0)
			alls_setup(sm, now_ms);	// the next mode on this frequency, or the sweep's next step
	}
	if (!dmrs_on()) {
		tune(sm, now_ms);	// nobody looks at the search any more: the channel again
		return;
	}
	if (now_ms - *shown_ms >= 250u) {
		*shown_ms = now_ms;
		changed(sm);	// the screen follows the search about four times a second
	}
	outputs(sm);
}

// RREQ_ALL_SEARCH: arg the low end (with the step's tag, radio/srange.h; 0 = stop), arg2 the high end
static void __attribute__((noinline)) alls_request(radio_sm_t *sm, const radio_req_t *req, bool busy, uint32_t now_ms)
{
	if (req->arg == 0) {
		if (dmrs_on())
			tune(sm, now_ms);	// ends the search: the channel again
		return;
	}
	bool aprs_busy = false;
#if CONFIG_APRS
	aprs_busy = sm->aprs_phase != APRS_RF_OFF;
#endif
	if (busy || sm->ptt || aprs_busy || SEARCH_WATCH_ON(sm))
		return;	// not over a watch (hole hunt B3): the screen says "Stop the scan or watch first"
	if (scan_on(&sm->scan))
		scan_stop(sm, false, now_ms);	// a plain scan
	bool again = dmrs_on();		// a new range (the DMR search was ended by radio_sm_request: dmrs_own_request)
	uint32_t lo = (uint32_t)req->arg, step = 12500u;
#if CONFIG_SEARCH_RANGE
	step = srange_step_hz(srange_tag_of(lo));
	lo = srange_lo_of(lo);
#endif
	(void)dmrs_start_all(lo, req->arg2, step, now_ms);
	if (!again && sm->board_mode == RADIO_MODE_DMR)
		dmr_end(sm);	// the channel's call, a repeater role: the search's own probes start the stack anew
	alls_setup(sm, now_ms);
	alls_epoch_seen = dmrs_epoch();
	outputs(sm);
	changed(sm);
}
