/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Airfields (integration/radiofoundry/air): the shipped table (tools/airfields.py's output) and
 * the math: every row's frequencies on the air band's 25 / 8.33 kHz channel names, the tuned frequency of an 8.33 kHz
 * name on the grid radio_am_step uses, the order nearest first from several places against a double precision great
 * circle, the walk forward and back, the ranks, and the table's order without a position. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui/airfield.h"
#include "ui/airfield_data.h"
#include "refpack_fixture.h"
#include "radio/radio_am.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* radio_am_step's grid (radio_am.c), restated: point n is n x 25000 / 3 Hz rounded */
static uint32_t grid_hz(uint64_t n)
{
	return (uint32_t)((n * 25000u + 1u) / 3u);
}

static double great_circle_m(double la1, double lo1, double la2, double lo2)
{
	const double r = 6371008.8, d = M_PI / 180.0;
	double a = sin((la2 - la1) * d / 2) * sin((la2 - la1) * d / 2) +
		   cos(la1 * d) * cos(la2 * d) * sin((lo2 - lo1) * d / 2) * sin((lo2 - lo1) * d / 2);
	return 2 * r * asin(sqrt(a));
}

static double dist_to(uint16_t i, double la, double lo)
{
	return great_circle_m(la, lo, air_fields[i].lat_e5 / 1e5, air_fields[i].lon_e5 / 1e5);
}

/* The walk from (la, lo): every airfield once, forward and back, ranks 0.., nearest first within the equirectangular
 * key's tolerance against the great circle (0.3 % of the distance, 200 m at least) */
static void order_from(double la, double lo, const char *nearest)
{
	air_here_t h;
	air_here_set(&h, true, (int32_t)lrint(la * 1e7), (int32_t)lrint(lo * 1e7));
	uint16_t seen[256] = { 0 }, order[256], n = 0;
	for (uint16_t i = air_next(&h, AIR_NONE, 1); i != AIR_NONE && n < 256; i = air_next(&h, i, 1)) {
		CHECK(i < AIR_FIELDS && seen[i] == 0);
		seen[i]++;
		CHECK(air_rank(&h, i) == n);
		order[n++] = i;
	}
	CHECK(n == AIR_FIELDS);
	for (uint16_t k = 1; k < n; k++) {
		double a = dist_to(order[k - 1], la, lo), b = dist_to(order[k], la, lo);
		double tol = a * 0.003 > 200.0 ? a * 0.003 : 200.0;
		if (!(a <= b + tol)) {
			printf("FAIL order from %.2f %.2f: %s %.0f m before %s %.0f m\n", la, lo, air_fields[order[k - 1]].code, a,
			       air_fields[order[k]].code, b);
			failures++;
		}
	}
	/* Backwards: the same order reversed */
	uint16_t k = n;
	for (uint16_t i = air_next(&h, AIR_NONE, -1); i != AIR_NONE; i = air_next(&h, i, -1)) {
		CHECK(k > 0 && order[k - 1] == i);
		k--;
	}
	CHECK(k == 0);
	CHECK(strcmp(air_fields[air_nearest(&h)].code, nearest) == 0);
	if (strcmp(air_fields[air_nearest(&h)].code, nearest) != 0)
		printf("  nearest from %.2f %.2f: %s, expected %s\n", la, lo, air_fields[air_nearest(&h)].code, nearest);
	/* air_way: the true distance and a bearing */
	uint32_t m;
	uint16_t brg;
	air_way(order[0], (int32_t)lrint(la * 1e7), (int32_t)lrint(lo * 1e7), &m, &brg);
	double want = dist_to(order[0], la, lo);
	CHECK(fabs((double)m - want) <= want * 0.005 + 20.0 && brg < 360);
}

int main(void)
{
	/* the tables are the map pack's on the data chip since 1.1 (refpack_fixture.h) */
	if (!refpack_fixture_install(REFPACK_AIR)) {
		printf("FAIL: the map pack fixture\n");
		return 1;
	}
	CHECK(air_field_count == AIR_FIELDS && AIR_FIELDS >= 60 && AIR_FREQS >= 100);	/* after the cleanup: 74, 122 */
	/* The table: sorted by code, frequencies in place, on a channel name of the band, the 8.33 kHz grid */
	uint32_t next_first = 0;
	unsigned n833 = 0;
	for (uint16_t i = 0; i < AIR_FIELDS; i++) {
		const air_field_t *a = &air_fields[i];
		CHECK(a->first == next_first && a->kind <= AIR_SMALL && a->code[0] && strlen(a->name) <= AIR_NAME_MAX);
		CHECK(a->lat_e5 > 3500000 && a->lat_e5 < 4300000 && a->lon_e5 > 2500000 && a->lon_e5 < 4600000);
		if (i)
			CHECK(strcmp(air_fields[i - 1].code, a->code) < 0);
		CHECK(a->count > 0 || a->kind != AIR_SMALL);	/* a small field only with a frequency */
		next_first += a->count;
		for (uint8_t k = 0; k < a->count; k++) {
			const air_freq_t *f = &air_freqs[a->first + k];
			uint32_t khz = air_freq_khz(f), hz = air_freq_hz(f);
			CHECK(khz >= 118000 && khz < 137000 && khz % 5 == 0 && khz % 25 != 20);
			CHECK(f->type < AIR_TYPES && strcmp(air_type_name(f->type), "?") != 0);
			CHECK(hz >= RADIO_AM_LO_HZ && hz < RADIO_AM_HI_HZ);	/* radio_am_band: AM, never TX */
			if (k)
				CHECK(air_freqs[a->first + k - 1].type <= f->type);	/* ATIS, DEL, GND, ... */
			if (air_freq_833(f)) {
				n833++;
				uint64_t n = (uint64_t)hz * 3u / 25000u;
				CHECK(grid_hz(n) == hz || grid_hz(n + 1) == hz);	/* on the grid */
			} else {
				CHECK(hz == khz * 1000u);
			}
		}
	}
	CHECK(next_first == AIR_FREQS);
	(void)n833;

	/* 8.33 kHz names: x.005 -> x.000, x.010 -> x.00833, x.015 -> x.01667; x.025 is 25 kHz */
	air_freq_t f = { (121580 - AIR_CH_BASE_KHZ) / 5, AIR_GND };
	CHECK(air_freq_833(&f) && air_freq_khz(&f) == 121580 && air_freq_hz(&f) == 121575000u);
	f.ch = (121585 - AIR_CH_BASE_KHZ) / 5;
	CHECK(air_freq_hz(&f) == 121583333u);
	f.ch = (121590 - AIR_CH_BASE_KHZ) / 5;
	CHECK(air_freq_hz(&f) == 121591667u);
	f.ch = (118675 - AIR_CH_BASE_KHZ) / 5;
	CHECK(!air_freq_833(&f) && air_freq_hz(&f) == 118675000u);
	f.ch = (136990 - AIR_CH_BASE_KHZ) / 5;
	CHECK(air_freq_hz(&f) == 136991667u);	/* 136.975 + 16.667 kHz */
	/* ... the same points as Up / Down on the radio's 8.33 kHz step (radio_am_step's grid) */
	CHECK(grid_hz(121575000ull * 3u / 25000u + 1u) == 121583333u && grid_hz(121575000ull * 3u / 25000u + 2u) == 121591667u);

	/* Known airfields and their data */
	int esb = -1;
	for (uint16_t i = 0; i < AIR_FIELDS; i++)
		if (strcmp(air_fields[i].code, "LTAC") == 0)
			esb = i;
	CHECK(esb >= 0);
	if (esb >= 0) {
		const air_field_t *a = &air_fields[esb];
		CHECK(a->kind == AIR_LARGE && a->count >= 4);
		bool twr = false;
		for (uint8_t k = 0; k < a->count; k++)
			twr |= air_freqs[a->first + k].type == AIR_TWR && air_freq_khz(&air_freqs[a->first + k]) == 118100;
		CHECK(twr);
	}

	/* Nearest first from Ankara's Kizilay, Esenboga itself, Istanbul's Taksim, Antalya, Van */
	order_from(39.9208, 32.8541, "LTAB");
	order_from(40.1281, 32.9951, "LTAC");
	order_from(41.0369, 28.9850, "LTBA");
	order_from(36.8969, 30.7133, "LTAI");
	order_from(38.5012, 43.3729, "LTCI");

	/* Without a position: the table's order (by code) */
	air_here_t none;
	air_here_set(&none, false, 0, 0);
	uint16_t i = air_next(&none, AIR_NONE, 1), k = 0;
	for (; i != AIR_NONE; i = air_next(&none, i, 1), k++)
		CHECK(i == k);
	CHECK(k == AIR_FIELDS && air_next(&none, AIR_FIELDS, 1) == AIR_NONE);

	printf("airfields: %u airfields, %u frequencies (%u on 8.33 kHz names): %d failures\n", (unsigned)AIR_FIELDS,
	       (unsigned)AIR_FREQS, n833, failures);
	return failures != 0;
}
