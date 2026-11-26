/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus repeater list (rpt.h, tools/rpt_list.py). With an expect file (tools/test_classic.py rpt_fixture): every
 * field of the generated table back exactly, the order nearest first against a double precision great circle, the
 * order without a position, the walk forward and back, ranks, Istanbul -> Ankara, locators and what tuning the VFO
 * sets, the provinces the generator gave. Without one: the shipped table's invariants (sorted, terminated, positions
 * and provinces in range, one walk visits all). Both: each province's walk (rpt_next_in) is the order filtered. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/rpt.h"
#include "refpack_fixture.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static double haversine_m(double la1, double lo1, double la2, double lo2, double *brg)
{
	double r = 3.14159265358979 / 180.0, p1 = la1 * r, p2 = la2 * r, dl = (lo2 - lo1) * r;
	double a = sin((p2 - p1) / 2) * sin((p2 - p1) / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
	if (brg)
		*brg = fmod(atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) / r + 360.0, 360.0);
	return 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static uint16_t find(const char *name)
{
	for (uint16_t i = 0; i < rpt_info.count; i++)
		if (strcmp(rpt_table[i].name, name) == 0)
			return i;
	return RPT_NONE;
}

/* The walk visits every entry once, forward and back mirror each other, ranks count up */
static void walk_checks(const rpt_here_t *h, uint16_t *order)
{
	uint16_t n = (uint16_t)rpt_info.count, k = 0;
	char *seen = calloc(n ? n : 1, 1);
	for (uint16_t i = rpt_next(h, RPT_NONE, 1); i != RPT_NONE; i = rpt_next(h, i, 1)) {
		CHECK(i < n && !seen[i] && k < n);
		if (i >= n || seen[i] || k >= n)
			break;
		seen[i] = 1;
		CHECK(rpt_rank(h, i) == k);
		order[k++] = i;
	}
	CHECK(k == n);
	for (uint16_t i = rpt_next(h, RPT_NONE, -1); i != RPT_NONE && k > 0; i = rpt_next(h, i, -1))
		CHECK(order[--k] == i);
	CHECK(k == 0);
	CHECK(rpt_next(h, n, 1) == RPT_NONE);
	free(seen);
}

/* By province (rpt_next_in / rpt_rank_in / rpt_count_in): each province's walk is the whole order with only its
 * entries kept, forward and back, ranked from 0; the counts add up to the table's */
static void province_checks(const rpt_here_t *h, const uint16_t *order)
{
	uint16_t n = (uint16_t)rpt_info.count, total = 0;
	CHECK(rpt_count_in(RPT_PROV_ALL) == n);
	for (unsigned p = 0; p <= RPT_PROVS; p++) {
		uint16_t c = rpt_count_in((uint8_t)p), k = 0, last = RPT_NONE;
		total = (uint16_t)(total + c);
		for (uint16_t j = 0; j < n; j++) {	/* the full order filtered by hand */
			if (rpt_table[order[j]].prov != p)
				continue;
			uint16_t i = rpt_next_in(h, last, 1, (uint8_t)p);
			CHECK(i == order[j]);
			CHECK(rpt_rank_in(h, order[j], (uint8_t)p) == k);
			if (last != RPT_NONE)
				CHECK(rpt_next_in(h, order[j], -1, (uint8_t)p) == last);
			last = order[j];
			k++;
		}
		CHECK(k == c);
		CHECK(rpt_next_in(h, last, 1, (uint8_t)p) == RPT_NONE);
		CHECK(c == 0 || rpt_next_in(h, RPT_NONE, -1, (uint8_t)p) == last);
	}
	CHECK(total == n);
}

static uint8_t prov_of(const char *name)
{
	for (uint16_t i = 0; i < rpt_info.count; i++)
		if (strcmp(rpt_table[i].name, name) == 0)
			return rpt_table[i].prov;
	return 0xEEu;
}

static void shipped(void)
{
	uint16_t n = (uint16_t)rpt_info.count;
	CHECK(n > 0);
	CHECK(rpt_info.date[0] && rpt_info.source[0]);
	for (uint16_t i = 0; i < n; i++) {
		const rpt_entry_t *e = &rpt_table[i];
		CHECK(e->name[0] && memchr(e->name, 0, sizeof e->name) && memchr(e->city, 0, sizeof e->city));
		CHECK(i == 0 || rpt_table[i - 1].rx_hz <= e->rx_hz);
		CHECK(e->rx_hz % RPT_STEP_HZ == 0 && e->rx_hz >= 1000000u);
		CHECK((e->mode & RPT_MODE_MASK) < RPT_MODES);
		CHECK(e->lat_e5 == RPT_NOPOS || (e->lat_e5 >= -9000000 && e->lat_e5 <= 9000000 &&
						 e->lon_e5 >= -18000000 && e->lon_e5 <= 18000000));
		CHECK(e->prov <= RPT_PROVS);
	}
	uint16_t *order = calloc(n, sizeof *order);
	rpt_here_t h;
	rpt_here_set(&h, true, 410082000, 289784000);
	walk_checks(&h, order);
	province_checks(&h, order);
	rpt_here_set(&h, false, 0, 0);
	walk_checks(&h, order);
	province_checks(&h, order);
	for (uint16_t i = 0; i < n; i++)
		CHECK(order[i] == i);
	free(order);
}

static void fixture(const char *path)
{
	FILE *f = fopen(path, "r");
	CHECK(f != NULL);
	if (!f)
		return;
	char line[256];
	uint16_t i = 0;
	while (fgets(line, sizeof line, f)) {
		int lat, lon, off;
		unsigned rx, ctcss, mode, dmr;
		char name[32] = "", city[32] = "";
		if (sscanf(line, "%d\t%d\t%u\t%d\t%u\t%u\t%u\t%31[^\t]\t%31[^\n]", &lat, &lon, &rx, &off, &ctcss, &mode, &dmr,
			   name, city) != 9) {
			printf("bad expect line: %s", line);
			failures++;
			continue;
		}
		CHECK(i < rpt_info.count);
		if (i >= rpt_info.count)
			break;
		const rpt_entry_t *e = &rpt_table[i++];
		if (e->lat_e5 != lat || e->lon_e5 != lon || e->rx_hz != rx || e->off != off || e->ctcss10 != ctcss ||
		    e->mode != mode || e->dmr != dmr || strcmp(e->name, name) || strcmp(e->city, strcmp(city, "-") ? city : "")) {
			printf("FAIL entry %u: got %d %d %u %d %u %u %u '%s' '%s', want %s", i - 1u, (int)e->lat_e5, (int)e->lon_e5,
			       (unsigned)e->rx_hz, e->off, e->ctcss10, e->mode, e->dmr, e->name, e->city, line);
			failures++;
		}
	}
	fclose(f);
	CHECK(i == rpt_info.count);
	CHECK(rpt_info.sample == 0 && strcmp(rpt_info.date, "2026-01-02") == 0);
	CHECK(strcmp(rpt_info.source, "own+RadioID.net") == 0);

	/* The order from Ankara against the great circle (the fixture's distances differ by 20 km and more) */
	uint16_t n = (uint16_t)rpt_info.count, order[64], want[64];
	CHECK(n <= 64);
	rpt_here_t h;
	double la = 39.9334, lo = 32.8597;
	rpt_here_set(&h, true, 399334000, 328597000);
	walk_checks(&h, order);
	province_checks(&h, order);
	/* Provinces (tools/rpt_list.py): inside the map's outlines from lat / lon, a locator and the gazetteer's town
	 * centres; no position and a city that names none: unknown */
	CHECK(prov_of("TA1AAA") == 6 && prov_of("TA1BBB") == 34 && prov_of("TA1CCC") == 63 && prov_of("TA1DDD") == 52);
	CHECK(prov_of("TA1EEE") == RPT_PROV_UNKNOWN && prov_of("TA1FFF") == 33 && prov_of("YM0AAA") == 16);
	CHECK(prov_of("YM0BBB") == 65);
	CHECK(rpt_count_in(6) == 1 && rpt_count_in(RPT_PROV_UNKNOWN) == 1 && rpt_count_in(1) == 0);
	CHECK(rpt_next_in(&h, RPT_NONE, 1, 1) == RPT_NONE && rpt_next_in(&h, RPT_NONE, -1, 1) == RPT_NONE);
	CHECK(rpt_next_in(&h, RPT_NONE, 1, 65) == find("YM0BBB") && rpt_next_in(&h, find("YM0BBB"), 1, 65) == RPT_NONE);
	uint16_t k = 0;
	for (uint16_t i = 0; i < n; i++)	/* insertion sort by the double distance, the unplaced last */
		if (rpt_table[i].lat_e5 != RPT_NOPOS) {
			double d = haversine_m(la, lo, rpt_table[i].lat_e5 / 1e5, rpt_table[i].lon_e5 / 1e5, NULL);
			uint16_t j = k++;
			while (j > 0 && haversine_m(la, lo, rpt_table[want[j - 1]].lat_e5 / 1e5,
						    rpt_table[want[j - 1]].lon_e5 / 1e5, NULL) > d) {
				want[j] = want[j - 1];
				j--;
			}
			want[j] = i;
		}
	for (uint16_t i = 0; i < n; i++)
		if (rpt_table[i].lat_e5 == RPT_NOPOS)
			want[k++] = i;
	for (uint16_t i = 0; i < n; i++)
		CHECK(order[i] == want[i]);
	CHECK(rpt_nearest(&h) == find("TA1AAA"));
	CHECK(order[n - 1] == find("TA1EEE"));	/* no position: last */
	CHECK(rpt_rank(&h, find("TA1EEE")) == n - 1u);
	rpt_here_set(&h, true, 384891000, 434089000);	/* in Van */
	CHECK(rpt_nearest(&h) == find("YM0BBB"));
	rpt_here_set(&h, true, 403000000, 290000000);	/* near Bursa */
	CHECK(rpt_nearest(&h) == find("YM0AAA"));

	/* Without a position: the table's order (by frequency) */
	rpt_here_set(&h, false, 399334000, 328597000);
	walk_checks(&h, order);
	province_checks(&h, order);
	for (uint16_t i = 0; i < n; i++)
		CHECK(order[i] == i);
	CHECK(rpt_nearest(&h) == 0 && rpt_next(&h, 0, -1) == RPT_NONE && rpt_next(&h, (uint16_t)(n - 1u), 1) == RPT_NONE);

	/* Istanbul -> Ankara (TA1AAA): ~351 km, bearing against double precision */
	uint32_t m;
	uint16_t brg;
	double wb, wm = haversine_m(41.0082, 28.9784, 39.9334, 32.8597, &wb);
	CHECK(rpt_way(&rpt_table[find("TA1AAA")], 410082000, 289784000, &m, &brg));
	CHECK(m > 345000u && m < 357000u && fabs(m - wm) < wm * 0.003);
	CHECK(abs((int)brg - (int)lrint(wb)) <= 1);
	CHECK(!rpt_way(&rpt_table[find("TA1EEE")], 410082000, 289784000, &m, &brg));

	/* Locators: Ankara KM69kw; a locator given in the CSV comes back as itself */
	char loc[8];
	rpt_locator(&rpt_table[find("TA1AAA")], loc);
	CHECK(strcmp(loc, "KM69kw") == 0);
	rpt_locator(&rpt_table[find("TA1BBB")], loc);
	CHECK(strcmp(loc, "KN41ka") == 0);
	rpt_locator(&rpt_table[find("TA1EEE")], loc);
	CHECK(loc[0] == '\0');

	/* Tuning the VFO */
	rpt_tune_t t;
	CHECK(rpt_tune_plan(&rpt_table[find("TA1AAA")], &t));
	CHECK(!t.dmr && !t.narrow && t.rx_hz == 145600000u && t.tx_hz == 145000000u && t.ctcss10 == 885u);
	CHECK(rpt_tune_plan(&rpt_table[find("TA1FFF")], &t));	/* + offset, 12.5 kHz */
	CHECK(!t.dmr && t.narrow && t.rx_hz == 145725000u && t.tx_hz == 146325000u && t.ctcss10 == 1230u);
	CHECK(rpt_tune_plan(&rpt_table[find("TA1EEE")], &t));	/* NFM simplex, no tone */
	CHECK(!t.dmr && t.narrow && t.tx_hz == t.rx_hz && t.ctcss10 == 0u);
	CHECK(rpt_tune_plan(&rpt_table[find("TA1BBB")], &t));	/* DMR CC2 TS2 */
	CHECK(t.dmr && t.cc == 2u && t.ts == 2u && t.rx_hz == 439162500u && t.tx_hz == 431562500u && t.ctcss10 == 0u);
	CHECK(rpt_tune_plan(&rpt_table[find("TA1CCC")], &t));	/* both slots: TS1 */
	CHECK(t.dmr && t.cc == 15u && t.ts == 1u && t.tx_hz == 431600000u);
	CHECK(rpt_tune_plan(&rpt_table[find("YM0BBB")], &t));	/* slot not known: TS1 */
	CHECK(t.dmr && t.cc == 3u && t.ts == 1u);
	CHECK(!rpt_tune_plan(&rpt_table[find("TA1DDD")], &t));	/* C4FM: information only */
	CHECK(strcmp(rpt_mode_name(&rpt_table[find("TA1DDD")]), "C4FM") == 0);
	CHECK(strcmp(rpt_mode_name(&rpt_table[find("TA1BBB")]), "DMR") == 0);
}

/* --- T15 (docs/mappack-design.md C.7): the TX refusal set in program flash (rpt_freqs.c, generated from this table's
 * rows) refuses exactly what the old known_repeater loop over the rows refused, on every frequency the radio can
 * transmit on (136-174 / 400-480 MHz and their 6,250 Hz rims); the pack's repeaters only ever add refusals --- */

static bool old_loop(uint32_t hz, const rpt_entry_t *extra)
{
	for (uint32_t i = 0; i <= rpt_info.count; i++) {
		const rpt_entry_t *e = i < rpt_info.count ? &rpt_table[i] : extra;
		if (e == NULL)
			continue;
		uint32_t in = (uint32_t)((int32_t)e->rx_hz + (int32_t)e->off * (int32_t)RPT_STEP_HZ);
		if ((hz + 6250u > e->rx_hz && hz < e->rx_hz + 6250u) || (e->off != 0 && hz + 6250u > in && hz < in + 6250u))
			return true;
	}
	return false;
}

static bool in_rims(uint32_t hz)
{
	return (hz >= 135993751u && hz <= 174006249u) || (hz >= 399993751u && hz <= 480006249u);
}

static unsigned t15_compared, t15_differ;
static void t15_one(uint32_t hz)
{
	t15_compared++;
	bool want = in_rims(hz) && old_loop(hz, NULL), got = rpt_freq_listed(hz);
	if (want != got) {
		if (t15_differ++ < 5)
			printf("FAIL refusal set at %u Hz: %d, the old loop %d\n", (unsigned)hz, got, want);
	}
}

static void t15(void)
{
	for (uint32_t hz = 136000000u; hz <= 174000000u; hz += 625u)
		t15_one(hz);
	for (uint32_t hz = 400000000u; hz <= 480000000u; hz += 625u)
		t15_one(hz);
	for (uint32_t hz = 144000000u; hz <= 146000000u; hz += 10u)
		t15_one(hz);
	for (uint32_t hz = 430000000u; hz <= 440000000u; hz += 10u)
		t15_one(hz);
	for (uint32_t i = 0; i < rpt_info.count; i++) {
		const rpt_entry_t *e = &rpt_table[i];
		uint32_t f[2] = { e->rx_hz, (uint32_t)((int32_t)e->rx_hz + (int32_t)e->off * (int32_t)RPT_STEP_HZ) };
		for (int k = 0; k < (e->off != 0 ? 2 : 1); k++) {
			t15_one(f[k] - 6250u), t15_one(f[k] - 6249u), t15_one(f[k]), t15_one(f[k] + 6249u), t15_one(f[k] + 6250u);
		}
	}
	static const uint32_t edges[] = { 0u, 135993750u, 135993751u, 135999999u, 136000000u, 136006249u, 173993751u,
					  174006249u, 174006250u, 399993750u, 399999999u, 400000000u, 480006249u, 480006250u,
					  0xFFFFFFFFu };
	for (unsigned k = 0; k < sizeof edges / sizeof edges[0]; k++)
		t15_one(edges[k]);
	CHECK(!rpt_freq_listed(0) && !rpt_freq_listed(135993750u) && !rpt_freq_listed(480006250u) &&
	      !rpt_freq_listed(0xFFFFFFFFu));
	CHECK(t15_differ == 0u);
	/* the OR rule: a pack repeater the set lacks adds exactly its refusals (its output and input), none go */
	rpt_entry_t x = { .lat_e5 = RPT_NOPOS, .lon_e5 = RPT_NOPOS, .rx_hz = 145212500u, .off = -480, .mode = RPT_FM };
	unsigned added = 0, removed = 0;
	for (uint32_t hz = 144000000u; hz <= 146000000u; hz += 125u) {
		bool before = rpt_freq_listed(hz), after = before || rpt_entry_near(&x, hz);
		added += after && !before;
		removed += before && !after;
		CHECK(after == old_loop(hz, &x));
	}
	CHECK(added > 0u && removed == 0u);
	printf("  refusal set: %u frequencies (%u + %u steps) equal to the old loop on %u frequencies\n",
	       (unsigned)(rpt_fq_vhf_n + rpt_fq_uhf_n), (unsigned)rpt_fq_vhf_n, (unsigned)rpt_fq_uhf_n, t15_compared);
}

int main(int argc, char **argv)
{
	/* the tables are the map pack's on the data chip since 1.1 (refpack_fixture.h) */
	if (!refpack_fixture_install(REFPACK_RPT)) {
		printf("FAIL: the map pack fixture\n");
		return 1;
	}
	t15();
	if (argc > 1 && strcmp(argv[1], "--t15") == 0) {
		printf("test_rpt: %s (T15 only, %u repeaters)\n", failures ? "FAILED" : "ok", (unsigned)rpt_info.count);
		return failures != 0;
	}
	if (argc > 1)
		fixture(argv[1]);
	else
		shipped();
	if (failures) {
		printf("test_rpt: %d failures\n", failures);
		return 1;
	}
	printf("test_rpt: ok (%u repeaters%s)\n", (unsigned)rpt_info.count, argc > 1 ? ", fixture" : "");
	return 0;
}
