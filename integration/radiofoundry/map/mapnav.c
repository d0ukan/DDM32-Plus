/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus map navigation (mapnav.h): APRS stations' order on the map, the GPS track ring, the auto repeater's
 * choice. Integer only (CK803S has no FPU); the auto repeater's distances come from rpt_way (the APRS engine's great
 * circle) twice a check, at most every 30 s. */
#include "config.h"
#if CONFIG_MAP || CONFIG_AUTO_RPT
#include <string.h>
#include "ui/mapnav.h"
#include "turkey_map_data.h"

#define M_PER_DEG	111195	/* metres per degree on the great circle (6,371 km) */

uint16_t mapnav_cos_q15(int32_t lat_e7)
{
	int64_t c = (lat_e7 < 0 ? -(int64_t)lat_e7 : lat_e7) / 100000;	/* centidegrees */
	if (c > 9000)
		c = 9000;
	int64_t num = 324000000 - 4 * c * c, den = 324000000 + c * c;	/* (32400 - 4 x^2) / (32400 + x^2), x deg */
	return (uint16_t)((num * 32768 + den / 2) / den > 32767 ? 32767 : (num * 32768 + den / 2) / den);
}

uint64_t mapnav_dist2_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7)
{
	int64_t dy = ((int64_t)lat2_e7 - lat1_e7) * M_PER_DEG / 10000000;
	int64_t dx = ((int64_t)lon2_e7 - lon1_e7) * M_PER_DEG / 10000000 * mapnav_cos_q15(lat1_e7) / 32768;
	return (uint64_t)(dx * dx) + (uint64_t)(dy * dy);
}

#if CONFIG_MAP
/* --- APRS stations --- */

uint16_t mapnav_call_hash(const char *call)
{
	uint32_t h = 2166136261u;	/* FNV-1a, folded to 16 bits */
	for (; *call; call++)
		h = (h ^ (uint8_t)*call) * 16777619u;
	h = (h >> 16) ^ (h & 0xFFFFu);
	return (uint16_t)(h != 0 ? h : 1u);
}

/* (ka, ha, ia) before (kb, hb, ib) */
static bool st_before(uint64_t ka, uint16_t ha, uint8_t ia, uint64_t kb, uint16_t hb, uint8_t ib)
{
	return ka < kb || (ka == kb && (ha < hb || (ha == hb && ia < ib)));
}

uint16_t mapnav_station_next(mapnav_station_fn get, uint8_t count, int32_t lat_e7, int32_t lon_e7, uint16_t cur)
{
	aprs_station_t s;
	uint64_t kc = 0, kb = 0, kf = 0;
	uint8_t ic = 0, ib = 0, iff = 0;
	uint16_t best = 0, first = 0;
	bool found = false;
	/* where cur is in the order */
	for (uint8_t i = 0; cur != 0 && i < count; i++) {
		if (get(i, &s) && s.has_pos && mapnav_call_hash(s.call) == cur) {
			kc = mapnav_dist2_m(lat_e7, lon_e7, s.lat, s.lon);
			ic = i;
			found = true;
			break;
		}
	}
	for (uint8_t i = 0; i < count; i++) {
		if (!get(i, &s) || !s.has_pos)
			continue;
		uint16_t h = mapnav_call_hash(s.call);
		uint64_t k = mapnav_dist2_m(lat_e7, lon_e7, s.lat, s.lon);
		if (first == 0 || st_before(k, h, i, kf, first, iff)) {	/* the nearest of all */
			first = h;
			kf = k;
			iff = i;
		}
		if (found && st_before(kc, cur, ic, k, h, i) && (best == 0 || st_before(k, h, i, kb, best, ib))) {
			best = h;
			kb = k;
			ib = i;
		}
	}
	return best != 0 ? best : first;
}

int mapnav_station_find(mapnav_station_fn get, uint8_t count, uint16_t hash, aprs_station_t *out)
{
	for (uint8_t i = 0; hash != 0 && i < count; i++)
		if (get(i, out) && out->has_pos && mapnav_call_hash(out->call) == hash)
			return i;
	return -1;
}

void mapnav_comment(const char *text, char *out, size_t len)
{
	size_t n = 0;
	const char *p = text;
	while (*p == ' ')
		p++;
	while (*p && n + 1u < len) {
		if (p[0] == '/' && p[1] == 'A' && p[2] == '=') {
			p += 3;
			for (int k = 0; k < 6 && ((*p >= '0' && *p <= '9') || *p == '-'); k++)
				p++;
			continue;
		}
		if (*p == ' ' && n > 0 && out[n - 1u] == ' ') {
			p++;	/* one space where the altitude was cut out, or several stood */
			continue;
		}
		out[n++] = *p++;
	}
	while (n > 0 && out[n - 1u] == ' ')
		n--;
	if (len > 0)
		out[n] = '\0';
}

/* --- Track --- */

/* Nearest unit (0.001 degree) from the origin; false outside int16 */
static bool unit_of(int32_t e7, int32_t origin, int16_t *out)
{
	int64_t d = (int64_t)e7 - origin;
	int64_t u = (d >= 0 ? d + TURKEY_MAP_UNIT_E7 / 2 : d - TURKEY_MAP_UNIT_E7 / 2) / TURKEY_MAP_UNIT_E7;
	if (u < INT16_MIN || u > INT16_MAX)
		return false;
	*out = (int16_t)u;
	return true;
}

void track_clear(track_t *t)
{
	memset(t, 0, sizeof *t);
}

bool track_add(track_t *t, int32_t lat_e7, int32_t lon_e7, uint32_t now_ms)
{
	track_pt_t q;
	if (!unit_of(lon_e7, TURKEY_MAP_LON0_E7, &q.x) || !unit_of(lat_e7, TURKEY_MAP_LAT0_E7, &q.y))
		return false;
	if (t->started) {
		if (now_ms - t->last_ms < TRACK_EVERY_MS)
			return false;
		if (mapnav_dist2_m(t->last_lat, t->last_lon, lat_e7, lon_e7) < (uint64_t)TRACK_MIN_M * TRACK_MIN_M)
			return false;
		const track_pt_t *l = &t->pt[(t->head + TRACK_POINTS - 1u) % TRACK_POINTS];
		if (t->n != 0 && l->x == q.x && l->y == q.y)
			return false;	/* the same cell: wait until we leave it (last_* stay) */
	} else {
		t->started = true;
		t->start_lat = lat_e7;
		t->start_lon = lon_e7;
	}
	t->pt[t->head] = q;
	t->head = (uint8_t)((t->head + 1u) % TRACK_POINTS);
	if (t->n < TRACK_POINTS)
		t->n++;
	t->last_lat = lat_e7;
	t->last_lon = lon_e7;
	t->last_ms = now_ms;
	return true;
}

bool track_get(const track_t *t, uint8_t i, int32_t *lat_e7, int32_t *lon_e7)
{
	if (i >= t->n)
		return false;
	const track_pt_t *p = &t->pt[(t->head + TRACK_POINTS - t->n + i) % TRACK_POINTS];
	*lat_e7 = TURKEY_MAP_LAT0_E7 + (int32_t)p->y * TURKEY_MAP_UNIT_E7;
	*lon_e7 = TURKEY_MAP_LON0_E7 + (int32_t)p->x * TURKEY_MAP_UNIT_E7;
	return true;
}

bool track_start(const track_t *t, int32_t *lat_e7, int32_t *lon_e7)
{
	if (!t->started)
		return false;
	*lat_e7 = t->start_lat;
	*lon_e7 = t->start_lon;
	return true;
}

uint16_t track_draw(const map_view_t *v, const track_t *t, map_line_fn fn, void *ctx)
{
	uint16_t n = 0;
	int32_t rx0 = v->x, ry0 = v->y, rx1 = v->x + v->w - 1, ry1 = v->y + v->h - 1;
	int lx = 0, ly = 0;
	for (uint8_t i = 0; i < t->n; i++) {
		int32_t la, lo;
		int x, y;
		track_get(t, i, &la, &lo);
		map_project(v, la, lo, &x, &y);
		if (i != 0 && (x != lx || y != ly)) {
			int32_t a = lx, b = ly, c = x, d = y;
			if (map_clip(&a, &b, &c, &d, rx0, ry0, rx1, ry1)) {
				fn(ctx, (int)a, (int)b, (int)c, (int)d, 0xFFu);
				n++;
			}
		}
		lx = x;
		ly = y;
	}
	return n;
}

#if CONFIG_MAP_COVERAGE
uint8_t cov_byte(int16_t dbm)
{
	if (dbm == 0)
		return 0;	/* no reading */
	int v = (int)dbm + 141;
	return (uint8_t)(v < 1 ? 1 : v > 140 ? 140 : v);
}

int16_t cov_dbm(uint8_t b)
{
	return b == 0 ? 0 : (int16_t)((int)b - 141);
}

uint8_t cov_class(uint8_t b)
{
	int16_t dbm = cov_dbm(b);
	return b == 0 ? COV_NONE : dbm >= COV_GOOD_DBM ? COV_GOOD : dbm >= COV_FAIR_DBM ? COV_FAIR : COV_POOR;
}
#endif
#endif

/* --- Auto repeater --- */

#if CONFIG_AUTO_RPT
typedef struct {
	bool dmr;
	arpt_band_fn in_band;
} arpt_ctx_t;

void arpt_set(arpt_t *s, bool on, uint32_t now_ms)
{
	s->on = on;
	s->cur = RPT_NONE;
	s->next_ms = now_ms;
}

bool arpt_suitable(const rpt_entry_t *e, bool dmr, arpt_band_fn in_band)
{
	rpt_tune_t t;
	if ((e->mode & RPT_MODE_MASK) != (dmr ? RPT_DMR : RPT_FM) || e->lat_e5 == RPT_NOPOS || e->lon_e5 == RPT_NOPOS ||
	    !rpt_tune_plan(e, &t))
		return false;
	return in_band(t.rx_hz, false) && in_band(t.tx_hz, true);
}

static bool suitable_cb(const rpt_entry_t *e, void *ctx)
{
	const arpt_ctx_t *c = ctx;
	return arpt_suitable(e, c->dmr, c->in_band);
}

uint16_t arpt_best(int32_t lat_e7, int32_t lon_e7, bool dmr, arpt_band_fn in_band)
{
	rpt_here_t h;
	arpt_ctx_t c = { dmr, in_band };
	rpt_here_set(&h, true, lat_e7, lon_e7);
	return rpt_nearest_if(&h, suitable_cb, &c);
}

uint16_t arpt_check(arpt_t *s, bool fix, int32_t lat_e7, int32_t lon_e7, bool dmr, arpt_band_fn in_band,
		    uint32_t tuned_hz, uint32_t now_ms)
{
	if (!s->on || (int32_t)(now_ms - s->next_ms) < 0)
		return RPT_NONE;
	s->next_ms = now_ms + ARPT_EVERY_MS;
	if (!fix)
		return RPT_NONE;
	uint16_t best = arpt_best(lat_e7, lon_e7, dmr, in_band);
	uint32_t mb, mc;
	uint16_t brg;
	/* rpt_get keeps one copy: what is needed of the best entry is taken before the current one is read */
	const rpt_entry_t *e = best != RPT_NONE && best != s->cur ? rpt_get(best) : NULL;
	if (e == NULL || !rpt_way(e, lat_e7, lon_e7, &mb, &brg))
		return RPT_NONE;
	uint32_t best_hz = e->rx_hz;
	e = s->cur < rpt_count() ? rpt_get(s->cur) : NULL;
	if (e != NULL && arpt_suitable(e, dmr, in_band) && rpt_way(e, lat_e7, lon_e7, &mc, &brg) &&
	    ((uint64_t)mb * 100u > (uint64_t)mc * (100u - ARPT_GAIN_PCT) || mc <= mb + ARPT_GAIN_M))
		return RPT_NONE;	/* not clearly nearer than the one offered before */
	s->cur = best;
	if (tuned_hz == best_hz)
		return RPT_NONE;	/* there already */
	return best;
}
#endif
#endif
