/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR search engine (dmrsearch.h). No RTOS, no hardware. A sibling of p25search.c (the same sweep, skip
 * list and revisits) kept separate so that either search can be built without the other. With CONFIG_ALL_SEARCH it is
 * the all digital search's engine too (a second kind: the probe goes through the modes in priority order). */
#include "config.h"
#if CONFIG_DMR_SEARCH
#include <string.h>
#include "radio/dmrsearch.h"
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

static dmrs_view_t s;
#if CONFIG_ALL_SEARCH
/* The modes a hit is probed in, the owner's priority order (the first confirmed wins); those not built left out */
static const uint8_t order[] = { DMRS_MODE_P25, DMRS_MODE_DMR,
#if CONFIG_YSF
				 DMRS_MODE_YSF,
#endif
#if CONFIG_NXDN
				 DMRS_MODE_NXDN,
#endif
};
static uint8_t tried;		/* 1 << mode of the modes probed on the frequency held now */
static uint8_t epoch;		/* dmrs_regained */
#endif
static uint32_t tuned_ms;
static int32_t floor_q4;		/* floor in dBm x 16 */
static int16_t peak;			/* the step's strongest reading so far */
static uint8_t nreads;			/* ... and how many were taken */
static struct { uint32_t hz, until_ms; } skip[DMRS_SKIP_MAX];
static volatile uint32_t seq;		/* odd while written (snapshot retries) */
/* The PC's search log (searchlog_view.c): entry serials and the calls counted since power on */
static uint8_t ids;
static uint16_t calls_seq;
_Static_assert(sizeof(dmrs_found_t) == 24u, "the found entry's serial fits in the mode's byte");
static volatile uint32_t reads;		/* snapshots taken: the screen still looks */
static uint32_t reads_seen, read_ms;
/* The probe of the frequency held now: since when, DMR heard there yet, a call playing at the last look, the end of
 * the last call (the hang runs from it) */
static uint32_t hit_ms, end_ms;
static bool heard_here, was_call, ended;
/* Revisits (as P25 search): every DMRS_REVISIT_STEPS sweep steps one frequency of the found list that lies in the
 * range is read between two steps, round robin, so a new call there is caught in about two seconds instead of a
 * whole pass; the sweep then goes on where it was */
static uint32_t resume_hz;
static uint8_t since_revisit, revisit_i;
static bool revisiting;
/* The found list's key (note): found[0] is the entry of the probe held now (held), a call counted there and when */
static bool held, credited;
static uint32_t credit_ms;
#if CONFIG_SEARCH_FAST
/* The faster sweep (DMRS_QUICK_MS): the step's stage, a strong first reading, the step before (read the quick way) and
 * the step a look back came from */
enum { ST_QUICK, ST_MORE, ST_BACK, ST_AIM };
static uint8_t stage;
static bool maybe, peaked;
static uint32_t prev_hz, back_from;
#endif
#if CONFIG_SEARCH_SURE
/* The surer search (radio/searchsure.h): the floor, the run being followed and the last probe's skirt; ST_AIM reads
 * the run's middle before it is probed. low: the step's weakest settled reading, tdma_hint: the hit's readings rose
 * and fell as a direct mode DMR carrier's (the all digital search probes DMR first then). The probe's readings
 * (dmrs_heard_t.rssi_dbm): a level heard, when it was last on, ups (rises from a gap: a TDMA carrier), something of
 * a mode heard (some: never skipped). step_q4: a sweep step's time (ms x 16, the screen's steps a second) */
static sure_t u;
static int16_t low;
static uint8_t nset;		/* the step's settled readings */
static bool tdma_hint, level_known, low_seen, tdma, some;
static uint8_t ups;
static uint32_t on_ms, step_ms0, step_q4;
#endif
#if CONFIG_SEARCH_HYBRID
static bool low_seen_any;	/* the probe read a gap (under the hit's half margin) at least once */
#endif
#if CONFIG_SEARCH_HYBRID
/* The faster search (radio/searchhy.h, dmrsearch.h DMRS_SAMPLE_MS): the step's samples on the radio task's 10 ms tick
 * and its polls from DMRS_SAMPLE_MS after the retune (their peak, the first and last one's ms after the retune, the
 * longest gap between two, how many); the revisit read now is a watched carrier's */
static int16_t fpeak, sup_lvl;
static uint8_t fn, ffirst, flast, fgap;
static bool watching;
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
	for (unsigned i = 0; i < DMRS_SKIP_MAX; i++)
		if (same_hz(skip[i].hz, hz) && (int32_t)(skip[i].until_ms - now_ms) > 0)
			return true;
	return false;
}

static void add_skip(uint32_t hz, uint32_t now_ms)
{
#if CONFIG_SEARCH_FAST
	/* Its own entry (running or run out: a repeat gets the long skip), else one run out, else the one ending first */
	unsigned pick = DMRS_SKIP_MAX;
	for (unsigned i = 0; i < DMRS_SKIP_MAX && pick == DMRS_SKIP_MAX; i++)
		if (same_hz(skip[i].hz, hz))
			pick = i;
	bool again = pick < DMRS_SKIP_MAX && (int32_t)(now_ms - skip[pick].until_ms) < (int32_t)DMRS_AGAIN_MS;
	for (unsigned i = 0; i < DMRS_SKIP_MAX && pick == DMRS_SKIP_MAX; i++)
		if ((int32_t)(skip[i].until_ms - now_ms) <= 0)
			pick = i;
	if (pick == DMRS_SKIP_MAX) {
		pick = 0;
		for (unsigned i = 1; i < DMRS_SKIP_MAX; i++)
			if ((int32_t)(skip[i].until_ms - skip[pick].until_ms) < 0)
				pick = i;
	}
	skip[pick].hz = hz;
#if CONFIG_SEARCH_HYBRID
	/* fast: a steady carrier none of the search's modes, again on a later pass (a busy FM carrier, a beacon), is
	 * skipped for minutes; one whose level rose and fell (a direct mode DMR carrier's TDMA, a level that came and went)
	 * keeps classic's (radio/searchhy.h sh_reject) */
	bool steady = !tdma && !tdma_hint && !some && !low_seen_any;
	bool longer = sh_reject(hz, s.sweeps, steady) && again;
	skip[pick].until_ms = now_ms + (longer ? SH_REJECT_MS : again ? DMRS_SKIP_LONG_MS : DMRS_SKIP_MS);
#else
	skip[pick].until_ms = now_ms + (again ? DMRS_SKIP_LONG_MS : DMRS_SKIP_MS);
#endif
}
#else
	unsigned pick = 0;
	for (unsigned i = 0; i < DMRS_SKIP_MAX; i++) {
		if (skip[i].hz == hz || (int32_t)(skip[i].until_ms - now_ms) <= 0) {
			pick = i;
			break;
		}
		if ((int32_t)(skip[i].until_ms - skip[pick].until_ms) < 0)
			pick = i;	/* full: replace the one expiring first */
	}
	skip[pick].hz = hz;
	skip[pick].until_ms = now_ms + DMRS_SKIP_MS;
}
#endif

/* The next found frequency to read again (round robin), 0 = none in the range */
static uint32_t revisit_pick(uint32_t now_ms)
{
	for (unsigned k = 0; k < s.found_n; k++) {
		revisit_i = (uint8_t)((revisit_i + 1u) % s.found_n);
		uint32_t hz = s.found[revisit_i].hz;
		bool again = false;	/* a frequency with more entries (modes, codes, groups) is read once a round */
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
	 * DMRS_REVISIT_STEPS steps), and in a found list revisit's place when the list has none in the range */
	if (sh_fast() && since_revisit + 1u == DMRS_REVISIT_STEPS / 2u) {
		uint32_t w = sh_watch_pick(s.lo_hz, s.hi_hz, now_ms, skipped);
		if (w != 0 && w != n) {
			since_revisit++;
			resume_hz = n;
			revisiting = watching = true;
			return w;
		}
	}
#endif
	if (++since_revisit >= DMRS_REVISIT_STEPS) {
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

/* A step is tuned now: its readings start after the settle */
static void tuned(uint32_t now_ms)
{
	tuned_ms = now_ms;
	nreads = 0;
#if CONFIG_SEARCH_FAST
	maybe = peaked = false;
#endif
#if CONFIG_SEARCH_SURE
	nset = 0;
#endif
#if CONFIG_SEARCH_HYBRID
	fn = 0;
#endif
}

static uint32_t start(uint8_t kind, uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms)
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
	if (s.kind != kind)
		s.found_n = 0;	/* the other kind's finds: not this search's */
	s.kind = kind;
	s.mode = DMRS_MODE_DMR;
	s.phase = DMRS_SWEEP;	/* the found list outlives a restart (a new range) */
	s.playing = false;
	s.floor_dbm = -125;
	s.lo_hz = lo_hz;
	s.hi_hz = hi_hz;
	s.step_hz = step_hz ? step_hz : 12500u;
	s.at_hz = lo_hz;
	s.sweeps = s.hits = s.found_dmr = 0;
	end_write();
#if CONFIG_SEARCH_HYBRID
	sh_start(kind == DMRS_KIND_DMR ? SH_DMR : SH_ALL, now_ms);
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
	tdma_hint = false;
	s.step_ms = 0;
	step_q4 = 0;
	step_ms0 = now_ms;
#endif
	read_ms = now_ms;
	reads_seen = reads;
	tuned(now_ms);
	return lo_hz;
}

uint32_t dmrs_start(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms)
{
	return start(DMRS_KIND_DMR, lo_hz, hi_hz, step_hz, now_ms);
}

#if CONFIG_ALL_SEARCH
uint32_t dmrs_start_all(uint32_t lo_hz, uint32_t hi_hz, uint32_t step_hz, uint32_t now_ms)
{
	return start(DMRS_KIND_ALL, lo_hz, hi_hz, step_hz, now_ms);
}

/* The next mode to probe on the frequency held now (DMRS_MODES: none left); it counts as tried */
static uint8_t next_mode(void)
{
	for (unsigned i = 0; i < sizeof order; i++) {
		if (!(tried & (1u << order[i]))) {
			tried |= (uint8_t)(1u << order[i]);
			return order[i];
		}
	}
	return DMRS_MODES;
}
#endif

/* A hit's first mode: the DMR search's is DMR; the all digital search tries the mode a find there had first (a
 * revisit), else the priority order */
static uint8_t first_mode(void)
{
#if CONFIG_ALL_SEARCH
	tried = 0;
	if (s.kind != DMRS_KIND_ALL)
		return DMRS_MODE_DMR;
	for (unsigned i = 0; i < s.found_n; i++) {
		if (s.found[i].hz == s.at_hz) {
			tried = (uint8_t)(1u << s.found[i].mode);
			return s.found[i].mode;
		}
	}
#if CONFIG_SEARCH_SURE
	if (tdma_hint) {
		tried = (uint8_t)(1u << DMRS_MODE_DMR);	/* rose and fell as a direct mode DMR carrier: P25 can't be it */
		return DMRS_MODE_DMR;
	}
#endif
	return next_mode();
#else
	return DMRS_MODE_DMR;
#endif
}

/* How long the probe of the mode held now waits for it */
static uint32_t window_ms(void)
{
#if CONFIG_ALL_SEARCH
	if (s.mode == DMRS_MODE_P25)
		return DMRS_P25_PROBE_MS;
	if (s.mode == DMRS_MODE_YSF)
		return DMRS_YSF_PROBE_MS;
	if (s.mode == DMRS_MODE_NXDN)
		return DMRS_NXDN_PROBE_MS;
#endif
#if CONFIG_SEARCH_SURE
	if (tdma || tdma_hint || some)
		return DMRS_PROBE_LONG_MS;	/* a direct mode carrier, or DMR heard: its sync comes once a superframe */
#endif
	return DMRS_PROBE_MS;
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

/* The frequency held now is a hit: the probe starts (within begin_write). lvl: its level */
static void probe_here(int16_t lvl, uint32_t now_ms)
{
	if (!u.climbing) {
		u.run_lo = u.run_hi = s.at_hz;	/* a hit without a run (a revisit, the range's top): its own skirt */
		u.run_lvl = lvl;
	}
	u.climbing = false;
	tdma_hint = peaked && lvl >= DMRS_HIT_MIN_DBM && lvl - low >= 15;
	s.phase = DMRS_PROBE;
	s.mode = first_mode();
	s.hits++;
	held = false;	/* a new probe: no entry of it yet */
	stage = ST_QUICK;
	prev_hz = 0;
	hit_ms = on_ms = now_ms;
	heard_here = was_call = ended = false;
	level_known = low_seen = tdma = some = false;
	ups = 0;
#if CONFIG_SEARCH_HYBRID
	sh_probe(watching, now_ms);
	watching = false;
	low_seen_any = false;
#endif
}

/* A step's readings are complete (strong against the floor; its level the peak): follow a run, probe its middle, or
 * go on (radio/searchsure.h) */
static uint32_t sure_step(bool strong, uint32_t now_ms, bool *hit)
{
	int16_t lvl = peak;
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
		/* nothing there now (the over ended): on past the run, nothing skipped */
		u.climbing = false;
#if CONFIG_SEARCH_HYBRID
		sh_unsettled(s.at_hz, now_ms);	/* fast: read again between steps for a while */
#endif
		if (sure_wide_floor(&u, &floor_q4))
			s.floor_dbm = (int16_t)(floor_q4 / 16);
		revisiting = false;
		s.at_hz = resume_hz;
		end_write();
		tuned(now_ms);
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
			tuned(now_ms);
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
			sure_set_skirt(&u, now_ms, s.sweeps);	/* a carrier known not to be ours: its steps passed over */
			revisiting = false;
			s.at_hz = resume_hz;
			end_write();
			tuned(now_ms);
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
		tuned(now_ms);
		return t;
		}
	}
	if (stage == ST_BACK)
		s.at_hz = back_from;	/* nothing there either: on from where the look back began */
	prev_hz = stage != ST_BACK && !strong ? s.at_hz : 0u;	/* a strong (skipped) step is known: no look back to it */
	stage = ST_QUICK;
	s.at_hz = advance(now_ms);
	step_note(now_ms);
	end_write();
	tuned(now_ms);
	return s.at_hz;
}

/* A probe's reading (dmrs_heard_t.rssi_dbm): on (the hit's half margin) or in a gap; rises from gaps: TDMA */
static void level_note(int16_t lvl, uint32_t now_ms)
{
	int16_t floor = (int16_t)(floor_q4 / 16);
	bool on = lvl - floor >= DMRS_HIT_DB / 2 && lvl >= DMRS_HIT_MIN_DBM - DMRS_HIT_DB / 2;
	if (on) {
		on_ms = now_ms;
		if (low_seen && ups < 255u && ++ups >= 2u)
			tdma = true;
		low_seen = false;
	} else {
		low_seen = true;
	}
	level_known = true;
#if CONFIG_SEARCH_HYBRID
	if (!on)
		low_seen_any = true;
#endif
}
#endif

#if CONFIG_SEARCH_HYBRID
bool dmrs_sampling(uint32_t now_ms)
{
	return sh_fast() && s.phase == DMRS_SWEEP && stage == ST_QUICK && now_ms - tuned_ms >= DMRS_SAMPLE_MS &&
	       now_ms - tuned_ms < 250u;
}

void dmrs_sample(int16_t rssi_dbm, uint32_t now_ms)
{
	if (!dmrs_sampling(now_ms))
		return;
	if (rssi_dbm == 0)
		rssi_dbm = -140;
	uint8_t at = (uint8_t)(now_ms - tuned_ms);
	if (fn == 0 || rssi_dbm > fpeak)
		fpeak = rssi_dbm;
	if (fn == 0) {
		ffirst = at;
		fgap = 0;
	} else if ((uint8_t)(at - flast) > fgap) {
		fgap = (uint8_t)(at - flast);
	}
	flast = at;
	if (fn < 255u)
		fn++;
}
#endif

uint32_t dmrs_sweep(int16_t rssi_dbm, uint32_t now_ms, bool *hit)
{
	*hit = false;
	if (s.phase != DMRS_SWEEP)
		return 0;
	if (reads != reads_seen) {
		reads_seen = reads;
		read_ms = now_ms;
	} else if (now_ms - read_ms >= DMRS_IDLE_MS) {
		dmrs_stop();	/* nobody looks: the radio task tunes the channel again */
		return 0;
	}
#if CONFIG_SEARCH_FAST
	if (now_ms - tuned_ms < (stage == ST_BACK || stage == ST_AIM ? DMRS_SETTLE_MS : DMRS_QUICK_MS))
		return 0;
#else
	if (now_ms - tuned_ms < DMRS_SETTLE_MS)
		return 0;
#endif
	if (rssi_dbm == 0)
		rssi_dbm = -140;
	int16_t floor = (int16_t)(floor_q4 / 16);
#if CONFIG_SEARCH_HYBRID
	if (sh_fast() && stage == ST_QUICK) {
		/* fast: the samples since DMRS_SAMPLE_MS (the 10 ms tick's and the polls') cover DMRS_SPAN_MS and none came
		 * near a hit (half its margin, as the quick reading): a quiet step, two polls instead of three. Anything
		 * else: the readings below, as classic */
		dmrs_sample(rssi_dbm, now_ms);
		if (fn != 0 && (unsigned)(flast - ffirst) >= DMRS_SPAN_MS && fgap <= DMRS_GAP_MS &&
		    !(fpeak >= DMRS_HIT_MIN_DBM - DMRS_HIT_DB / 2 && fpeak - floor >= DMRS_HIT_DB / 2)) {
			peak = low = fpeak;
			return sure_step(false, now_ms, hit);
		}
	}
#endif
#if CONFIG_SEARCH_FAST
	if (now_ms - tuned_ms < DMRS_SETTLE_MS) {
		/* The quick reading: it only marks the step for more readings, it never makes a hit by itself. Half the hit's
		 * margin: a burst that began just before it reads only part of its level yet */
		maybe = rssi_dbm >= DMRS_HIT_MIN_DBM - DMRS_HIT_DB / 2 && rssi_dbm - floor >= DMRS_HIT_DB / 2;
		nreads++;
		return 0;
	}
	if (!peaked || rssi_dbm > peak)
		peak = rssi_dbm;	/* the settled readings' peak */
#if CONFIG_SEARCH_SURE
	if (!peaked || rssi_dbm < low)
		low = rssi_dbm;		/* ... and their weakest */
	nset++;
#endif
	peaked = true;
#else
	if (nreads == 0 || rssi_dbm > peak)
		peak = rssi_dbm;
#endif
	nreads++;
	bool strong = peak >= DMRS_HIT_MIN_DBM && peak - floor >= DMRS_HIT_DB;
#if CONFIG_SEARCH_SURE
	if ((stage == ST_AIM || strong) && nset < DMRS_READS)
		return 0;	/* a strong step and the run's middle: all their settled readings, so the run compares the
				 * peaks of whole frames (a direct mode burst caught or half caught) and the gaps show */
#endif
#if CONFIG_SEARCH_FAST
	if (!strong && nreads < DMRS_READS + (stage == ST_MORE ? DMRS_MORE_READS : 0u))
		return 0;	/* a direct mode burst may still come in the other half of the frame */
	if (!strong && maybe && stage == ST_QUICK) {
		stage = ST_MORE;	/* only the quick reading was strong: more settled readings */
		return 0;
	}
	if (!strong && stage == ST_MORE && prev_hz != 0) {
		/* Nothing settled here: the step before, read the quick way, may be where the signal is */
		begin_write();
		back_from = s.at_hz;
		s.at_hz = prev_hz;
		prev_hz = 0;
		stage = ST_BACK;
		end_write();
		tuned(now_ms);
		return s.at_hz;
	}
#else
	if (!strong && nreads < DMRS_READS)
		return 0;	/* a direct mode burst may still come in the other half of the frame */
#endif
#if CONFIG_SEARCH_SURE
	return sure_step(strong, now_ms, hit);
#endif
	begin_write();
	if (!strong) {
		floor_q4 += ((int32_t)peak * 16 - floor_q4) / 8;	/* only quiet steps move the floor */
		s.floor_dbm = (int16_t)(floor_q4 / 16);
	}
	if (strong && !skipped(s.at_hz, now_ms)) {
		s.phase = DMRS_PROBE;
		s.mode = first_mode();
		s.hits++;
		held = false;	/* a new probe: no entry of it yet */
#if CONFIG_SEARCH_FAST
		stage = ST_QUICK;
		prev_hz = 0;
#endif
		end_write();
		hit_ms = now_ms;
		heard_here = was_call = ended = false;
		*hit = true;
		return 0;
	}
#if CONFIG_SEARCH_FAST
	if (stage == ST_BACK)
		s.at_hz = back_from;	/* nothing there either: on from where the look back began */
	prev_hz = stage != ST_BACK && !strong ? s.at_hz : 0u;	/* a strong (skipped) step is known: no look back to it */
	stage = ST_QUICK;
#endif
	s.at_hz = advance(now_ms);
	end_write();
	tuned(now_ms);
	return s.at_hz;
}

/* The found list's key (dmrsearch.h): frequency + mode + code (P25 NAC, DMR color code, YSF DG-ID, NXDN RAN) + group
 * (the talkgroup or called ID; YSF none: its dst / src hold the callsign). The group of an entry or a report */
static uint32_t group_of(uint8_t mode, uint32_t dst)
{
	return mode == DMRS_MODE_YSF ? 0u : dst;
}

/* An entry fits a report when frequency and mode are the same and code and group the same or not known on one side */
static bool fits(const dmrs_found_t *f, uint16_t cc, uint32_t grp)
{
	uint32_t g = group_of(f->mode, f->dst);
	return f->hz == s.at_hz && f->mode == s.mode && (f->cc == cc || f->cc == DMRS_CC_NONE || cc == DMRS_CC_NONE) &&
	       (g == grp || g == 0 || grp == 0);
}

/* The entry a report goes to, moved to the top: the newest that fits (two entries of a frequency and mode always differ
 * in a code or group both know, so a report knowing both fits one at most), else a new one on top (the oldest drops
 * when the list is full). True when it is new */
static bool pick(uint16_t cc, uint32_t grp)
{
	unsigned i = 0;
	while (i < s.found_n && !fits(&s.found[i], cc, grp))
		i++;
	dmrs_found_t f;
	bool fresh = i >= s.found_n;
	if (!fresh) {
		f = s.found[i];
	} else {
		memset(&f, 0, sizeof f);
		f.hz = s.at_hz;
		f.cc = DMRS_CC_NONE;
		f.mode = s.mode;
		f.id = ++ids;
		if (s.found_n < DMRS_FOUND_MAX)
			s.found_n++;
		i = s.found_n - 1u;	/* the oldest drops when full */
	}
	memmove(&s.found[1], &s.found[0], i * sizeof f);
	s.found[0] = f;
	return fresh;
}

/* The frequency held now into the found list, with what was heard there (owner 2026-11-21: ONE entry per frequency +
 * mode + code + group with a call counter, the newest on top; a mixed mode repeater has an entry for each mode, a
 * frequency with two talkgroups one for each). A report that doesn't know the code or the group yet updates the entry
 * this probe holds (found[0]), or the newest that fits; a call counted on the held entry whose group then turns out
 * another within DMRS_MOVE_MS (its link control came late) moves to the right entry */
static void note(const dmrs_heard_t *h, bool new_call, uint32_t now_ms)
{
	uint32_t grp = group_of(s.mode, h->dst);
	bool moved = false, undone = false;
	if (!held || !fits(&s.found[0], h->cc, grp)) {
		moved = held && credited && now_ms - credit_ms < DMRS_MOVE_MS;
		bool fresh = pick(h->cc, grp);
		undone = moved && s.found[1].calls;
		if (undone)
			s.found[1].calls--;	/* the held entry, one down now */
		/* the call moves with its key; a key change in a call playing is a call there too */
		if (moved || (held && h->call && !new_call && (fresh || now_ms - s.found[0].heard_ms > DMRS_HANG_MS)))
			new_call = true;
		held = true;
		credited = false;
	}
	dmrs_found_t *f = &s.found[0];
	if (h->cc != DMRS_CC_NONE)
		f->cc = h->cc;
	f->ts_mask |= h->ts_mask;
	f->sync_mask |= h->sync_mask;
#if CONFIG_ALL_SEARCH
	if (s.mode == DMRS_MODE_YSF) {
		if (h->src != 0) {	/* a callsign: both halves are its (a shorter one leaves no tail of the last) */
			f->src = h->src;
			f->dst = h->dst;
		}
	} else
#endif
	{
		if (h->dst != 0) {
			f->dst = h->dst;
			f->group = h->group;
		}
		if (h->src != 0)
			f->src = h->src;
	}
	if (new_call) {
		f->calls++;
		if (!undone)
			calls_seq++;	/* a moved call (taken off the held entry) was counted when it began */
		credited = true;
		credit_ms = now_ms;
	}
	if (h->other)
		f->enc = true;
	else if (h->call)
		f->enc = false;
	f->heard_ms = now_ms;
}

/* Back to sweeping: where the sweep was before a revisit, else the next step */
static uint32_t resume(uint32_t now_ms)
{
	s.phase = DMRS_SWEEP;
	s.playing = false;
	held = false;
#if CONFIG_SEARCH_SURE
	if (sure_wide_floor(&u, &floor_q4))
		s.floor_dbm = (int16_t)(floor_q4 / 16);	/* a wide run's target probed: its level the floor */
	sure_set_skirt(&u, now_ms, s.sweeps);	/* the probed carrier's other steps are its skirt */
	step_ms0 = now_ms;
#endif
#if CONFIG_SEARCH_FAST
	stage = ST_QUICK;
	prev_hz = 0;	/* the last readings were the probe's */
#endif
	if (revisiting) {
		revisiting = false;
		s.at_hz = resume_hz;
	} else {
		s.at_hz = next_step(s.at_hz, now_ms);
	}
	read_ms = now_ms;
	reads_seen = reads;
	tuned(now_ms);
	return s.at_hz;
}

uint32_t dmrs_probe(const dmrs_heard_t *h, uint32_t now_ms)
{
	if (s.phase != DMRS_PROBE && s.phase != DMRS_CALL)
		return 0;
	bool dmr = h->bursts >= DMRS_MIN_BURSTS || h->call || h->other;
	uint32_t hz = 0;
	begin_write();
#if CONFIG_SEARCH_SURE
#if CONFIG_SEARCH_HYBRID
	if ((h->bursts != 0 || dmr) && !some)
		sh_sync(now_ms - hit_ms);	/* the first frame since the mode's window began */
#endif
	if (h->bursts != 0 || dmr)
		some = true;	/* frames of the mode probed (one is not enough to name it): this frequency is never skipped */
	if (h->rssi_dbm != 0)
		level_note(h->rssi_dbm, now_ms);
#endif
	if (dmr) {
		if (!heard_here) {
			heard_here = true;
			s.found_dmr++;
#if CONFIG_SEARCH_HYBRID
			sh_found(s.at_hz, now_ms);
#endif
		}
		note(h, h->call && !was_call, now_ms);
	}
	was_call = h->call;
	if (h->call) {
		s.phase = DMRS_CALL;	/* it plays (the call stack hears every talkgroup); held while it lasts */
		s.playing = true;
		ended = false;
	} else {
		s.playing = false;
		bool leave;
		if (s.phase == DMRS_CALL) {
			if (!ended) {
				ended = true;
				end_ms = now_ms;
			}
			leave = now_ms - end_ms >= DMRS_HANG_MS;	/* a reply in the hang plays too */
#if CONFIG_SEARCH_HYBRID
			if (leave)
				sh_end(SH_END_FOUND, s.at_hz, now_ms);
#endif
		} else if (!heard_here) {
#if CONFIG_SEARCH_SURE
			/* The carrier went off the air (its level under the hit's half margin for SURE_GONE_MS): the over ended
			 * before the mode was heard. On at once, nothing skipped, no other mode tried */
			bool gone = level_known && now_ms - on_ms >= SURE_GONE_MS;
			leave = gone || now_ms - hit_ms >= window_ms();
#else
			const bool gone = false;
			(void)gone;
			leave = now_ms - hit_ms >= window_ms();
#endif
#if CONFIG_ALL_SEARCH
			if (leave && !gone && s.kind == DMRS_KIND_ALL) {
				uint8_t m = next_mode();
				if (m != DMRS_MODES) {
					s.mode = m;	/* not this mode: the next one, on the same frequency */
					held = false;
					hit_ms = now_ms;
					was_call = ended = false;
#if CONFIG_SEARCH_SURE
					on_ms = now_ms;	/* that mode's receiver reads anew */
					level_known = low_seen = false;
#endif
					end_write();
					return s.at_hz;	/* the receiver is set up for it */
				}
			}
#endif
#if CONFIG_SEARCH_HYBRID
			if (leave)
				sh_end(gone ? SH_END_GONE : some ? SH_END_PARTIAL : SH_END_NOSYNC, s.at_hz, now_ms);
#endif
#if CONFIG_SEARCH_SURE
			if (leave && !gone && !some)
#else
			if (leave)
#endif
				add_skip(s.at_hz, now_ms);	/* a carrier, not DMR (FM, P25, data of another kind); the all
								 * digital search: none of its modes (FM, a beacon, data) */
		} else {
			leave = now_ms - hit_ms >= DMRS_LATE_MS;	/* DMR, nothing to play: idle, data, encrypted */
#if CONFIG_SEARCH_HYBRID
			if (leave)
				sh_end(SH_END_FOUND, s.at_hz, now_ms);
#endif
		}
		if (leave)
			hz = resume(now_ms);
	}
	end_write();
	return hz;
}

void dmrs_stop(void)
{
#if CONFIG_SEARCH_HYBRID
	if (s.phase != DMRS_OFF)
		sh_stop();	/* a probe stopped midway counts as gone (radio/searchhy.h) */
#endif
	begin_write();
	s.phase = DMRS_OFF;
	s.playing = false;
	end_write();
}

bool dmrs_on(void)
{
	return s.phase != DMRS_OFF;
}

uint8_t dmrs_phase(void)
{
	return s.phase;
}

uint32_t dmrs_at_hz(void)
{
	return s.at_hz;
}

/* The PC's search log (debug link section, tools/plus_searchlog.py): the state and found entry i, read like the
 * snapshot but not counted as a look, so the search still stops once its screen was left. Small on the caller's stack
 * (the host link task): one entry at a time. False when i is past the list or the copy kept changing. Another kind's
 * state reads as stopped with nothing found */
static bool peek(uint8_t kind, unsigned i, dmrs_peek_t *out)
{
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		bool mine = s.kind == kind;
		out->phase = mine ? s.phase : (uint8_t)DMRS_OFF;
		out->mode = s.mode;
		out->at_hz = s.at_hz;
		out->lo_hz = s.lo_hz;
		out->hi_hz = s.hi_hz;
		out->found_n = mine ? s.found_n : 0u;
		out->seq = calls_seq;
		bool have = i < out->found_n && i < DMRS_FOUND_MAX;
		if (have)
			memcpy(&out->f, &s.found[i], sizeof out->f);
		__asm__ volatile("" ::: "memory");
		if (seq == a)
			return have;
	}
	return false;
}

bool dmrs_peek(unsigned i, dmrs_peek_t *out)
{
	return peek(DMRS_KIND_DMR, i, out);
}

/* A consistent copy; a look only when it is the kind running (another kind's screen keeps nothing alive) */
static bool snapshot(uint8_t kind, dmrs_view_t *out)
{
	if (s.kind == kind)
		reads++;
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		memcpy(out, &s, sizeof *out);
		__asm__ volatile("" ::: "memory");
		if (seq == a) {
			if (out->kind != kind) {
				out->phase = DMRS_OFF;	/* the other kind's: stopped, nothing found */
				out->playing = false;
				out->found_n = 0;
				out->kind = kind;
			}
			return true;
		}
	}
	return false;
}

bool dmrs_snapshot(dmrs_view_t *out)
{
	return snapshot(DMRS_KIND_DMR, out);
}

#if CONFIG_ALL_SEARCH
bool dmrs_snapshot_all(dmrs_view_t *out)
{
	return snapshot(DMRS_KIND_ALL, out);
}

bool dmrs_peek_all(unsigned i, dmrs_peek_t *out)
{
	return peek(DMRS_KIND_ALL, i, out);
}

uint8_t dmrs_kind(void)
{
	return s.kind;
}

uint8_t dmrs_mode(void)
{
	return s.mode;
}

void dmrs_retuned(uint32_t now_ms)
{
	if (s.phase == DMRS_SWEEP) {
		tuned(now_ms);	/* the step settles anew, read from its start */
#if CONFIG_SEARCH_FAST
#if CONFIG_SEARCH_SURE
		if (stage != ST_AIM)	/* the run's middle stays aimed at */
#endif
		stage = ST_QUICK;
		prev_hz = 0;
#endif
	} else {
		hit_ms = now_ms;	/* the probe's window (or the late wait) from now */
#if CONFIG_SEARCH_SURE
		on_ms = now_ms;
#endif
	}
}

void dmrs_regained(void)
{
	epoch++;
}

uint8_t dmrs_epoch(void)
{
	return epoch;
}

uint32_t dmrs_native_hz(bool *ysf)
{
	bool probing = s.kind == DMRS_KIND_ALL && (s.phase == DMRS_PROBE || s.phase == DMRS_CALL);
	*ysf = s.mode == DMRS_MODE_YSF;
	return probing && (s.mode == DMRS_MODE_P25 || *ysf) ? s.at_hz : 0u;
}

uint32_t dmrs_nxdn_hz(void)
{
	bool probing = s.kind == DMRS_KIND_ALL && (s.phase == DMRS_PROBE || s.phase == DMRS_CALL);
	return probing && s.mode == DMRS_MODE_NXDN ? s.at_hz : 0u;
}

/* Base 40: space, A-Z, 0-9, '/', '-', '.' */
static const char call_set[41] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/-.";

static uint32_t call_part(const char *call, unsigned from, unsigned n, bool *done)
{
	uint32_t v = 0;
	for (unsigned k = 0; k < n; k++) {
		char c = *done ? '\0' : call[from + k];
		if (c == '\0')
			*done = true;
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		unsigned d = 39;	/* '.': a character the set lacks */
		if (*done)
			d = 0;
		else
			for (unsigned j = 0; j < 40u; j++)
				if (call_set[j] == c)
					d = j;
		v = v * 40u + d;
	}
	return v;
}

void dmrs_call_pack(const char *call, uint32_t *hi, uint32_t *lo)
{
	bool done = call == NULL;
	*lo = call_part(call, 0, 6, &done);
	*hi = call_part(call, 6, 4, &done);
}

void dmrs_call_unpack(uint32_t hi, uint32_t lo, char out[11])
{
	for (int k = 5; k >= 0; k--) {
		out[k] = call_set[lo % 40u];
		lo /= 40u;
	}
	for (int k = 9; k >= 6; k--) {
		out[k] = call_set[hi % 40u];
		hi /= 40u;
	}
	int n = 10;
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

const char *dmrs_mode_name(uint8_t mode)
{
	static const char names[DMRS_MODES][5] = { "DMR", "P25", "YSF", "NXDN" };
	return mode < DMRS_MODES ? names[mode] : "?";
}
#endif
#endif
