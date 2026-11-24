// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus priority channel and temporary skip (CONFIG_SCAN_PRIO, docs/plus.md "Priority channel and temporary
// skip"). Included by core/radio/radio_sm.c before scan_service (tools/plus_features.py prepare_scanx).
//
// Upstream's priority (scan_cfg_t.priority, the debug link's set.scan_priority) revisits the zone scan's start
// channel after every 2 others. With a priority channel set (radio/scanprio.h) Plus does this instead, in a zone scan
// and in dual watch / multi-watch:
// - while the scan or the watch steps (LISTEN), the priority channel is the next stop once it hasn't been heard for
//   SCANPRIO_EVERY_MS: a stop like any other (dwell, probe, hold, the wait), so a signal there holds it as a scan stop
//   does. A channel of the scanned zone or of the ring still gets its own turn too: priority adds stops, never fewer;
// - in the wait after a signal (RESUME, between overs) it looks there for one dwell when that and a dwell back fit in
//   what is left of the wait (dual watch's rule for its looks). A signal there holds it (the wait's channel is left);
//   nothing heard: back to the wait's channel and the wait goes on where it was;
// - never during a hold (a call being heard), a probe, our own transmission (the radio core doesn't run the scan
//   then), with the monitor key down, or while the native P25 receiver has a channel (the radio core waits).
// The priority channel may be in another zone than the one scanned: the stop reads that zone (as dual watch does for a
// channel of another zone) and the next stop of the sweep reads the scanned one again. Nuisance delete on it: no more
// priority stops in this scan. A radio with a second receiver (board.rx2) watches as upstream, without priority.

#include "radio/scanprio.h"

#define SCX_NONE	0xFFFFu
#define SCX_RING_NONE	0xFFu
#define SCX_SOON	3u	// zone scan: the sweep's own stop on the priority channel this close makes an extra one pointless
// A stop here: when its retune was done is noted (reading the zone and the channel takes time: radio_scanfast.inc fsc_now)
#if CONFIG_SCAN_FAST
#define SCX_TUNED(sm, now_ms)	fsc_now(sm, now_ms)
#else
#define SCX_TUNED(sm, now_ms)	(now_ms)
#endif

static volatile uint32_t scx_word;	// the setting (radio_scan_prio_set; settings_t.unused_dmr_id at start-up)

static struct {
	uint32_t word;		// resolved from this setting
	uint32_t seen_ms;	// the receiver was last on the priority channel then (a stop, a look, a hold)
	uint32_t wait_ms;	// a look: when the wait it interrupts began
	uint16_t zone, pos;	// where the priority channel is, pos SCX_NONE: off or not found
	uint16_t scan_zone;	// zone scan: the zone scanned
	uint16_t back;		// a look: the wait's position (zone scan) or ring index (watch)
	bool look;		// a look in a wait runs
	bool skipped;		// nuisance-deleted for this scan
	bool fresh;		// find it again before the next use
} scx;

void radio_scan_prio_set(uint32_t word)
{
	scx_word = word;
}

// A zone scan, or a watch on one receiver (CONFIG_DUAL_WATCH: its channels have their changes for now kept). Never
// the air watch's ring (DDM32 Plus, radio_airwatch.inc): only the airfield's letters, all in AM; a priority stop there
// was the user's FM channel in the middle of it (owner's beta.7 report)
static bool scx_kind_ok(const radio_sm_t *sm)
{
#if CONFIG_AIR_WATCH
	if (sm->air) {
		return false;
	}
#endif
	if (sm->scan.kind == SCAN_ZONE) {
		return true;
	}
#if CONFIG_DUAL_WATCH
	return sm->scan.kind == SCAN_DUAL && board.rx2 == NULL;
#else
	return false;
#endif
}

static bool scx_here(const radio_sm_t *sm)
{
	return scx.pos != SCX_NONE && !sm->view.vfo && sm->view.zone_index == scx.zone && sm->view.zone_pos == scx.pos;
}

#if CONFIG_DUAL_WATCH
// The watch: the priority channel's letter in the ring, SCX_RING_NONE when the ring doesn't hold it
static uint8_t scx_ring(const radio_sm_t *sm)
{
	for (uint8_t i = 0; i < dual_count(sm); i++) {
		const radio_dual_ref_t *r = &sm->dual[i];
		if (!r->vfo && r->zone == scx.zone && r->pos == scx.pos) {
			return i;
		}
	}
	return SCX_RING_NONE;
}
#endif

// Where the channel is: the zone scanned (a watch: the zone in use) first, then the zone it was set from
static void __attribute__((noinline)) scx_resolve(radio_sm_t *sm)
{
	zone_t buf;
	channel_t ch;
	uint16_t num, hint;
	scx.word = scx_word;
	scx.pos = SCX_NONE;
	if (!scanprio_unpack(scx.word, &num, &hint) || !sm->have_channels) {
		return;
	}
	uint16_t first = sm->scan.kind == SCAN_ZONE ? scx.scan_zone : sm->view.zone_index;
	for (uint8_t k = 0; k < 2; k++) {
		uint16_t zi = k == 0 ? first : hint;
		const zone_t *z = &sm->zone;
		if (k == 1 && hint == first) {
			break;
		}
		if (zi != sm->view.zone_index) {
			if (!read_zone(zi, &buf)) {
				continue;
			}
			z = &buf;
		}
		for (uint16_t i = 0; i < z->count; i++) {
			if (z->channels[i] == num) {
				scx.zone = zi;
				scx.pos = i;
				break;
			}
		}
		if (scx.pos != SCX_NONE) {
			break;
		}
	}
	if (scx.pos == SCX_NONE || !codeplug_channel(num, &ch) ||
#if CONFIG_P25_SCAN
	    (codeplug_mode_p25(ch.mode) && !codeplug_p25_valid(&ch))) {
#else
	    codeplug_mode_p25(ch.mode)) {
#endif
		scx.pos = SCX_NONE;
	}
}

// The setting changed, or a new scan: find the channel again. A priority channel is due at once (the first stop)
static void scx_check(radio_sm_t *sm, uint32_t now_ms)
{
	if (!scx.fresh && scx.word == scx_word) {
		return;
	}
	scx.fresh = false;
	scx_resolve(sm);
	scx.seen_ms = now_ms - SCANPRIO_EVERY_MS;
}

static bool scx_due(uint32_t now_ms)
{
	return scx.pos != SCX_NONE && !scx.skipped && now_ms - scx.seen_ms >= SCANPRIO_EVERY_MS;
}

// Tune position pos of zone zi (read when it isn't the zone in use). False: it can't be read now
static bool __attribute__((noinline)) scx_tune_at(radio_sm_t *sm, uint16_t zi, uint16_t pos, uint32_t now_ms)
{
	zone_t buf;
	channel_t ch;
	const zone_t *z = &sm->zone;
	if (zi != sm->view.zone_index || !sm->have_channels) {
		if (!read_zone(zi, &buf)) {
			return false;
		}
		z = &buf;
	}
	if (pos >= z->count || !codeplug_channel(z->channels[pos], &ch)) {
		return false;
	}
	if (z != &sm->zone) {
		set_zone(sm, zi, z);
	}
	set_channel(sm, pos, &ch);
	tune(sm, now_ms);
	return true;
}

// The priority channel. In a watch: its letter's own tune when the ring holds it, else the channel the receiver
// leaves keeps its changes for now (dual_tune brings them back) and dual_at stays: the ring goes on from there
static bool scx_tune(radio_sm_t *sm, uint32_t now_ms)
{
#if CONFIG_DUAL_WATCH
	if (sm->scan.kind == SCAN_DUAL) {
		uint8_t r = scx_ring(sm);
		if (r != SCX_RING_NONE) {
			return dual_tune(sm, r, now_ms);
		}
		sm->dual_peek = false;
		if (dual_here(sm, sm->dual_at)) {
			dual_keep_save(sm, sm->dual_at);
		}
	}
#endif
	return scx_tune_at(sm, scx.zone, scx.pos, now_ms);
}

// Zone scan: the scanned zone in use again after a stop on a priority channel of another zone
static bool __attribute__((noinline)) scx_zone_back(radio_sm_t *sm)
{
	zone_t buf;
	if (!read_zone(scx.scan_zone, &buf)) {
		return false;
	}
	set_zone(sm, scx.scan_zone, &buf);
	return true;
}

// scan_config: a scan or a watch begins. A priority channel set replaces upstream's start channel revisits
static void scx_config(const radio_sm_t *sm, uint8_t kind, scan_cfg_t *cfg)
{
	scx.look = scx.skipped = false;
	scx.fresh = true;
	scx.pos = SCX_NONE;
	scx.scan_zone = sm->view.zone_index;
	if (kind != SCAN_VFO && scanprio_number(scx_word) != 0) {
		cfg->priority = false;
	}
}

// scan_step: the priority channel's stop when it is due, or back from a look to its wait (-1: neither, the step is
// upstream's; else what scan_step returns)
static int scx_step(radio_sm_t *sm, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (!scx_kind_ok(sm)) {
		return -1;
	}
	scx_check(sm, now_ms);
	if (scx.look) {
		// Nothing there: back to the channel of the wait, and the wait goes on where it was
		bool ok;
		scx.look = false;
#if CONFIG_DUAL_WATCH
		if (sc->kind == SCAN_DUAL) {
			ok = dual_tune(sm, (uint8_t)scx.back, now_ms);
		} else
#endif
		{
			ok = scx_tune_at(sm, scx.scan_zone, scx.back, now_ms);
		}
		if (!ok) {
			scan_stop(sm, false, now_ms);
			return 0;
		}
		scan_tuned(sc, SCX_TUNED(sm, now_ms));
		sc->phase = SCAN_PH_RESUME;
		sc->phase_ms = scx.wait_ms;
		scan_publish(sm);
		return 1;
	}
	if (sc->kind == SCAN_ZONE && sm->have_channels && sm->view.zone_index != scx.scan_zone && !scx_zone_back(sm)) {
		scan_stop(sm, false, now_ms);
		return 0;
	}
	if (scx_here(sm) || !scx_due(now_ms)) {
		return -1;
	}
	if (sc->kind == SCAN_ZONE && scx.zone == scx.scan_zone && sm->zone.count > 0) {
		// One of the zone scanned that the sweep reaches within SCX_SOON stops anyway: no extra stop just before it
		uint16_t ahead = (uint16_t)((scx.pos + sm->zone.count - sc->sweep_pos) % sm->zone.count);
		if (ahead != 0 && ahead <= SCX_SOON && !scan_pos_skipped(sc, scx.pos)) {
			return -1;
		}
	}
	if (!scx_tune(sm, now_ms)) {
		scx.fresh = true;	// the codeplug changed: found again next time
		return -1;
	}
	scx.seen_ms = now_ms;
	scan_tuned(sc, SCX_TUNED(sm, now_ms));
	scan_publish(sm);
	return 1;
}

// scan_service, every poll: a look at the priority channel in the wait after a signal. True: it starts (retuned)
static bool scx_service(radio_sm_t *sm, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (!scx_kind_ok(sm)) {
		return false;
	}
	scx_check(sm, now_ms);
	if (scx_here(sm)) {
		scx.seen_ms = now_ms;
		if (sc->phase == SCAN_PH_HOLD) {
			scx.look = false;	// the look found a signal: a stop here now, the wait's channel is left
		}
		return false;
	}
	scx.look = false;	// (a look left some other way: PTT or a request in a watch)
	if (!scx_due(now_ms) || sc->phase != SCAN_PH_RESUME || sm->view.monitor) {
		return false;
	}
	uint32_t dwell = sc->cfg.dwell_ms;
	if (now_ms - sc->tuned_ms < dwell || now_ms - sc->phase_ms < dwell ||
	    now_ms - sc->phase_ms + 2u * dwell > sc->cfg.delay_ms) {
		return false;	// no time for a look and a dwell back in this wait
	}
#if CONFIG_DUAL_WATCH
	if (sc->kind == SCAN_DUAL && sm->dual_peek) {
		return false;	// dual watch's own look is out
	}
	uint16_t back = sc->kind == SCAN_DUAL ? sm->dual_at : sm->view.zone_pos;
#else
	uint16_t back = sm->view.zone_pos;
#endif
	uint32_t wait = sc->phase_ms;
	if (!scx_tune(sm, now_ms)) {
		scx.fresh = true;
		return false;
	}
	scx.look = true;
	scx.back = back;
	scx.wait_ms = wait;
	scx.seen_ms = now_ms;
	scan_tuned(sc, SCX_TUNED(sm, now_ms));	// a stop: a signal there holds it
	return true;
}

// A watch's wait looks (dual_wait_service) are its own ring's: not while a look of ours runs, nor while the receiver
// holds a priority channel the ring doesn't have (dual_at is still the ring's channel then)
static bool scx_owns(const radio_sm_t *sm)
{
#if CONFIG_DUAL_WATCH
	return scx.look || (scx_here(sm) && scx_ring(sm) == SCX_RING_NONE);
#else
	(void)sm;
	return scx.look;
#endif
}

// scan_stop: a look ends back on the wait's channel (PTT then talks back there, as on a signal: during a look the
// scan is still on that channel's wait); a cancel from a priority channel of another zone goes back to the start
// channel of the scanned zone. Returns the cancel to use
static bool scx_stop(radio_sm_t *sm, bool cancel, uint32_t now_ms)
{
	if (sm->scan.kind == SCAN_ZONE && scx.pos != SCX_NONE) {
		if (scx.look) {
			if (scx_tune_at(sm, scx.scan_zone, scx.back, now_ms)) {
				cancel = false;
			}
		} else if (cancel && sm->view.zone_index != scx.scan_zone) {
			scx_tune_at(sm, scx.scan_zone, sm->scan.origin_pos, now_ms);
		}
	}
	scx.look = false;
	scx.pos = SCX_NONE;
	scx.fresh = true;
	return cancel;
}

// RSCAN_NUISANCE on the priority channel (zone scan): no more priority stops in this scan; the sweep passes it over
// too when it is in the scanned zone. A look goes back to its wait. False: not the priority channel, upstream's
static bool scx_nuisance(radio_sm_t *sm, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (sc->kind != SCAN_ZONE || !scx_here(sm) || scx.skipped) {
		return false;
	}
	scx.skipped = true;
	if (scx.zone != scx.scan_zone || !scan_nuisance_pos(sc, scx.pos)) {
		sc->skipped++;	// counted for the view
	}
	scan_step(sm, now_ms);
	return true;
}

// RSCAN_UNSKIP: every temporary skip of this scan is undone (the priority channel's too)
static void scx_unskip(radio_sm_t *sm)
{
	scan_t *sc = &sm->scan;
	if (!scan_on(sc)) {
		return;
	}
	memset(sc->skip, 0, sizeof(sc->skip));
	memset(sc->skip_hz, 0, sizeof(sc->skip_hz));
	sc->skipped = 0;
	scx.skipped = false;
	scan_publish(sm);
}
