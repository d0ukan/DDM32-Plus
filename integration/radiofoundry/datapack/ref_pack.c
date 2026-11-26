/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus map pack: validation and the UI task's readers (ref_pack.h). Installed as core/storage/ref_pack.c. */
#include "config.h"
#if CONFIG_DATAPACK
#include <string.h>
#include "storage/storage.h"
#include "storage/datapack.h"
#include "storage/ref_pack.h"
#include "ui/turkey_map_data.h"

#define PROVS		TURKEY_MAP_PROVINCES
#define GROUP_MAP	0x01FFu		/* sections 1-9 (bit id - 1) */
#define GROUP_RPT	0x0E00u		/* 10-12 */
#define GROUP_AIR	0x7000u		/* 13-15 */
#define RPT_MAX		65534u		/* below rpt.h RPT_NONE / airfield.h AIR_NONE */
#define AIR_FREQS_MAX	64u		/* air_pack.h AIR_PACK_FREQS */
#define BIT(id)		(1u << ((id) - 1u))

static const uint8_t rec_size[REF_IDS] = { 32, 18, 8, 2, 1, 4, 2, 2, 1, 32, 44, 10, 44, 4, 10 };

uint16_t ref_rec_size(uint8_t id)
{
	return id >= 1u && id <= REF_IDS ? rec_size[id - 1u] : 0u;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

uint32_t ref_coord_id(void)
{
	static const int32_t k[7] = { TURKEY_MAP_LON0_E7, TURKEY_MAP_LAT0_E7, TURKEY_MAP_UNIT_E7, TURKEY_MAP_XMIN,
				      TURKEY_MAP_YMIN, TURKEY_MAP_XMAX, TURKEY_MAP_YMAX };
	uint8_t b[28];
	for (unsigned i = 0; i < 7u; i++)
		for (unsigned j = 0; j < 4u; j++)
			b[4u * i + j] = (uint8_t)((uint32_t)k[i] >> (8u * j));
	return storage_crc32(storage_crc32(0, b, sizeof b), TURKEY_MAP_CHARSET, sizeof TURKEY_MAP_CHARSET - 1u);
}

uint16_t ref_hint(void)
{
	return DPK_REF_HINT(REF_DIR_VERSION, ref_coord_id());
}

/* ---- Validation (raw reads of the slot being checked; nothing shared written) ---- */

typedef struct {
	uint16_t off, n;	/* a payload is at most 65,472 bytes: both fit */
} sec_t;

/* Walks want NUL-terminated names in the blob [base, base + blen) from pos; the position after the last, -1 when the
 * blob ends first */
static int32_t skip_names(uint32_t base, uint32_t blen, uint32_t pos, uint32_t want, uint8_t *b)
{
	while (want != 0u) {
		if (pos >= blen)
			return -1;
		uint32_t n = blen - pos < 64u ? blen - pos : 64u;
		if (!dpk_raw(base + pos, b, n))
			return -1;
		for (uint32_t i = 0; i < n && want != 0u; i++, pos++)
			if (b[i] == 0u)
				want--;
	}
	return (int32_t)pos;
}

/* The map sections' rules (C.2 rules 2-5); s[] holds every map section */
static bool map_rules(uint32_t pa, const sec_t *s, uint8_t *b)
{
#define S(id)	s[(id) - 1u]
	if (S(REF_S_META).n != 1u || !dpk_raw(pa + S(REF_S_META).off, b, 32))
		return false;
	/* the origin, unit and bounds the code was compiled with (the coordinate id matched: a builder error if not) */
	if ((int32_t)le32(b) != TURKEY_MAP_LON0_E7 || (int32_t)le32(b + 4) != TURKEY_MAP_LAT0_E7 ||
	    (int32_t)le32(b + 8) != TURKEY_MAP_UNIT_E7 || (int16_t)le16(b + 12) != TURKEY_MAP_XMIN ||
	    (int16_t)le16(b + 14) != TURKEY_MAP_YMIN || (int16_t)le16(b + 16) != TURKEY_MAP_XMAX ||
	    (int16_t)le16(b + 18) != TURKEY_MAP_YMAX)
		return false;
	uint32_t rings = S(REF_S_RING).n, steps = S(REF_S_STEP).n, dists = S(REF_S_DPOS).n;
	uint32_t pnames = S(REF_S_PNAME).n, dnames = S(REF_S_DNAME).n;
	if (le16(b + 20) != PROVS || S(REF_S_PROV).n != PROVS || le16(b + 22) != rings || le16(b + 24) != steps ||
	    le16(b + 26) != dists || S(REF_S_DFIRST).n != PROVS + 1u || S(REF_S_DNFIRST).n != PROVS || pnames == 0u ||
	    dnames == 0u)
		return false;
	/* rings: first + count inside the steps (u32: map.c walks them with uint16_t), at least one step */
	for (uint32_t r = 0; r < rings; r++) {
		if (!dpk_raw(pa + S(REF_S_RING).off + 8u * r, b, 8))
			return false;
		uint32_t first = le16(b + 4), count = le16(b + 6);
		if (count == 0u || first + count > steps)
			return false;
	}
	/* provinces: their rings, their name, their box */
	for (uint32_t p = 0; p < PROVS; p++) {
		if (!dpk_raw(pa + S(REF_S_PROV).off + 18u * p, b, 18))
			return false;
		if ((uint32_t)le16(b) + b[2] > rings || le16(b + 4) >= pnames || (int16_t)le16(b + 10) > (int16_t)le16(b + 14) ||
		    (int16_t)le16(b + 12) > (int16_t)le16(b + 16))
			return false;
	}
	/* the name blobs end with a NUL */
	if (!dpk_raw(pa + S(REF_S_PNAME).off + pnames - 1u, b, 1) || b[0] != 0u ||
	    !dpk_raw(pa + S(REF_S_DNAME).off + dnames - 1u, b + 1, 1) || b[1] != 0u)
		return false;
	/* districts: first[] from 0 to the district count, never down; each province's names inside the blob, as many as
	 * it has districts */
	uint32_t prev = 0, nprev = 0;
	for (uint32_t p = 0; p <= PROVS; p++) {
		if (!dpk_raw(pa + S(REF_S_DFIRST).off + 2u * p, b, 2))
			return false;
		uint32_t f = le16(b);
		if ((p == 0u && f != 0u) || f < prev || (p == PROVS && f != dists))
			return false;
		if (p != 0u) {
			if (!dpk_raw(pa + S(REF_S_DNFIRST).off + 2u * (p - 1u), b, 2))
				return false;
			uint32_t at = le16(b);
			if (at < nprev || at >= dnames || skip_names(pa + S(REF_S_DNAME).off, dnames, at, f - prev, b) < 0)
				return false;
			nprev = at;
		}
		prev = f;
	}
	return true;
#undef S
}

/* Key i equals its record's lat / lon / prov / mode-or-kind */
static bool keys_ok(uint32_t pa, const sec_t *rec, const sec_t *key, uint32_t prov_at, uint32_t mode_at, uint8_t *b)
{
	for (uint32_t i = 0; i < rec->n; i++) {
		if (!dpk_raw(pa + rec->off + 44u * i, b, 44) || !dpk_raw(pa + key->off + 10u * i, b + 44, 10) ||
		    memcmp(b, b + 44, 8) != 0 || b[prov_at] != b[52] || b[mode_at] != b[53])
			return false;
	}
	return true;
}

uint8_t ref_validate(uint32_t pa, uint32_t len, uint16_t flags)
{
	sec_t s[REF_IDS];
	uint8_t b[64];
	uint16_t have = 0;
	if (len < REF_HEAD || len > 0xFFFFu || !dpk_raw(pa, b, REF_HEAD) || memcmp(b, "REF1", 4) != 0)
		return DPK_BAD;
	uint32_t n = b[5];
	/* The version items first: another directory version, another coordinate id, a hint that disagrees */
	if (b[4] != REF_DIR_VERSION || le32(b + 8) != ref_coord_id() || (flags != 0u && flags != ref_hint()))
		return DPK_OTHER;
	if (n > REF_SECTIONS || REF_HEAD + REF_ENTRY * n > len)
		return DPK_BAD;
	bool bad = false;
	memset(s, 0, sizeof s);
	for (uint32_t k = 0; k < n; k++) {
		if (!dpk_raw(pa + REF_HEAD + REF_ENTRY * k, b, REF_ENTRY))
			return DPK_BAD;
		uint32_t id = le16(b), rec = le16(b + 2), off = le32(b + 4), count = le32(b + 8);
		if (id < 1u || id > REF_IDS)
			continue;		/* unknown sections are ignored */
		if (rec != rec_size[id - 1u])
			return DPK_OTHER;	/* a known section with another layout */
		if ((have & BIT(id)) || off % 4u != 0u || off < REF_HEAD + REF_ENTRY * n || off > len ||
		    count > (len - off) / rec) {
			bad = true;
			continue;
		}
		have = (uint16_t)(have | BIT(id));
		s[id - 1u].off = (uint16_t)off;
		s[id - 1u].n = (uint16_t)count;
	}
	if (bad)
		return DPK_BAD;
	/* no two known sections overlap */
	for (uint32_t i = 0; i < REF_IDS; i++)
		for (uint32_t j = i + 1u; j < REF_IDS; j++) {
			uint32_t ei = s[i].off + (uint32_t)rec_size[i] * s[i].n, ej = s[j].off + (uint32_t)rec_size[j] * s[j].n;
			if ((have & (1u << i)) && (have & (1u << j)) && ei > s[i].off && ej > s[j].off && s[i].off < ej &&
			    s[j].off < ei)
				return DPK_BAD;
		}
	/* a feature's sections all there or none */
	uint16_t g[3] = { GROUP_MAP, GROUP_RPT, GROUP_AIR };
	for (unsigned i = 0; i < 3u; i++)
		if ((have & g[i]) != 0u && (have & g[i]) != g[i])
			return DPK_BAD;
	if ((have & GROUP_MAP) && !map_rules(pa, s, b))
		return DPK_BAD;
	if (have & GROUP_RPT) {
		const sec_t *r = &s[REF_S_RPT - 1u], *k = &s[REF_S_RKEY - 1u];
		if (s[REF_S_RINFO - 1u].n != 1u || r->n != k->n || r->n > RPT_MAX ||
		    !keys_ok(pa, r, k, REF_RPT_PROV, REF_RPT_MODE, b))
			return DPK_BAD;
	}
	if (have & GROUP_AIR) {
		const sec_t *a = &s[REF_S_AIR - 1u], *k = &s[REF_S_AKEY - 1u];
		if (a->n != k->n || a->n > RPT_MAX || !keys_ok(pa, a, k, REF_AIR_PROV, REF_AIR_FIRST + 3u, b))
			return DPK_BAD;
		for (uint32_t i = 0; i < a->n; i++) {
			if (!dpk_raw(pa + a->off + 44u * i + REF_AIR_FIRST, b, 3))
				return DPK_BAD;
			if (b[2] > AIR_FREQS_MAX || (uint32_t)le16(b) + b[2] > s[REF_S_AFREQ - 1u].n)
				return DPK_BAD;
		}
	}
	return DPK_OK;
}

/* ---- The UI task's readers ---- */

/* The directory copy of the pack in force when it was last looked at (its generation: payload address, sequence and
 * length) */
static struct {
	uint32_t pa, seq, len;
	sec_t s[REF_IDS];
	uint8_t feat;
} dir;

/* Read windows: a tag (chip address, 0 empty) and the bytes. The stream window takes sequential runs (steps, names,
 * key passes), the record window the small records read one after another (provinces, rows) */
#define BIG	128u
#define SMALL	48u
static struct {
	uint32_t a;
	uint8_t b[BIG];
} big;
static struct {
	uint32_t a;
	uint8_t b[SMALL];
} small;

static void sync(void)
{
	const dpk_info_t *p = dpk_active(DPK_REF);
	uint32_t slot = dpk_active_addr(DPK_REF), pa = p != NULL && slot != 0u ? slot + DPK_HDR : 0u;
	uint32_t seq = p != NULL ? p->seq : 0u, len = p != NULL ? p->len : 0u;
	if (pa == dir.pa && seq == dir.seq && len == dir.len)
		return;
	memset(&dir, 0, sizeof dir);
	big.a = small.a = 0;
	dir.seq = seq;
	dir.len = len;
	uint8_t b[REF_ENTRY];
	uint16_t have = 0;
	/* raw reads from this generation's own slot (a validated pack: the bounds below are the last guard) */
	if (pa != 0u && dpk_raw(pa, b, REF_ENTRY) && memcmp(b, "REF1", 4) == 0 && b[5] <= REF_SECTIONS) {
		for (uint32_t k = 0, n = b[5]; k < n; k++) {
			if (!dpk_raw(pa + REF_HEAD + REF_ENTRY * k, b, REF_ENTRY))
				break;
			uint32_t id = le16(b), off = le32(b + 4), count = le32(b + 8);
			if (id < 1u || id > REF_IDS || le16(b + 2) != rec_size[id - 1u] || off > len ||
			    count > (len - off) / rec_size[id - 1u])
				continue;
			dir.s[id - 1u].off = (uint16_t)off;
			dir.s[id - 1u].n = (uint16_t)count;
			have = (uint16_t)(have | BIT(id));
		}
	}
	dir.feat = (uint8_t)(((have & GROUP_MAP) == GROUP_MAP ? REF_MAP : 0u) |
			     ((have & GROUP_RPT) == GROUP_RPT ? REF_RPT : 0u) | ((have & GROUP_AIR) == GROUP_AIR ? REF_AIR : 0u));
	dir.pa = pa;
}

bool ref_has(uint8_t feature)
{
	sync();
	return (dir.feat & feature) == feature;
}

uint16_t ref_count(uint8_t id)
{
	sync();
	return id >= 1u && id <= REF_IDS ? dir.s[id - 1u].n : 0u;
}

/* off .. off + n inside section id (u32: no wrap); its chip address */
static bool where(uint8_t id, uint32_t off, uint32_t n, uint32_t *a)
{
	if (id < 1u || id > REF_IDS || dir.pa == 0u)
		return false;
	uint32_t size = (uint32_t)rec_size[id - 1u] * dir.s[id - 1u].n;
	if (off > size || n > size - off)
		return false;
	*a = dir.pa + dir.s[id - 1u].off + off;
	return true;
}

static bool wread(uint32_t *tag, uint8_t *buf, uint32_t size, uint32_t a, void *out, uint32_t n)
{
	if (n > size)
		return dpk_raw(a, out, n);
	if (*tag == 0u || a < *tag || a + n > *tag + size) {
		uint32_t s = a < DPK_END - size ? a : DPK_END - size;
		if (!dpk_raw(s, buf, size)) {
			*tag = 0;
			return false;
		}
		*tag = s;
	}
	memcpy(out, buf + (a - *tag), n);
	return true;
}

bool ref_rec(uint8_t id, uint32_t i, void *out)
{
	uint32_t a, r = ref_rec_size(id);
	return i < 0x10000u && where(id, i * r, r, &a) && wread(&small.a, small.b, SMALL, a, out, r);
}

bool ref_bytes(uint8_t id, uint32_t off, void *out, uint32_t n)
{
	uint32_t a;
	return where(id, off, n, &a) && wread(&big.a, big.b, BIG, a, out, n);
}

bool ref_direct(uint8_t id, uint32_t off, void *out, uint32_t n)
{
	uint32_t a;
	return where(id, off, n, &a) && dpk_raw(a, out, n);
}

bool ref_key(uint8_t id, uint32_t i, ref_key_t *k)
{
	uint8_t b[10];
	if ((id != REF_S_RKEY && id != REF_S_AKEY) || i >= 0x10000u || !ref_bytes(id, 10u * i, b, sizeof b))
		return false;
	k->lat_e5 = (int32_t)le32(b);
	k->lon_e5 = (int32_t)le32(b + 4);
	k->prov = b[8];
	k->mode = b[9];
	return true;
}

uint32_t ref_name(uint8_t id, uint32_t off, char *out, size_t n)
{
	size_t k = 0;
	if (n != 0u)
		out[0] = '\0';
	for (uint32_t at = off;; at++) {
		char c;
		if (!ref_bytes(id, at, &c, 1)) {
			if (n != 0u)
				out[0] = '\0';
			return 0;
		}
		if (c == '\0') {
			if (n != 0u)
				out[k] = '\0';
			return at - off + 1u;
		}
		if (k + 1u < n)
			out[k++] = c;
	}
}
#endif
