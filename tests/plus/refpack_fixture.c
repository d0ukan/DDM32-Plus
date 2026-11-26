/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The map pack host fixture (refpack_fixture.h): the payload serialised from the compiled tables, section by section
 * in tools/refpack.py's order and layout, then committed on the simulated data chip. */
#include <string.h>
#include "config.h"
#include "board.h"
#include "storage/storage.h"
#include "storage/datapack.h"
#include "storage/ref_pack.h"
#include "sim/sim_flash.h"
#include "ui/turkey_map_data.h"
#include "ui/turkey_district_data.h"
#include "ui/rpt.h"
#include "ui/airfield.h"
#include "refpack_fixture.h"

bool refpack_fixture_auto = true;

/* Weak through #pragma: some suites build with -D__attribute__(x)= */
#pragma weak turkey_map_provinces
#pragma weak turkey_map_rings
#pragma weak turkey_map_steps
#pragma weak turkey_map_names
#pragma weak turkey_district_pos
#pragma weak turkey_district_first
#pragma weak turkey_district_name_first
#pragma weak turkey_district_names
#pragma weak rpt_info
#pragma weak rpt_table
#pragma weak air_fields
#pragma weak air_freqs
#pragma weak air_field_count
#pragma weak storage_lock
#pragma weak storage_unlock
#pragma weak storage_quick_lock
#pragma weak storage_quick_unlock
#pragma weak tr
#pragma weak board
/* The tables, weak: a test links the ones its feature needs */
extern const turkey_map_province_t turkey_map_provinces[];
extern const turkey_map_ring_t turkey_map_rings[];
extern const int8_t turkey_map_steps[][2];
extern const char turkey_map_names[];
extern const turkey_map_pt_t turkey_district_pos[];
extern const uint16_t turkey_district_first[];
extern const uint16_t turkey_district_name_first[];
extern const char turkey_district_names[];
extern const rpt_info_t rpt_info;
extern const rpt_entry_t rpt_table[];
extern const air_field_t air_fields[];
extern const air_freq_t air_freqs[];
extern const uint16_t air_field_count;

/* What a test that does not have them would otherwise miss (a UI build has its own) */
void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
const char *tr(const char *en) { return en; }
const board_t board = { .name = "refpack fixture", .flash = &sim_flash };

/* A weak table's address without the compiler deciding it for us */
static bool present(const void *p)
{
	const void *volatile v = p;
	return v != NULL;
}

typedef struct {
	uint16_t id;
	uint32_t count;
	const void *data;
} sec_t;

size_t refpack_fixture_payload(uint8_t *out, size_t cap, unsigned groups)
{
	sec_t s[REF_IDS];
	unsigned n = 0;
	static uint8_t meta[32], rkey[8192u * 10u], akey[1024u * 10u];
	if ((groups & REFPACK_MAP) && present(turkey_map_provinces) && present(turkey_district_pos)) {
		int32_t k3[3] = { TURKEY_MAP_LON0_E7, TURKEY_MAP_LAT0_E7, TURKEY_MAP_UNIT_E7 };
		int16_t k4[4] = { TURKEY_MAP_XMIN, TURKEY_MAP_YMIN, TURKEY_MAP_XMAX, TURKEY_MAP_YMAX };
		uint16_t c4[4] = { TURKEY_MAP_PROVINCES, TURKEY_MAP_RINGS, TURKEY_MAP_STEPS, TURKEY_DISTRICTS };
		memset(meta, 0, sizeof meta);
		memcpy(meta, k3, 12);	/* little-endian host, as the radio */
		memcpy(meta + 12, k4, 8);
		memcpy(meta + 20, c4, 8);
		s[n++] = (sec_t){ REF_S_META, 1, meta };
		s[n++] = (sec_t){ REF_S_PROV, TURKEY_MAP_PROVINCES, turkey_map_provinces };
		s[n++] = (sec_t){ REF_S_RING, TURKEY_MAP_RINGS, turkey_map_rings };
		s[n++] = (sec_t){ REF_S_STEP, TURKEY_MAP_STEPS, turkey_map_steps };
		s[n++] = (sec_t){ REF_S_PNAME, sizeof turkey_map_names, turkey_map_names };
		s[n++] = (sec_t){ REF_S_DPOS, TURKEY_DISTRICTS, turkey_district_pos };
		s[n++] = (sec_t){ REF_S_DFIRST, TURKEY_MAP_PROVINCES + 1u, turkey_district_first };
		s[n++] = (sec_t){ REF_S_DNFIRST, TURKEY_MAP_PROVINCES, turkey_district_name_first };
		s[n++] = (sec_t){ REF_S_DNAME, sizeof turkey_district_names, turkey_district_names };
	}
	if ((groups & REFPACK_RPT) && present(&rpt_info) && present(rpt_table) && rpt_info.count * 10u <= sizeof rkey) {
		for (uint32_t i = 0; i < rpt_info.count; i++) {
			memcpy(rkey + 10u * i, &rpt_table[i].lat_e5, 4);
			memcpy(rkey + 10u * i + 4u, &rpt_table[i].lon_e5, 4);
			rkey[10u * i + 8u] = rpt_table[i].prov;
			rkey[10u * i + 9u] = rpt_table[i].mode;
		}
		s[n++] = (sec_t){ REF_S_RINFO, 1, &rpt_info };
		s[n++] = (sec_t){ REF_S_RPT, rpt_info.count, rpt_table };
		s[n++] = (sec_t){ REF_S_RKEY, rpt_info.count, rkey };
	}
	if ((groups & REFPACK_AIR) && present(air_fields) && present(&air_field_count) && air_field_count * 10u <= sizeof akey) {
		uint32_t nf = 0;
		for (uint32_t i = 0; i < air_field_count; i++) {
			if ((uint32_t)air_fields[i].first + air_fields[i].count > nf)
				nf = (uint32_t)air_fields[i].first + air_fields[i].count;
			memcpy(akey + 10u * i, &air_fields[i].lat_e5, 4);
			memcpy(akey + 10u * i + 4u, &air_fields[i].lon_e5, 4);
			akey[10u * i + 8u] = air_fields[i].prov;
			akey[10u * i + 9u] = air_fields[i].kind;
		}
		s[n++] = (sec_t){ REF_S_AIR, air_field_count, air_fields };
		s[n++] = (sec_t){ REF_S_AFREQ, nf, air_freqs };
		s[n++] = (sec_t){ REF_S_AKEY, air_field_count, akey };
	}
	/* the head and the directory, the sections after it, each 4-byte aligned (refpack.py payload()) */
	uint32_t off = REF_HEAD + REF_ENTRY * n, coord = ref_coord_id();
	off += (4u - off % 4u) % 4u;
	if (cap < off)
		return 0;
	memset(out, 0, off);
	memcpy(out, "REF1", 4);
	out[4] = REF_DIR_VERSION;
	out[5] = (uint8_t)n;
	memcpy(out + 8, &coord, 4);
	for (unsigned k = 0; k < n; k++) {
		uint16_t rec = ref_rec_size((uint8_t)s[k].id);
		uint32_t bytes = rec * s[k].count;
		uint8_t *e = out + REF_HEAD + REF_ENTRY * k;
		memcpy(e, &s[k].id, 2);
		memcpy(e + 2, &rec, 2);
		memcpy(e + 4, &off, 4);
		memcpy(e + 8, &s[k].count, 4);
		uint32_t pad = (4u - bytes % 4u) % 4u;
		if (off + bytes + pad > cap)
			return 0;
		memcpy(out + off, s[k].data, bytes);
		memset(out + off + bytes, 0, pad);
		off += bytes + pad;
	}
	return off;
}

bool refpack_fixture_install(unsigned groups)
{
	static uint8_t p[REFPACK_MAX];
	if (sim_flash_data == NULL)
		sim_flash_create(0x1000000u, NULL);
	dpk_scan();
	size_t n = refpack_fixture_payload(p, sizeof p, groups);
	if (n == 0u)
		return false;
	dpk_hdr_t h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, "DPK1", 4);
	h.type = DPK_REF;
	h.fmt = 1;
	h.flags = ref_hint();
	h.len = (uint32_t)n;
	h.crc = storage_crc32(0, p, (uint32_t)n);
	memcpy(h.date, "20261119", 8);
	strncpy(h.name, "DDM32 TR test", sizeof h.name);
	strncpy(h.tag, "fixture", sizeof h.tag);
	dpk_writer_t w;
	return dpk_begin(&w, &h) && dpk_put(&w, 0, p, (uint32_t)n) && dpk_commit(&w) && dpk_active(DPK_REF) != NULL;
}
