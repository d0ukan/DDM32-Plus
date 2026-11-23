/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 search engine (p25search.h). No RTOS, no hardware. */
#include "config.h"
#if CONFIG_P25_SEARCH
#include <string.h>
#include "radio/p25search.h"
#if CONFIG_SEARCH_SURE
#if !CONFIG_SEARCH_FAST
#error "SEARCH_SURE builds on SEARCH_FAST"
#endif
#include "radio/searchsure.h"
#endif
#if CONFIG_SEARCH_HYBRID
#if !CONFIG_SEARCH_SURE
#error "SEARCH_HYBRID builds on SEARCH_SURE"
#endif
#include "radio/searchhy.h"
#endif

static p25s_view_t s;
static uint32_t tuned_ms;
static int32_t floor_q4;		/* floor in dBm x 16 */
static struct { uint32_t hz, until_ms; } skip[P25S_SKIP_MAX];
static volatile uint32_t seq;		/* odd while written (snapshot retries) */
static volatile uint32_t reads;		/* snapshots taken: the screen still looks */
static uint32_t reads_seen, read_ms, epoch;
/* Revisits (2026-11-17, the owner wanted it faster): every P25S_REVISIT_STEPS sweep steps one frequency of the
 * found list that lies in the range is read between two steps, round robin, so a new call on a channel found
 * before is caught in about a second instead of a whole sweep; the sweep then goes on where it was */
static uint32_t resume_hz;
static uint8_t since_revisit, revisit_i;
static bool revisiting;
/* The found list's key (p25search.h): found[0] is the entry of the probe held now (held), a call counted there and when */
static bool held, credited;
static uint32_t credit_ms;
/* The PC's search log (searchlog_view.c): entry serials and the calls counted since power on */
static uint16_t ids, calls_seq;
#if CONFIG_SEARCH_FAST
/* The faster sweep (P25S_QUICK_MS): the step's stage, the step before it when only its quick reading was taken, and
 * the step a look back came from */
enum { ST_QUICK, ST_CONFIRM, ST_BACK, ST_AIM };
static uint8_t stage;
static uint32_t prev_hz, back_from;
#endif
#if CONFIG_SEARCH_SURE
/* The surer search (radio/searchsure.h): the floor, the run being followed and the last probe's skirt; ST_AIM reads
 * the run's middle before it is probed. While the native receiver probes, the radio task still reads the level
 * (p25s_level): when it was last on, so a carrier that went off the air during the probe is not skipped.
 * step_q4: a sweep step's time (ms x 16, the screen's steps a second) */
static sure_t u;
static bool level_known;
static uint32_t on_ms, step_ms0, step_q4;
#endif
#if CONFIG_SEARCH_HYBRID
/* The faster search (radio/searchhy.h): the revisit read now is a watched carrier's; the probe's level read a gap
 * (under the hit's half margin) at least once */
static bool watching, low_seen_any;
static int16_t sup_lvl;
static uint32_t sup_hz;		/* the first weaker run a stronger one ended in the run followed now (and its level) */
#endif

static void begin_write(void) { seq++; __asm__ volatile("" ::: "memory"); }
static void end_write(void) { __asm__ volatile("" ::: "memory"); seq++; }

/* A skip entry's frequency: the same, or (the surer search) within half a step (a run's middle may come out a half
 * step off from one pass to the next) */
static bool same_hz(uint32_t a, uint32_t b)
{
#if CONFIG_SEARCH_SURE
	return (a > b ? a - b : b - a) <= s.step_hz / 2u;
#else
	return a == b;
#endif
}

static bool skipped(uint32_t hz, uint32_t now_ms)
{
	for (unsigned i = 0; i < P25S_SKIP_MAX; i++)
		if (same_hz(skip[i].hz, hz) && (int32_t)(skip[i].until_ms - now_ms) > 0)
			return true;
	return false;
}

static void add_skip(uint32_t hz, uint32_t now_ms)
{
#if CONFIG_SEARCH_FAST
	/* Its own entry (running or run out: a repeat gets the long skip), else one run out, else the one ending first */
	unsigned pick = P25S_SKIP_MAX;
	for (unsigned i = 0; i < P25S_SKIP_MAX && pick == P25S_SKIP_MAX; i++)
		if (same_hz(skip[i].hz, hz))
			pick = i;
	bool again = pick < P25S_SKIP_MAX && (int32_t)(now_ms - skip[pick].until_ms) < (int32_t)P25S_AGAIN_MS;
	for (unsigned i = 0; i < P25S_SKIP_MAX && pick == P25S_SKIP_MAX; i++)
		if ((int32_t)(skip[i].until_ms - now_ms) <= 0)
			pick = i;
	if (pick == P25S_SKIP_MAX) {
		pick = 0;
		for (unsigned i = 1; i < P25S_SKIP_MAX; i++)
			if ((int32_t)(skip[i].until_ms - skip[pick].until_ms) < 0)
				pick = i;
	}
	skip[pick].hz = hz;
#if CONFIG_SEARCH_HYBRID
	/* fast: a steady carrier (its level never under the hit's half margin during the probe) not P25 again on a later
	 * pass (a busy FM carrier, a beacon) is skipped for minutes; otherwise classic's (radio/searchhy.h sh_reject) */
	bool longer = sh_reject(hz, s.sweeps, !low_seen_any) && again;
	skip[pick].until_ms = now_ms + (longer ? SH_REJECT_MS : again ? P25S_SKIP_LONG_MS : P25S_SKIP_MS);
#else
	skip[pick].until_ms = now_ms + (again ? P25S_SKIP_LONG_MS : P25S_SKIP_MS);
#endif
}
#else
	unsigned pick = 0;
	for (unsigned i = 0; i < P25S_SKIP_MAX; i++) {
		if (skip[i].hz == hz || (int32_t)(skip[i].until_ms - now_ms) <= 0) {
			pick = i;
			break;
		}
		if ((int32_t)(skip[i].until_ms - skip[pick].until_ms) < 0)
			pick = i;	/* full: replace the one expiring first */
	}
	skip[pick].hz = hz;
	skip[pick].until_ms = now_ms + P25S_SKIP_MS;
}
#endif

/* The next found frequency to read again (round robin), 0 = none in the range */
static uint32_t revisit_pick(uint32_t now_ms)
{
	for (unsigned k = 0; k < s.found_n; k++) {
		revisit_i = (uint8_t)((revisit_i + 1u) % s.found_n);
		uint32_t hz = s.found[revisit_i].hz;
		bool again = false;	/* a frequency with more entries (NACs, talkgroups) is read once a round */
		for (unsigned j = 0; j < revisit_i && !again; j++)
			again = s.found[j].hz == hz;
		if (!again && hz >= s.lo_hz && hz <= s.hi_hz && !skipped(hz, now_ms))
			return hz;
	}
	return 0;
}

/* The next step after `hz`, wrapping to the bottom (a full sweep) */
static uint32_t next_step(uint32_t hz, uint32_t now_ms)
{
	uint32_t n = hz + s.step_hz;
	if (n > s.hi_hz) {
		n = s.lo_hz;
		s.sweeps++;
#if CONFIG_SEARCH_HYBRID
		sh_pass(now_ms);
#endif
	}
	(void)now_ms;
	return n;
}

/* Where to go from s.at_hz: back to the sweep after a revisit, else the next step or, now and then, a revisit */
static uint32_t advance(uint32_t now_ms)
{
	if (revisiting) {
		revisiting = false;
#if CONFIG_SEARCH_HYBRID
		watching = false;
#endif
		return resume_hz;
	}
	uint32_t n = next_step(s.at_hz, now_ms);
#if CONFIG_SEARCH_HYBRID
	/* fast: a watched carrier is read halfway between two found list revisits (which keep classic's every
	 * P25S_REVISIT_STEPS steps), and in a found list revisit's place when the list has none in the range */
	if (sh_fast() && since_revisit + 1u == P25S_REVISIT_STEPS / 2u) {
		uint32_t w = sh_watch_pick(s.lo_hz, s.hi_hz, now_ms, skipped);
		if (w != 0 && w != n) {
			since_revisit++;
			resume_hz = n;
			revisiting = watching = true;
			return w;
		}
	}
#endif
	if (++since_revisit >= P25S_REVISIT_STEPS) {
		since_revisit = 0;
		uint32_t r = revisit_pick(now_ms);
#if CONFIG_SEARCH_HYBRID
		if (r == 0 && sh_fast()) {
			r = sh_watch_pick(s.lo_hz, s.hi_hz, now_ms, skipped);
			watching = r != 0;
		}
#endif
		if (r != 0 && r != n) {
			resume_hz = n;
			revisiting = true;
			return r;
		}
	}
	return n;
}

uint32_t p25s_start(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms)
{
	if (hi_hz < lo_hz) {
		uint32_t t = lo_hz;
		lo_hz = hi_hz;
		hi_hz = t;
	}
#if CONFIG_SEARCH_SURE
	{
		/* The steps on the channel grid (multiples of the step), whatever start was typed: a 12.5 kHz search of
		 * 436.010-436.500 reads 436.0125, ..., 436.2750, not 436.0100, ..., 436.2725 (3.75 kHz off every channel) */
		uint32_t st = step_hz ? step_hz : 12500u, a = lo_hz + (st - lo_hz % st) % st;
		if (a <= hi_hz)
			lo_hz = a;
	}
#endif
	begin_write();
	uint8_t keep_n = s.found_n;
	p25s_found_t keep[P25S_FOUND_MAX];
	memcpy(keep, s.found, sizeof keep);
	memset(&s, 0, sizeof s);
	memcpy(s.found, keep, sizeof keep);	/* the found list outlives a restart (a new range) */
	s.found_n = keep_n;
	s.lo_hz = lo_hz;
	s.hi_hz = hi_hz;
	s.step_hz = step_hz ? step_hz : 12500u;
	s.at_hz = lo_hz;
	s.phase = P25S_SWEEP;
	s.floor_dbm = -125;
	end_write();
#if CONFIG_SEARCH_HYBRID
	sh_start(SH_P25, now_ms);
	watching = false;
	sup_hz = 0;
#endif
	floor_q4 = -125 * 16;
	memset(skip, 0, sizeof skip);
	revisiting = false;
	since_revisit = revisit_i = 0;
	resume_hz = 0;
	held = false;
#if CONFIG_SEARCH_FAST
	stage = ST_QUICK;
	prev_hz = 0;
#endif
#if CONFIG_SEARCH_SURE
	memset(&u, 0, sizeof u);	/* the floor learned anew from the first step */
	step_q4 = 0;
	step_ms0 = now_ms;
#endif
	tuned_ms = read_ms = now_ms;
	reads_seen = reads;
	epoch++;
	return lo_hz;
}

#if CONFIG_SEARCH_SURE
/* A sweep step done: the screen's step time (probes and the hand-offs left out) */
static void step_note(uint32_t now_ms)
{
	uint32_t dt = now_ms - step_ms0;
	step_ms0 = now_ms;
#if CONFIG_SEARCH_HYBRID
	sh_step(now_ms);
#endif
	if (dt >= 1000u)
		return;
	step_q4 = step_q4 ? (uint32_t)((int32_t)step_q4 + ((int32_t)(dt * 16u) - (int32_t)step_q4) / 8) : dt * 16u;
	s.step_ms = (uint16_t)((step_q4 + 8u) / 16u);
}

/* The frequency held now is a hit: the native receiver probes it (within begin_write) */
static void probe_here(int16_t lvl, uint32_t now_ms)
{
	if (!u.climbing) {
		u.run_lo = u.run_hi = s.at_hz;	/* a hit without a run (a revisit, the range's top): its own skirt */
		u.run_lvl = lvl;
	}
	u.climbing = false;
#if CONFIG_SEARCH_HYBRID
	sh_probe(watching, now_ms);
	watching = low_seen_any = false;
#endif
	s.phase = P25S_PROBE;
	s.hits++;
	held = false;
	stage = ST_QUICK;
	prev_hz = 0;
	level_known = false;
	on_ms = now_ms;
}

/* A step's reading is complete (strong against the floor): follow a run, probe its middle, or go on
 * (radio/searchsure.h) */
static uint32_t sure_step(bool strong, int16_t lvl, uint32_t now_ms, bool *hit)
{
	begin_write();
	if (!u.floor_ok)
		strong = false;	/* the first step teaches the floor */
	if (stage == ST_AIM) {
		stage = ST_QUICK;
		if (strong && !skipped(s.at_hz, now_ms)) {
			probe_here(lvl, now_ms);
			end_write();
			*hit = true;
			return 0;
		}
		u.climbing = false;	/* nothing there now (the over ended): on past the run, nothing skipped */
#if CONFIG_SEARCH_HYBRID
		sh_unsettled(s.at_hz, now_ms);	/* fast: read again between steps for a while */
#endif
		if (sure_wide_floor(&u, &floor_q4))
			s.floor_dbm = (int16_t)(floor_q4 / 16);
		revisiting = false;
		s.at_hz = resume_hz;
		end_write();
		tuned_ms = now_ms;
		return s.at_hz;
	}
	bool skirt = strong && sure_skirt(&u, s.at_hz, lvl, now_ms, s.sweeps);
	sure_floor(&u, &floor_q4, lvl, strong);
	s.floor_dbm = (int16_t)(floor_q4 / 16);
	bool cand = strong && !skirt;
#if CONFIG_SEARCH_HYBRID
	if (cand)
		sh_seen(s.at_hz, lvl, now_ms);
#endif
	if (revisiting && cand && !skipped(s.at_hz, now_ms)) {
		probe_here(lvl, now_ms);	/* a find read again: its own frequency, no run */
		end_write();
		*hit = true;
		return 0;
	}
	if (!revisiting && (u.climbing || cand)) {
		stage = ST_QUICK;
		prev_hz = 0;
#if CONFIG_SEARCH_HYBRID
		bool was = u.climbing;
		uint32_t before = was ? sure_target(&u, s.step_hz) : 0u, lo0 = u.run_lo;
		int16_t blvl = u.best_lvl;
#endif
		int r = sure_climb(&u, s.at_hz, lvl, cand);
#if CONFIG_SEARCH_HYBRID
		/* fast: the first weaker run a stronger one ended (classic forgets it): a carrier of its own may be there (a
		 * weak digital signal just below a strong FM one, whose run took it in). Kept for the run's end (below) */
		if (!was)
			sup_hz = 0;
		else if (sh_fast() && r == SURE_MORE && u.run_lo == s.at_hz && u.run_lo != lo0 && sup_hz == 0) {
			sup_hz = before;
			sup_lvl = blvl;
		}
#endif
		if (r == SURE_WIDE && !sure_peaked(&u)) {
			/* the band's level raised, no carrier in it: the floor (one step clearly over the rest: probed below) */
			if (sure_wide_floor(&u, &floor_q4))
				s.floor_dbm = (int16_t)(floor_q4 / 16);
			sure_set_skirt(&u, now_ms, s.sweeps);
			strong = false;
		} else if (r == SURE_MORE && s.at_hz + s.step_hz <= s.hi_hz) {
			s.at_hz += s.step_hz;	/* the next step (no revisit inside a run) */
			step_note(now_ms);
			end_write();
			tuned_ms = now_ms;
			return s.at_hz;
		} else {
		/* The run is complete: its target is probed (radio/searchsure.h sure_target); the sweep then goes on from the
		 * step after the run. SURE_WIDE with one step clearly over the rest: that step (never hidden, hole hunt A3);
		 * the run's level becomes the floor when it ends */
		uint32_t t = sure_target(&u, s.step_hz);
#if CONFIG_SEARCH_HYBRID
		/* fast: that weaker run, two steps or more below the target and reading more than SH_SKIRT_DB under it (a
		 * receiver's selectivity puts the target's own skirt about 45 dB down two steps off: verifier, fastsrch), is
		 * watched: read again between steps and probed on its own frequency if still strong */
		if (sup_hz != 0 && sup_hz + 2u * s.step_hz <= t && sup_lvl > u.run_lvl - SH_SKIRT_DB)
			sh_unsettled(sup_hz, now_ms);
		sup_hz = 0;
#endif
		resume_hz = u.run_hi + s.step_hz <= s.hi_hz ? u.run_hi + s.step_hz : next_step(u.run_hi, now_ms);
		revisiting = true;
		if (skipped(t, now_ms)) {
#if CONFIG_SEARCH_HYBRID
			sh_skipped();
#endif
			if (sure_wide_floor(&u, &floor_q4))
				s.floor_dbm = (int16_t)(floor_q4 / 16);	/* the band's level raised, no carrier: the floor */
			sure_set_skirt(&u, now_ms, s.sweeps);	/* a carrier known not to be P25: its steps passed over */
			revisiting = false;
			s.at_hz = resume_hz;
			end_write();
			tuned_ms = now_ms;
			return s.at_hz;
		}
		if (t == s.at_hz) {
			probe_here(u.run_lvl, now_ms);
			end_write();
			*hit = true;
			return 0;
		}
		stage = ST_AIM;
		s.at_hz = t;
		end_write();
		tuned_ms = now_ms;
		return t;
		}
	}
	if (!strong && stage == ST_CONFIRM && prev_hz != 0) {
		/* Strong at once, quiet settled: the step before, read quickly, may be where the signal is */
		back_from = s.at_hz;
		s.at_hz = prev_hz;
		prev_hz = 0;
		stage = ST_BACK;
		end_write();
		tuned_ms = now_ms;
		return s.at_hz;
	}
	if (stage == ST_BACK)
		s.at_hz = back_from;	/* nothing there either: on from where the look back began */
	prev_hz = stage == ST_QUICK ? s.at_hz : 0u;
	stage = ST_QUICK;
	s.at_hz = advance(now_ms);
	step_note(now_ms);
	end_write();
	tuned_ms = now_ms;
	return s.at_hz;
}
#endif

uint32_t p25s_sweep(int16_t rssi_dbm, uint32_t now_ms, bool *hit)
{
	*hit = false;
	if (s.phase != P25S_SWEEP)
		return 0;
	if (reads != reads_seen) {
		reads_seen = reads;
		read_ms = now_ms;
	} else if (now_ms - read_ms >= P25S_IDLE_MS) {
		p25s_stop();	/* nobody looks: the radio task tunes the channel again */
		return 0;
	}
#if CONFIG_SEARCH_FAST
	if (now_ms - tuned_ms < (stage == ST_QUICK ? P25S_QUICK_MS : P25S_SETTLE_MS))
		return 0;
#else
	if (now_ms - tuned_ms < P25S_SETTLE_MS)
		return 0;
#endif
	if (rssi_dbm == 0)
		rssi_dbm = -140;
	int16_t floor = (int16_t)(floor_q4 / 16);
	bool strong = rssi_dbm >= P25S_HIT_MIN_DBM && rssi_dbm - floor >= P25S_HIT_DB;
#if CONFIG_SEARCH_FAST
	if (strong && stage == ST_QUICK) {
		stage = ST_CONFIRM;	/* read again settled, on the same step (no retune) */
		return 0;
	}
#endif
#if CONFIG_SEARCH_SURE
	return sure_step(strong, rssi_dbm, now_ms, hit);
#endif
	begin_write();
	if (!strong) {
		floor_q4 += ((int32_t)rssi_dbm * 16 - floor_q4) / 8;	/* only quiet steps move the floor */
		s.floor_dbm = (int16_t)(floor_q4 / 16);
	}
	if (strong && !skipped(s.at_hz, now_ms)) {
		s.phase = P25S_PROBE;
		s.hits++;
		held = false;	/* a new probe: no entry of it yet */
#if CONFIG_SEARCH_FAST
		stage = ST_QUICK;
		prev_hz = 0;
#endif
		end_write();
		*hit = true;
		return 0;
	}
#if CONFIG_SEARCH_FAST
	if (!strong && stage == ST_CONFIRM && prev_hz != 0) {
		/* Strong at once, quiet settled: the step before, read quickly, may be where the signal is */
		back_from = s.at_hz;
		s.at_hz = prev_hz;
		prev_hz = 0;
		stage = ST_BACK;
		end_write();
		tuned_ms = now_ms;
		return s.at_hz;
	}
	if (stage == ST_BACK)
		s.at_hz = back_from;	/* nothing there either: on from where the look back began */
	prev_hz = stage == ST_QUICK ? s.at_hz : 0u;
	stage = ST_QUICK;
#endif
	s.at_hz = advance(now_ms);
	end_write();
	tuned_ms = now_ms;
	return s.at_hz;
}

/* The found list's key (p25search.h): frequency + NAC + talkgroup. An entry fits a report when the frequency is the
 * same and the NAC and talkgroup are the same or not known on one side; the same key: all three equal */
static bool fits(const p25s_found_t *f, uint16_t nac, uint16_t tg)
{
	return f->hz == s.at_hz && (f->nac == nac || f->nac == P25S_NAC_NONE || nac == P25S_NAC_NONE) &&
	       (f->tg == tg || f->tg == 0 || tg == 0);
}

/* Entry i to the top (newest first) */
static void to_front(unsigned i)
{
	p25s_found_t f = s.found[i];
	memmove(&s.found[1], &s.found[0], i * sizeof f);
	s.found[0] = f;
}

/* The entry a report goes to, moved to the top: the newest that fits (two entries of a frequency always differ in a NAC
 * or talkgroup both know, so a report knowing both fits one at most), else a new one on top (the oldest drops when the
 * list is full; none without a NAC). False: none */
static bool pick(uint16_t nac, uint16_t tg, bool *fresh)
{
	unsigned i = 0;
	while (i < s.found_n && !fits(&s.found[i], nac, tg))
		i++;
	if (i < s.found_n) {
		to_front(i);
		*fresh = false;
		return true;
	}
	if (nac == P25S_NAC_NONE)
		return false;
	if (s.found_n < P25S_FOUND_MAX)
		s.found_n++;
	memmove(&s.found[1], &s.found[0], (size_t)(s.found_n - 1u) * sizeof s.found[0]);
	memset(&s.found[0], 0, sizeof s.found[0]);
	s.found[0].hz = s.at_hz;
	s.found[0].nac = P25S_NAC_NONE;
	s.found[0].id = ++ids;
	*fresh = true;
	return true;
}

void p25s_probe_found(uint16_t nac, uint16_t tg, uint32_t src, uint32_t now_ms)
{
	if (s.phase != P25S_PROBE)
		return;
	if (nac > 0xFFFu)
		nac = P25S_NAC_NONE;	/* no NAC known (the receiver's "any" after a terminator or a damaged frame) */
	begin_write();
	if (!s.playing) {
		s.probes_p25++;
#if CONFIG_SEARCH_HYBRID
		sh_found(s.at_hz, now_ms);
#endif
	}
	s.playing = true;
	bool fresh = false, call, moved = false, undone = false;
	if (held && fits(&s.found[0], nac, tg)) {
		call = now_ms - s.found[0].heard_ms > P25S_CALL_GAP_MS;	/* the probe's entry (on top) */
	} else {
		/* Not held yet, or the report names another NAC / talkgroup than the held entry's: that key's entry. A call
		 * just counted on the held entry was this one's (its link control came late) */
		moved = held && credited && now_ms - credit_ms < P25S_MOVE_MS;
		if (!pick(nac, tg, &fresh)) {
			end_write();
			return;
		}
		undone = moved && s.found[1].calls;
		if (undone)
			s.found[1].calls--;	/* the held entry, one down now */
		held = true;
		credited = false;
		call = fresh || moved || s.found[0].heard_ms == 0 || now_ms - s.found[0].heard_ms > P25S_CALL_GAP_MS;
	}
	p25s_found_t *f = &s.found[0];
	if (f->nac == P25S_NAC_NONE)
		f->nac = nac;
	if (f->tg == 0)
		f->tg = tg;
	if (src)
		f->src = src;
	if (call) {
		f->calls++;
		if (!undone)
			calls_seq++;	/* a moved call (taken off the held entry) was counted when it began */
		credited = true;
		credit_ms = now_ms;
	}
	f->heard_ms = now_ms;
	end_write();
}

uint32_t p25s_probe_done(bool was_p25, uint32_t now_ms)
{
	if (s.phase != P25S_PROBE)
		return 0;
#if CONFIG_SEARCH_HYBRID
	sh_end(was_p25 ? SH_END_FOUND : p25s_carrier_gone(now_ms) ? SH_END_GONE : SH_END_NOSYNC, s.at_hz, now_ms);
	watching = false;
#endif
#if CONFIG_SEARCH_SURE
	if (!was_p25 && !p25s_carrier_gone(now_ms))	/* a carrier that went off the air may have been P25 */
#else
	if (!was_p25)
#endif
		add_skip(s.at_hz, now_ms);
	begin_write();
#if CONFIG_SEARCH_SURE
	if (sure_wide_floor(&u, &floor_q4))
		s.floor_dbm = (int16_t)(floor_q4 / 16);	/* a wide run's target probed: its level the floor */
	sure_set_skirt(&u, now_ms, s.sweeps);	/* the probed carrier's other steps are its skirt */
	step_ms0 = now_ms;
#endif
	s.playing = false;
	s.phase = P25S_SWEEP;
	held = false;
	if (revisiting) {
		revisiting = false;	/* a revisit's probe: the sweep goes on where it was */
		s.at_hz = resume_hz;
	} else {
		s.at_hz = next_step(s.at_hz, now_ms);
	}
	end_write();
#if CONFIG_SEARCH_FAST
	stage = ST_QUICK;
	prev_hz = 0;	/* the last reading was the probe's */
#endif
	tuned_ms = read_ms = now_ms;
	reads_seen = reads;
	epoch++;
	return s.at_hz;
}

void p25s_retuned(uint32_t now_ms)
{
	tuned_ms = now_ms;
#if CONFIG_SEARCH_FAST
	if (stage == ST_CONFIRM)
		stage = ST_QUICK;	/* read anew from the start */
#endif
}

#if CONFIG_SEARCH_SURE
void p25s_level(int16_t rssi_dbm, uint32_t now_ms)
{
	if (s.phase != P25S_PROBE || rssi_dbm == 0)
		return;
	int16_t floor = (int16_t)(floor_q4 / 16);
	if (rssi_dbm - floor >= P25S_HIT_DB / 2 && rssi_dbm >= P25S_HIT_MIN_DBM - P25S_HIT_DB / 2)
		on_ms = now_ms;
#if CONFIG_SEARCH_HYBRID
	else
		low_seen_any = true;
#endif
	level_known = true;
}

bool p25s_carrier_gone(uint32_t now_ms)
{
	return s.phase == P25S_PROBE && level_known && now_ms - on_ms >= SURE_GONE_MS;
}
#endif

uint32_t p25s_epoch(void)
{
	return epoch;
}

void p25s_stop(void)
{
#if CONFIG_SEARCH_HYBRID
	if (s.phase != P25S_OFF)
		sh_stop();	/* a probe stopped midway counts as gone (radio/searchhy.h) */
#endif
	begin_write();
	s.phase = P25S_OFF;
	s.playing = false;
	end_write();
}

bool p25s_on(void)
{
	return s.phase != P25S_OFF;
}

uint8_t p25s_phase(void)
{
	return s.phase;
}

uint32_t p25s_at_hz(void)
{
	return s.at_hz;
}

/* The PC's search log (debug link section, tools/plus_searchlog.py): the state and found entry i, read like the
 * snapshot but not counted as a look, so the search still stops once its screen was left. Small on the caller's stack
 * (the host link task): one entry at a time. False when i is past the list or the copy kept changing */
bool p25s_peek(unsigned i, p25s_peek_t *out)
{
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		out->phase = s.phase;
		out->at_hz = s.at_hz;
		out->lo_hz = s.lo_hz;
		out->hi_hz = s.hi_hz;
		out->found_n = s.found_n;
		out->seq = calls_seq;
		bool have = i < s.found_n && i < P25S_FOUND_MAX;
		if (have)
			memcpy(&out->f, &s.found[i], sizeof out->f);
		__asm__ volatile("" ::: "memory");
		if (seq == a)
			return have;
	}
	return false;
}

bool p25s_snapshot(p25s_view_t *out)
{
	reads++;
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		memcpy(out, &s, sizeof *out);
		__asm__ volatile("" ::: "memory");
		if (seq == a)
			return true;
	}
	return false;
}
#endif
