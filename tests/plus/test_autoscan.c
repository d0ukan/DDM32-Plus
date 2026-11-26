/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus auto airfield scan and auto repeater scan (docs/autoscan-design.md): the selection (autoscan.c) on the
 * map pack made from the build's tables (tests/plus/refpack_fixture.c on the simulated data chip), against brute force
 * written here from the same pack:
 *   - the nearest repeaters (FM, DMR; a position; RX in band) within 60 km, nearest first, at most six, at positions
 *     all over the country, and the places with none or one (found by a grid search);
 *   - the nearest airfields with a frequency to watch within 100 km, their frequencies round robin by use (TWR, APP /
 *     DEP, AFIS / INFO / CTAF, GND, the rest), never an ATIS, never one frequency twice;
 *   - the refresh (as_merge): fills, keeps, replaces the farthest only when 20 % and 2 km nearer, drops one out of
 *     reach; a drive along a road never flaps; the ring's order FM before DMR (as_group);
 *   - when a refresh is due (as_due): the first one, 30 s and 1 km (3 km for airfields), no fix, a new map pack, a
 *     ring that should be there. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/autoscan.h"
#include "ui/rpt.h"
#include "ui/airfield.h"
#include "aprs/aprs_fmt.h"
#include "storage/ref_pack.h"
#include "refpack_fixture.h"

static int checks, fails;
static char tag[96];
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #c, tag); } } while (0)

/* The DM-32UV's receive bands (VHF, UHF) without the air band; the air band */
static bool band_rpt(uint32_t hz) { return (hz >= 136000000u && hz <= 174000000u) || (hz >= 400000000u && hz <= 480000000u); }
static bool band_air(uint32_t hz) { return hz >= 108000000u && hz < 137000000u; }

static int32_t e7(double deg) { return (int32_t)lround(deg * 1e7); }

/* ---- Repeaters ---- */

typedef struct { uint16_t i; uint32_t d; } ref_t;

/* Brute force: every FM / DMR repeater with a position, its RX in band, within radius, by (distance, index) */
static int brute_rpt(int32_t lat, int32_t lon, uint32_t radius, ref_t out[], int max)
{
	int n = 0;
	for (uint16_t i = 0; i < rpt_count(); i++) {
		const rpt_entry_t *e = rpt_get(i);
		uint8_t m = e->mode & RPT_MODE_MASK;
		if ((m != RPT_FM && m != RPT_DMR) || e->lat_e5 == RPT_NOPOS || !band_rpt(e->rx_hz))
			continue;
		uint32_t d = aprs_distance_m(lat, lon, e->lat_e5 * 100, e->lon_e5 * 100);
		if (d > radius)
			continue;
		int at = n;
		while (at > 0 && out[at - 1].d > d)
			at--;
		if (at >= max)
			continue;
		memmove(&out[at + 1], &out[at], (size_t)((n < max ? n : max - 1) - at) * sizeof out[0]);
		out[at] = (ref_t){ i, d };
		if (n < max)
			n++;
	}
	return n;
}

static void pick_rpt_at(double la, double lo, const char *where)
{
	as_pick_t got[AS_MAX];
	ref_t want[AS_MAX];
	snprintf(tag, sizeof tag, "repeaters at %s", where);
	int32_t lat = e7(la), lon = e7(lo);
	uint8_t n = as_pick_rpt(lat, lon, AS_RPT_RADIUS_M, band_rpt, got, AS_MAX);
	int w = brute_rpt(lat, lon, AS_RPT_RADIUS_M, want, AS_MAX);
	CHECK(n == w);
	for (int k = 0; k < n && k < w; k++) {
		CHECK(got[k].id == want[k].i && got[k].d_m == want[k].d && got[k].d_m <= AS_RPT_RADIUS_M);
		uint8_t m = rpt_get(got[k].id)->mode & RPT_MODE_MASK;
		CHECK(got[k].mode == (m == RPT_DMR ? AS_M_DMR : AS_M_FM));
		CHECK(as_rpt_dist(got[k].id, lat, lon) == got[k].d_m);
	}
}

static void test_pick_rpt(void)
{
	static const struct { double la, lo; const char *name; } at[] = {
		{ 39.9208, 32.8541, "Ankara" }, { 41.0082, 28.9784, "Istanbul" }, { 38.4237, 27.1428, "Izmir" },
		{ 36.8969, 30.7133, "Antalya" }, { 37.0000, 35.3213, "Adana" }, { 40.1885, 29.0610, "Bursa" },
		{ 41.0015, 39.7178, "Trabzon" }, { 37.9144, 40.2306, "Diyarbakir" }, { 38.7312, 35.4787, "Kayseri" },
	};
	for (size_t k = 0; k < sizeof at / sizeof at[0]; k++)
		pick_rpt_at(at[k].la, at[k].lo, at[k].name);
	/* A grid over the country: the selection and brute force agree everywhere; places with none and with one */
	int none = 0, one = 0, full = 0;
	for (double la = 36.0; la <= 42.0; la += 0.5)
		for (double lo = 26.0; lo <= 45.0; lo += 0.5) {
			char w[32];
			snprintf(w, sizeof w, "%.1f,%.1f", la, lo);
			pick_rpt_at(la, lo, w);
			as_pick_t got[AS_MAX];
			uint8_t n = as_pick_rpt(e7(la), e7(lo), AS_RPT_RADIUS_M, band_rpt, got, AS_MAX);
			none += n == 0;
			one += n == 1;
			full += n == AS_MAX;
		}
	snprintf(tag, sizeof tag, "repeater grid");
	CHECK(none > 0 && full > 0);
	printf("auto scan: repeaters nearest first as brute force at 9 cities and %d grid points (none %d, one %d, six %d)\n",
	       13 * 39, none, one, full);
}

/* ---- Airfields ---- */

static uint8_t rank_of(uint8_t type)
{
	switch (type) {
	case AIR_ATIS: return 0xFF;
	case AIR_TWR: return 0;
	case AIR_APP: case AIR_DEP: return 1;
	case AIR_AFIS: case AIR_INFO: case AIR_CTAF: return 2;
	case AIR_GND: return 3;
	default: return 4;
	}
}

/* Brute force: the frequencies of fields[] round robin, each field's in (rank, its order), a frequency once */
static int brute_air(const as_pick_t fields[], int nf, uint16_t out[], uint32_t hz[], int max)
{
	int n = 0, taken[AS_MAX] = { 0 };
	uint16_t order[AS_MAX][64];
	int cnt[AS_MAX] = { 0 };
	for (int f = 0; f < nf; f++) {
		const air_field_t *a = air_get(fields[f].id);
		uint16_t first = a->first;
		int c = a->count > 64 ? 64 : a->count;
		for (int r = 0; r <= 4; r++)
			for (int j = 0; j < c; j++) {
				air_freq_t q;
				if (air_freq_get((uint32_t)first + (uint32_t)j, &q) && rank_of(q.type) == r)
					order[f][cnt[f]++] = (uint16_t)j;
			}
	}
	for (bool more = true; more && n < max;) {
		more = false;
		for (int f = 0; f < nf && n < max; f++) {
			const air_field_t *a = air_get(fields[f].id);
			uint16_t first = a->first;
			while (taken[f] < cnt[f]) {
				air_freq_t q;
				uint16_t j = order[f][taken[f]++];
				air_freq_get((uint32_t)first + j, &q);
				uint32_t h = air_freq_hz(&q);
				bool dup = !band_air(h);
				for (int p = 0; p < n; p++)
					dup = dup || hz[p] == h;
				if (dup)
					continue;
				out[n] = (uint16_t)(fields[f].id << 6 | j);
				hz[n++] = h;
				more = true;
				break;
			}
		}
	}
	return n;
}

static void pick_air_at(double la, double lo, const char *where, int *letters)
{
	as_pick_t fields[AS_MAX], got[AS_MAX];
	uint16_t want[AS_MAX];
	uint32_t whz[AS_MAX];
	snprintf(tag, sizeof tag, "airfields at %s", where);
	int32_t lat = e7(la), lon = e7(lo);
	uint8_t nf = as_pick_fields(lat, lon, AS_AIR_RADIUS_M, band_air, fields, AS_MAX);
	/* the fields: nearest first, within reach, each with a frequency that isn't an ATIS */
	for (uint8_t f = 0; f < nf; f++) {
		CHECK(fields[f].d_m <= AS_AIR_RADIUS_M && (f == 0 || fields[f - 1].d_m <= fields[f].d_m));
		CHECK(as_field_dist(fields[f].id, lat, lon) == fields[f].d_m);
	}
	for (uint16_t i = 0; i < air_nfields(); i++) {	/* none nearer left out */
		const air_field_t *a = air_get(i);
		uint32_t d = aprs_distance_m(lat, lon, a->lat_e5 * 100, a->lon_e5 * 100);
		bool any = false;
		for (int j = 0; j < a->count && !any; j++) {
			air_freq_t q;
			any = air_freq_get((uint32_t)a->first + (uint32_t)j, &q) && q.type != AIR_ATIS;
		}
		bool in = false;
		for (uint8_t f = 0; f < nf; f++)
			in = in || fields[f].id == i;
		if (any && d <= AS_AIR_RADIUS_M && !in)
			CHECK(nf == AS_MAX && d >= fields[nf - 1].d_m);
	}
	uint8_t n = as_air_letters(fields, nf, band_air, got, AS_MAX);
	int w = brute_air(fields, nf, want, whz, AS_MAX);
	CHECK(n == w);
	for (int k = 0; k < n && k < w; k++) {
		air_freq_t q;
		const air_field_t *a = air_get((uint16_t)(got[k].id >> 6));
		CHECK(got[k].id == want[k] && air_freq_get((uint32_t)a->first + (got[k].id & 63u), &q) && q.type != AIR_ATIS);
		CHECK(got[k].mode == AS_M_AM && air_freq_hz(&q) == whz[k]);
	}
	if (nf > 0)	/* the nearest field's best first */
		CHECK(n > 0 && (got[0].id >> 6) == fields[0].id);
	*letters += n;
}

static void test_pick_air(void)
{
	int letters = 0;
	pick_air_at(36.8969, 30.7133, "Antalya", &letters);
	pick_air_at(40.1281, 32.9951, "Esenboga", &letters);
	pick_air_at(41.2753, 28.7519, "Istanbul Airport", &letters);
	pick_air_at(39.9208, 32.8541, "Ankara", &letters);
	for (double la = 36.0; la <= 42.0; la += 1.0)
		for (double lo = 26.0; lo <= 45.0; lo += 1.0) {
			char w[32];
			snprintf(w, sizeof w, "%.1f,%.1f", la, lo);
			pick_air_at(la, lo, w, &letters);
		}
	snprintf(tag, sizeof tag, "airfields");
	CHECK(letters > 100);
	printf("auto scan: airfields and their frequencies round robin as brute force (%d letters, no ATIS, none twice)\n",
	       letters);
}

/* ---- The refresh ---- */

static as_pick_t P(uint16_t id, uint8_t mode, uint32_t d) { return (as_pick_t){ id, mode, d }; }

static void test_merge(void)
{
	snprintf(tag, sizeof tag, "merge");
	as_pick_t cur[AS_MAX], cand[AS_MAX];
	/* Empty: the candidates in their order */
	cand[0] = P(7, AS_M_FM, 3000);
	cand[1] = P(3, AS_M_DMR, 9000);
	cand[2] = P(5, AS_M_FM, 20000);
	uint8_t n = as_merge(cur, 0, cand, 3, 66000);
	CHECK(n == 3 && cur[0].id == 7 && cur[1].id == 3 && cur[2].id == 5);
	/* Full: a new one not clearly nearer than the farthest (60 km) stays out; 47 km is (20 % and 2 km) */
	for (uint8_t k = 0; k < AS_MAX; k++)
		cur[k] = P((uint16_t)(10 + k), AS_M_FM, 10000u * (k + 1u));
	cand[0] = P(99, AS_M_FM, 49000);
	n = as_merge(cur, AS_MAX, cand, 1, 66000);
	CHECK(n == AS_MAX && cur[5].id == 15);
	cand[0] = P(99, AS_M_FM, 47000);
	n = as_merge(cur, AS_MAX, cand, 1, 66000);
	CHECK(n == AS_MAX && cur[5].id == 99 && cur[0].id == 10 && cur[4].id == 14);	/* the farthest's place */
	/* A letter on the list keeps its place, its distance updated */
	cand[0] = P(12, AS_M_FM, 5000);
	n = as_merge(cur, AS_MAX, cand, 1, 66000);
	CHECK(n == AS_MAX && cur[2].id == 12 && cur[2].d_m == 5000);
	/* Out of reach (or gone from the pack): the letter goes, the others close up in order */
	cur[1].d_m = 67000;
	cur[3].d_m = UINT32_MAX;
	n = as_merge(cur, AS_MAX, cand, 0, 66000);
	CHECK(n == 4 && cur[0].id == 10 && cur[1].id == 12 && cur[2].id == 14 && cur[3].id == 99);
	/* A short distance: 2 km rule (1.5 km vs 3 km is 50 % nearer but only 1.5 km) */
	for (uint8_t k = 0; k < AS_MAX; k++)
		cur[k] = P((uint16_t)(20 + k), AS_M_FM, 500u * (k + 1u));
	cand[0] = P(98, AS_M_FM, 1500);
	n = as_merge(cur, AS_MAX, cand, 1, 66000);
	CHECK(n == AS_MAX && cur[5].id == 25);
	/* The ring's order: FM ones first, DMR after, each in its order */
	as_pick_t g[5] = { P(1, AS_M_DMR, 1), P(2, AS_M_FM, 2), P(3, AS_M_DMR, 3), P(4, AS_M_FM, 4), P(5, AS_M_AM, 5) };
	as_group(g, 5);
	CHECK(g[0].id == 2 && g[1].id == 4 && g[2].id == 5 && g[3].id == 1 && g[4].id == 3);
	printf("auto scan: the refresh fills, keeps, replaces the farthest only 20 %% and 2 km nearer, drops one out of reach; "
	       "FM before DMR\n");
}

/* A drive from Ankara to Kirikkale and back, a refresh every 1 km: the letters change rarely, never back and forth */
static void test_drive(void)
{
	snprintf(tag, sizeof tag, "drive");
	as_pick_t cur[AS_MAX], cand[AS_MAX];
	uint8_t n = 0;
	uint16_t prev_ids[AS_MAX] = { 0 };
	uint8_t prev_n = 0;
	int changes = 0, flips = 0, steps = 0;
	static uint16_t gone[64];
	int ngone = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int s = 0; s <= 70; s++, steps++) {
			double t = (pass == 0 ? s : 70 - s) / 70.0;
			int32_t lat = e7(39.9208 + t * (39.8468 - 39.9208)), lon = e7(32.8541 + t * (33.5153 - 32.8541));
			for (uint8_t k = 0; k < n; k++)
				cur[k].d_m = as_rpt_dist(cur[k].id, lat, lon);
			uint8_t nc = as_pick_rpt(lat, lon, AS_RPT_RADIUS_M, band_rpt, cand, AS_MAX);
			n = as_merge(cur, n, cand, nc, AS_RPT_RADIUS_M / 100u * AS_KEEP_PCT);
			as_group(cur, n);
			for (uint8_t k = 0; k < n; k++) {
				CHECK(cur[k].d_m <= AS_RPT_RADIUS_M / 100u * AS_KEEP_PCT);
				for (uint8_t q = 0; q < k; q++)
					CHECK(cur[q].id != cur[k].id);
			}
			if (pass == 0)
				for (uint8_t k = 1; k < n; k++)
					CHECK(cur[k - 1].mode <= cur[k].mode || cur[k - 1].mode != AS_M_DMR);
			/* a repeater that left and came back within the same pass would be a flap */
			for (uint8_t q = 0; q < prev_n; q++) {
				bool kept = false;
				for (uint8_t k = 0; k < n; k++)
					kept = kept || cur[k].id == prev_ids[q];
				if (!kept && ngone < 64)
					gone[ngone++] = prev_ids[q];
			}
			for (uint8_t k = 0; k < n; k++) {
				bool was = false;
				for (uint8_t q = 0; q < prev_n; q++)
					was = was || prev_ids[q] == cur[k].id;
				if (was)
					continue;
				changes++;
				for (int g = 0; g < ngone && pass == 0; g++)
					flips += gone[g] == cur[k].id;
			}
			if (pass == 1 && s == 0)
				ngone = 0;
			prev_n = n;
			for (uint8_t k = 0; k < n; k++)
				prev_ids[k] = cur[k].id;
		}
	CHECK(flips == 0 && changes < steps);
	printf("auto scan: a 2 x 70 km drive, a refresh each km: %d letters came in, none left and came back\n", changes);
}

static void test_due(void)
{
	snprintf(tag, sizeof tag, "due");
	as_state_t s;
	memset(&s, 0, sizeof s);
	int32_t la = e7(39.92), lo = e7(32.85);
	CHECK(as_due(&s, true, la, lo, 0, 1, false) == AS_DUE_NO);	/* off */
	s.kind = AS_RPT;
	CHECK(as_due(&s, false, la, lo, 0, 1, false) == AS_DUE_NO);	/* no fix: waits */
	CHECK(as_due(&s, true, la, lo, 0, 1, false) == AS_DUE_YES);	/* the first one at once */
	s.picked = true;
	s.lat_e7 = la;
	s.lon_e7 = lo;
	s.next_ms = 30000;
	s.gen = 1;
	int32_t near = e7(39.92 + 0.005), far = e7(39.92 + 0.012), farther = e7(39.92 + 0.03);	/* 0.56, 1.3, 3.3 km */
	CHECK(as_due(&s, true, far, lo, 20000, 1, false) == AS_DUE_NO);	/* moved, but not 30 s yet */
	CHECK(as_due(&s, true, near, lo, 40000, 1, false) == AS_DUE_NO);	/* 30 s, but 0.6 km */
	CHECK(as_due(&s, true, far, lo, 40000, 1, false) == AS_DUE_YES);
	CHECK(as_due(&s, true, la, lo, 40000, 1, true) == AS_DUE_YES);	/* a ring that should be there */
	CHECK(as_due(&s, true, la, lo, 20000, 2, false) == AS_DUE_FULL);	/* a new map pack: at once */
	CHECK(as_due(&s, false, la, lo, 20000, 2, false) == AS_DUE_NO);	/* ... once there is a fix */
	s.next_ms = 0xFFFFF000u;	/* the clock wraps: 30 s after a time just before the wrap */
	CHECK(as_due(&s, true, far, lo, 0xFFFFF000u + 30000u, 1, false) == AS_DUE_YES);
	CHECK(as_due(&s, true, far, lo, 0xFFFFE000u, 1, false) == AS_DUE_NO);
	s.next_ms = 30000;
	s.kind = AS_AIR;
	CHECK(as_due(&s, true, far, lo, 40000, 1, false) == AS_DUE_NO);	/* airfields: 3 km */
	CHECK(as_due(&s, true, farther, lo, 40000, 1, false) == AS_DUE_YES);
	printf("auto scan: a refresh at the first fix, then 30 s and 1 km (airfields 3 km), a new map pack at once\n");
}

int main(void)
{
	if (!refpack_fixture_install(REFPACK_ALL)) {
		printf("FAIL: no map pack on the simulated chip\n");
		return 1;
	}
	snprintf(tag, sizeof tag, "pack");
	CHECK(rpt_count() > 100 && air_nfields() > 50 && ref_seq() != 0);
	test_pick_rpt();
	test_pick_air();
	test_merge();
	test_drive();
	test_due();
	/* A repeater index the pack doesn't have (a new, smaller pack): gone */
	snprintf(tag, sizeof tag, "gone");
	CHECK(as_rpt_dist(rpt_count(), e7(39.9), e7(32.8)) == UINT32_MAX && as_field_dist(air_nfields(), 0, 0) == UINT32_MAX);
	printf("auto scan (autoscan.c): %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
