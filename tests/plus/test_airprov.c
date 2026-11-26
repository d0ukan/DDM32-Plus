/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 16, airfields by province (CONFIG_AIR_PROV, airfield.h): the shipped table's walks within one
 * province. Built with -DCONFIG_AIRFIELDS=1 -DCONFIG_APRS=1 by tools/test_airprov.py under ASan / UBSan, the real
 * table linked. Checks: every airfield in exactly one group, the counts adding up; each province's walk forward visits
 * its airfields once each, nearest first, its rank as its place, and backward the reverse; All is the whole list's
 * order as before; a province without airfields walks to nothing; without a position each province by code. */
#include <stdio.h>
#include <string.h>
#include "ui/airfield.h"
#include "ui/airfield_data.h"
#include "refpack_fixture.h"

static int failures;
#define CHECK(c, what) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static uint16_t code_at(const char *code)
{
	for (uint16_t i = 0; i < air_field_count; i++)
		if (strcmp(air_fields[i].code, code) == 0)
			return i;
	return AIR_NONE;
}

/* Province p's walk from here, both ways, against the table */
static void walk(const air_here_t *h, uint8_t p)
{
	uint16_t order[AIR_FIELDS], n = 0;
	for (uint16_t i = air_next_in(h, AIR_NONE, 1, p); i != AIR_NONE && n <= AIR_FIELDS; i = air_next_in(h, i, 1, p))
		order[n++] = i;
	CHECK(n == air_count_in(p), "the walk visits the province's count");
	uint32_t prev_m = 0;
	for (uint16_t k = 0; k < n; k++) {
		CHECK(air_fields[order[k]].prov == p || p == AIR_PROV_ALL, "only the province's airfields");
		CHECK(air_rank_in(h, order[k], p) == k, "the rank is the place in the walk");
		for (uint16_t j = 0; j < k; j++)
			CHECK(order[j] != order[k], "each once");
		if (h->ok) {
			uint32_t m;
			uint16_t brg;
			air_way(order[k], h->lat_e5 * 100, h->lon_e5 * 100, &m, &brg);
			CHECK((uint64_t)m * 102u / 100u + 1000u >= prev_m, "nearest first (within the order's 1 %)");
			prev_m = m;
		} else {
			CHECK(k == 0 || order[k] > order[k - 1], "no position: by code");
		}
	}
	uint16_t back = 0;
	for (uint16_t i = air_next_in(h, AIR_NONE, -1, p); i != AIR_NONE && back < n; i = air_next_in(h, i, -1, p)) {
		CHECK(i == order[n - 1u - back], "backward: the reverse");
		back++;
	}
	CHECK(back == n, "backward: as many");
}

int main(void)
{
	/* the tables are the map pack's on the data chip since 1.1 (refpack_fixture.h) */
	if (!refpack_fixture_install(REFPACK_AIR)) {
		printf("FAIL: the map pack fixture\n");
		return 1;
	}
	/* Every airfield in one group; the shipped table places all of them */
	uint32_t sum = 0;
	for (uint16_t p = 0; p <= AIR_PROVS; p++)
		sum += air_count_in((uint8_t)p);
	CHECK(sum == air_field_count && air_count_in(AIR_PROV_ALL) == air_field_count, "the counts add up");
	CHECK(air_count_in(AIR_PROV_UNKNOWN) == 0, "the shipped table: every airfield in a province");
	for (uint16_t i = 0; i < air_field_count; i++)
		CHECK(air_fields[i].prov >= 1u && air_fields[i].prov <= AIR_PROVS, "a plate");
	/* Known places: Ankara, Istanbul (Sabiha Gokcen in Pendik: the data's region at the border), Mersin (Cukurova near
	 * Tarsus), Mugla, Ordu (off the simplified coast) */
	static const struct { const char *code; uint8_t prov; } known[] = {
		{ "LTAC", 6 }, { "LTAB", 6 }, { "LTFM", 34 }, { "LTFJ", 34 }, { "LTBA", 34 }, { "LTDB", 33 }, { "LTBS", 48 },
		{ "LTFE", 48 }, { "LTCB", 52 }, { "LTAI", 7 }, { "LTBJ", 35 }, { "LTAF", 1 },
	};
	for (unsigned k = 0; k < sizeof known / sizeof known[0]; k++) {
		uint16_t i = code_at(known[k].code);
		CHECK(i != AIR_NONE && air_fields[i].prov == known[k].prov, known[k].code);
	}

	/* From Kizilay: every province's walk, All as before */
	air_here_t h;
	air_here_set(&h, true, 399208000, 328541000);
	for (uint16_t p = 0; p <= AIR_PROVS; p++)
		walk(&h, (uint8_t)p);
	walk(&h, AIR_PROV_ALL);
	for (uint16_t i = air_nearest(&h), k = 0; i != AIR_NONE; i = air_next(&h, i, 1), k++)
		CHECK(air_next_in(&h, i, 1, AIR_PROV_ALL) == air_next(&h, i, 1) && air_rank(&h, i) == k, "All: the old walk");
	CHECK(strcmp(air_fields[air_next_in(&h, AIR_NONE, 1, 6)].code, "LTAB") == 0, "Ankara's nearest from Kizilay");
	CHECK(strcmp(air_fields[air_next_in(&h, AIR_NONE, 1, 7)].code, "LTAI") == 0, "Antalya's nearest from Kizilay");
	CHECK(air_next_in(&h, AIR_NONE, 1, 9) == AIR_NONE && air_count_in(9) == 0, "Aydin: none");
	CHECK(air_next_in(&h, code_at("LTAC"), 1, 7) == AIR_NONE || air_fields[air_next_in(&h, code_at("LTAC"), 1, 7)].prov == 7,
	      "from an airfield of another province: still within the filter");
	/* From Antalya: its own first */
	air_here_set(&h, true, 368969000, 307133000);
	walk(&h, 7);
	CHECK(strcmp(air_fields[air_next_in(&h, AIR_NONE, 1, 7)].code, "LTAI") == 0, "Antalya's nearest from Antalya");
	/* Without a position: by code */
	air_here_set(&h, false, 0, 0);
	for (uint16_t p = 0; p <= AIR_PROVS; p++)
		walk(&h, (uint8_t)p);
	printf("airfields by province: %u airfields, %u checks failed\n", (unsigned)air_field_count, (unsigned)failures);
	return failures != 0;
}
