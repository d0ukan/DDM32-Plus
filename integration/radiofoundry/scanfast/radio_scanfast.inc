// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus faster channel scan (CONFIG_SCAN_FAST, radio/scanfast.h, docs/plus.md "Faster channel scan"): the radio
// core's side. Included by core/radio/radio_sm.c before scan_service (tools/plus_fastscan.py).
//
// A zone or VFO scan's stop in its listen phase is read on the radio task's 10 ms tick (radio_sm_tick), from
// FS_SETTLE_MS after the retune. A reading is quiet when the scan's own carrier rule would see nothing with a margin:
// the noise reading FS_NOISE_MARGIN above the squelch's opening line (carrier_heard), and on a digital channel the
// strength FS_RSSI_MARGIN dB under the DMR strength line (dmr_strength_heard, which the scan uses for DMR and P25 /
// YSF stops) with no DMR call or burst heard since the tune. A squelch already open, AM or broadcast receive, the
// monitor, PTT, a hotspot or APRS on the receiver: the stop is left to the classic rules. The first reading that is
// not quiet hands the stop to them for good (its dwell, probe and hold, unchanged). When the quiet readings span the
// mode's time (FS_SPAN_MS; DMR a TDMA frame, FS_SPAN_DMR_MS) the scan steps at once, as its dwell's end would
// (scan_step: the priority channel, nuisance delete, the zone or band order are all its).
//
// After a step made on the tick the poll's squelch waits until the stop is FS_POLL_GUARD_MS old (a classic stop's
// first poll is that old), so a reading right after the retune never opens it. The counters (scanfast.c) follow every
// stop from here: the scan's own probe and hold counts say which stops heard something.

static uint16_t fsc_steps;	// scan_t.steps when last looked at
static uint16_t fsc_seen;	// scan_t.probes + holds then
static uint16_t fsc_holds;	// scan_t.holds then
static uint8_t fsc_mode;	// the current stop's FS_M_*
static uint32_t fsc_stepped_ms;	// the tune of the last stop the tick stepped to (fsc_guard)
static bool fsc_stepped;

static uint8_t fsc_mode_of(const radio_sm_t *sm)
{
	uint8_t m = sm->view.ch.mode;
	if (m == RADIO_MODE_DMR)
		return FS_M_DMR;
	if (codeplug_mode_p25(m))
		return FS_M_P25;
	return m == RADIO_MODE_FM ? FS_M_FM : FS_M_OTHER;
}

static bool fsc_kind(const radio_sm_t *sm)
{
	return sm->scan.kind == SCAN_ZONE || sm->scan.kind == SCAN_VFO;
}

// scan_start: a zone or VFO scan begins (its first stop follows)
static void fsc_begin(radio_sm_t *sm, uint32_t now_ms)
{
	fs_begin(now_ms);
	fsc_steps = (uint16_t)(sm->scan.steps - 1u);	// the first stop is seen as new
	fsc_seen = fsc_holds = 0;
	fsc_mode = FS_MODES;
	fsc_stepped = false;
}

// A new stop since the last look (scan_tuned), or the same one tuned anew (a pause's end: a fresh listen)
static void fsc_observe(radio_sm_t *sm)
{
	scan_t *sc = &sm->scan;
	fs_stop_t *s = fs_cur();
	if ((uint16_t)sc->steps != fsc_steps) {
		uint16_t seen = (uint16_t)(sc->probes + sc->holds);
		fs_stop_new(sc->tuned_ms, sc->kind == SCAN_VFO ? sm->view.ch.rx_hz : sc->sweep_pos, fsc_mode,
			    seen != fsc_seen, (uint16_t)sc->holds != fsc_holds);
		fsc_steps = (uint16_t)sc->steps;
		fsc_seen = seen;
		fsc_holds = (uint16_t)sc->holds;
		fsc_mode = fsc_mode_of(sm);
	} else if (s->state != FS_ST_OFF && s->tuned_ms != sc->tuned_ms) {
		s->tuned_ms = s->first_ms = s->last_ms = sc->tuned_ms;
		s->state = FS_ST_SAMPLING;
	}
}

// One reading: clearly nothing the scan would stop or probe for
static bool fsc_quiet(const radio_sm_t *sm, const radio_status_t *st)
{
	uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
	if ((uint16_t)st->noise < (uint16_t)radio_sm_squelch_open_below(sq) + FS_NOISE_MARGIN)
		return false;
	if (fsc_mode == FS_M_FM)
		return true;
#if CONFIG_DMR
	if (st->rssi_dbm == 0 || dmr_strength_heard(sm, (int16_t)(st->rssi_dbm + FS_RSSI_MARGIN)))
		return false;
	if (fsc_mode == FS_M_DMR) {
		uint32_t heard_ms;
		if (sm->view.dmr.state != RDMR_IDLE ||
		    (dmr_call_last_activity(&heard_ms) && (int32_t)(heard_ms - sm->scan.tuned_ms) >= 0))
			return false;
	}
#endif
	return true;
}

// radio_sm_tick: read the stop, step when it is quiet
static void fsc_tick(radio_sm_t *sm, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (!scan_on(sc) || !fsc_kind(sm))
		return;
	fsc_observe(sm);
	fs_stop_t *s = fs_cur();
	if (s->state != FS_ST_SAMPLING || sc->phase != SCAN_PH_LISTEN || sm->view.monitor || sm->ptt || sm->hotspot ||
	    sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED || now_ms - sc->tuned_ms < FS_SETTLE_MS)
		return;
#if CONFIG_APRS
	if (sm->aprs_phase != APRS_RF_OFF)
		return;		// the receiver is on the APRS frequency: these readings are not the stop's
#endif
#if CONFIG_BATTERY_SAVE
	if (sm->save_asleep)
		return;
#endif
	bool quiet = sm->view.state == RS_IDLE && !sm->view.am && !sm->view.bcast;
	if (quiet) {
		radio_status_t st;
		board.radio->status(&st);
		quiet = fsc_quiet(sm, &st);
	}
	uint8_t v = fs_feed(s, now_ms, quiet, fsc_mode == FS_M_DMR ? FS_SPAN_DMR_MS : FS_SPAN_MS);
	if (v == FS_WAIT)
		return;
	fs_count(v);
	if (v != FS_QUIET)
		return;
#if CONFIG_P25_SCAN
	if (sc->kind == SCAN_ZONE && codeplug_mode_p25(sm->view.ch.mode))
		(void)native_lab_scan_stop(native_lab_scan_key(sm), (uint8_t)sc->steps, false);	// its stop count
#endif
	scan_step(sm, now_ms);
	fsc_stepped = true;
	fsc_stepped_ms = sc->tuned_ms;
	outputs(sm);
	fsc_observe(sm);
}

// radio_sm_poll after the scan's service: true while a stop the tick stepped to is younger than a classic stop is at
// its first poll (the squelch waits). By the stop, not the algorithm: the switch to classic takes effect at the stop
// the tick has just stepped to, which is as young
static bool fsc_guard(const radio_sm_t *sm, uint32_t now_ms)
{
	const scan_t *sc = &sm->scan;
	return scan_on(sc) && fsc_kind(sm) && fsc_stepped && sc->tuned_ms == fsc_stepped_ms && sc->phase == SCAN_PH_LISTEN &&
	       sm->view.state == RS_IDLE && now_ms - sc->tuned_ms < FS_POLL_GUARD_MS;
}
