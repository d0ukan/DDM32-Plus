/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus map pack (core/storage/ref_pack.c, datapack.c type 3; docs/mappack-design.md G.1) on the simulated 16 MiB
 * data chip:
 *   T4  written through dpk_begin / dpk_put / dpk_commit, in force, a second write into the other slot, power cuts;
 *   T5  every structural rule of C.2 broken once with valid CRCs -> the whole slot BAD; the version items -> DPK_OTHER
 *       (About "for another version", the other valid slot in force, both rejected -> OTHER); flags 0 = no hint; no
 *       read outside the slot being checked; records without their NULs come out terminated (S3);
 *   T5b a newer bad slot never displaces an older good one; a pack without repeater sections is valid with the map and
 *       the airfields on; a CRC-valid but structurally bad pack is never committed (its header sector stays erased);
 *   T5c a pack committed between two reads of one use: the reads stay on the old slot until the next ref_has;
 *   T6-8 the map, repeater and airfield functions on the pack give what the old code gave on the flash tables;
 *   T10 a whole-country map_draw makes at most 300 flash reads; a repeater list redraw reads at most 40 KB. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "storage/storage.h"
#include "storage/datapack.h"
#include "storage/ref_pack.h"
#include "sim/sim_flash.h"
#include "ui/map.h"
#include "ui/turkey_map_data.h"
#include "ui/turkey_district_data.h"
#include "ui/rpt.h"
#include "ui/airfield.h"
#include "refpack_fixture.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* The board's flash: the simulated chip with every read counted and (when armed) held inside one window */
static hal_flash_t gf;
static uint32_t reads, read_bytes, win_lo, win_hi, out_of_window;
static bool counted_read(uint32_t addr, void *buf, uint32_t len)
{
	reads++;
	read_bytes += len;
	if (win_hi != 0u && (addr < win_lo || addr + len > win_hi))
		out_of_window++;
	return sim_flash.read(addr, buf, len);
}
const board_t board = { .name = "refpack test", .flash = &gf };

#define SA	0xEA0000u		/* map pack slot A */
#define SB	0xEE0000u		/* slot B */
static uint8_t full[REFPACK_MAX], work[REFPACK_MAX];
static size_t full_n;

/* A section's directory entry and data in a payload */
static uint8_t *entry(uint8_t *p, uint16_t id)
{
	for (unsigned k = 0; k < p[5]; k++) {
		uint8_t *e = p + REF_HEAD + REF_ENTRY * k;
		if ((e[0] | e[1] << 8) == id)
			return e;
	}
	return NULL;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static uint8_t *sec(uint8_t *p, uint16_t id) { return p + rd32(entry(p, id) + 4); }
static uint32_t cnt(uint8_t *p, uint16_t id) { return rd32(entry(p, id) + 8); }

static void reset_work(void)
{
	memcpy(work, full, full_n);
}

/* What the firmware decides for this payload in a slot (raw, no header: ref_validate only), reads held in the slot */
static uint8_t verdict(const uint8_t *p, size_t n, uint16_t flags)
{
	memset(sim_flash_data + SB, 0xFF, 0x10000u);
	memcpy(sim_flash_data + SB + DPK_HDR, p, n);
	win_lo = SB;
	win_hi = SB + 0x10000u;
	uint8_t v = ref_validate(SB + DPK_HDR, (uint32_t)n, flags);
	win_hi = 0;
	return v;
}

static dpk_hdr_t hdr(const uint8_t *p, size_t n, uint32_t seq, uint16_t flags)
{
	dpk_hdr_t h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, "DPK1", 4);
	h.type = DPK_REF;
	h.fmt = 1;
	h.flags = flags;
	h.seq = seq;
	h.len = (uint32_t)n;
	h.crc = storage_crc32(0, p, (uint32_t)n);
	memcpy(h.date, "20261119", 8);
	strncpy(h.name, "DDM32 TR test", sizeof h.name);
	dpk_seal(&h);
	return h;
}

/* A slot image put straight on the chip (header and payload, CRCs valid) */
static void put_slot(uint32_t a, const uint8_t *p, size_t n, uint32_t seq, uint16_t flags)
{
	dpk_hdr_t h = hdr(p, n, seq, flags);
	memset(sim_flash_data + a, 0xFF, 0x10000u);
	memcpy(sim_flash_data + a, &h, sizeof h);
	memcpy(sim_flash_data + a + DPK_HDR, p, n);
}

static bool write_pack(const uint8_t *p, size_t n, uint16_t flags)
{
	dpk_writer_t w;
	dpk_hdr_t h = hdr(p, n, 0, flags);
	return dpk_begin(&w, &h) && dpk_put(&w, 0, p, (uint32_t)n) && dpk_commit(&w);
}

static void test_write(void)
{
	memset(sim_flash_data + DPK_BASE, 0xFF, DPK_END - DPK_BASE);
	dpk_scan();
	CHECK(dpk_state(DPK_REF) == DPK_NONE && !ref_has(REF_MAP) && rpt_count() == 0 && air_nfields() == 0);
	CHECK(!map_ok() && map_draw(&(map_view_t){ 0 }, NULL, NULL) == 0 && map_province_at(399000000, 328000000) == MAP_NONE);
	CHECK(write_pack(full, full_n, ref_hint()));
	CHECK(dpk_state(DPK_REF) == DPK_OK && dpk_active_addr(DPK_REF) == SA && dpk_active(DPK_REF)->seq == 1u);
	CHECK(ref_has(REF_MAP | REF_RPT | REF_AIR) && map_ok() && rpt_count() == rpt_info.count && air_nfields() == air_field_count);
	char t[40];
	CHECK(dpk_about(DPK_REF, t, sizeof t) && strncmp(t, "DDM32 TR test, 2026-11-19, ", 27) == 0);
	CHECK(write_pack(full, full_n, ref_hint()));
	CHECK(dpk_active_addr(DPK_REF) == SB && dpk_active(DPK_REF)->seq == 2u && ref_has(REF_MAP));
	/* power cuts while the third write goes into slot A: the second pack, or the whole third one */
	uint8_t *before = malloc(DPK_END - DPK_BASE);
	memcpy(before, sim_flash_data + DPK_BASE, DPK_END - DPK_BASE);
	uint32_t u0 = sim_flash_units();
	CHECK(write_pack(full, full_n, ref_hint()));
	uint32_t units = sim_flash_units() - u0, cuts[4] = { 1, units / 3u, units - 64u, units - 1u };
	for (int k = 0; k < 4; k++) {
		memcpy(sim_flash_data + DPK_BASE, before, DPK_END - DPK_BASE);
		dpk_scan();
		sim_flash_cut_after(cuts[k]);
		(void)write_pack(full, full_n, ref_hint());
		sim_flash_power_on();
		sim_flash_cut_after(0);
		dpk_scan();
		uint32_t s = dpk_active(DPK_REF) != NULL ? dpk_active(DPK_REF)->seq : 0u;
		CHECK((s == 2u && dpk_active_addr(DPK_REF) == SB) || (s == 3u && dpk_active_addr(DPK_REF) == SA));
		CHECK(ref_has(REF_MAP | REF_RPT | REF_AIR));
	}
	free(before);
}

/* A payload with one more directory entry (an unknown id) in front of the sections */
static size_t with_unknown(uint8_t *out)
{
	unsigned n = full[5];
	uint32_t old0 = REF_HEAD + REF_ENTRY * n, shift = REF_ENTRY;
	memcpy(out, full, REF_HEAD);
	out[5] = (uint8_t)(n + 1u);
	for (unsigned k = 0; k < n; k++) {
		memcpy(out + REF_HEAD + REF_ENTRY * k, full + REF_HEAD + REF_ENTRY * k, REF_ENTRY);
		wr32(out + REF_HEAD + REF_ENTRY * k + 4, rd32(full + REF_HEAD + REF_ENTRY * k + 4) + shift);
	}
	uint8_t *e = out + REF_HEAD + REF_ENTRY * n;
	wr16(e, 99), wr16(e + 2, 7), wr32(e + 4, (uint32_t)full_n + shift), wr32(e + 8, 0);
	memcpy(out + old0 + shift, full + old0, full_n - old0);
	return full_n + shift;
}

static void test_validate(void)
{
	uint16_t hint = ref_hint();
	CHECK(verdict(full, full_n, hint) == DPK_OK && verdict(full, full_n, 0) == DPK_OK);	/* 0 = no hint */
	CHECK(out_of_window == 0u);
#define BAD(what) do { CHECK(verdict(work, full_n, hint) == DPK_BAD); if (verdict(work, full_n, hint) != DPK_BAD) printf("  (%s)\n", what); reset_work(); } while (0)
#define OTHER(what, flags) do { CHECK(verdict(work, full_n, flags) == DPK_OTHER); if (verdict(work, full_n, flags) != DPK_OTHER) printf("  (%s)\n", what); reset_work(); } while (0)
	reset_work();
	wr32(entry(work, REF_S_STEP) + 8, 40000);		/* the steps run past the payload */
	BAD("section past the payload");
	wr32(entry(work, REF_S_DPOS) + 8, 0x40000001u);		/* off + 4 x count wraps 32 bits */
	BAD("u32 overflow");
	wr32(entry(work, REF_S_RING) + 4, rd32(entry(work, REF_S_PROV) + 4));	/* rings on the provinces */
	BAD("overlap");
	wr32(entry(work, REF_S_RING) + 4, rd32(entry(work, REF_S_RING) + 4) + 2u);	/* not 4-byte aligned */
	BAD("alignment");
	wr16(sec(work, REF_S_RING) + 4, (uint16_t)(cnt(work, REF_S_STEP) - 1u));	/* ring 0: first + count past the steps */
	BAD("ring past the steps");
	wr16(sec(work, REF_S_RING) + 4, 0xFFF0u), wr16(sec(work, REF_S_RING) + 6, 0x20u);	/* wraps 16 bits */
	BAD("ring wrapping 16 bits");
	wr16(sec(work, REF_S_RING) + 6, 0);
	BAD("ring of no steps");
	wr16(sec(work, REF_S_PROV) + 18u * 80u, (uint16_t)cnt(work, REF_S_RING));	/* province 80's ring past the rings */
	BAD("province ring past the rings");
	wr16(sec(work, REF_S_PROV) + 4, (uint16_t)cnt(work, REF_S_PNAME));	/* its name past the blob */
	BAD("province name past the blob");
	wr16(sec(work, REF_S_DFIRST) + 2u * 5u, 0);		/* first[] goes down */
	BAD("district first decreasing");
	wr16(sec(work, REF_S_DFIRST) + 2u * 81u, (uint16_t)(cnt(work, REF_S_DPOS) - 1u));
	BAD("first[81] != district count");
	wr16(sec(work, REF_S_DNFIRST) + 2u * 80u, (uint16_t)cnt(work, REF_S_DNAME));
	BAD("district name offset past the blob");
	wr16(sec(work, REF_S_DNFIRST) + 2u * 80u, (uint16_t)(cnt(work, REF_S_DNAME) - 2u));	/* the last names run out */
	BAD("fewer names than districts");
	sec(work, REF_S_PNAME)[cnt(work, REF_S_PNAME) - 1u] = 'x';
	BAD("province names without the final NUL");
	sec(work, REF_S_DNAME)[cnt(work, REF_S_DNAME) - 1u] = 'x';
	BAD("district names without the final NUL");
	wr32(entry(work, REF_S_RKEY) + 8, cnt(work, REF_S_RKEY) - 1u);
	BAD("RPT / RPT_KEY counts");
	sec(work, REF_S_RKEY)[10u * 3u + 8u] ^= 1u;		/* key 3's province */
	BAD("a key differing from its record");
	wr16(sec(work, REF_S_AIR) + 44u * 2u + 8u, (uint16_t)cnt(work, REF_S_AFREQ));	/* airfield 2's list past the freqs */
	BAD("air first + count past the frequencies");
	sec(work, REF_S_AIR)[44u * 2u + 10u] = 65;
	BAD("air count over 64");
	wr16(entry(work, REF_S_RKEY), 77);			/* the repeater group incomplete */
	BAD("a group missing a section");
	work[5] = 17;
	BAD("more than 16 sections");
	memcpy(work, "REF2", 4);
	BAD("not a directory");
	/* the version items: DPK_OTHER, checked before the structure */
	work[4] = 2;
	OTHER("directory version 2", hint);
	wr16(entry(work, REF_S_PROV) + 2, 20);
	OTHER("a known section with another record size", hint);
	wr32(work + 8, rd32(work + 8) ^ 1u);
	OTHER("another coordinate id", hint);
	OTHER("a hint that disagrees", (uint16_t)(hint ^ 0x0100u));
	CHECK(verdict(full, full_n, 0x0001u) == DPK_OTHER);	/* version 1 with another coordinate byte */
	/* an unknown section is ignored */
	static uint8_t more[REFPACK_MAX + 16];
	size_t mn = with_unknown(more);
	CHECK(verdict(more, mn, hint) == DPK_OK);
	CHECK(out_of_window == 0u);		/* no read left the slot in any case */
	/* through the slots: fmt 2 is BAD; OTHER next to a valid pack: the valid one, About keeps its text */
	memset(sim_flash_data + DPK_BASE, 0xFF, DPK_END - DPK_BASE);
	put_slot(SA, full, full_n, 1, hint);
	reset_work();
	work[4] = 2;
	put_slot(SB, work, full_n, 9, hint);
	dpk_scan();
	CHECK(dpk_state(DPK_REF) == DPK_OK && dpk_active_addr(DPK_REF) == SA && dpk_active(DPK_REF)->seq == 1u);
	memset(sim_flash_data + SA, 0xFF, 0x10000u);
	dpk_scan();
	CHECK(dpk_state(DPK_REF) == DPK_OTHER && !ref_has(REF_MAP));
	reset_work();
	wr32(entry(work, REF_S_STEP) + 8, 40000);
	put_slot(SA, work, full_n, 3, hint);
	dpk_scan();
	CHECK(dpk_state(DPK_REF) == DPK_OTHER);		/* one BAD, one OTHER: OTHER (the more useful word) */
	put_slot(SB, full, full_n, 4, hint);
	sim_flash_data[SB + 5] = 2;			/* fmt 2 */
	{
		dpk_hdr_t h;
		memcpy(&h, sim_flash_data + SB, sizeof h);
		dpk_seal(&h);
		memcpy(sim_flash_data + SB, &h, sizeof h);
	}
	dpk_scan();
	CHECK(dpk_state(DPK_REF) == DPK_BAD);
	reset_work();
}

/* S3: records whose strings have no NUL come out terminated */
static void test_sanitise(void)
{
	reset_work();
	memset(sec(work, REF_S_RPT) + 18, 'A', 11 + 13);	/* entry 0's name and city, no NUL */
	memset(sec(work, REF_S_RINFO) + 5, 'B', 11 + 16);	/* the info's date and source */
	memset(sec(work, REF_S_AIR) + 12, 'C', 8 + 23);		/* airfield 0's code and name */
	CHECK(verdict(work, full_n, ref_hint()) == DPK_OK);	/* not a structural matter */
	CHECK(write_pack(work, full_n, ref_hint()) && ref_has(REF_RPT | REF_AIR));	/* a new use: the new pack */
	const rpt_entry_t *e = rpt_get(0);
	CHECK(e != NULL && strlen(e->name) == RPT_NAME_MAX && strlen(e->city) == RPT_CITY_MAX);
	rpt_info_t ri;
	CHECK(rpt_info_get(&ri) && strlen(ri.date) == 10u && strlen(ri.source) == 15u);
	const air_field_t *a = air_get(0);
	CHECK(a != NULL && strlen(a->code) == AIR_CODE_MAX && strlen(a->name) == AIR_NAME_MAX);
	CHECK(rpt_get(rpt_count()) == NULL && air_get(air_nfields()) == NULL);
	reset_work();
}

static void test_slots(void)
{
	uint16_t hint = ref_hint();
	/* T5b: a newer slot bad in its map part only next to an older good one: the older (all or nothing) */
	memset(sim_flash_data + DPK_BASE, 0xFF, DPK_END - DPK_BASE);
	put_slot(SA, full, full_n, 1, hint);
	reset_work();
	sec(work, REF_S_DNAME)[cnt(work, REF_S_DNAME) - 1u] = 'x';
	put_slot(SB, work, full_n, 2, hint);
	dpk_scan();
	CHECK(dpk_active_addr(DPK_REF) == SA && ref_has(REF_RPT | REF_AIR | REF_MAP));
	/* a pack without the repeater sections: valid, the map and airfields on, the repeaters off */
	static uint8_t norpt[REFPACK_MAX];
	size_t nn = refpack_fixture_payload(norpt, sizeof norpt, REFPACK_MAP | REFPACK_AIR);
	CHECK(nn != 0u && verdict(norpt, nn, hint) == DPK_OK);
	CHECK(write_pack(norpt, nn, hint));
	CHECK(ref_has(REF_MAP | REF_AIR) && !ref_has(REF_RPT) && rpt_count() == 0 && air_nfields() == air_field_count);
	/* a CRC-valid but structurally bad pack (a DOTA transfer): dpk_commit refuses before the header, old pack stays */
	CHECK(write_pack(full, full_n, hint));
	uint32_t in_force = dpk_active_addr(DPK_REF), target = in_force == SA ? SB : SA, seq = dpk_active(DPK_REF)->seq;
	reset_work();
	wr32(entry(work, REF_S_STEP) + 8, 40000);
	CHECK(!write_pack(work, full_n, hint));
	CHECK(memcmp(sim_flash_data + target, "\xFF\xFF\xFF\xFF", 4) == 0);	/* its header sector stays erased */
	CHECK(dpk_active_addr(DPK_REF) == in_force && dpk_active(DPK_REF)->seq == seq);
	/* one for another version is not committed either */
	CHECK(!write_pack(full, full_n, (uint16_t)(hint ^ 0x0100u)));
	CHECK(dpk_active_addr(DPK_REF) == in_force);
	reset_work();
}

/* T5c: the UI's directory copy reloads only at the next use; a use in progress keeps reading its own slot */
static void test_race(void)
{
	memset(sim_flash_data + DPK_BASE, 0xFF, DPK_END - DPK_BASE);
	dpk_scan();
	CHECK(write_pack(full, full_n, ref_hint()) && ref_has(REF_RPT));
	uint32_t first = dpk_active_addr(DPK_REF);
	uint16_t n0 = ref_count(REF_S_RPT);
	rpt_entry_t a, b;
	CHECK(ref_rec(REF_S_RPT, 0, &a));
	/* another task commits a pack without repeaters (a CPS close, a DOTA commit) in the middle of a draw */
	static uint8_t norpt[REFPACK_MAX];
	size_t nn = refpack_fixture_payload(norpt, sizeof norpt, REFPACK_MAP | REFPACK_AIR);
	CHECK(write_pack(norpt, nn, ref_hint()) && dpk_active_addr(DPK_REF) != first);
	win_lo = first;
	win_hi = first + 0x10000u;
	out_of_window = 0;
	CHECK(ref_rec(REF_S_RPT, n0 - 1u, &b) && ref_rec(REF_S_RPT, 0, &b) && memcmp(&a, &b, sizeof a) == 0);
	CHECK(out_of_window == 0u);		/* still the old slot, bounded by the old counts */
	win_hi = 0;
	CHECK(!ref_has(REF_RPT) && ref_count(REF_S_RPT) == 0 && !ref_rec(REF_S_RPT, 0, &b));	/* the next use: the new one */
	CHECK(ref_has(REF_MAP | REF_AIR));
}

/* The old code on the flash tables (pre-1.1 map.c / rpt.c), for T6 / T7 */
static bool old_inside(const turkey_map_province_t *pr, int32_t px, int32_t py)
{
	if (px < pr->min.x * 10 || px > pr->max.x * 10 || py < pr->min.y * 10 || py > pr->max.y * 10)
		return false;
	bool in = false;
	for (uint16_t r = pr->first_ring; r < pr->first_ring + pr->ring_count; r++) {
		const turkey_map_ring_t *ring = &turkey_map_rings[r];
		int32_t ax = ring->start.x * 10, ay = ring->start.y * 10;
		for (uint16_t i = ring->first; i < ring->first + ring->count; i++) {
			int32_t bx = ax + turkey_map_steps[i][0] * 10, by = ay + turkey_map_steps[i][1] * 10;
			if ((ay > py) != (by > py)) {
				int32_t lhs = (px - ax) * (by - ay), rhs = (py - ay) * (bx - ax);
				if (by > ay ? lhs < rhs : lhs > rhs)
					in = !in;
			}
			ax = bx;
			ay = by;
		}
	}
	return in;
}

static int old_province_at(int32_t lat_e7, int32_t lon_e7)
{
	int32_t px = (lon_e7 - TURKEY_MAP_LON0_E7) / 1000 - ((lon_e7 - TURKEY_MAP_LON0_E7) % 1000 < 0);
	int32_t py = (lat_e7 - TURKEY_MAP_LAT0_E7) / 1000 - ((lat_e7 - TURKEY_MAP_LAT0_E7) % 1000 < 0);
	for (uint8_t p = 0; p < TURKEY_MAP_PROVINCES; p++)
		if (old_inside(&turkey_map_provinces[p], px, py))
			return p;
	return MAP_NONE;
}

static uint32_t segs;
static void seg_count(void *ctx, int x0, int y0, int x1, int y1, uint8_t province)
{
	(void)ctx, (void)x0, (void)y0, (void)x1, (void)y1, (void)province;
	segs++;
}

static void test_equal(void)
{
	memset(sim_flash_data + DPK_BASE, 0xFF, DPK_END - DPK_BASE);
	dpk_scan();
	CHECK(write_pack(full, full_n, ref_hint()));
	/* T6: the province lookup on a 5,000 point grid over Turkey and around it */
	uint32_t rng = 7, differ = 0;
	for (int k = 0; k < 5000; k++) {
		rng = rng * 1103515245u + 12345u;
		int32_t lat = 355000000 + (int32_t)(rng >> 8) % 72000000;
		rng = rng * 1103515245u + 12345u;
		int32_t lon = 250000000 + (int32_t)(rng >> 8) % 200000000;
		differ += map_province_at(lat, lon) != old_province_at(lat, lon);
	}
	CHECK(differ == 0u);
	char name[MAP_NAME_MAX];
	for (uint8_t p = 0; p < TURKEY_MAP_PROVINCES; p++) {
		map_province_name(p, name, sizeof name);
		CHECK(strcmp(name, &turkey_map_names[turkey_map_provinces[p].name]) == 0);
	}
	/* T7: the repeater order from 200 positions equals the order of the flash table's keys */
	for (int k = 0; k < 200; k++) {
		rng = rng * 1103515245u + 12345u;
		int32_t lat = 360000000 + (int32_t)(rng >> 8) % 60000000, lon = 260000000 + (int32_t)(rng >> 4) % 190000000;
		rpt_here_t h;
		rpt_here_set(&h, true, lat, lon);
		uint16_t i = rpt_next(&h, RPT_NONE, 1), seen = 0;
		uint64_t last = 0;
		while (i != RPT_NONE && seen <= rpt_info.count) {
			const rpt_entry_t *e = &rpt_table[i];
			int64_t dlat = (int64_t)e->lat_e5 - h.lat_e5, dlon = ((int64_t)e->lon_e5 - h.lon_e5) * h.cos_q15 / 32768;
			uint64_t key = e->lat_e5 == RPT_NOPOS ? UINT64_MAX : (uint64_t)(dlat * dlat) + (uint64_t)(dlon * dlon);
			CHECK(key >= last);
			last = key;
			seen++;
			i = rpt_next(&h, i, 1);
		}
		CHECK(seen == rpt_info.count);
	}
	/* T8: the airfields' provinces and codes as the table has them */
	for (uint16_t i = 0; i < air_field_count; i++) {
		const air_field_t *a = air_get(i);
		CHECK(a != NULL && strcmp(a->code, air_fields[i].code) == 0 && a->prov == air_fields[i].prov &&
		      a->first == air_fields[i].first && a->count == air_fields[i].count);
	}
	/* T10: a whole-country draw through the windows; a repeater list redraw through the key section */
	map_view_t v;
	map_view_init(&v, 0, 20, 240, 270);
	reads = read_bytes = 0;
	segs = 0;
	uint32_t n = map_draw(&v, seg_count, NULL);
	printf("  whole-country map_draw: %u segments, %u flash reads, %u bytes\n", (unsigned)n, (unsigned)reads,
	       (unsigned)read_bytes);
	CHECK(n > 1000u && n == segs && reads <= 300u);
	rpt_here_t h;
	rpt_here_set(&h, true, 399000000, 328000000);
	reads = read_bytes = 0;
	uint16_t i = rpt_next(&h, RPT_NONE, 1);
	(void)rpt_rank(&h, i);
	for (int k = 0; k < 10 && i != RPT_NONE; k++) {
		CHECK(rpt_get(i) != NULL);
		i = rpt_next(&h, i, 1);
	}
	printf("  repeater list redraw (10 rows): %u flash reads, %u bytes\n", (unsigned)reads, (unsigned)read_bytes);
	CHECK(read_bytes <= 40u * 1024u);
}

int main(void)
{
	sim_flash_create(0x1000000u, NULL);
	gf = sim_flash;
	gf.read = counted_read;
	full_n = refpack_fixture_payload(full, sizeof full, REFPACK_ALL);
	CHECK(full_n > 40000u && full_n <= REFPACK_MAX);
	test_write();
	test_validate();
	test_sanitise();
	test_slots();
	test_race();
	test_equal();
	printf("map pack: %zu-byte payload, %d checks, %d failures\n", full_n, checks, failures);
	return failures != 0;
}
