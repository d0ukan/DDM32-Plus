// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus multi-watch (CONFIG_MULTI_WATCH, docs/plus.md "Multi-watch"): dual watch's pair grown into a ring of n
// channels, dual[0..n-1] (A..F), on the one receiver. Included by core/radio/radio_sm.c where dual_start was
// (tools/plus_features.py prepare_multi_watch). The ring keeps every rule of dual watch (radio-features.md 2.1): each
// channel gets the scan's dwell, a signal holds the receiver, the wait after it (dual_delay_ms) stays for the reply
// and looks at the other channels in turn, PTT transmits on the selected one (dual_sel) and the ring goes on after.
// n = 2 is dual watch exactly: dual_next is !dual_at, a look goes to !dual_at and back, as upstream.

// The wait after a signal on the ring: the next channel to look at (each of the others in turn), and the one the look
// goes back to
static uint8_t dual_look_next(radio_sm_t *sm)
{
	if (dual_count(sm) <= 2) {
		return (uint8_t)!sm->dual_at;
	}
	sm->dual_home = sm->dual_at;
	do {
		sm->dual_look = (uint8_t)((sm->dual_look + 1u) % dual_count(sm));
	} while (sm->dual_look == sm->dual_home);
	return sm->dual_look;
}

static uint8_t dual_back(const radio_sm_t *sm)
{
	return dual_count(sm) <= 2 ? (uint8_t)!sm->dual_at : sm->dual_home;
}

// Every channel's changes for now go (the user stopped the watch)
static void dual_keep_drop(radio_sm_t *sm)
{
	for (uint8_t i = 0; i < RADIO_DUAL_SLOTS; i++) {
		sm->dual_keep[i].valid = false;
	}
}

// Another channel could already be on this VFO (the ring: any of them)
static bool dual_vfo_taken(const radio_sm_t *sm, uint8_t vfo)
{
	for (uint8_t i = 0; i < dual_count(sm); i++) {
		if (i != sm->dual_sel && sm->dual[i].vfo && sm->dual[i].pos == vfo) {
			return true;
		}
	}
	return false;
}

// Each one readable, none twice
static bool dual_refs_ok(const radio_sm_t *sm, const radio_dual_ref_t ref[], uint8_t n)
{
	for (uint8_t i = 0; i < n; i++) {
		if (!dual_ok(sm, &ref[i])) {
			return false;
		}
		for (uint8_t k = 0; k < i; k++) {
			if (dual_same(&ref[i], &ref[k])) {
				return false;
			}
		}
	}
	return true;
}

// The watch on ref[0..n-1], ref[sel] selected: dual_start's rules for n channels. The same ring again only changes
// the selection; a new one keeps the receiver where it is when that channel is still in it (a call it holds too), else
// it starts on the selected one. A channel that changed (or left the ring) drops its changes for now and its Busy mark
static void dual_start_n(radio_sm_t *sm, const radio_dual_ref_t ref[], uint8_t n, uint8_t sel, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (n < 2 || n > RADIO_DUAL_SLOTS || sel >= n || !dual_refs_ok(sm, ref, n)) {
		return;
	}
	bool running = dual_running(sm);
	if (scan_on(sc) && !running) {
		scan_stop(sm, false, now_ms);
	}
	uint8_t was_n = dual_count(sm);
	for (uint8_t i = 0; i < RADIO_DUAL_SLOTS; i++) {
		if (i >= n || i >= was_n || !dual_same(&sm->dual[i], &ref[i])) {
			sm->dual_keep[i].valid = false;	// another channel: its changes for now went with the old one
			dual_busy_set(sm, (uint8_t)(sm->view.dual_busy & ~(1u << i)));
		}
	}
	for (uint8_t i = 0; i < n; i++) {
		sm->dual[i] = ref[i];
	}
	sm->dual_n = n;
	sm->dual_sel = sel;
	sm->view.dual_sel = sel;
	if (sm->dual_look >= n) {
		sm->dual_look = 0;
	}
	bool here = false;
	for (uint8_t i = 0; i < n; i++) {
		if (dual_here(sm, i)) {
			sm->dual_at = i;
			here = true;
		}
	}
	if (n > 2) {
		rx2_release(sm);	// a second receiver takes the pair's other channel only; the ring alternates
	}
	if (running && here) {
		if (board.rx2 != NULL && n == 2) {
			rx2_engage(sm, now_ms);	// the selection may have changed: the receivers swap channels
			scan_publish(sm);
		}
		return;
	}
	if (!running) {
		scan_cfg_t cfg;
		scan_config(sm, SCAN_DUAL, &cfg);
		scan_begin(sc, SCAN_DUAL, &cfg, now_ms);
		activity(sm, now_ms);
	}
	if (board.rx2 != NULL && n == 2 && rx2_engage(sm, now_ms)) {
		scan_publish(sm);
		return;
	}
	if (!here) {
		if (!dual_tune(sm, sel, now_ms)) {
			scan_end(sc);
			scan_publish(sm);
			return;
		}
		scan_tuned(sc, now_ms);
	}
	scan_publish(sm);
}

static radio_dual_ref_t mw_ref_buf[RADIO_DUAL_SLOTS];	// a ring asked for: only the radio task uses it

// RREQ_MWATCH: the ring the UI asked for (radio_req_mwatch left it with the radio task). Refused, as RREQ_DUAL is,
// while transmitting or in hotspot mode
static void mw_request(radio_sm_t *sm, uint32_t now_ms)
{
	uint8_t n = 0, sel = 0;
	if (!radio_mwatch_take(mw_ref_buf, &n, &sel) || sm->view.state == RS_TX || sm->hotspot || sm->ptt) {
		return;
	}
	dual_start_n(sm, mw_ref_buf, n, sel, now_ms);
}

#if CONFIG_P25_CHANNELS
// DDM32 Plus (docs/plus.md "The TX target", second review): a watch request (RREQ_MWATCH, RREQ_DUAL) whose ring still
// has the channel the receiver is on: dual_start_n keeps the receiver there (only the selection or the ring around it
// changes), so the native receiver keeps a P25 call it holds there instead of letting it go (native_lab.c). Peeks at
// the request, takes nothing
static radio_dual_ref_t dual_ref(uint32_t a);
bool radio_sm_dual_keeps(radio_sm_t *sm, const radio_req_t *req)
{
	uint8_t n = 0, sel = 0;
	if (!dual_running(sm)) {
		return false;
	}
	if (req->type == RREQ_MWATCH) {
		if (!radio_mwatch_take(mw_ref_buf, &n, &sel)) {
			return false;
		}
	} else if (req->type == RREQ_DUAL) {
		uint32_t a = (uint32_t)req->arg, b = req->arg2;
		if (((a | b) & RADIO_DUAL_SEL) == 0) {
			return false;	// a stop: the radio stays on the selected channel, maybe another one
		}
		mw_ref_buf[0] = dual_ref(a);
		mw_ref_buf[1] = dual_ref(b);
		n = 2;
	} else {
		return false;
	}
	for (uint8_t i = 0; i < n && i < RADIO_DUAL_SLOTS; i++) {
		if (dual_ref_here(sm, &mw_ref_buf[i])) {
			return true;
		}
	}
	return false;
}
#endif

#if CONFIG_DMR_EMERGENCY
static void scan_stop_asked(radio_sm_t *sm, bool cancel, uint32_t now_ms);

// Letter i can carry the emergency (hole hunt B2): a DMR channel or VFO (as its slot keeps it) that passes the TX checks
// every PTT press gets (radio_sm_tx_check: TX inhibit, receive only, the band, the air band, a marine VFO, Any color
// code or slot) with an own DMR ID. The check reads the view: the letter is put there for it, then the view as it was
static bool __attribute__((noinline)) dual_dmr_tx(radio_sm_t *sm, uint8_t i)
{
	const radio_dual_ref_t *r = &sm->dual[i];
	channel_t ch;
	bool here = dual_ref_here(sm, r);
	if (here) {
		ch = sm->view.ch;
	} else if (r->vfo) {
#if CONFIG_VFO_MODES
		uint32_t talk;
		bool seeded;
		if (r->pos >= 2 && r->pos < RADIO_DUAL_SLOTS)
			vfox_slot_get(&sm->vfox_x[r->pos - 2u], &ch, &talk, &seeded);
		else if (sm->vfox_at >= 2 && r->pos == sm->vfo_idx)
			vfox_slot_get(&sm->vfox_park, &ch, &talk, &seeded);	// waiting while a C..F VFO is in use
		else
#endif
			ch = r->pos == sm->vfo_idx ? sm->vfo_ch : sm->vfo_other.ch;
	} else {
		zone_t z;
		if (dual_read(sm, r, &z, &ch) == NULL)
			return false;
	}
	if (ch.mode != RADIO_MODE_DMR || (ch.dmr_id == 0 && sm->own_id == 0))
		return false;
	if (here)
		return radio_sm_tx_check(sm) == TXDENY_NONE;
	channel_t keep = sm->view.ch;
	bool vfo = sm->view.vfo;
	sm->view.ch = ch;
	sm->view.vfo = r->vfo;
#if CONFIG_AM_RX
	bool am = sm->view.am;
	sm->view.am = false;		// the receiver's letter's (an air band letter is refused by its frequency)
#endif
	uint8_t deny = radio_sm_tx_check(sm);
	sm->view.ch = keep;
	sm->view.vfo = vfo;
#if CONFIG_AM_RX
	sm->view.am = am;
#endif
	return deny == TXDENY_NONE;
}

// DDM32 Plus (docs/plus.md "The TX target", third review): an emergency asked for (the user's key, the lone worker, SOS)
// while the watch runs. Its alarm, hot mic and talk target are the channel the radio is on, which in the ring is the
// letter it happens to be on: the watch ends on the selected letter when it is a DMR letter that may transmit
// (dual_dmr_tx), else on the first such letter A..F (the UI follows the stop and says which: "Watch off: on A"). None:
// the watch goes on and the emergency fails as on any FM channel (SOS / the lone worker fall back). Called from emergency_tick before dmr_emergency_tick takes the start
static void __attribute__((noinline)) mw_emergency(radio_sm_t *sm, uint32_t now_ms)
{
	uint8_t n = dual_count(sm), pick = RADIO_DUAL_SLOTS;
	if (sm->ptt || sm->view.state == RS_TX) {
		return;	// PTT first: it ends, then the emergency goes on the channel it was on
	}
	if (sm->dual_sel < n && dual_dmr_tx(sm, sm->dual_sel)) {
		pick = sm->dual_sel;
	}
	for (uint8_t i = 0; i < n && pick == RADIO_DUAL_SLOTS; i++) {
		if (dual_dmr_tx(sm, i)) {
			pick = i;
		}
	}
	if (pick == RADIO_DUAL_SLOTS) {
		return;
	}
	sm->dual_sel = pick;
	sm->view.dual_sel = pick;
	scan_stop_asked(sm, true, now_ms);	// on that letter, counted (view.dual_stops): the UI stops its watch there
}
#endif
