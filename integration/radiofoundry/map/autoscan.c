/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus auto airfield scan and auto repeater scan: the selection (autoscan.h). Reads the map pack's repeater and
 * airfield sections through rpt.c / airfield.c (the key sections for the passes, a whole record only for a candidate
 * that would be kept); the working arrays are static (the UI task's stack is short). */
#include "config.h"
#if CONFIG_AUTO_SCAN
#include <string.h>
#include "ui/autoscan.h"
#include "ui/rpt.h"
#include "ui/airfield.h"
#include "aprs/aprs_fmt.h"
#include "storage/ref_pack.h"

/* A cheap first look: a key more than radius away in latitude, or twice that in longitude (cos >= 0.5 below 60
 * degrees), is not within it. 1e-5 degree of latitude is 1.11 m */
static bool near_box(int32_t lat_e7, int32_t lon_e7, const ref_key_t *k, uint32_t radius_m)
{
	int64_t r = (int64_t)radius_m * 100000 / 111000 + 1;	/* the radius in 1e-5 degree, rounded up */
	int64_t dlat = (int64_t)k->lat_e5 - lat_e7 / 100, dlon = (int64_t)k->lon_e5 - lon_e7 / 100;
	return dlat <= r && dlat >= -r && dlon <= 2 * r && dlon >= -2 * r;
}

static uint32_t key_dist(int32_t lat_e7, int32_t lon_e7, const ref_key_t *k)
{
	return aprs_distance_m(lat_e7, lon_e7, k->lat_e5 * 100, k->lon_e5 * 100);
}

/* Into out[0..*n) (at most max, nearest first; an equal distance after the ones there) */
static void insert(as_pick_t out[], uint8_t *n, uint8_t max, const as_pick_t *p)
{
	uint8_t at = *n;
	while (at > 0 && out[at - 1u].d_m > p->d_m)
		at--;
	if (at >= max)
		return;
	uint8_t last = *n < max ? *n : (uint8_t)(max - 1u);
	memmove(&out[at + 1u], &out[at], (size_t)(last - at) * sizeof out[0]);
	out[at] = *p;
	if (*n < max)
		(*n)++;
}

/* A candidate at d_m can still go in */
static bool room(const as_pick_t out[], uint8_t n, uint8_t max, uint32_t d_m)
{
	return n < max || d_m < out[n - 1u].d_m;
}

uint32_t as_rpt_dist(uint16_t i, int32_t lat_e7, int32_t lon_e7)
{
	ref_key_t k;
	if (i >= rpt_count() || !ref_key(REF_S_RKEY, i, &k) || k.lat_e5 == RPT_NOPOS || k.lon_e5 == RPT_NOPOS)
		return UINT32_MAX;
	return key_dist(lat_e7, lon_e7, &k);
}

uint32_t as_field_dist(uint16_t f, int32_t lat_e7, int32_t lon_e7)
{
	ref_key_t k;
	if (f >= air_nfields() || !ref_key(REF_S_AKEY, f, &k))
		return UINT32_MAX;
	return key_dist(lat_e7, lon_e7, &k);
}

uint8_t as_pick_rpt(int32_t lat_e7, int32_t lon_e7, uint32_t radius_m, as_band_fn band, as_pick_t out[], uint8_t max)
{
	uint16_t count = rpt_count();
	uint8_t n = 0;
	for (uint16_t i = 0; i < count && max > 0; i++) {
		ref_key_t k;
		if (!ref_key(REF_S_RKEY, i, &k) || k.lat_e5 == RPT_NOPOS || k.lon_e5 == RPT_NOPOS)
			continue;
		uint8_t m = k.mode & RPT_MODE_MASK;
		if ((m != RPT_FM && m != RPT_DMR) || !near_box(lat_e7, lon_e7, &k, radius_m))
			continue;
		as_pick_t p = { i, m == RPT_DMR ? AS_M_DMR : AS_M_FM, key_dist(lat_e7, lon_e7, &k) };
		if (p.d_m > radius_m || !room(out, n, max, p.d_m))
			continue;
		const rpt_entry_t *e = rpt_get(i);	/* the whole record only for one that would go in */
		rpt_tune_t t;
		if (e == NULL || !rpt_tune_plan(e, &t) || !band(t.rx_hz))
			continue;
		insert(out, &n, max, &p);
	}
	return n;
}

/* The order of a frequency's use for the scan: 0 best .. 4; 0xFF never (ATIS: it never stops talking) */
static uint8_t use_rank(uint8_t type)
{
	switch (type) {
	case AIR_ATIS: return 0xFFu;
	case AIR_TWR: return 0;
	case AIR_APP: case AIR_DEP: return 1;
	case AIR_AFIS: case AIR_INFO: case AIR_CTAF: return 2;
	case AIR_GND: return 3;
	default: return 4;
	}
}

/* Airfield f's first frequency and count (false: none, or past the pack) */
static bool field_freqs(uint16_t f, uint16_t *first, uint8_t *count)
{
	const air_field_t *a = air_get(f);
	if (a == NULL || a->count == 0)
		return false;
	*first = a->first;
	*count = a->count > 64u ? 64u : a->count;
	return true;
}

uint8_t as_pick_fields(int32_t lat_e7, int32_t lon_e7, uint32_t radius_m, as_band_fn band, as_pick_t out[],
		       uint8_t max)
{
	uint16_t count = air_nfields();
	uint8_t n = 0;
	for (uint16_t i = 0; i < count && i < AS_FIELD_MAX && max > 0; i++) {
		ref_key_t k;
		if (!ref_key(REF_S_AKEY, i, &k) || !near_box(lat_e7, lon_e7, &k, radius_m))
			continue;
		as_pick_t p = { i, AS_M_AM, key_dist(lat_e7, lon_e7, &k) };
		uint16_t first;
		uint8_t c;
		if (p.d_m > radius_m || !room(out, n, max, p.d_m) || !field_freqs(i, &first, &c))
			continue;
		bool any = false;	/* a frequency to watch: not an ATIS, one the radio receives */
		for (uint8_t j = 0; j < c && !any; j++) {
			air_freq_t fq;
			any = air_freq_get((uint32_t)first + j, &fq) && use_rank(fq.type) != 0xFFu && band(air_freq_hz(&fq));
		}
		if (any)
			insert(out, &n, max, &p);
	}
	return n;
}

uint8_t as_air_letters(const as_pick_t fields[], uint8_t nf, as_band_fn band, as_pick_t out[], uint8_t max)
{
	static uint16_t first[AS_MAX], last[AS_MAX];	/* each field's frequencies, the rank taken last (0xFFFF none) */
	static uint8_t count[AS_MAX];
	static uint32_t hz[AS_MAX];
	uint8_t n = 0;
	if (nf > AS_MAX)
		nf = AS_MAX;
	if (max > AS_MAX)
		max = AS_MAX;
	for (uint8_t f = 0; f < nf; f++) {
		last[f] = 0xFFFFu;
		if (fields[f].id >= AS_FIELD_MAX || !field_freqs(fields[f].id, &first[f], &count[f]))
			count[f] = 0;
	}
	bool more = true;
	while (n < max && more) {	/* a round: each field's next best */
		more = false;
		for (uint8_t f = 0; f < nf && n < max; f++) {
			uint16_t best = 0xFFFFu;
			uint32_t best_hz = 0;
			for (uint8_t j = 0; j < count[f]; j++) {
				air_freq_t fq;
				if (!air_freq_get((uint32_t)first[f] + j, &fq))
					continue;
				uint8_t r = use_rank(fq.type);
				uint16_t rank = (uint16_t)(r << 6 | j);
				if (r == 0xFFu || (last[f] != 0xFFFFu && rank <= last[f]) || rank >= best)
					continue;
				uint32_t h = air_freq_hz(&fq);
				bool dup = !band(h);
				for (uint8_t q = 0; q < n && !dup; q++)
					dup = hz[q] == h;
				if (!dup) {
					best = rank;
					best_hz = h;
				}
			}
			if (best == 0xFFFFu)
				continue;
			last[f] = best;
			hz[n] = best_hz;
			out[n] = (as_pick_t){ (uint16_t)(fields[f].id << 6 | (best & 63u)), AS_M_AM, fields[f].d_m };
			n++;
			more = true;
		}
	}
	return n;
}

uint8_t as_letter_fields(const as_pick_t l[], uint8_t n, int32_t lat_e7, int32_t lon_e7, as_pick_t out[])
{
	uint8_t m = 0;
	for (uint8_t k = 0; k < n && k < AS_MAX; k++) {
		uint16_t f = (uint16_t)(l[k].id >> 6);
		bool seen = false;
		for (uint8_t q = 0; q < m && !seen; q++)
			seen = out[q].id == f;
		if (!seen)
			out[m++] = (as_pick_t){ f, AS_M_AM, as_field_dist(f, lat_e7, lon_e7) };
	}
	return m;
}

uint8_t as_merge(as_pick_t cur[], uint8_t n, const as_pick_t cand[], uint8_t nc, uint32_t keep_m)
{
	uint8_t m = 0;
	if (n > AS_MAX)
		n = AS_MAX;
	for (uint8_t k = 0; k < n; k++)	/* out of reach (or gone): the letter goes */
		if (cur[k].d_m <= keep_m)
			cur[m++] = cur[k];
	n = m;
	for (uint8_t c = 0; c < nc; c++) {
		uint8_t k = 0;
		while (k < n && cur[k].id != cand[c].id)
			k++;
		if (k < n) {
			cur[k].d_m = cand[c].d_m;
			continue;
		}
		if (n < AS_MAX) {
			cur[n++] = cand[c];
			continue;
		}
		uint8_t w = 0;	/* the farthest letter */
		for (k = 1; k < n; k++)
			if (cur[k].d_m >= cur[w].d_m)
				w = k;
		uint64_t d = cand[c].d_m;
		if (d * 100u <= (uint64_t)cur[w].d_m * (100u - AS_GAIN_PCT) && d + AS_GAIN_M <= cur[w].d_m)
			cur[w] = cand[c];
	}
	return n;
}

uint8_t as_place(as_pick_t cur[], uint8_t n, const as_pick_t want[], uint8_t m)
{
	uint8_t kept = 0, taken = 0, gone = 0;	/* bit k: cur[k] stays; bit j: want[j] placed; bit k: place k empty */
	if (n > AS_MAX)
		n = AS_MAX;
	if (m > AS_MAX)
		m = AS_MAX;
	for (uint8_t k = 0; k < n; k++)
		for (uint8_t j = 0; j < m; j++)
			if (!((taken >> j) & 1u) && want[j].id == cur[k].id) {
				cur[k].d_m = want[j].d_m;
				kept |= (uint8_t)(1u << k);
				taken |= (uint8_t)(1u << j);
				break;
			}
	uint8_t j = 0;
	for (uint8_t k = 0; k < n; k++) {	/* a place whose letter left: the next new one */
		if ((kept >> k) & 1u)
			continue;
		while (j < m && ((taken >> j) & 1u))
			j++;
		if (j < m) {
			cur[k] = want[j];
			taken |= (uint8_t)(1u << j);
		} else {
			gone |= (uint8_t)(1u << k);
		}
	}
	for (; j < m && n < AS_MAX; j++)	/* more: after the last */
		if (!((taken >> j) & 1u))
			cur[n++] = want[j];
	uint8_t c = 0;	/* places left empty: closed up (fewer letters: a new ring) */
	for (uint8_t k = 0; k < n; k++)
		if (!((gone >> k) & 1u))
			cur[c++] = cur[k];
	return c;
}

void as_group(as_pick_t l[], uint8_t n)
{
	for (uint8_t k = 1; k < n && k < AS_MAX; k++) {	/* each non-DMR letter moves before the DMR ones ahead of it */
		if (l[k].mode == AS_M_DMR)
			continue;
		as_pick_t t = l[k];
		uint8_t j = k;
		while (j > 0 && l[j - 1u].mode == AS_M_DMR) {
			l[j] = l[j - 1u];
			j--;
		}
		l[j] = t;
	}
}

uint8_t as_due(const as_state_t *s, bool fix, int32_t lat_e7, int32_t lon_e7, uint32_t now_ms, uint32_t gen,
	       bool stale)
{
	if (s->kind == AS_OFF)
		return AS_DUE_NO;
	if (s->picked && gen != s->gen)
		return fix ? AS_DUE_FULL : AS_DUE_NO;
	if (!fix)
		return AS_DUE_NO;
	if (!s->picked)
		return AS_DUE_YES;
	if ((int32_t)(now_ms - s->next_ms) < 0)
		return AS_DUE_NO;
	if (stale)
		return AS_DUE_YES;
	uint32_t moved = aprs_distance_m(s->lat_e7, s->lon_e7, lat_e7, lon_e7);
	return moved >= (s->kind == AS_AIR ? AS_AIR_MOVE_M : AS_RPT_MOVE_M) ? AS_DUE_YES : AS_DUE_NO;
}
#endif
