/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 13 ideas 2, the portable parts (tools/test_toolbox.py builds this with -DCONFIG_HEARD=1
 * -DCONFIG_SPECTRUM_WF=1 -DCONFIG_MAP=1 -DCONFIG_MAP_COVERAGE=1 -DCONFIG_GPS_DASH=1 -DCONFIG_CLOCK_TOOLS=1):
 *   - the heard list: merge by (mode, source, destination), newest first, the call count (a new call, or heard again
 *     after HEARD_GAP_S of quiet; at most 255), the oldest dropped when full, old entries expire, YSF callsigns packed;
 *   - the waterfall: levels above the floor, their colour slots, the row ring (newest first, wrap, a new point count
 *     starts over), nothing without a buffer;
 *   - coverage: the byte (clamped, 0 = no reading) and the S-meter thresholds;
 *   - the trip meter: standing still adds nothing, steps from TRIP_MIN_M, a jump is not counted, the top speed without
 *     spikes, no fix no change, the 16 compass points;
 *   - the clock tools: stopwatch and laps, the countdown's end once, the alarm on entering its minute (local time with
 *     the offset), not for a big jump, not without a clock, not for the minute it was set in. */
#include <stdio.h>
#include <string.h>
#include "ui/heard.h"
#include "ui/wfall.h"
#include "ui/mapnav.h"
#include "ui/tripm.h"
#include "ui/clockx.h"
#include "refpack_fixture.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

/* --- heard list --- */

static heard_t key(uint8_t kind, uint32_t id, uint32_t tg, uint16_t nac, uint32_t hz)
{
	heard_t k;
	memset(&k, 0, sizeof k);
	k.kind = kind;
	k.id = id;
	k.tg = tg;
	k.nac = nac;
	k.hz = hz;
	return k;
}

static void test_heard(void)
{
	heard_list_t l;
	heard_clear(&l);
	heard_t a = key(2, 77285, 101, 0x659, 433500000), b = key(1, 3101234, 91, 1 | 1 << 4, 438700000),
		c = key(2, 77285, 102, 0x659, 433500000);
	heard_note(&l, &a, true, 100);
	heard_note(&l, &b, true, 110);
	heard_note(&l, &c, true, 120);
	CHECK(l.n == 3 && l.e[0].tg == 102 && l.e[1].id == 3101234 && l.e[2].tg == 101, "three apart, newest first");
	/* the first again: merged, on top, counted */
	heard_note(&l, &a, true, 130);
	CHECK(l.n == 3 && l.e[0].tg == 101 && l.e[0].count == 2 && l.e[0].last_s == 130 && l.e[1].tg == 102 &&
	      l.e[2].id == 3101234, "merged and moved up: count %u", (unsigned)l.e[0].count);
	/* the same call going on: no count until it was quiet HEARD_GAP_S */
	heard_note(&l, &a, false, 131);
	heard_note(&l, &a, false, 133);
	CHECK(l.e[0].count == 2 && l.e[0].last_s == 133, "a call going on is not counted again");
	heard_note(&l, &a, false, 133 + HEARD_GAP_S);
	CHECK(l.e[0].count == 3, "heard again after the gap: another call");
	/* the same IDs in another mode, or a private call, are other stations */
	heard_t ap = key(2 | HEARD_PRIV, 77285, 101, 0x659, 433500000), ad = key(1, 77285, 101, 1, 433500000);
	heard_note(&l, &ap, true, 140);
	heard_note(&l, &ad, true, 141);
	CHECK(l.n == 5 && l.e[0].kind == 1 && l.e[1].kind == (2 | HEARD_PRIV), "mode and private flag kept apart");
	/* NAC and frequency as heard now; a frequency of 0 keeps the one known */
	heard_t a2 = key(2, 77285, 101, 0x293, 0);
	heard_note(&l, &a2, true, 150);
	CHECK(l.e[0].nac == 0x293 && l.e[0].hz == 433500000, "NAC updated, frequency kept");
	/* saturating count */
	for (int i = 0; i < 300; i++)
		heard_note(&l, &a, true, 151);
	CHECK(l.e[0].count == 255, "count stops at 255 (%u)", (unsigned)l.e[0].count);
	/* full: the least recently heard goes */
	heard_clear(&l);
	for (uint32_t i = 0; i < HEARD_MAX + 3u; i++) {
		heard_t k = key(1, 1000 + i, 91, 1, 438000000);
		heard_note(&l, &k, true, 200 + i);
	}
	CHECK(l.n == HEARD_MAX && l.e[0].id == 1000 + HEARD_MAX + 2u && l.e[HEARD_MAX - 1u].id == 1003,
	      "full: the oldest three dropped (last %u)", (unsigned)l.e[HEARD_MAX - 1u].id);
	for (uint8_t i = 1; i < l.n; i++)
		CHECK(heard_age_s(&l.e[i - 1u], 300) <= heard_age_s(&l.e[i], 300), "sorted by last heard at %u", i);
	/* an old entry heard again moves up without growing the list */
	heard_t old = key(1, 1005, 91, 1, 438000000);
	heard_note(&l, &old, true, 400);
	CHECK(l.n == HEARD_MAX && l.e[0].id == 1005 && l.e[0].count == 2, "old entry merged");
	/* expiry before the 20-bit clock wraps */
	heard_expire(&l, 203 + HEARD_AGE_MAX_S - 1u);	/* the oldest (1003) was heard at 203 */
	CHECK(l.n == HEARD_MAX, "nothing too old yet");
	heard_expire(&l, 203 + HEARD_AGE_MAX_S);
	CHECK(l.n == HEARD_MAX - 1u && l.e[l.n - 1u].id == 1004, "the oldest expired");
	heard_expire(&l, 400 + HEARD_AGE_MAX_S);
	CHECK(l.n == 0, "all expired (%u left)", l.n);
	/* the clock wraps at 2^20 s: ages stay right across it */
	heard_t w = key(1, 7, 9, 1, 1);
	heard_note(&l, &w, true, HEARD_CLOCK_MASK - 2u);
	CHECK(heard_age_s(&l.e[0], HEARD_CLOCK_MASK + 3u) == 5u, "age across the wrap");
	/* YSF callsigns */
	heard_t y;
	char call[HEARD_CALL_CHARS + 1];
	memset(&y, 0, sizeof y);
	heard_pack_call("ta2dou", &y);
	heard_unpack_call(&y, call);
	CHECK(strcmp(call, "TA2DOU") == 0, "callsign round trip (%s)", call);
	heard_pack_call("TA1ABC-7/M", &y);
	heard_unpack_call(&y, call);
	CHECK(strcmp(call, "TA1ABC-7") == 0, "cut at 8 (%s)", call);
	heard_pack_call("A_B", &y);
	heard_unpack_call(&y, call);
	CHECK(strcmp(call, "A?B") == 0, "unknown characters as ? (%s)", call);
	heard_t y1, y2;
	memset(&y1, 0, sizeof y1);
	memset(&y2, 0, sizeof y2);
	heard_pack_call("TA2DOU", &y1);
	heard_pack_call("TA2DOV", &y2);
	CHECK(y1.id != y2.id || y1.tg != y2.tg, "two callsigns differ");
}

/* --- waterfall --- */

static void test_waterfall(void)
{
	CHECK(wf_level(0, -120) == 0, "unread");
	CHECK(wf_level(-120, -120) == 0 && wf_level(-118, -120) == 0, "at the floor");
	CHECK(wf_level(-117, -120) == 1 && wf_level(-114, -120) == 2, "3 dB a level");
	CHECK(wf_level(-75, -120) == 15 && wf_level(-20, -120) == 15, "15 at most");
	CHECK(wf_level(-130, -120) == 0, "below the floor");
	CHECK(wf_slot(0) == 0 && wf_slot(15) == WF_SLOTS - 1u, "slots: darkest to brightest");
	for (uint8_t l = 1; l < 16u; l++)
		CHECK(wf_slot(l) >= wf_slot((uint8_t)(l - 1u)) && wf_slot(l) < WF_SLOTS, "slots never fall (%u)", l);
	CHECK(wf_slot(1) != 0, "a level above the floor is never the background");
	wf_t w;
	memset(&w, 0, sizeof w);
	int16_t dbm[64];
	for (int i = 0; i < 64; i++)
		dbm[i] = -120;
	wf_push(&w, dbm, 64, -120, 1);
	CHECK(w.n == 0, "no buffer: nothing");
	static uint8_t buf[WF_BYTES];
	w.buf = buf;
	wf_reset(&w);
	for (uint32_t s = 1; s <= WF_ROWS + 5u; s++) {
		dbm[s % 64u] = -60;		/* a strong point a row, moving */
		wf_push(&w, dbm, 64, -120, s);
		dbm[s % 64u] = -120;
	}
	CHECK(w.n == WF_ROWS && w.sweeps == WF_ROWS + 5u, "the ring is full");
	for (uint8_t r = 0; r < WF_ROWS; r++) {
		uint32_t s = WF_ROWS + 5u - r;
		CHECK(wf_get(&w, r, (uint8_t)(s % 64u)) == 15, "row %u: its strong point", r);
		CHECK(wf_get(&w, r, (uint8_t)((s + 1u) % 64u)) == 0, "row %u: the others at the floor", r);
	}
	CHECK(wf_get(&w, WF_ROWS, 0) == 0 && wf_get(&w, 0, 64) == 0, "outside: 0");
	wf_push(&w, dbm, 32, -120, 99);
	CHECK(w.n == 1 && w.points == 32, "another point count starts over");
	wf_push(&w, dbm, 200, -120, 100);
	CHECK(w.points == WF_POINTS && w.n == 1, "more points than a row: cut, and started over");
}

/* --- coverage --- */

static void test_coverage(void)
{
	CHECK(cov_byte(0) == 0 && cov_class(0) == COV_NONE, "no reading");
	CHECK(cov_dbm(cov_byte(-93)) == -93 && cov_dbm(cov_byte(-140)) == -140, "round trip");
	CHECK(cov_byte(-160) == 1 && cov_dbm(cov_byte(-160)) == -140, "clamped low");
	CHECK(cov_byte(-1) == 140 && cov_byte(10) == 140, "clamped high");
	CHECK(cov_class(cov_byte(-93)) == COV_GOOD && cov_class(cov_byte(COV_GOOD_DBM)) == COV_GOOD, "S9, S7: good");
	CHECK(cov_class(cov_byte(COV_GOOD_DBM - 1)) == COV_FAIR && cov_class(cov_byte(COV_FAIR_DBM)) == COV_FAIR,
	      "below S7 to S5: fair");
	CHECK(cov_class(cov_byte(COV_FAIR_DBM - 1)) == COV_POOR && cov_class(cov_byte(-140)) == COV_POOR, "below S5: poor");
	CHECK(COV_GOOD_DBM == -93 - 2 * 6 && COV_FAIR_DBM == -93 - 4 * 6, "S7 and S5 on the VHF / UHF scale");
	track_t t;
	memset(&t, 0, sizeof t);
	t.head = 2;
	t.n = 5;
	CHECK(track_slot(&t, 0) == TRACK_POINTS - 3u && track_slot(&t, 4) == 1, "slots follow the ring");
}

/* --- trip meter --- */

#define LAT0	399300000	/* Ankara, 39.93 N */
#define LON0	328600000
#define DLAT_M(m)	((int32_t)((int64_t)(m) * 10000000 / 111195))	/* north m metres */

static void test_trip(void)
{
	trip_t t;
	trip_reset(&t);
	trip_feed(&t, false, LAT0, LON0, 50, 0);
	CHECK(!t.have && t.max_kmh == 0, "no fix: nothing");
	trip_feed(&t, true, LAT0, LON0, 0, 1000);
	/* standing still: the fix wanders up to 15 m for ten minutes */
	for (uint32_t s = 2; s < 600; s++) {
		int32_t j = (int32_t)(s * 7919u % 31u) - 15;
		trip_feed(&t, true, LAT0 + DLAT_M(j), LON0 + DLAT_M(j / 2), 1, s * 1000u);
	}
	CHECK(t.meters == 0, "standing still adds nothing (%u m)", (unsigned)t.meters);
	/* north at 36 km/h (10 m/s) for 100 s: 1000 m, counted in steps */
	trip_reset(&t);
	for (uint32_t s = 0; s <= 100; s++)
		trip_feed(&t, true, LAT0 + DLAT_M(s * 10u), LON0, 36, s * 1000u);
	CHECK(t.meters >= 975 && t.meters <= 1005, "1 km north: %u m", (unsigned)t.meters);
	CHECK(t.max_kmh == 36, "top speed");
	/* a jump of 50 km in a second: not counted, counting goes on from there */
	uint32_t before = t.meters;
	trip_feed(&t, true, LAT0 + DLAT_M(51000), LON0, 36, 101000);
	CHECK(t.meters == before, "a jump is not counted");
	trip_feed(&t, true, LAT0 + DLAT_M(51030), LON0, 36, 104000);
	CHECK(t.meters >= before + 29 && t.meters <= before + 31, "counted from the new point (%u)",
	      (unsigned)(t.meters - before));
	/* east: the longitude shrinks with the latitude */
	uint32_t d = trip_dist_m(LAT0, LON0, LAT0, LON0 + 10000000);
	CHECK(d > 85000 && d < 85400, "1 degree east at 39.93 N: %u m", (unsigned)d);
	/* a speed spike is not the top speed; unknown is ignored */
	trip_feed(&t, true, LAT0 + DLAT_M(51030), LON0, 900, 105000);
	trip_feed(&t, true, LAT0 + DLAT_M(51030), LON0, 0xFFFF, 106000);
	CHECK(t.max_kmh == 36, "spikes ignored (%u)", t.max_kmh);
	/* after the GPS was lost for a while, the way since is counted when it is plausible */
	before = t.meters;
	trip_feed(&t, true, LAT0 + DLAT_M(51030 + 5000), LON0, 50, 106000 + 600000);
	CHECK(t.meters == before + 5000 || t.meters == before + 4999 || t.meters == before + 5001,
	      "5 km in 10 min after an outage (%u)", (unsigned)(t.meters - before));
	CHECK(!strcmp(trip_compass16(0), "N") && !strcmp(trip_compass16(11), "N") && !strcmp(trip_compass16(12), "NNE") &&
	      !strcmp(trip_compass16(45), "NE") && !strcmp(trip_compass16(180), "S") && !strcmp(trip_compass16(348), "NNW") &&
	      !strcmp(trip_compass16(349), "N") && !strcmp(trip_compass16(359), "N") && !strcmp(trip_compass16(720), "N"),
	      "compass points");
}

/* --- clock tools --- */

static void test_clock(void)
{
	clockx_t c;
	clk_init(&c);
	/* stopwatch */
	clk_sw_toggle(&c, 1000);
	CHECK(c.sw_run && clk_sw_ms(&c, 3500) == 2500, "runs");
	clk_sw_hash(&c, 3500);			/* lap 1: 2.5 s */
	clk_sw_hash(&c, 6000);			/* lap 2: 2.5 s */
	clk_sw_toggle(&c, 7000);		/* stop at 6 s */
	CHECK(!c.sw_run && clk_sw_ms(&c, 99999) == 6000, "stopped holds (%u)", (unsigned)clk_sw_ms(&c, 99999));
	clk_sw_toggle(&c, 10000);		/* on again */
	CHECK(clk_sw_ms(&c, 11000) == 7000, "resumes");
	clk_sw_hash(&c, 12000);			/* lap 3 at 8 s: 3 s */
	uint32_t lap;
	uint8_t n;
	CHECK(clk_sw_lap(&c, 0, &lap, &n) && n == 3 && lap == 3000, "lap 3: %u", (unsigned)lap);
	CHECK(clk_sw_lap(&c, 1, &lap, &n) && n == 2 && lap == 2500, "lap 2");
	CHECK(clk_sw_lap(&c, 2, &lap, &n) && n == 1 && lap == 2500, "lap 1 from the start");
	CHECK(!clk_sw_lap(&c, 3, &lap, &n), "no lap 0");
	clk_sw_hash(&c, 13000);
	clk_sw_hash(&c, 14000);			/* 5 laps: the oldest kept is lap 2, its own time unknown */
	CHECK(clk_sw_lap(&c, 2, &lap, &n) && n == 3 && !clk_sw_lap(&c, 3, &lap, &n), "the oldest kept mark has no time");
	clk_sw_toggle(&c, 15000);
	clk_sw_hash(&c, 15000);			/* stopped: reset */
	CHECK(clk_sw_ms(&c, 20000) == 0 && c.laps == 0, "reset");
	/* countdown */
	CHECK(clk_cd_left_s(&c, 0) == CLK_CD_DEFAULT_S, "the time set");
	clk_cd_adjust(&c, -1000000);
	CHECK(c.cd_set_s == 1, "at least 1 s");
	clk_cd_adjust(&c, 1000000);
	CHECK(c.cd_set_s == CLK_CD_MAX_S, "at most 99 min");
	c.cd_set_s = 10;
	clk_cd_toggle(&c, 50000);
	clk_cd_adjust(&c, 60);
	CHECK(c.cd_set_s == 10, "not changed while it runs");
	CHECK(clk_cd_left_s(&c, 50001) == 10 && clk_cd_left_s(&c, 59001) == 1, "counts down, rounded up");
	CHECK(clk_tick(&c, 59999) == 0, "not yet");
	CHECK(clk_tick(&c, 60000) == CLK_EV_COUNTDOWN && !c.cd_run, "ends at 0");
	CHECK(clk_tick(&c, 61000) == 0 && clk_cd_left_s(&c, 61000) == 10, "once; the time set again");
	clk_cd_toggle(&c, 70000);
	clk_cd_toggle(&c, 72000);		/* stopped early */
	CHECK(clk_tick(&c, 90000) == 0, "a stopped countdown never ends");
	/* alarm: no clock, no alarm */
	c.al_min = 7 * 60;
	clk_al_toggle(&c);
	for (uint32_t t = 0; t < 120000; t += 1000)
		CHECK(clk_tick(&c, t) == 0, "no clock: nothing");
	/* GPS 03:59:50 UTC = 06:59:50 at +03:00 */
	clk_gps(&c, false, 35950, 200000);
	CHECK(!c.clk_ok, "no fix: no time");
	clk_gps(&c, true, 35950, 200000);
	uint32_t sod;
	CHECK(clk_local_s(&c, 200000, &sod) && sod == 6 * 3600 + 59 * 60 + 50, "local time");
	CHECK(clk_tick(&c, 200000) == 0 && clk_tick(&c, 209000) == 0, "06:59:59");
	CHECK(clk_tick(&c, 210000) == CLK_EV_ALARM && !c.al_on, "07:00:00 rings, then off");
	CHECK(clk_tick(&c, 211000) == 0, "once");
	/* set for the minute it is now: not now (the next day) */
	clk_al_toggle(&c);
	CHECK(clk_tick(&c, 215000) == 0 && clk_tick(&c, 260000) == 0, "the minute it was set in does not ring");
	CHECK(clk_tick(&c, 209000 + 86400000u) == 0, "06:59:59 the next day");
	CHECK(clk_tick(&c, 210000 + 86400000u) == CLK_EV_ALARM, "the next day it does");
	/* a big jump over the alarm (a new GPS time) does not ring; a few minutes do */
	clk_gps(&c, true, 30000, 300000);	/* 06:00 local */
	c.al_min = 6 * 60 + 30;
	clk_al_toggle(&c);
	CHECK(clk_tick(&c, 300000) == 0, "06:00");
	clk_gps(&c, true, 40000, 301000);	/* 07:00 local: an hour on at once */
	CHECK(clk_tick(&c, 301000) == 0 && c.al_on, "jumped over: still waiting");
	clk_gps(&c, true, 32700, 302000);	/* 06:27 */
	clk_tick(&c, 302000);
	clk_gps(&c, true, 33100, 303000);	/* 06:31: four minutes on */
	CHECK(clk_tick(&c, 303000) == CLK_EV_ALARM, "a few minutes jumped over: rings");
	/* the offset: round the day, and it moves local time */
	clk_init(&c);
	clk_gps(&c, true, 230000, 0);		/* 23:00 UTC */
	CHECK(clk_local_s(&c, 0, &sod) && sod == 2 * 3600, "+03:00 over midnight");
	clk_tz_adjust(&c, -6 * 60);
	CHECK(clk_local_s(&c, 0, &sod) && sod == 20 * 3600, "-03:00");
	c.tz_min = 14 * 60;
	clk_tz_adjust(&c, 30);
	CHECK(c.tz_min == -12 * 60, "wraps");
	clk_al_adjust(&c, -1);
	CHECK(c.al_min == 7 * 60 - 1, "minute down");
	c.al_min = 0;
	clk_al_adjust(&c, -10);
	CHECK(c.al_min == 1430, "round the day");
	clk_gps(&c, true, 250000, 0);
	CHECK(clk_local_s(&c, 0, &sod), "a bad time is ignored (the last kept)");
}

int main(void)
{
	/* the tables are the map pack's on the data chip since 1.1 (refpack_fixture.h) */
	if (!refpack_fixture_install(REFPACK_MAP)) {
		printf("FAIL: the map pack fixture\n");
		return 1;
	}
	test_heard();
	test_waterfall();
	test_coverage();
	test_trip();
	test_clock();
	printf("Plus round 13 ideas 2 (heard list, waterfall, coverage, trip meter, clock tools): %d failures\n", fails);
	return fails != 0;
}
