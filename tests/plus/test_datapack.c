/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus user data packs (core/storage/datapack.c) on the simulated 16 MiB data chip: the region guard (nothing
 * outside 0xE70000-0xEFFFFF is ever erased or programmed), the two slots and their sequence, the header written last,
 * the payload CRC read back, broken slots, and a power cut at every flash operation of a pack being written (the pack
 * in force before stays in force, or the new one is whole). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "storage/datapack.h"
#include "sim/sim_flash.h"
#include "board.h"
#include "ui/airfield.h"
#include "ui/air_pack.h"
#include "refpack_fixture.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
const char *tr(const char *en) { return en; }

#define FLASH_SIZE	0x1000000u
static const hal_flash_map_t map = {
	.settings_addr = 0x711000u, .settings_size = 0x8000u, .journal_addr = 0x719000u,
	.openfw_eeprom = 0x700000u, .openfw_flash = 0x720000u, .openfw_end = 0xF00000u,
	.calibration_addr = 0x710000u, .calibration_size = 0x200u,
};
const board_t board = { .name = "test", .flash = &sim_flash };

static uint32_t rng = 1;
static uint8_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return (uint8_t)(rng >> 16);
}

/* Everything outside the region, hashed (it must never change) */
static uint32_t outside(void)
{
	return storage_crc32(storage_crc32(0, sim_flash_data, DPK_BASE), sim_flash_data + DPK_END, FLASH_SIZE - DPK_END);
}

static dpk_hdr_t header(uint8_t type, const uint8_t *p, uint32_t len, const char *name)
{
	dpk_hdr_t h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, "DPK1", 4);
	h.type = type;
	h.fmt = 1;
	h.len = len;
	h.crc = storage_crc32(0, p, len);
	memcpy(h.date, "20261120", 8);
	strncpy(h.name, name, sizeof h.name);
	strncpy(h.tag, "test", sizeof h.tag);
	return h;
}

/* A whole pack, written in pieces of step bytes in a scrambled order */
static bool write_pack(uint8_t type, const uint8_t *p, uint32_t len, const char *name, uint32_t step)
{
	dpk_writer_t w;
	dpk_hdr_t h = header(type, p, len, name);
	if (!dpk_begin(&w, &h))
		return false;
	uint32_t pieces = (len + step - 1u) / step;
	for (uint32_t k = 0; k < pieces; k++) {
		uint32_t i = (k * 7u + 3u) % pieces;	/* every piece once, out of order */
		uint32_t off = i * step, n = len - off < step ? len - off : step;
		if (pieces % 7u == 0u)
			i = k, off = i * step, n = len - off < step ? len - off : step;
		if (!dpk_put(&w, off, p + off, n))
			return false;
	}
	return dpk_commit(&w);
}

static bool active_is(uint8_t type, const uint8_t *p, uint32_t len)
{
	const dpk_info_t *h = dpk_active(type);
	if (h == NULL || h->len != len)
		return false;
	uint8_t b[256];
	for (uint32_t off = 0; off < len; off += sizeof b) {
		uint32_t n = len - off < sizeof b ? len - off : (uint32_t)sizeof b;
		if (!dpk_read(type, off, b, n) || memcmp(b, p + off, n) != 0)
			return false;
	}
	return true;
}

static void test_guard(void)
{
	uint8_t b[8] = { 0 };
	CHECK(!dpk_erase(DPK_BASE - DPK_SECTOR) && !dpk_erase(DPK_END) && !dpk_erase(0x710000u) && !dpk_erase(0));
	CHECK(!dpk_erase(DPK_BASE + 1u));	/* not a sector start */
	CHECK(!dpk_program(DPK_BASE - 1u, b, 2) && !dpk_program(DPK_END - 4u, b, 8) && !dpk_program(DPK_END, b, 1));
	CHECK(!dpk_program(0xFFFFFFFCu, b, 8));	/* wraps */
	CHECK(dpk_slot_addr(DPK_RADIOID, 0) == 0xE70000u && dpk_slot_addr(DPK_RADIOID, 1) == 0xEB0000u);
	CHECK(dpk_slot_addr(DPK_AIR, 0) == 0xEF0000u && dpk_slot_addr(DPK_AIR, 1) == 0xEF4000u);
	CHECK(dpk_slot_addr(DPK_AIR, 1) + dpk_slot_size(DPK_AIR) <= DPK_END && dpk_slot_addr(4, 0) == 0u);
	/* T1, layout 2 (docs/mappack-design.md A.1): RadioID keeps 1.0.x's starts in 192 KB, the map pack in the old tails;
	 * every slot inside the region, 4 KB aligned, no two overlapping */
	CHECK(dpk_slot_size(DPK_RADIOID) == 0x30000u && dpk_slot_size(DPK_REF) == 0x10000u && dpk_slot_size(DPK_AIR) == 0x4000u);
	CHECK(dpk_slot_addr(DPK_REF, 0) == 0xEA0000u && dpk_slot_addr(DPK_REF, 1) == 0xEE0000u && dpk_slot_size(0) == 0u);
	for (uint8_t t = 1; t <= DPK_TYPES; t++)
		for (uint8_t s = 0; s < 2u; s++) {
			uint32_t a = dpk_slot_addr(t, s), z = dpk_slot_size(t);
			CHECK(a >= DPK_BASE && a % DPK_SECTOR == 0u && z <= DPK_END - a);
			for (uint8_t t2 = 1; t2 <= DPK_TYPES; t2++)
				for (uint8_t s2 = 0; s2 < 2u; s2++) {
					uint32_t b2 = dpk_slot_addr(t2, s2), z2 = dpk_slot_size(t2);
					CHECK((t2 == t && s2 == s) || a + z <= b2 || b2 + z2 <= a);
				}
		}
	CHECK(!dpk_erase(0xE6F000u) && !dpk_erase(0xF00000u) && !dpk_program(0xE6FFFFu, b, 2));
	/* the header check of a type the build does not know: no wrap of its slot size (0 - 64) */
	dpk_hdr_t u = header(DPK_AIR, b, 8, "x");
	u.type = 7;
	dpk_seal(&u);
	CHECK(!dpk_hdr_ok(&u, 7) && !dpk_hdr_ok(&u, 0));
	/* a header claiming more than its slot holds is refused before anything is erased */
	dpk_writer_t w;
	dpk_hdr_t h = header(DPK_AIR, b, 8, "x");
	h.len = dpk_slot_size(DPK_AIR);
	CHECK(!dpk_begin(&w, &h));
}

static uint8_t pa[100000], pb[100000], pc[12000], air[6000];

static void test_slots(void)
{
	dpk_scan();
	CHECK(dpk_active(DPK_RADIOID) == NULL && dpk_state(DPK_RADIOID) == DPK_NONE && dpk_state(DPK_AIR) == DPK_NONE);
	for (size_t i = 0; i < sizeof pa; i++)
		pa[i] = rnd(), pb[i] = rnd();
	for (size_t i = 0; i < sizeof air; i++)
		air[i] = rnd();
	CHECK(write_pack(DPK_RADIOID, pa, sizeof pa, "RadioID TR", 192));
	CHECK(active_is(DPK_RADIOID, pa, sizeof pa) && dpk_active(DPK_RADIOID)->seq == 1u);
	CHECK(write_pack(DPK_RADIOID, pb, sizeof pb, "RadioID TR2", 500));
	CHECK(active_is(DPK_RADIOID, pb, sizeof pb) && dpk_active(DPK_RADIOID)->seq == 2u);
	dpk_hdr_t hd;
	CHECK(dpk_header(DPK_RADIOID, &hd) && strncmp(hd.name, "RadioID TR2", 16) == 0 && memcmp(hd.date, "20261120", 8) == 0);
	CHECK(write_pack(DPK_AIR, air, sizeof air, "TR aerodromes", 128) && active_is(DPK_AIR, air, sizeof air));
	CHECK(active_is(DPK_RADIOID, pb, sizeof pb));	/* the types keep to their own slots */
	/* after a restart the same */
	dpk_scan();
	CHECK(active_is(DPK_RADIOID, pb, sizeof pb) && active_is(DPK_AIR, air, sizeof air));
	/* a wrong payload never commits: the old pack stays */
	dpk_writer_t w;
	dpk_hdr_t h = header(DPK_RADIOID, pa, 4096, "bad");
	CHECK(dpk_begin(&w, &h));
	pa[5] ^= 1u;
	CHECK(dpk_put(&w, 0, pa, 4096));
	pa[5] ^= 1u;
	CHECK(!dpk_commit(&w) && active_is(DPK_RADIOID, pb, sizeof pb));
	CHECK(!dpk_put(&w, 0, pa, 1));		/* the writer is closed */
	/* a byte outside the payload or written twice: refused (dpk_put's range) */
	CHECK(dpk_begin(&w, &h) && !dpk_put(&w, 4090, pa, 10));
}

static void test_broken(void)
{
	/* the pack in force spoilt on the chip (a flipped bit): the older one takes over; both spoilt: "broken" */
	uint32_t a1 = dpk_slot_addr(DPK_RADIOID, 1), a0 = dpk_slot_addr(DPK_RADIOID, 0);
	/* slot 0 holds pa? rewrite it so it does */
	CHECK(write_pack(DPK_RADIOID, pa, sizeof pa, "RadioID TR", 1000));	/* goes into the slot not in force */
	uint32_t in_force = dpk_active(DPK_RADIOID)->seq;
	uint32_t a = in_force % 2u == 1u ? a0 : a1;	/* sequence 1, 3, ... landed in slot 0 */
	(void)a;
	uint32_t live = active_is(DPK_RADIOID, pa, sizeof pa) ? 1u : 0u;
	CHECK(live == 1u);
	/* flip a payload bit of whichever slot is in force */
	uint32_t slot_addr = memcmp(sim_flash_data + a0 + DPK_HDR, pa, 64) == 0 ? a0 : a1;
	sim_flash_data[slot_addr + DPK_HDR + 100] ^= 0x10u;
	dpk_scan();
	CHECK(active_is(DPK_RADIOID, pb, sizeof pb) && dpk_state(DPK_RADIOID) == DPK_OK);
	uint32_t other = slot_addr == a0 ? a1 : a0;
	sim_flash_data[other + 20] ^= 0x01u;		/* its header's length: the header CRC fails */
	dpk_scan();
	CHECK(dpk_active(DPK_RADIOID) == NULL && dpk_state(DPK_RADIOID) == DPK_BAD);
	CHECK(active_is(DPK_AIR, air, sizeof air));
	/* a fresh pack after that */
	CHECK(write_pack(DPK_RADIOID, pb, sizeof pb, "again", 4096) && active_is(DPK_RADIOID, pb, sizeof pb));
}

/* A power cut at every flash operation while a pack is written: after power on the old pack, or the whole new one */
static void test_cuts(void)
{
	for (size_t i = 0; i < sizeof pc; i++)
		pc[i] = rnd();
	uint8_t *before = malloc(DPK_END - DPK_BASE);
	memcpy(before, sim_flash_data + DPK_BASE, DPK_END - DPK_BASE);
	uint32_t old_seq = dpk_active(DPK_RADIOID)->seq;
	/* how many flash operations a whole write takes */
	uint32_t u0 = sim_flash_units();
	CHECK(write_pack(DPK_RADIOID, pc, sizeof pc, "cut", 300));
	uint32_t units = sim_flash_units() - u0;
	int olds = 0, news = 0;
	for (uint32_t cut = 1; cut <= units + 1u; cut++) {
		memcpy(sim_flash_data + DPK_BASE, before, DPK_END - DPK_BASE);
		dpk_scan();
		sim_flash_cut_after(cut);
		(void)write_pack(DPK_RADIOID, pc, sizeof pc, "cut", 300);
		sim_flash_power_on();
		dpk_scan();
		bool is_new = active_is(DPK_RADIOID, pc, sizeof pc);
		bool is_old = active_is(DPK_RADIOID, pb, sizeof pb) && dpk_active(DPK_RADIOID)->seq == old_seq;
		CHECK(is_new || is_old);
		olds += is_old;
		news += is_new;
	}
	sim_flash_cut_after(0);
	CHECK(olds > 0 && news > 0);
	printf("  power cuts at each of %u flash operations: %d left the old pack, %d the whole new one\n", units + 1u,
	       olds, news);
	free(before);
}

/* --- The RadioID pack: records as tools/datapack.py writes them --- */
static uint8_t code6(char c)
{
	return c == ' ' ? 0u : c >= '0' && c <= '9' ? (uint8_t)(1 + c - '0') : c >= 'A' && c <= 'Z' ? (uint8_t)(11 + c - 'A') :
	       c >= 'a' && c <= 'z' ? (uint8_t)(37 + c - 'a') : 63u;
}

static uint32_t id_pack(uint8_t *out, uint32_t n, uint32_t first, uint32_t step)
{
	const uint32_t chars = 16, rec = 3 + chars * 3 / 4;
	out[0] = (uint8_t)n, out[1] = (uint8_t)(n >> 8), out[2] = (uint8_t)(n >> 16), out[3] = (uint8_t)(n >> 24);
	out[4] = (uint8_t)rec, out[5] = (uint8_t)chars, out[6] = out[7] = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint8_t *r = out + DPK_ID_HDR + i * rec;
		uint32_t id = first + i * step;
		char t[17];
		snprintf(t, sizeof t, "TA%uX%-3u Ali%-5u", (unsigned)(i % 9u), (unsigned)(i % 1000u), (unsigned)(i % 100u));
		r[0] = (uint8_t)id, r[1] = (uint8_t)(id >> 8), r[2] = (uint8_t)(id >> 16);
		for (uint32_t k = 0; k < chars; k += 4) {
			uint32_t v = (uint32_t)code6(t[k]) << 18 | (uint32_t)code6(t[k + 1]) << 12 | (uint32_t)code6(t[k + 2]) << 6 |
				     code6(t[k + 3]);
			r[3 + k / 4 * 3] = (uint8_t)(v >> 16), r[4 + k / 4 * 3] = (uint8_t)(v >> 8), r[5 + k / 4 * 3] = (uint8_t)v;
		}
	}
	return DPK_ID_HDR + n * rec;
}

static uint8_t ids[DPK_ID_HDR + 9000 * 15];

static void test_radioid(void)
{
	uint32_t len = id_pack(ids, 9000, 2860001, 7);
	CHECK(write_pack(DPK_RADIOID, ids, len, "RadioID TR", 192));
	char t[40];
	CHECK(dpk_id_text(2860001, t, sizeof t) && strcmp(t, "TA0X0   Ali0") == 0);
	CHECK(dpk_id_text(2860001 + 7 * 8999, t, sizeof t) && strncmp(t, "TA8X999 Ali99", 13) == 0);
	CHECK(dpk_id_text(2860001 + 7 * 1234, t, sizeof t) && strncmp(t, "TA1X234 Ali34", 13) == 0);
	CHECK(!dpk_id_text(2860002, t, sizeof t) && t[0] == '\0');		/* between two IDs */
	CHECK(!dpk_id_text(2860000, t, sizeof t) && !dpk_id_text(2860001 + 7 * 9000, t, sizeof t));
	CHECK(dpk_id_text(2860001, t, 5) && strcmp(t, "TA0X") == 0);		/* cut to the room */
	CHECK(dpk_about(DPK_RADIOID, t, sizeof t) && strcmp(t, "RadioID TR, 2026-11-20, 132 KB") == 0);
	/* no pack in force: nothing */
	uint32_t a = dpk_slot_addr(DPK_RADIOID, 0), b = dpk_slot_addr(DPK_RADIOID, 1);
	CHECK(dpk_erase(a) && dpk_erase(b));
	dpk_scan();
	CHECK(!dpk_id_text(2860001, t, sizeof t) && !dpk_about(DPK_RADIOID, t, sizeof t) && dpk_state(DPK_RADIOID) == DPK_NONE);
	CHECK(dpk_about(DPK_AIR, t, sizeof t));
}

/* --- The air band pack over the shipped airfield table (air_pack.c) --- */
static uint16_t field(const char *code)
{
	for (uint16_t i = 0; i < air_field_count; i++)
		if (strcmp(air_fields[i].code, code) == 0)
			return i;
	return AIR_NONE;
}

static void test_air(void)
{
	uint16_t ltac = field("LTAC"), ltab = field("LTAB");
	CHECK(ltac != AIR_NONE && ltab != AIR_NONE);
	/* no pack: the table's own */
	uint32_t a0 = dpk_slot_addr(DPK_AIR, 0), a1 = dpk_slot_addr(DPK_AIR, 1);
	CHECK(dpk_erase(a0) && dpk_erase(a1));
	dpk_scan();
	CHECK(air_count(ltac) == air_fields[ltac].count && air_freq(ltac, 0) != NULL &&
	      air_freq(ltac, 0)->ch == air_freqs[air_fields[ltac].first].ch &&
	      air_freq(ltac, 0)->type == air_freqs[air_fields[ltac].first].type);	/* the map pack's copy of the table */
	CHECK(air_freq(ltac, air_fields[ltac].count) == NULL);
	/* a pack: LTAC with three frequencies, LTAB with none, an airfield the table lacks */
	uint8_t p[64];
	uint32_t n = 0;
	p[n++] = 3, p[n++] = 0, p[n++] = 0, p[n++] = 0;
	memcpy(p + n, "LTXX\0\0\0\0", 8), n += 8, p[n++] = 1, p[n++] = 0x10, p[n++] = 0x02, p[n++] = AIR_TWR;
	memcpy(p + n, "LTAC\0\0\0\0", 8), n += 8, p[n++] = 3;
	uint16_t ch[3] = { 2020, 2780, 3120 };	/* 118.100, 121.900, 123.600 */
	uint8_t ty[3] = { AIR_TWR, AIR_GND, AIR_ATIS };
	for (int k = 0; k < 3; k++)
		p[n++] = (uint8_t)ch[k], p[n++] = (uint8_t)(ch[k] >> 8), p[n++] = ty[k];
	memcpy(p + n, "LTAB\0\0\0\0", 8), n += 8, p[n++] = 0;
	CHECK(write_pack(DPK_AIR, p, n, "TR aerodromes", 7));
	CHECK(air_count(ltac) == 3 && air_count(ltab) == 0);
	for (uint8_t k = 0; k < 3; k++)
		CHECK(air_freq(ltac, k) != NULL && air_freq(ltac, k)->ch == ch[k] && air_freq(ltac, k)->type == ty[k]);
	CHECK(air_freq(ltac, 3) == NULL && air_freq(ltab, 0) == NULL);
	CHECK(air_freq_khz(air_freq(ltac, 1)) == 121900u);
	/* every other airfield answers from the table */
	uint16_t other = field("LTAI");
	CHECK(other != AIR_NONE && air_count(other) == air_fields[other].count);
	CHECK(air_freq(other, 1)->ch == air_freqs[air_fields[other].first + 1].ch);
	/* the pack damaged: the table again */
	sim_flash_data[dpk_slot_addr(DPK_AIR, 0) + DPK_HDR + 5] ^= 1u;
	sim_flash_data[dpk_slot_addr(DPK_AIR, 1) + DPK_HDR + 5] ^= 1u;
	dpk_scan();
	CHECK(dpk_state(DPK_AIR) == DPK_BAD && air_count(ltac) == air_fields[ltac].count);
}

/* --- T2: an upgraded 1.0.x radio (256 KB RadioID slots at 0xE70000 / 0xEB0000), nothing written by the scan --- */

/* A 1.0.x slot image put straight on the chip (as 1.0.x's writer left it): header and payload */
static void old_pack(uint32_t addr, uint32_t len, uint32_t seq, uint8_t fill)
{
	static uint8_t p[0x40000];
	for (uint32_t i = 0; i < len; i++)
		p[i] = (uint8_t)(fill + i * 13u);
	dpk_hdr_t h = header(DPK_RADIOID, p, len, "RadioID TR");
	h.seq = seq;
	dpk_seal(&h);
	memcpy(sim_flash_data + addr, &h, sizeof h);
	memcpy(sim_flash_data + addr + DPK_HDR, p, len);
}

static void wipe_region(void)
{
	memset(sim_flash_data + DPK_BASE, 0xFF, DPK_END - DPK_BASE);
	dpk_scan();
}

static void test_migration(void)
{
	uint32_t h0 = outside(), u0;
	/* (a) the owner's case: a 98 KB pack in old slot A stays in force, unchanged */
	wipe_region();
	old_pack(0xE70000u, 98u * 1024u, 3, 1);
	u0 = sim_flash_units();
	dpk_scan();
	CHECK(sim_flash_units() == u0);		/* the scan erases and programs nothing */
	CHECK(dpk_state(DPK_RADIOID) == DPK_OK && dpk_active(DPK_RADIOID)->len == 98u * 1024u &&
	      dpk_active_addr(DPK_RADIOID) == 0xE70000u);
	CHECK(dpk_state(DPK_REF) == DPK_NONE);	/* the map pack slots: nothing committed there */
	/* (b) another in old slot B with a higher sequence: B */
	old_pack(0xEB0000u, 60u * 1024u, 4, 9);
	dpk_scan();
	CHECK(dpk_active_addr(DPK_RADIOID) == 0xEB0000u && dpk_active(DPK_RADIOID)->seq == 4u);
	/* (d) a 200 KB pack in old B (longer than layout 2's slot): DPK_BIG, the valid A in force */
	old_pack(0xEB0000u, 200u * 1024u, 5, 2);
	dpk_scan();
	CHECK(dpk_state(DPK_RADIOID) == DPK_OK && dpk_active_addr(DPK_RADIOID) == 0xE70000u);
	CHECK(dpk_state(DPK_REF) == DPK_NONE);	/* B's payload runs over map B's start: no "DPK1" there */
	memset(sim_flash_data + 0xE70000u, 0xFF, DPK_SECTOR);
	dpk_scan();
	CHECK(dpk_state(DPK_RADIOID) == DPK_BIG && dpk_active(DPK_RADIOID) == NULL);
	char t[40];
	CHECK(!dpk_about(DPK_RADIOID, t, sizeof t));
	/* (e) old A over 192 KB: map A's start holds its payload bytes -> map pack NONE, not BAD */
	wipe_region();
	old_pack(0xE70000u, 220u * 1024u, 1, 5);
	dpk_scan();
	CHECK(dpk_state(DPK_REF) == DPK_NONE && dpk_state(DPK_RADIOID) == DPK_BIG);
	/* a damaged header at a map slot start ("DPK1" but its CRC wrong) is BAD */
	dpk_hdr_t bad = header(DPK_REF, (const uint8_t *)t, 4, "x");
	bad.hcrc = 1;
	memcpy(sim_flash_data + 0xEE0000u, &bad, sizeof bad);
	dpk_scan();
	CHECK(dpk_state(DPK_REF) == DPK_BAD);
	/* a RadioID pack of 196,544 bytes (layout 2's limit) is taken, one byte more is DPK_BIG */
	wipe_region();
	old_pack(0xE70000u, 0x30000u - DPK_HDR, 1, 3);
	dpk_scan();
	CHECK(dpk_state(DPK_RADIOID) == DPK_OK);
	old_pack(0xE70000u, 0x30000u - DPK_HDR + 1u, 1, 3);
	dpk_scan();
	CHECK(dpk_state(DPK_RADIOID) == DPK_BIG);
	/* the firmware refuses such a pack from a writer too (DOTA from a 1.0.x radio, an old programmer's header) */
	dpk_writer_t w;
	dpk_hdr_t big = header(DPK_RADIOID, (const uint8_t *)t, 4, "x");
	big.len = 200u * 1024u;
	CHECK(!dpk_begin(&w, &big));
	CHECK(outside() == h0);
	wipe_region();
	printf("  migration from 1.0.x: kept, DPK_BIG, map slots NONE, no write by the scan\n");
}

/* --- Images tools/datapack.py built (argv): written as the radio writes them, then read --- */
static uint8_t img[0x40000];

static uint32_t load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
		return 0;
	uint32_t n = (uint32_t)fread(img, 1, sizeof img, f);
	fclose(f);
	return n;
}

static void test_builder(const char *radioid, const char *air)
{
	dpk_hdr_t h;
	uint32_t n = load(radioid);
	CHECK(n > DPK_HDR);
	memcpy(&h, img, sizeof h);
	CHECK(dpk_hdr_ok(&h, DPK_RADIOID) && h.len == n - DPK_HDR && h.crc == storage_crc32(0, img + DPK_HDR, h.len));
	CHECK(write_pack(DPK_RADIOID, img + DPK_HDR, h.len, "RadioID TR", 192));
	char t[40];
	CHECK(dpk_id_text(2860001, t, sizeof t) && strcmp(t, "TA1ABC Ahmet") == 0);
	CHECK(dpk_id_text(2860105, t, sizeof t) && strcmp(t, "TA2XYZ Ayse") == 0);	/* folded */
	CHECK(dpk_id_text(3101234, t, sizeof t) && strncmp(t, "TA3AA Mehmet", 12) == 0);	/* iddb.py's short name */
	CHECK(!dpk_id_text(2860002, t, sizeof t));
	n = load(air);
	memcpy(&h, img, sizeof h);
	CHECK(n > DPK_HDR && dpk_hdr_ok(&h, DPK_AIR) && write_pack(DPK_AIR, img + DPK_HDR, h.len, "TR aerodromes", 64));
	uint16_t ltac = field("LTAC");
	CHECK(air_count(ltac) == 3 && air_freq(ltac, 0)->type == AIR_ATIS && air_freq_khz(air_freq(ltac, 0)) == 123600u);
	CHECK(air_freq(ltac, 1)->type == AIR_TWR && air_freq_khz(air_freq(ltac, 1)) == 118100u);
	CHECK(air_freq(ltac, 2)->type == AIR_APP && air_freq_khz(air_freq(ltac, 2)) == 119100u);
}

int main(int argc, char **argv)
{
	sim_flash_create(FLASH_SIZE, &map);
	for (uint32_t i = 0; i < FLASH_SIZE; i++)
		sim_flash_data[i] = i < DPK_BASE || i >= DPK_END ? (uint8_t)(i * 31u + 7u) : 0xFFu;
	uint32_t h0 = outside();
	test_guard();
	test_slots();
	test_broken();
	test_cuts();
	test_radioid();
	CHECK(refpack_fixture_install(REFPACK_AIR));	/* the airfield table is the map pack's (docs/mappack-design.md) */
	test_air();
	if (argc == 3)
		test_builder(argv[1], argv[2]);
	test_migration();
	CHECK(outside() == h0);		/* not one byte outside the region changed */
	CHECK(sim_flash_violations() == 0);
	printf("datapack store: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
