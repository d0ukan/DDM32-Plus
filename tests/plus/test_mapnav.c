/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus map navigation core (integration/radiofoundry/map/mapnav.c, docs/map.md "APRS stations, auto repeater,
 * track"): the integer cos and distance against double precision; the APRS station pick over a heard list (order
 * nearest the centre first, the walk and its wrap, stations without a position, one that went, the comment without
 * its altitude); the track ring (the first point at once, 30 s and 50 m, one cell, the wrap and its order, the start
 * kept, out of range, clear) and its drawing through the map renderer into a framebuffer (dotted segments inside the
 * viewport only, clipped at its edge); the auto repeater's choice and hysteresis over a synthetic repeater table
 * (FM / DMR by the channel's mode, bands, no position, C4FM, the first offer, 30 s pacing, no repeat offers, 20 % and
 * 2 km, no flip-flop back, a mode change, already tuned, no fix, off).
 * Built by tools/test_classic.py with -DCONFIG_MAP=1 -DCONFIG_AUTO_RPT=1 -DCONFIG_REPEATERS=1 -DCONFIG_APRS=1. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "ui/mapnav.h"
#include "ui/rpt.h"
#include "turkey_map_data.h"
#include "refpack_fixture.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define E7(d) ((int32_t)lround((d) * 1e7))

static double dist_m(double la1, double lo1, double la2, double lo2)
{
	double r = M_PI / 180.0, a = sin((la2 - la1) * r / 2), b = sin((lo2 - lo1) * r / 2);
	double h = a * a + cos(la1 * r) * cos(la2 * r) * b * b;
	return 2 * 6371000.0 * atan2(sqrt(h), sqrt(1 - h));
}

static void test_math(void)
{
	for (int d = -89; d <= 89; d++) {
		double want = cos(d * M_PI / 180.0) * 32768.0;
		CHECK(fabs(mapnav_cos_q15(E7(d)) - want) <= 0.002 * 32768.0);
	}
	CHECK(mapnav_cos_q15(E7(0)) == 32767);
	CHECK(mapnav_cos_q15(E7(90)) == 0);
	/* Istanbul -> Izmit (90 km), Ankara -> Kirikkale (60 km), 50 m steps: within 0.5 % of the great circle */
	static const double pts[][4] = {
		{ 41.0082, 28.9784, 40.7654, 29.9408 }, { 39.9334, 32.8597, 39.8468, 33.5153 },
		{ 39.9, 32.8, 39.9 + 0.00045, 32.8 }, { 39.9, 32.8, 39.9, 32.8 + 0.00059 }, { 36.9, 30.7, 37.0, 30.6 },
	};
	for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++) {
		double want = dist_m(pts[i][0], pts[i][1], pts[i][2], pts[i][3]);
		double got = sqrt((double)mapnav_dist2_m(E7(pts[i][0]), E7(pts[i][1]), E7(pts[i][2]), E7(pts[i][3])));
		CHECK(fabs(got - want) <= want * 0.005 + 2.0);
	}
	CHECK(mapnav_dist2_m(E7(39.9), E7(32.8), E7(39.9), E7(32.8)) == 0);
	/* far apart (Edirne -> Kars) does not overflow */
	CHECK(sqrt((double)mapnav_dist2_m(E7(41.67), E7(26.56), E7(40.6), E7(43.1))) > 1300000.0);
}

/* --- APRS stations: a heard list like the engine's (most recent first) --- */

static aprs_station_t list[APRS_STATIONS];
static uint8_t nlist;

static bool get_station(uint8_t i, aprs_station_t *out)
{
	if (i >= nlist)
		return false;
	*out = list[i];
	return true;
}

static void add_station(const char *call, bool pos, double la, double lo, const char *text, int16_t alt)
{
	aprs_station_t *s = &list[nlist++];
	memset(s, 0, sizeof *s);
	strcpy(s->call, call);
	s->has_pos = pos;
	s->lat = E7(la);
	s->lon = E7(lo);
	s->sym_table = '/';
	s->sym_code = '>';
	strcpy(s->text, text);
	s->alt_m = alt;
}

static const char *name_of(uint16_t h)
{
	for (uint8_t i = 0; i < nlist; i++)
		if (mapnav_call_hash(list[i].call) == h)
			return list[i].call;
	return h == 0 ? "(none)" : "(?)";
}

static void test_stations(void)
{
	/* hashes: never 0, the twelve different */
	static const char *calls[] = { "TA1ABC", "TA1ABC-9", "TA2XYZ-7", "YM1KTC", "TB3A", "TA7B-15", "N0CALL",
				       "TA1AAA", "TA1AAB", "12345678", "TA9Z-1", "YM2ANK" };
	for (unsigned i = 0; i < 12; i++) {
		CHECK(mapnav_call_hash(calls[i]) != 0);
		for (unsigned j = 0; j < i; j++)
			CHECK(mapnav_call_hash(calls[i]) != mapnav_call_hash(calls[j]));
	}
	CHECK(mapnav_call_hash("") != 0);

	/* none at all, then one without a position */
	nlist = 0;
	CHECK(mapnav_station_next(get_station, nlist, E7(40), E7(32), 0) == 0);
	add_station("TB3A", false, 0, 0, "status only", MAPNAV_ALT_NONE);
	CHECK(mapnav_station_next(get_station, nlist, E7(40), E7(32), 0) == 0);
	aprs_station_t st;
	CHECK(mapnav_station_find(get_station, nlist, mapnav_call_hash("TB3A"), &st) < 0);

	/* four with positions around Ankara (39.93, 32.86), in list order not by distance */
	add_station("TA2XYZ-7", true, 40.20, 32.86, "", MAPNAV_ALT_NONE);	/* 30 km N */
	add_station("TA1ABC-9", true, 39.95, 32.88, "Hello /A=001234 mobile", 376);	/* 2.7 km */
	add_station("YM2ANK", true, 39.93, 33.50, "Digi", 1200);	/* 55 km E */
	add_station("TA7B-15", true, 39.80, 32.60, "", MAPNAV_ALT_NONE);	/* 26 km SW */
	int32_t la = E7(39.93), lo = E7(32.86);
	static const char *order[] = { "TA1ABC-9", "TA7B-15", "TA2XYZ-7", "YM2ANK" };
	uint16_t h = 0;
	for (int round = 0; round < 2; round++) {
		for (unsigned k = 0; k < 4; k++) {
			h = mapnav_station_next(get_station, nlist, la, lo, h);
			if (strcmp(name_of(h), order[k]) != 0)
				printf("  station order %u: got %s want %s\n", k, name_of(h), order[k]);
			CHECK(strcmp(name_of(h), order[k]) == 0);
		}
	}
	/* from somewhere else: Kirikkale side, YM2ANK nearest */
	CHECK(strcmp(name_of(mapnav_station_next(get_station, nlist, E7(39.85), E7(33.52), 0)), "YM2ANK") == 0);
	/* found, its index and fields */
	int at = mapnav_station_find(get_station, nlist, mapnav_call_hash("TA1ABC-9"), &st);
	CHECK(at == 2 && st.alt_m == 376 && strcmp(st.call, "TA1ABC-9") == 0);
	CHECK(mapnav_station_find(get_station, nlist, 0, &st) < 0);
	/* the picked one went (the list dropped it): the walk starts over at the nearest */
	h = mapnav_call_hash("TA7B-15");
	nlist--;	/* TA7B-15 was last */
	CHECK(mapnav_station_find(get_station, nlist, h, &st) < 0);
	CHECK(strcmp(name_of(mapnav_station_next(get_station, nlist, la, lo, h)), "TA1ABC-9") == 0);
	/* two at the same spot: both visited, then round */
	add_station("TA9Z-1", true, 39.95, 32.88, "", MAPNAV_ALT_NONE);
	h = 0;
	int seen = 0, twin = 0;
	for (int k = 0; k < 4; k++) {
		h = mapnav_station_next(get_station, nlist, la, lo, h);
		seen |= 1 << (strcmp(name_of(h), "TA1ABC-9") == 0 ? 0 : strcmp(name_of(h), "TA9Z-1") == 0 ? 1 : 2);
		twin += k < 2 && (strcmp(name_of(h), "TA1ABC-9") == 0 || strcmp(name_of(h), "TA9Z-1") == 0);
	}
	CHECK(seen == 7 && twin == 2);
	CHECK(strcmp(name_of(mapnav_station_next(get_station, nlist, la, lo, h)), "TA1ABC-9") == 0 ||
	      strcmp(name_of(mapnav_station_next(get_station, nlist, la, lo, h)), "TA9Z-1") == 0);

	/* the comment without its altitude */
	char c[24];
	mapnav_comment("Hello /A=001234 mobile", c, sizeof c);
	CHECK(strcmp(c, "Hello mobile") == 0);
	mapnav_comment("a   b", c, sizeof c);
	CHECK(strcmp(c, "a b") == 0);
	mapnav_comment("/A=000123", c, sizeof c);
	CHECK(strcmp(c, "") == 0);
	mapnav_comment("  Digi on 144.800  ", c, sizeof c);
	CHECK(strcmp(c, "Digi on 144.800") == 0);
	mapnav_comment("abcdefghijklmnopqrstuvwxyz", c, 8);
	CHECK(strcmp(c, "abcdefg") == 0);
	mapnav_comment("x/A=-00012y", c, sizeof c);
	CHECK(strcmp(c, "xy") == 0);
}

/* --- Track --- */

static void test_track(void)
{
	track_t t;
	int32_t la, lo;
	track_clear(&t);
	CHECK(track_count(&t) == 0 && !track_start(&t, &la, &lo) && !track_get(&t, 0, &la, &lo));
	/* the first fix at once, kept exactly as the start */
	CHECK(track_add(&t, E7(39.92077), E7(32.85411), 1000));
	CHECK(track_count(&t) == 1 && track_start(&t, &la, &lo) && la == E7(39.92077) && lo == E7(32.85411));
	CHECK(track_get(&t, 0, &la, &lo) && la == E7(39.921) && lo == E7(32.854));	/* the nearest 0.001 degree */
	/* not before 30 s, however far */
	CHECK(!track_add(&t, E7(40.5), E7(33.0), 1000 + TRACK_EVERY_MS - 1));
	/* 30 s on but under 50 m from where the last point was taken (GPS jitter while standing) */
	CHECK(!track_add(&t, E7(39.92077 + 0.0004), E7(32.85411), 1000 + TRACK_EVERY_MS));
	/* 50 m and more (67 m north) but still in the same 0.001 degree cell: waits */
	CHECK(!track_add(&t, E7(39.92077 + 0.0006), E7(32.85411), 1000 + 2 * TRACK_EVERY_MS));
	/* moved on: kept */
	CHECK(track_add(&t, E7(39.9230), E7(32.8541), 1000 + 3 * TRACK_EVERY_MS));
	CHECK(track_count(&t) == 2 && track_get(&t, 1, &la, &lo) && la == E7(39.923));
	/* the 30 s count from the last kept point */
	CHECK(!track_add(&t, E7(39.93), E7(32.8541), 1000 + 4 * TRACK_EVERY_MS - 1));
	/* a drive north, 0.002 degree a minute for 100 points: the last TRACK_POINTS kept, oldest first */
	uint32_t now = 1000 + 4 * TRACK_EVERY_MS;
	for (int i = 0; i < 100; i++, now += 60000u)
		CHECK(track_add(&t, E7(39.93 + 0.002 * i), E7(32.86), now));
	CHECK(track_count(&t) == TRACK_POINTS);
	for (uint8_t i = 0; i < TRACK_POINTS; i++) {
		CHECK(track_get(&t, i, &la, &lo));
		CHECK(abs(la - E7(39.93 + 0.002 * (100 - TRACK_POINTS + i))) <= 1);
	}
	CHECK(!track_get(&t, TRACK_POINTS, &la, &lo));
	/* the start survives the wrap */
	CHECK(track_start(&t, &la, &lo) && la == E7(39.92077) && lo == E7(32.85411));
	/* outside the int16 range (Boston): nothing kept, nothing started */
	track_clear(&t);
	CHECK(!track_add(&t, E7(42.36), E7(-71.06), 5000) && track_count(&t) == 0 && !track_start(&t, &la, &lo));
	CHECK(track_add(&t, E7(41.0), E7(29.0), 5000) && track_count(&t) == 1);
	/* after a clear the next fix is the new start */
	track_clear(&t);
	CHECK(track_add(&t, E7(38.0), E7(27.0), 9000) && track_start(&t, &la, &lo) && la == E7(38.0));
	/* standing on a cell's edge: GPS jitter of 11 m crosses into the next cell, under 50 m: nothing */
	track_clear(&t);
	CHECK(track_add(&t, E7(39.92149), E7(32.854), 100));
	CHECK(!track_add(&t, E7(39.92159), E7(32.854), 100 + TRACK_EVERY_MS));
	CHECK(!track_add(&t, E7(39.92139), E7(32.854), 100 + 2 * TRACK_EVERY_MS));
	CHECK(track_count(&t) == 1);
	/* the time wraps (uint32 ms after 49 days) */
	track_clear(&t);
	CHECK(track_add(&t, E7(38.0), E7(27.0), 0xFFFFF000u));
	CHECK(track_add(&t, E7(38.01), E7(27.0), 0xFFFFF000u + TRACK_EVERY_MS));
	CHECK(track_count(&t) == 2);
}

/* The track drawn into a framebuffer through the map renderer */
#define FB_W 160
#define FB_H 128
typedef struct {
	uint8_t px[FB_H][FB_W];
	int x0, y0, x1, y1;
	int outside, segs, wrong_province, dots;
} fb_t;

static void fb_seg(void *ctx, int x0, int y0, int x1, int y1, uint8_t province)
{
	fb_t *fb = ctx;
	fb->segs++;
	if (province != 0xFFu)
		fb->wrong_province++;
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
	for (unsigned k = 0;; k++) {
		if (x0 < fb->x0 || x0 > fb->x1 || y0 < fb->y0 || y0 > fb->y1) {
			fb->outside++;
			return;
		}
		if (k % 2 == 0 && !fb->px[y0][x0]) {	/* dotted, as the screen draws it */
			fb->px[y0][x0] = 1;
			fb->dots++;
		}
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * e;
		if (e2 >= dy) {
			e += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			e += dx;
			y0 += sy;
		}
	}
}

static void test_track_draw(void)
{
	track_t t;
	fb_t fb;
	map_view_t v;
	track_clear(&t);
	/* an L: 20 points east along 39.93 N from 32.80 E 0.004 degree apart (340 m), then 20 north 0.002 apart (220 m) */
	uint32_t now = 0;
	for (int i = 0; i < 20; i++, now += 60000u)
		CHECK(track_add(&t, E7(39.93), E7(32.80 + 0.004 * i), now));
	for (int i = 1; i <= 20; i++, now += 60000u)
		CHECK(track_add(&t, E7(39.93 + 0.002 * i), E7(32.876), now));
	CHECK(track_count(&t) == 40);
	/* zoom 6 (0.0016 degree a pixel): the whole L inside a 160 x 128 view at (0, 0), each step a pixel or more */
	map_view_init(&v, 0, 0, FB_W, FB_H);
	map_view_set(&v, E7(39.95), E7(32.84), 6);
	memset(&fb, 0, sizeof fb);
	fb.x1 = FB_W - 1;
	fb.y1 = FB_H - 1;
	uint16_t n = track_draw(&v, &t, fb_seg, &fb);
	CHECK(n == 39 && fb.segs == 39 && fb.outside == 0 && fb.wrong_province == 0);
	CHECK(fb.dots > 20);
	/* the corner point's pixel is on the track; a pixel far from it is not */
	int x, y;
	CHECK(map_project(&v, E7(39.93), E7(32.876), &x, &y));
	int near = 0;
	for (int dy = -1; dy <= 1; dy++)
		for (int dx = -1; dx <= 1; dx++)
			if (y + dy >= 0 && y + dy < FB_H && x + dx >= 0 && x + dx < FB_W)
				near += fb.px[y + dy][x + dx];
	CHECK(near >= 1);
	CHECK(fb.px[5][5] == 0);
	/* dotted: along the east leg at most two pixels in a row (where two segments meet), at most 3 in 4 lit */
	int xa = 0, xb = 0;
	CHECK(map_project(&v, E7(39.93), E7(32.80), &xa, &y) && map_project(&v, E7(39.93), E7(32.876), &xb, &y));
	int run = 0, max_run = 0, lit = 0;
	for (int xx = xa; xx <= xb; xx++) {
		run = fb.px[y][xx] ? run + 1 : 0;
		lit += fb.px[y][xx];
		max_run = run > max_run ? run : max_run;
	}
	CHECK(xb - xa > 30 && max_run <= 2 && lit * 4 <= (xb - xa + 1) * 3 && lit * 3 >= xb - xa);
	/* a small offset viewport the L crosses (left and top): everything stays inside, segments clipped */
	map_view_init(&v, 20, 10, 40, 30);
	map_view_set(&v, E7(39.93), E7(32.876), 6);
	memset(&fb, 0, sizeof fb);
	fb.x0 = 20;
	fb.y0 = 10;
	fb.x1 = 59;
	fb.y1 = 39;
	n = track_draw(&v, &t, fb_seg, &fb);
	CHECK(n > 5 && n < 39 && fb.outside == 0);
	/* far from it (Istanbul): nothing */
	map_view_set(&v, E7(41.0), E7(29.0), 6);
	memset(&fb, 0, sizeof fb);
	fb.x0 = 20;
	fb.y0 = 10;
	fb.x1 = 59;
	fb.y1 = 39;
	CHECK(track_draw(&v, &t, fb_seg, &fb) == 0 && fb.segs == 0);
	/* zoom 0: the whole L on a pixel or two, nothing outside */
	map_view_init(&v, 0, 0, FB_W, FB_H);
	memset(&fb, 0, sizeof fb);
	fb.x1 = FB_W - 1;
	fb.y1 = FB_H - 1;
	n = track_draw(&v, &t, fb_seg, &fb);
	CHECK(n <= 3 && fb.outside == 0);
	/* one point only: no segment */
	track_clear(&t);
	track_add(&t, E7(39.93), E7(32.80), 0);
	CHECK(track_draw(&v, &t, fb_seg, &fb) == 0);
}

/* --- Auto repeater: a synthetic table, sorted by RX frequency as the generator makes it --- */

#define P(la, lo) (int32_t)((la) * 1e5 + 0.5), (int32_t)((lo) * 1e5 + 0.5)
const rpt_entry_t rpt_table[] = {
	{ P(39.93, 32.86), 145600000, -480, 885, RPT_FM, 0, "TA2RAK", "Ankara", RPT_PROV_UNKNOWN },		/* 0: Ankara centre */
	{ P(39.80, 32.60), 145625000, -480, 0, RPT_FM, 0, "TA2RGB", "Golbasi", RPT_PROV_UNKNOWN },		/* 1: 26 km SW */
	{ P(40.20, 32.86), 145650000, -480, 1000, RPT_FM | RPT_NARROW, 0, "TA2RKZ", "Kazan", RPT_PROV_UNKNOWN },	/* 2: 30 km N */
	{ P(39.94, 32.87), 145675000, -480, 0, RPT_C4FM, 0, "TA2YSF", "Ankara", RPT_PROV_UNKNOWN },		/* 3: C4FM (refused) */
	{ RPT_NOPOS, RPT_NOPOS, 145700000, -480, 0, RPT_FM, 0, "TA2NOP", "", RPT_PROV_UNKNOWN },			/* 4: no position */
	{ P(39.931, 32.861), 145725000, 0, 0, RPT_FM, 0, "TA2SPX", "Ankara", RPT_PROV_UNKNOWN },			/* 5: simplex, 150 m */
	{ P(39.85, 33.52), 145750000, -480, 0, RPT_FM, 0, "TA2RKK", "Kirikkale", RPT_PROV_UNKNOWN },		/* 6: 57 km E */
	{ P(39.95, 32.88), 145787500, -480, 0, RPT_FM, 0, "TA2RCK", "Cankaya", RPT_PROV_UNKNOWN },		/* 7: 2.8 km NE */
	{ P(39.92, 32.85), 439100000, -6080, 0, RPT_DMR, 1 | 3 << 4, "TA2DMR", "Ankara", RPT_PROV_UNKNOWN },	/* 8: DMR */
	{ P(39.40, 32.85), 439200000, -6080, 0, RPT_DMR, 2 | 2 << 4, "TA2DMS", "Haymana", RPT_PROV_UNKNOWN },	/* 9: DMR, 59 km S */
	{ P(39.92, 32.84), 1297000000, -24000, 0, RPT_FM, 0, "TA2SHF", "Ankara", RPT_PROV_UNKNOWN },		/* 10: 23 cm: not in band */
};
const rpt_info_t rpt_info = { sizeof rpt_table / sizeof rpt_table[0], 0, "2026-11-19", "test" };

/* The radio's bands: 136-174 and 400-480 MHz; TX on 145.725 refused (a "receive only" corner case) */
static bool bands(uint32_t hz, bool need_tx)
{
	if (need_tx && hz == 145725000u)
		return false;
	return (hz >= 136000000u && hz <= 174000000u) || (hz >= 400000000u && hz <= 480000000u);
}

static bool no_bands(uint32_t hz, bool need_tx)
{
	(void)hz;
	(void)need_tx;
	return false;
}

static const char *rname(uint16_t i)
{
	return i < rpt_info.count ? rpt_table[i].name : "none";
}

#define ACHECK(s, fix, la, lo, dmr, tuned, now, want) do { \
	uint16_t got_ = arpt_check(s, fix, E7(la), E7(lo), dmr, bands, tuned, now); \
	if (got_ != (want)) \
		printf("  arpt at %.3f %.3f t=%u: got %s want %s\n", (double)(la), (double)(lo), (unsigned)(now), \
		       rname(got_), rname(want)); \
	CHECK(got_ == (want)); \
} while (0)

static void test_auto_repeater(void)
{
	/* suitability */
	CHECK(arpt_suitable(&rpt_table[0], false, bands));
	CHECK(!arpt_suitable(&rpt_table[0], true, bands));		/* FM on a DMR channel */
	CHECK(arpt_suitable(&rpt_table[8], true, bands) && !arpt_suitable(&rpt_table[8], false, bands));
	CHECK(!arpt_suitable(&rpt_table[3], false, bands) && !arpt_suitable(&rpt_table[3], true, bands));	/* C4FM */
	CHECK(!arpt_suitable(&rpt_table[4], false, bands));	/* no position */
	CHECK(!arpt_suitable(&rpt_table[5], false, bands));	/* its TX is refused */
	CHECK(!arpt_suitable(&rpt_table[10], false, bands));	/* outside the bands */
	/* the nearest suitable: not the C4FM 1 km away, not the simplex 150 m away, not the 23 cm one */
	CHECK(arpt_best(E7(39.931), E7(32.861), false, bands) == 0);
	CHECK(arpt_best(E7(39.931), E7(32.861), true, bands) == 8);
	CHECK(arpt_best(E7(39.20), E7(32.86), true, bands) == 9);
	CHECK(arpt_best(E7(39.931), E7(32.861), false, no_bands) == RPT_NONE);

	arpt_t s;
	memset(&s, 0, sizeof s);
	s.cur = RPT_NONE;
	/* off: nothing, ever */
	ACHECK(&s, true, 39.93, 32.86, false, 0, 1000, RPT_NONE);
	/* on in Ankara: the nearest at once */
	arpt_set(&s, true, 5000);
	ACHECK(&s, true, 39.93, 32.86, false, 0, 5000, 0);
	/* the same place: nothing again, neither before 30 s nor after (no spam) */
	ACHECK(&s, true, 39.93, 32.86, false, 0, 6000, RPT_NONE);
	ACHECK(&s, true, 39.93, 32.86, false, 0, 5000 + ARPT_EVERY_MS - 1, RPT_NONE);
	ACHECK(&s, true, 39.93, 32.86, false, 0, 5000 + ARPT_EVERY_MS, RPT_NONE);
	for (uint32_t k = 2; k < 20; k++)
		ACHECK(&s, true, 39.93, 32.86, false, 0, 5000 + k * ARPT_EVERY_MS, RPT_NONE);
	uint32_t now = 5000 + 20 * ARPT_EVERY_MS;
	/* far south: TA2RGB 31 km against TA2RAK's 37 km, 5 km nearer but only 15 %: no offer */
	ACHECK(&s, true, 39.60, 32.86, false, 0, now, RPT_NONE);
	now += ARPT_EVERY_MS;
	ACHECK(&s, true, 39.93, 32.86, false, 0, now, RPT_NONE);
	now += ARPT_EVERY_MS;
	/* driving south-west towards Golbasi: past the halfway point TA2RGB is nearer, but not yet 20 % nearer */
	ACHECK(&s, true, 39.862, 32.725, false, 0, now, RPT_NONE);	/* about 13.5 vs 13.0 km */
	now += ARPT_EVERY_MS;
	/* clearly nearer (well under 80 % and more than 2 km): offered */
	ACHECK(&s, true, 39.83, 32.66, false, 0, now, 1);
	CHECK(s.cur == 1);
	/* back towards the middle: Ankara is nearer again but not by 20 %: no flip-flop */
	now += ARPT_EVERY_MS;
	ACHECK(&s, true, 39.866, 32.73, false, 0, now, RPT_NONE);
	now += ARPT_EVERY_MS;
	/* the check is not due before 30 s, wherever we are */
	ACHECK(&s, true, 39.93, 32.86, false, 0, now - 1, RPT_NONE);
	/* home again: Ankara offered again (it is not the one offered last) */
	ACHECK(&s, true, 39.93, 32.86, false, 0, now, 0);
	now += ARPT_EVERY_MS;
	/* 20 % nearer but not 2 km: TA2RCK is 2.8 km from TA2RAK; 1.0 km from it and 1.8 km from TA2RAK: no offer */
	ACHECK(&s, true, 39.9429, 32.8729, false, 0, now, RPT_NONE);
	now += ARPT_EVERY_MS;
	/* past it, 2.8 against 5.6 km: offered */
	ACHECK(&s, true, 39.97, 32.90, false, 0, now, 7);
	now += ARPT_EVERY_MS;
	/* the channel goes DMR: the FM one offered before no longer counts, the DMR one at once */
	ACHECK(&s, true, 39.931, 32.861, true, 0, now, 8);
	/* tuned to it already (a fresh start on DMR): no offer, but it is the current one */
	arpt_set(&s, true, now);
	ACHECK(&s, true, 39.931, 32.861, true, 439100000, now, RPT_NONE);
	CHECK(s.cur == 8);
	now += ARPT_EVERY_MS;
	/* driving south on DMR: Haymana clearly nearer: offered */
	ACHECK(&s, true, 39.45, 32.85, true, 439100000, now, 9);
	/* no fix: nothing, and the next check is still 30 s on */
	now += ARPT_EVERY_MS;
	ACHECK(&s, false, 0, 0, true, 0, now, RPT_NONE);
	ACHECK(&s, true, 39.93, 32.86, true, 0, now + 1, RPT_NONE);
	ACHECK(&s, true, 39.93, 32.86, true, 0, now + ARPT_EVERY_MS, 8);
	/* nothing in the bands: never an offer */
	now += 2 * ARPT_EVERY_MS;
	arpt_set(&s, true, now);
	CHECK(arpt_check(&s, true, E7(39.93), E7(32.86), false, no_bands, 0, now) == RPT_NONE && s.cur == RPT_NONE);
	/* off again: nothing */
	arpt_set(&s, false, 0);
	ACHECK(&s, true, 39.93, 32.86, false, 0, 0, RPT_NONE);
	/* ms counter wrap: due checks still run */
	arpt_set(&s, true, 0xFFFFFFF0u);
	ACHECK(&s, true, 39.93, 32.86, false, 0, 0xFFFFFFF0u, 0);
	ACHECK(&s, true, 39.83, 32.66, false, 0, 0xFFFFFFF0u + ARPT_EVERY_MS, 1);
}

int main(void)
{
	/* the tables are the map pack's on the data chip since 1.1 (refpack_fixture.h) */
	if (!refpack_fixture_install(REFPACK_MAP | REFPACK_RPT)) {
		printf("FAIL: the map pack fixture\n");
		return 1;
	}
	test_math();
	test_stations();
	test_track();
	test_track_draw();
	test_auto_repeater();
	printf("Plus map navigation: cos/distance, APRS station pick, track ring and drawing, auto repeater: %d checks, "
	       "%d failures\n", checks, failures);
	return failures != 0;
}
