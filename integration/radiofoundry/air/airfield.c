/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus airfield list math (airfield.h). Reads the map pack's airfield sections (the walks only its key section:
 * position, province), writes only the caller's results and its one record copy. */
#include "config.h"
#if CONFIG_AIRFIELDS
#if !CONFIG_DATAPACK
#error "the map, the repeaters and the airfields read the map pack (CONFIG_DATAPACK)"
#endif
#include <stddef.h>
#include "ui/airfield.h"
#include "aprs/aprs_fmt.h"
#include "storage/ref_pack.h"

_Static_assert(offsetof(air_field_t, first) == REF_AIR_FIRST && offsetof(air_field_t, count) == REF_AIR_FIRST + 2u &&
	       offsetof(air_field_t, kind) == REF_AIR_FIRST + 3u && offsetof(air_field_t, prov) == REF_AIR_PROV,
	       "ref_pack.h: the airfield record's layout");
_Static_assert(sizeof(air_freq_t) == 4u, "tools/refpack.py AFREQ");

uint16_t air_nfields(void)
{
	return ref_has(REF_AIR) ? ref_count(REF_S_AIR) : 0u;
}

const air_field_t *air_get(uint16_t i)
{
	static air_field_t a;
	if (!ref_rec(REF_S_AIR, i, &a))
		return NULL;
	a.code[AIR_CODE_MAX] = '\0';	/* whatever the pack holds */
	a.name[AIR_NAME_MAX] = '\0';
	return &a;
}

bool air_freq_get(uint32_t k, air_freq_t *f)
{
	if (!ref_rec(REF_S_AFREQ, k, f))
		return false;
	if (f->type >= AIR_TYPES)
		f->type = AIR_OTH;
	return true;
}

void air_here_set(air_here_t *h, bool ok, int32_t lat_e7, int32_t lon_e7)
{
	h->ok = ok;
	h->lat_e5 = ok ? lat_e7 / 100 : 0;
	h->lon_e5 = ok ? lon_e7 / 100 : 0;
}

/* cos(degrees) in Q15 for 0..90 (table) with linear interpolation: lat_e5 in 1e-5 degree */
static int32_t cos_q15(int32_t lat_e5)
{
	static const uint16_t c[19] = { 32767, 32643, 32270, 31650, 30792, 29697, 28378, 26842, 25102, 23170, 21063,
					18795, 16384, 13848, 11207, 8481, 5690, 2856, 0 };	/* every 5 degrees */
	if (lat_e5 < 0)
		lat_e5 = -lat_e5;
	if (lat_e5 >= 9000000)
		return 0;
	int32_t k = lat_e5 / 500000, f = lat_e5 % 500000;
	return c[k] + (int32_t)(((int64_t)c[k + 1] - c[k]) * f / 500000);
}

/* The order's key of airfield i: the squared equirectangular distance at the pair's middle latitude (1e-5 degree
 * units; ordering within 0.3 % of the great circle up to a few hundred km, 1 % across the country); 0 for every
 * airfield without our position (the table's order) */
static uint64_t key(const air_here_t *h, uint16_t i, uint8_t *p)
{
	ref_key_t a;
	if (!ref_key(REF_S_AKEY, i, &a)) {
		*p = AIR_PROV_UNKNOWN;
		return UINT64_MAX;
	}
	*p = a.prov;
	if (!h->ok)
		return 0;
	int64_t dlat = (int64_t)a.lat_e5 - h->lat_e5;
	int64_t dlon = ((int64_t)a.lon_e5 - h->lon_e5) * cos_q15((a.lat_e5 + h->lat_e5) / 2) / 32768;
	return (uint64_t)(dlat * dlat) + (uint64_t)(dlon * dlon);
}

/* (ka, a) before (kb, b) */
static bool before(uint64_t ka, uint16_t a, uint64_t kb, uint16_t b)
{
	return ka < kb || (ka == kb && a < b);
}

static bool in_prov(uint8_t p, uint8_t prov)
{
	return prov == AIR_PROV_ALL || p == prov;
}

uint16_t air_next_in(const air_here_t *h, uint16_t cur, int dir, uint8_t prov)
{
	uint16_t n = air_nfields(), best = AIR_NONE;
	uint8_t p;
	if (cur != AIR_NONE && cur >= n)
		return AIR_NONE;
	uint64_t kc = cur < n ? key(h, cur, &p) : 0, kb = 0;
	for (uint16_t i = 0; i < n; i++) {
		uint64_t k = key(h, i, &p);
		if (!in_prov(p, prov))
			continue;
		if (cur != AIR_NONE && (dir > 0 ? !before(kc, cur, k, i) : !before(k, i, kc, cur)))
			continue;
		if (best == AIR_NONE || (dir > 0 ? before(k, i, kb, best) : before(kb, best, k, i))) {
			best = i;
			kb = k;
		}
	}
	return best;
}

uint16_t air_rank_in(const air_here_t *h, uint16_t i, uint8_t prov)
{
	uint16_t n = air_nfields(), r = 0;
	uint8_t p;
	if (i >= n)
		return 0;
	uint64_t ki = key(h, i, &p);
	for (uint16_t j = 0; j < n; j++) {
		uint64_t k = key(h, j, &p);
		if (in_prov(p, prov) && before(k, j, ki, i))
			r++;
	}
	return r;
}

uint16_t air_count_in(uint8_t prov)
{
	uint16_t r = 0;
	for (uint16_t i = 0, n = air_nfields(); i < n; i++) {
		ref_key_t a;
		if (ref_key(REF_S_AKEY, i, &a))
			r += in_prov(a.prov, prov);
	}
	return r;
}

void air_way(uint16_t i, int32_t lat_e7, int32_t lon_e7, uint32_t *meters, uint16_t *bearing)
{
	ref_key_t a = { 0, 0, 0, 0 };
	(void)ref_key(REF_S_AKEY, i, &a);
	*meters = aprs_distance_m(lat_e7, lon_e7, a.lat_e5 * 100, a.lon_e5 * 100);
	*bearing = aprs_bearing_deg(lat_e7, lon_e7, a.lat_e5 * 100, a.lon_e5 * 100);
}

uint32_t air_freq_khz(const air_freq_t *f)
{
	return AIR_CH_BASE_KHZ + 5u * f->ch;
}

bool air_freq_833(const air_freq_t *f)
{
	return air_freq_khz(f) % 25u != 0;
}

uint32_t air_freq_hz(const air_freq_t *f)
{
	uint32_t khz = air_freq_khz(f), r = khz % 25u;
	if (r == 0)
		return khz * 1000u;
	/* Grid point n is n x 25000 / 3 Hz (radio_am_step): x.005, x.010, x.015 are the block's points 0, 1, 2 */
	uint64_t n = (uint64_t)(khz - r) * 3u / 25u + (r / 5u - 1u);
	return (uint32_t)((n * 25000u + 1u) / 3u);
}

const char *air_type_name(uint8_t type)
{
	static const char *const names[AIR_TYPES] = { "ATIS", "DEL", "GND", "RMP", "TWR", "APP", "DEP", "AFIS", "INFO",
						      "CTAF", "PAR", "SAR", "EMR", "OPS", "OTH" };
	return type < AIR_TYPES ? names[type] : "?";
}
#endif
