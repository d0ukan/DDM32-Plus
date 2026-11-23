/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus faster channel scan (scanfast.h) */
#include <string.h>
#include "config.h"
#if CONFIG_SCAN_FAST
#include "radio/scanfast.h"

#ifndef FS_DEFAULT_FAST
#define FS_DEFAULT_FAST 1	/* host tests of the classic scan build it 0 */
#endif
static volatile bool wanted = FS_DEFAULT_FAST;	/* the switch: fast unless the owner chose classic */
static fs_stats_t st;
static fs_stop_t cur;
static uint32_t stop_ms;		/* the current stop's first tune (a pause's end tunes it anew: not a new stop) */
static uint32_t pass_t0, last_where;
static uint16_t cur_stops;
static bool have_stop, have_pass;

void fs_set_fast(bool fast)
{
	wanted = fast;
}

bool fs_fast_wanted(void)
{
	return wanted;
}

uint8_t fs_feed(fs_stop_t *s, uint32_t now_ms, bool quiet, uint32_t span_ms)
{
	if (s->state != FS_ST_SAMPLING || now_ms - s->tuned_ms < FS_SETTLE_MS)
		return FS_WAIT;
	if (!quiet) {
		s->state = FS_ST_CLASSIC;
		return FS_CLASSIC;
	}
	if (s->first_ms != s->tuned_ms && now_ms - s->last_ms > FS_GAP_MS) {
		s->first_ms = s->tuned_ms;	/* a late reading: the span starts again from this one */
		st.late++;
	}
	if (s->first_ms == s->tuned_ms)
		s->first_ms = now_ms;		/* (now_ms - tuned_ms >= FS_SETTLE_MS: never the tune itself) */
	s->last_ms = now_ms;
	if (now_ms - s->first_ms >= span_ms) {
		s->state = FS_ST_DONE;
		return FS_QUIET;
	}
	return FS_WAIT;
}

static void restart(uint32_t now_ms)
{
	bool fast = wanted;
	memset(&st, 0, sizeof st);
	st.on = true;
	st.fast = fast;
	st.start_ms = st.mark_ms = now_ms;
	have_stop = have_pass = false;
	last_where = 0;
	cur_stops = 0;
}

void fs_begin(uint32_t now_ms)
{
	restart(now_ms);
	memset(&cur, 0, sizeof cur);
}

void fs_end(uint32_t now_ms)
{
	if (st.on) {
		st.on = false;
		st.mark_ms = now_ms;
	}
	cur.state = FS_ST_OFF;
}

bool fs_fast(void)
{
	return st.fast;
}

void fs_stop_new(uint32_t tuned_ms, uint32_t where, uint8_t prev_mode, bool prev_noticed, bool prev_held)
{
	if (wanted != st.fast) {
		restart(tuned_ms);	/* the switch moved: the counters start again with this stop */
	} else if (have_stop) {
		st.stops++;
		if (prev_noticed) {
			st.noticed++;
			st.held += prev_held;
		} else if (prev_mode < FS_MODES) {
			st.q_n[prev_mode]++;
			st.q_ms[prev_mode] += tuned_ms - stop_ms;
		}
	}
	if (have_stop && where < last_where) {
		/* the sweep wrapped: a pass ends (the first wrap only starts the count) */
		if (have_pass) {
			st.passes++;
			st.pass_ms = tuned_ms - pass_t0;
			st.pass_stops = cur_stops;
		}
		have_pass = true;
		pass_t0 = tuned_ms;
		cur_stops = 0;
	}
	cur_stops++;
	last_where = where;
	stop_ms = tuned_ms;
	have_stop = true;
	st.mark_ms = tuned_ms;
	cur.tuned_ms = cur.first_ms = cur.last_ms = tuned_ms;
	cur.state = st.fast ? FS_ST_SAMPLING : FS_ST_OFF;
}

#ifdef FS_TEST_TOTALS
uint32_t fs_total_quiet;	/* host tests: stops ended by the fast rule in every scan */
#endif

void fs_count(uint8_t what)
{
#ifdef FS_TEST_TOTALS
	fs_total_quiet += what == FS_QUIET;
#endif
	if (what == FS_QUIET)
		st.fast_quiet++;
	else if (what == FS_CLASSIC)
		st.fallback++;
}

fs_stop_t *fs_cur(void)
{
	return &cur;
}

void fs_view(uint32_t now_ms, fs_stats_t *out)
{
	*out = st;	/* a torn copy only mixes two moments' counts: the PC reads again next second */
	if (out->on)
		out->mark_ms = now_ms;
}
#endif
