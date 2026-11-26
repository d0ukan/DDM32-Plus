/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus repeater list math (rpt.h). Reads the map pack's repeater sections (the walks only its key section:
 * position, province, mode), writes only the caller's results and its one record copy. */
#include "config.h"
#if CONFIG_REPEATERS
#if !CONFIG_DATAPACK
#error "the map, the repeaters and the airfields read the map pack (CONFIG_DATAPACK)"
#endif
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "ui/rpt.h"
#include "aprs/aprs_fmt.h"
#include "storage/ref_pack.h"

_Static_assert(offsetof(rpt_entry_t, mode) == REF_RPT_MODE && offsetof(rpt_entry_t, prov) == REF_RPT_PROV,
	       "ref_pack.h: the repeater record's layout");
_Static_assert(sizeof(rpt_info_t) == 32u, "tools/refpack.py RINFO");

uint16_t rpt_count(void)
{
	return ref_has(REF_RPT) ? ref_count(REF_S_RPT) : 0u;
}

const rpt_entry_t *rpt_get(uint16_t i)
{
	static rpt_entry_t e;
	if (!ref_rec(REF_S_RPT, i, &e))
		return NULL;
	e.name[RPT_NAME_MAX] = '\0';	/* whatever the pack holds */
	e.city[RPT_CITY_MAX] = '\0';
	return &e;
}

bool rpt_info_get(rpt_info_t *out)
{
	bool ok = ref_has(REF_RPT) && ref_rec(REF_S_RINFO, 0, out);
	if (!ok)
		memset(out, 0, sizeof *out);
	out->date[sizeof out->date - 1u] = '\0';
	out->source[sizeof out->source - 1u] = '\0';
	return ok;
}

bool rpt_sample(void)
{
	uint8_t s = 0;
	return ref_has(REF_RPT) && ref_bytes(REF_S_RINFO, offsetof(rpt_info_t, sample), &s, 1) && s != 0u;
}

bool rpt_entry_near(const rpt_entry_t *e, uint32_t hz)
{
	uint32_t in = (uint32_t)((int32_t)e->rx_hz + (int32_t)e->off * (int32_t)RPT_STEP_HZ);
	return (hz + 6250u > e->rx_hz && hz < e->rx_hz + 6250u) || (e->off != 0 && hz + 6250u > in && hz < in + 6250u);
}

/* The first entry of t[0..n) whose frequency base + step x RPT_STEP_HZ is above hz - 6,250, then the old loop's test
 * on it in Hz (an entry further up is further away) */
static bool in_set(const uint16_t *t, uint16_t n, uint32_t base, uint32_t hz)
{
	int64_t lo = (int64_t)hz - 6249 - (int64_t)base;	/* the lowest frequency that can still match, above base */
	uint32_t want = lo <= 0 ? 0u : (uint32_t)((lo + RPT_STEP_HZ - 1) / RPT_STEP_HZ);
	uint16_t a = 0, b = n;
	while (a < b) {
		uint16_t m = (uint16_t)(a + (b - a) / 2u);
		if (t[m] < want)
			a = (uint16_t)(m + 1u);
		else
			b = m;
	}
	if (a >= n)
		return false;
	uint32_t f = base + (uint32_t)t[a] * RPT_STEP_HZ;
	return hz + 6250u > f && hz < f + 6250u;
}

bool rpt_freq_listed(uint32_t hz)
{
	/* the bands the set covers (136-174, 400-480 MHz) widened by 6,250 Hz: no entry outside can match */
	if (hz >= 135993751u && hz <= 174006249u)
		return in_set(rpt_fq_vhf, rpt_fq_vhf_n, RPT_FQ_VHF_BASE, hz);
	if (hz >= 399993751u && hz <= 480006249u)
		return in_set(rpt_fq_uhf, rpt_fq_uhf_n, RPT_FQ_UHF_BASE, hz);
	return false;
}

void rpt_here_set(rpt_here_t *h, bool ok, int32_t lat_e7, int32_t lon_e7)
{
	h->ok = ok;
	h->lat_e5 = ok ? lat_e7 / 100 : 0;
	h->lon_e5 = ok ? lon_e7 / 100 : 0;
	float c = cosf((float)lat_e7 * (3.14159265f / 180.0f / 1e7f));
	h->cos_q15 = ok && c > 0.0f ? (uint16_t)(c * 32767.0f + 0.5f) : 0u;
}

/* The order's key of entry i: the squared equirectangular distance (1e-5 degree units); without our position 0 for
 * every entry (the table's order), else the largest for an entry without its own */
static uint64_t key_of(const rpt_here_t *h, const ref_key_t *e)
{
	if (!h->ok)
		return 0;
	if (e->lat_e5 == RPT_NOPOS || e->lon_e5 == RPT_NOPOS)
		return UINT64_MAX;
	int64_t dlat = (int64_t)e->lat_e5 - h->lat_e5;
	int64_t dlon = ((int64_t)e->lon_e5 - h->lon_e5) * h->cos_q15 / 32768;
	return (uint64_t)(dlat * dlat) + (uint64_t)(dlon * dlon);
}

/* Entry i's order key and (*p) its province, from the key section; an entry the pack does not give is last */
static uint64_t key(const rpt_here_t *h, uint16_t i, uint8_t *p)
{
	ref_key_t k;
	if (!ref_key(REF_S_RKEY, i, &k)) {
		*p = RPT_PROV_UNKNOWN;
		return UINT64_MAX;
	}
	*p = k.prov;
	return key_of(h, &k);
}

/* (ka, a) before (kb, b) */
static bool before(uint64_t ka, uint16_t a, uint64_t kb, uint16_t b)
{
	return ka < kb || (ka == kb && a < b);
}

static bool in_prov(uint8_t p, uint8_t prov)
{
	return prov == RPT_PROV_ALL || p == prov;
}

uint16_t rpt_next_in(const rpt_here_t *h, uint16_t cur, int dir, uint8_t prov)
{
	uint16_t n = rpt_count(), best = RPT_NONE;
	uint8_t p;
	uint64_t kc = cur < n ? key(h, cur, &p) : 0, kb = 0;
	if (cur != RPT_NONE && cur >= n)
		return RPT_NONE;
	for (uint16_t i = 0; i < n; i++) {
		uint64_t k = key(h, i, &p);
		if (!in_prov(p, prov))
			continue;
		if (cur != RPT_NONE && (dir > 0 ? !before(kc, cur, k, i) : !before(k, i, kc, cur)))
			continue;
		if (best == RPT_NONE || (dir > 0 ? before(k, i, kb, best) : before(kb, best, k, i))) {
			best = i;
			kb = k;
		}
	}
	return best;
}

uint16_t rpt_nearest_if(const rpt_here_t *h, bool (*ok)(const rpt_entry_t *e, void *ctx), void *ctx)
{
	uint16_t n = rpt_count(), best = RPT_NONE;
	uint64_t kb = 0;
	if (!h->ok)
		return RPT_NONE;
	for (uint16_t i = 0; i < n; i++) {
		ref_key_t e;
		if (!ref_key(REF_S_RKEY, i, &e) || e.lat_e5 == RPT_NOPOS || e.lon_e5 == RPT_NOPOS)
			continue;
		uint64_t k = key_of(h, &e);
		if (best != RPT_NONE && k >= kb)
			continue;	/* the whole record only for an entry nearer than the best so far */
		const rpt_entry_t *r = rpt_get(i);
		if (r == NULL || !ok(r, ctx))
			continue;
		best = i;
		kb = k;
	}
	return best;
}

uint16_t rpt_rank_in(const rpt_here_t *h, uint16_t i, uint8_t prov)
{
	uint16_t n = rpt_count(), r = 0;
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

uint16_t rpt_count_in(uint8_t prov)
{
	uint16_t n = rpt_count(), c = 0;
	for (uint16_t i = 0; i < n; i++) {
		ref_key_t k;
		if (ref_key(REF_S_RKEY, i, &k))
			c += in_prov(k.prov, prov);
	}
	return c;
}

bool rpt_way(const rpt_entry_t *e, int32_t lat_e7, int32_t lon_e7, uint32_t *meters, uint16_t *bearing)
{
	if (e->lat_e5 == RPT_NOPOS || e->lon_e5 == RPT_NOPOS)
		return false;
	*meters = aprs_distance_m(lat_e7, lon_e7, e->lat_e5 * 100, e->lon_e5 * 100);
	*bearing = aprs_bearing_deg(lat_e7, lon_e7, e->lat_e5 * 100, e->lon_e5 * 100);
	return true;
}

void rpt_locator(const rpt_entry_t *e, char *out)
{
	out[0] = '\0';
	if (e->lat_e5 == RPT_NOPOS || e->lon_e5 == RPT_NOPOS)
		return;
	int32_t lo = e->lon_e5 + 18000000, la = e->lat_e5 + 9000000;	/* 0 .. 360 / 180 degrees, 1e-5 */
	if (lo < 0)
		lo = 0;
	if (lo > 35999999)
		lo = 35999999;
	if (la < 0)
		la = 0;
	if (la > 17999999)
		la = 17999999;
	out[0] = (char)('A' + lo / 2000000);
	out[1] = (char)('A' + la / 1000000);
	out[2] = (char)('0' + lo % 2000000 / 200000);
	out[3] = (char)('0' + la % 1000000 / 100000);
	out[4] = (char)('a' + lo % 200000 * 12 / 100000);
	out[5] = (char)('a' + la % 100000 * 24 / 100000);
	out[6] = '\0';
}

const char *rpt_mode_name(const rpt_entry_t *e)
{
	static const char *const names[RPT_MODES] = { "FM", "DMR", "C4FM", "D-STAR", "P25", "NXDN", "M17" };
	uint8_t m = e->mode & RPT_MODE_MASK;
	return m < RPT_MODES ? names[m] : "?";
}

bool rpt_tune_plan(const rpt_entry_t *e, rpt_tune_t *out)
{
	uint8_t m = e->mode & RPT_MODE_MASK;
	if (m != RPT_FM && m != RPT_DMR)
		return false;
	out->rx_hz = e->rx_hz;
	out->tx_hz = (uint32_t)((int32_t)e->rx_hz + (int32_t)e->off * (int32_t)RPT_STEP_HZ);
	out->dmr = m == RPT_DMR;
	out->narrow = !out->dmr && (e->mode & RPT_NARROW) != 0;
	out->ctcss10 = out->dmr ? 0u : e->ctcss10;
	out->cc = out->dmr ? (uint8_t)(e->dmr & 0x0Fu) : 0u;
	out->ts = out->dmr ? ((e->dmr >> 4) == 2u ? 2u : 1u) : 0u;
	return true;
}
#endif
