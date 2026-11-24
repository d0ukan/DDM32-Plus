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
// After a step made on the tick the poll's squelch doesn't open until the retune is FS_POLL_GUARD_MS old (a classic
// stop's first poll is that old), so a reading right after the retune never opens it: a new stop, and the return from a
// priority look to the wait it interrupted (radio_scanprio.inc scx_step: phase RESUME there). Only the opening waits:
// the rest of the poll (VOX, roaming, the floor) runs as always. The counters (scanfast.c) follow every
// stop from here: the scan's own probe and hold counts say which stops heard something.
//
// v1.0.2 (docs/plus.md "A zone scans slowly"): those ages count from the end of the retune (fsc_now), and a step the
// poll made (classic, a priority stop) gets the guard too (FS_RETUNE_GUARD_MS), so the time a step spends reading the
// channel never puts the first reading or the first poll inside the noise reading's dip. Where the time goes is
// counted for scanst: the longest stop, the reading behind the last fall-back, the radio task's tick gap and step time,
// stops in another zone, FM / DMR switches.

#include "hal_time.h"	// hal_millis: a step's time (fs_radio_cost)

static uint16_t fsc_steps;	// scan_t.steps when last looked at
static uint16_t fsc_seen;	// scan_t.probes + holds then
static uint16_t fsc_holds;	// scan_t.holds then
static uint8_t fsc_mode;	// the current stop's FS_M_*
static uint32_t fsc_stepped_ms;	// the tune of the last stop the tick stepped to (fsc_guard)
static bool fsc_stepped;
static uint32_t fsc_retune_tag;	// the stop fsc_now saw (its scan_t.tuned_ms) and when its retune was done
static uint32_t fsc_retune_ms;
static uint16_t fsc_zone;	// the zone scanned (a stop elsewhere is a priority channel's)
static uint32_t fsc_tick_ms;	// the last tick while a stop listened (the radio task's gaps, fs_radio_cost)
static bool fsc_tick_listen;
#if FS_DIP_GUARD
static uint32_t fsc_dip_tag;	// v1.0.3: the stop (its tuned_ms) whose readings met a young not-quiet one (fsc_tick)
static bool fsc_dip_seen;
static uint32_t fsc_idle_tag;	// ... the stop whose squelch a poll last saw closed (fsc_guard), and the retune's age then
static uint8_t fsc_idle_age;
static uint32_t fsc_hold_tag;	// ... the stop whose hold began with a squelch opened early (fsc_active)
static bool fsc_hold_early;
#endif

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

// scan_step and the priority channel's stops (radio_scanprio.inc), as the stop is tuned: notes when its retune was
// done and returns now_ms (the stop's tuned_ms, so its dwell, probe and hold are timed as in v1.0.1). The step began
// at now_ms (the poll or the tick), then read the next channel (on the radio each read walks the codeplug's settings
// area on the data chip, several for a P25 channel) and retuned. The squelch guard and the fast rule's first reading
// count from the retune, so the time those reads took never eats into them (v1.0.2: after a slow step the first poll
// met the noise reading's dip right after the retune, the squelch opened and the scan held a quiet channel for its
// resume delay, classic and fast). Dual watch: not noted. More than a second later: not a read (a clock that isn't
// this one, as in some host tests): now_ms
static uint32_t fsc_now(const radio_sm_t *sm, uint32_t now_ms)
{
	uint32_t t = hal_millis();
	if (fsc_kind(sm)) {
		fsc_retune_tag = now_ms;
		fsc_retune_ms = t - now_ms > 1000u ? now_ms : t;
	}
	return now_ms;
}

// When the current stop's retune was done (its tuned_ms when fsc_now didn't see it: a pause's end, our transmission)
static uint32_t fsc_retuned(const scan_t *sc)
{
	return sc->tuned_ms == fsc_retune_tag ? fsc_retune_ms : sc->tuned_ms;
}

// scan_start: a zone or VFO scan begins (its first stop follows)
static void fsc_begin(radio_sm_t *sm, uint32_t now_ms)
{
	fs_begin(now_ms);
	fsc_steps = (uint16_t)(sm->scan.steps - 1u);	// the first stop is seen as new
	fsc_seen = fsc_holds = 0;
	fsc_mode = FS_MODES;
	fsc_stepped = false;
	fsc_zone = sm->view.zone_index;
	fsc_tick_listen = false;
}

// The band of the stop (scanst.cost BANDS): the air band (108-137 MHz, or AM receive), VHF, UHF
static uint8_t fsc_band(const radio_sm_t *sm)
{
	uint32_t hz = sm->view.ch.rx_hz;
	if (sm->view.am || (hz >= 108000000u && hz < 137000000u))
		return FS_B_AIR;
	return hz < 300000000u ? FS_B_VHF : FS_B_UHF;
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
		uint8_t m = fsc_mode_of(sm);
		bool chan = sc->kind == SCAN_ZONE && !sm->view.vfo;
		fs_stop_about(chan ? sm->view.channel_number : 0, chan && sm->view.zone_index != fsc_zone,
			      fsc_mode != FS_MODES && (fsc_mode == FS_M_DMR) != (m == FS_M_DMR), fsc_band(sm),
			      sm->view.am);
		fsc_mode = m;
	} else if (s->state != FS_ST_OFF && s->tuned_ms != sc->tuned_ms) {
		s->tuned_ms = s->first_ms = s->last_ms = sc->tuned_ms;
		s->state = FS_ST_SAMPLING;
	}
}

// The noise reading a quiet reading must reach: the squelch's opening line and the margin
static uint16_t fsc_line(const radio_sm_t *sm)
{
	uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
	return (uint16_t)((uint16_t)radio_sm_squelch_open_below(sq) + FS_NOISE_MARGIN);
}

// One reading: 0 when clearly nothing the scan would stop or probe for, else why not (FS_FB_*: scanst's fall-back
// reason)
static uint8_t fsc_not_quiet(const radio_sm_t *sm, const radio_status_t *st)
{
	if ((uint16_t)st->noise < fsc_line(sm))
		return FS_FB_NOISE;
	if (fsc_mode == FS_M_FM)
		return 0;
#if CONFIG_DMR
	if (st->rssi_dbm == 0 || dmr_strength_heard(sm, (int16_t)(st->rssi_dbm + FS_RSSI_MARGIN)))
		return FS_FB_STRENGTH;
	if (fsc_mode == FS_M_DMR) {
		uint32_t heard_ms;
		if (sm->view.dmr.state != RDMR_IDLE ||
		    (dmr_call_last_activity(&heard_ms) && (int32_t)(heard_ms - sm->scan.tuned_ms) >= 0))
			return FS_FB_DMR;
	}
#endif
	return 0;
}

// radio_sm_tick: read the stop, step when it is quiet
static void fsc_tick(radio_sm_t *sm, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (!scan_on(sc) || !fsc_kind(sm)) {
		fsc_tick_listen = false;
		return;
	}
	fsc_observe(sm);
	// The radio task's gap since the last tick, both in a listen phase (a hold, the native receiver's or a pause's
	// time is not the task's): what reading the next channel and retuning cost, whoever stepped (counted to the stop
	// stepped to)
	if (fsc_tick_listen && sc->phase == SCAN_PH_LISTEN)
		fs_radio_cost(now_ms - fsc_tick_ms, 0);
	fsc_tick_ms = now_ms;
	fsc_tick_listen = sc->phase == SCAN_PH_LISTEN;
	fs_stop_t *s = fs_cur();
	if (s->state != FS_ST_SAMPLING || sc->phase != SCAN_PH_LISTEN || sm->view.monitor || sm->ptt || sm->hotspot ||
	    sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED || now_ms - sc->tuned_ms < FS_SETTLE_MS ||
	    (int32_t)(now_ms - fsc_retuned(sc)) < (int32_t)FS_SETTLE_RETUNE_MS)
		return;
#if CONFIG_APRS
	if (sm->aprs_phase != APRS_RF_OFF)
		return;		// the receiver is on the APRS frequency: these readings are not the stop's
#endif
#if CONFIG_BATTERY_SAVE
	if (sm->save_asleep)
		return;
#endif
	bool idle = sm->view.state == RS_IDLE && !sm->view.am && !sm->view.bcast;
	// Not idle: AM or broadcast receive (never read quiet), or the squelch already open
	uint8_t why = sm->view.am || sm->view.bcast ? FS_FB_AM : FS_FB_OPEN;
	radio_status_t st = { 0 };
	if (idle) {
		board.radio->status(&st);
		why = fsc_not_quiet(sm, &st);
	}
	bool quiet = why == 0;
#if FS_DIP_GUARD
	// v1.0.3: a reading that is not quiet while the retune is young may be the receiver's false carrier after it (or
	// the last frequency's strength): the span starts again and from then on counts only readings from FS_DIP_MS. AM
	// and broadcast are never read quiet: handed over at once as before
	if (why != FS_FB_AM && (int32_t)(now_ms - fsc_retuned(sc)) < (int32_t)FS_DIP_MS) {
		if (!quiet) {
			if (fsc_dip_tag != sc->tuned_ms || !fsc_dip_seen)
				fs_dip();
			fsc_dip_tag = sc->tuned_ms;
			fsc_dip_seen = true;
			s->first_ms = s->last_ms = s->tuned_ms;	// no span (fs_feed)
			return;
		}
		if (fsc_dip_seen && fsc_dip_tag == sc->tuned_ms)
			return;		// after a dip reading: not counted before FS_DIP_MS
	}
#endif
	uint32_t span = fsc_mode == FS_M_DMR ? FS_SPAN_DMR_MS : FS_SPAN_MS;
#if FS_DIP_GUARD
	if (fsc_dip_seen && fsc_dip_tag == sc->tuned_ms && span < FS_SPAN_DIP_MS)
		span = FS_SPAN_DIP_MS;	// after a dip reading: one reading more (a carrier near the line gets another chance)
#endif
	uint8_t v = fs_feed(s, now_ms, quiet, span);
	if (v == FS_WAIT)
		return;
	fs_count(v);
	if (v != FS_QUIET) {
		// What handed it to the classic rules (noise 0: the squelch was already open, or AM / broadcast)
		uint16_t line = fsc_line(sm);
		fs_fallback_reading(fsc_mode, why, idle ? st.noise : 0u, (uint8_t)(line > 255u ? 255u : line),
				    idle ? st.rssi_dbm : 0);
		return;
	}
#if CONFIG_P25_SCAN
	if (sc->kind == SCAN_ZONE && codeplug_mode_p25(sm->view.ch.mode))
		(void)native_lab_scan_stop(native_lab_scan_key(sm), (uint8_t)sc->steps, false);	// its stop count
#endif
	scan_step(sm, now_ms);
	uint32_t took = hal_millis() - now_ms;	// reading the next channel and retuning
	fsc_stepped = true;
	fsc_stepped_ms = sc->tuned_ms;
	outputs(sm);
	fsc_observe(sm);
	fs_radio_cost(0, took);			// the stop stepped to
}

// radio_sm_poll's squelch: true while the receiver's last retune was the tick's step and is younger than a classic
// stop is at its first poll (the squelch doesn't open yet). By the retune, not the phase or the algorithm: a quiet
// priority look ended on the tick tunes back to its wait (RESUME), and the switch to classic takes effect at the stop
// the tick has just stepped to, which is as young
// v1.0.2: also after any other step (the poll's: classic, a priority stop; a request's: the scan's start, a nuisance
// skip), FS_RETUNE_GUARD_MS. A classic step the poll made on time meets its first poll 22 ms or more later, so the
// guard holds back only a first poll that comes sooner: after a slow step, a step a request made between two polls,
// or polls off their grid. Not with a scan dwell under FS_RETUNE_GUARD_DWELL_MS: there a poll the guard held back could
// be the last before the dwell's end, and a channel that opens the squelch at once (FM, no tone) be stepped past on
// its first open; those dwells keep v1.0.1's squelch exactly
// v1.0.3 (FS_DIP_GUARD): the guard lasts until the retune is FS_DIP_MS old, cut to the dwell less two polls (a signal
// that opens the squelch at the first poll after it still holds the scan at the next one, before the dwell's end), and
// never shorter than v1.0.2's
static bool fsc_guard(const radio_sm_t *sm, uint32_t now_ms)
{
	const scan_t *sc = &sm->scan;
	if (!scan_on(sc) || !fsc_kind(sm) || sm->view.state != RS_IDLE)
		return false;
	int32_t age = (int32_t)(now_ms - fsc_retuned(sc));
#if FS_DIP_GUARD
	// The squelch is closed at this poll of the stop (called before the poll's squelch): a squelch open later opened
	// at this age or after (fsc_active)
	fsc_idle_tag = sc->tuned_ms;
	fsc_idle_age = (uint8_t)(age < 0 ? 0 : age > 255 ? 255 : age);
	uint32_t dwell = sc->cfg.dwell_ms, win = dwell > 2u * RADIO_POLL_MS ? dwell - 2u * RADIO_POLL_MS : 0u;
	if (win > FS_DIP_MS)
		win = FS_DIP_MS;
#else
	uint32_t win = 0;
#endif
	if (fsc_stepped && sc->tuned_ms == fsc_stepped_ms)
		return age < (int32_t)(win > FS_POLL_GUARD_MS ? win : FS_POLL_GUARD_MS);
	return sc->tuned_ms == fsc_retune_tag && sc->cfg.dwell_ms >= FS_RETUNE_GUARD_DWELL_MS &&
	       age < (int32_t)(win > FS_RETUNE_GUARD_MS ? win : FS_RETUNE_GUARD_MS);
}

// radio_sm_poll's scan service, an FM-path stop (FM, AM, NXDN, dPMR, and P25 / YSF before their own rule), before
// scan_poll; returns active as it is. v1.0.3 (FS_DIP_GUARD): a hold whose squelch opened while the retune was younger
// than FS_DIP_MS (a short dwell's guard is shorter; or the squelch was never seen closed at this stop) and that never
// heard a carrier after the settle (scan_t.heard: the noise reading under the line) was the receiver's false carrier
// after the retune: when the squelch has closed, the scan moves on at once instead of waiting the resume delay (2 s by
// default). The hold itself is kept (a weak station that opened the squelch early is still held, and hears its carrier).
// Broadcast FM: as before
static bool fsc_active(radio_sm_t *sm, bool active, uint32_t now_ms)
{
#if FS_DIP_GUARD
	scan_t *sc = &sm->scan;
	if (!fsc_kind(sm) || sm->view.bcast)
		return active;
	if (active && (sc->phase == SCAN_PH_LISTEN || sc->phase == SCAN_PH_PROBE)) {
		fsc_hold_tag = sc->tuned_ms;	// a hold starts at this poll: did its squelch open early?
		fsc_hold_early = fsc_idle_tag != sc->tuned_ms || fsc_idle_age < FS_DIP_MS;
	} else if (!active && sc->phase == SCAN_PH_RESUME && !sc->heard && fsc_hold_early &&
		   fsc_hold_tag == sc->tuned_ms && (uint32_t)(now_ms - sc->phase_ms) < sc->cfg.delay_ms) {
		sc->phase_ms = now_ms - sc->cfg.delay_ms;	// scan_poll: the wait is over
		fs_dip_hold();
	}
#else
	(void)sm;
	(void)now_ms;
#endif
	return active;
}
