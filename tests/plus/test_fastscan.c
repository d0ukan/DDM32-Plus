/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus faster channel scan (radio/scanfast.h): the engine alone. A stop's readings (settle, quiet span, late
 * readings, the hand-over to the classic rules, DMR's frame), the counters (stops, passes, quiet time by mode, the
 * switch restarting them) and the PC's copy. Built by tools/test_fastscan.py with CONFIG_SCAN_FAST=1. */
#include <stdio.h>
#include <string.h>
#include "radio/scanfast.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static fs_stop_t stop_at(uint32_t t)
{
	fs_stop_t s = { t, t, t, FS_ST_SAMPLING };
	return s;
}

static void test_feed(void)
{
	/* FM: readings before the settle are not used; three readings 10 ms apart after it end the stop */
	fs_stop_t s = stop_at(1000);
	CHECK(fs_feed(&s, 1010, true, FS_SPAN_MS) == FS_WAIT && s.first_ms == 1000);
	CHECK(fs_feed(&s, 1019, false, FS_SPAN_MS) == FS_WAIT && s.state == FS_ST_SAMPLING);	/* still settling */
	CHECK(fs_feed(&s, 1020, true, FS_SPAN_MS) == FS_WAIT && s.first_ms == 1020);
	CHECK(fs_feed(&s, 1030, true, FS_SPAN_MS) == FS_WAIT);
	CHECK(fs_feed(&s, 1040, true, FS_SPAN_MS) == FS_QUIET && s.state == FS_ST_DONE);
	CHECK(fs_feed(&s, 1050, true, FS_SPAN_MS) == FS_WAIT);	/* done: nothing more */

	/* One reading not clearly quiet: the classic rules for the rest of the stop, whatever follows */
	s = stop_at(2000);
	CHECK(fs_feed(&s, 2020, true, FS_SPAN_MS) == FS_WAIT);
	CHECK(fs_feed(&s, 2030, false, FS_SPAN_MS) == FS_CLASSIC && s.state == FS_ST_CLASSIC);
	CHECK(fs_feed(&s, 2040, true, FS_SPAN_MS) == FS_WAIT && fs_feed(&s, 2050, true, FS_SPAN_MS) == FS_WAIT);

	/* DMR: the readings must span a whole 60 ms frame */
	s = stop_at(3000);
	uint8_t v = FS_WAIT;
	uint32_t t = 3020;
	for (; t <= 3200 && v == FS_WAIT; t += 10)
		v = fs_feed(&s, t, true, FS_SPAN_DMR_MS);
	CHECK(v == FS_QUIET && t - 10 == 3080);

	/* A late reading (more than FS_GAP_MS after the last) starts the span again: DMR never steps on a gap that
	 * could hide a burst */
	fs_stats_t before, after;
	fs_view(0, &before);
	s = stop_at(4000);
	CHECK(fs_feed(&s, 4020, true, FS_SPAN_DMR_MS) == FS_WAIT);
	CHECK(fs_feed(&s, 4030, true, FS_SPAN_DMR_MS) == FS_WAIT);
	CHECK(fs_feed(&s, 4060, true, FS_SPAN_DMR_MS) == FS_WAIT && s.first_ms == 4060);	/* 30 ms gap */
	fs_view(0, &after);
	CHECK(after.late == (uint16_t)(before.late + 1u));
	v = FS_WAIT;
	for (t = 4072; t <= 4300 && v == FS_WAIT; t += 12)	/* 12 ms apart: still a span */
		v = fs_feed(&s, t, true, FS_SPAN_DMR_MS);
	CHECK(v == FS_QUIET && t - 12 == 4060 + 60);
	/* Off (the classic algorithm): never a verdict */
	s = stop_at(5000);
	s.state = FS_ST_OFF;
	CHECK(fs_feed(&s, 5100, true, FS_SPAN_MS) == FS_WAIT && fs_feed(&s, 5110, false, FS_SPAN_MS) == FS_WAIT);
	/* A gap of exactly FS_GAP_MS is no gap */
	s = stop_at(6000);
	CHECK(fs_feed(&s, 6020, true, FS_SPAN_DMR_MS) == FS_WAIT);
	CHECK(fs_feed(&s, 6032, true, FS_SPAN_DMR_MS) == FS_WAIT && s.first_ms == 6020);
	printf("Faster scan engine: settle, span, late readings, the hand-over, DMR's frame\n");
}

static void test_counters(void)
{
	fs_stats_t v;
	fs_set_fast(true);
	fs_begin(10000);
	fs_view(10000, &v);
	CHECK(v.on && v.fast && v.stops == 0 && v.passes == 0 && fs_fast());
	/* A zone of 4 swept 1, 2, 3, 0, 1, 2, 3, 0, 1: the first stop is not counted, the first wrap starts the passes */
	static const uint32_t where[] = { 1, 2, 3, 0, 1, 2, 3, 0, 1 };
	uint32_t t = 10000;
	for (unsigned i = 0; i < 9; i++) {
		/* stop i-1 (FM, quiet) lasted 40 ms; stop 5 (index 5 ends at i = 6) probed, stop 6 held */
		bool noticed = i == 6 || i == 7, held = i == 7;
		fs_stop_new(t, where[i], i == 2 ? FS_M_DMR : FS_M_FM, noticed, held);
		CHECK(fs_cur()->state == FS_ST_SAMPLING && fs_cur()->tuned_ms == t);
		t += i == 1 ? 80 : 40;
	}
	fs_view(t, &v);
	CHECK(v.stops == 8 && v.noticed == 2 && v.held == 1);
	CHECK(v.q_n[FS_M_DMR] == 1 && v.q_ms[FS_M_DMR] == 80 && v.q_n[FS_M_FM] == 5 && v.q_ms[FS_M_FM] == 200);
	CHECK(v.passes == 1 && v.pass_stops == 4 && v.pass_ms == 160);
	CHECK(v.mark_ms - v.start_ms == t - 10000);
	fs_count(FS_QUIET);
	fs_count(FS_CLASSIC);
	fs_count(FS_QUIET);
	fs_view(t, &v);
	CHECK(v.fast_quiet == 2 && v.fallback == 1);
	/* The switch: classic from the next stop, the counters start again */
	fs_set_fast(false);
	CHECK(!fs_fast_wanted() && fs_fast());
	fs_stop_new(t, 2, FS_M_FM, false, false);
	fs_view(t + 5, &v);
	CHECK(!v.fast && v.stops == 0 && v.fast_quiet == 0 && v.start_ms == t && fs_cur()->state == FS_ST_OFF);
	fs_stop_new(t + 150, 3, FS_M_FM, false, false);
	fs_view(t + 150, &v);
	CHECK(v.stops == 1 && v.q_ms[FS_M_FM] == 150);
	/* The end: stopped, its time kept */
	fs_end(t + 300);
	fs_view(t + 9000, &v);
	CHECK(!v.on && v.mark_ms == t + 300 && fs_cur()->state == FS_ST_OFF);
	fs_set_fast(true);
	printf("Faster scan counters: stops, passes, quiet time by mode, the switch, the end\n");
}

int main(void)
{
	test_feed();
	test_counters();
	printf("Faster scan engine: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
