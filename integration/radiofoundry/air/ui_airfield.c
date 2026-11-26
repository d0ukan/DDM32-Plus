/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Airfields (airfield.h): the Turkish airfields nearest first from the position APRS uses (GPS, else
 * the computer's, else the fixed one; aprs_position_now, as Tools > Repeaters), each with its distance and compass
 * direction and its first frequencies; by code without a position. Up / Down / the knob: pick (round from the end to
 * the start), OK: its details, #: the nearest one's details. In the details: Up / Down a frequency (TWR 118.100, GND,
 * APP, ATIS, ...), OK: listen to it, Back: the list. Listening is what Tools > Air band does: the VFO on that
 * frequency (radio_req_vfo + radio_req_vfo_set), received in AM because it is in the air band, and receive only: the
 * radio core refuses TX there whatever the VFO says (radio_sm_tx_check: TXDENY_AM). An 8.33 kHz channel name tunes its
 * grid point (air_freq_hz). The order is walked, never stored (air_next): the screen keeps the picked and the top
 * row's airfields and the picked frequency (6 B).
 * CONFIG_AIR_WATCH (DDM32 Plus, item 44b): # in the details watches them all: up to 6 (each channel name once, the
 * ATIS last; more: the first 6 and the toast says "6/7") on the multi-watch ring, each letter its own VFO, received
 * in AM; one frequency is simply tuned (ui_airwatch.inc).
 *
 * By province (CONFIG_AIR_PROV, DDM32 Plus round 16, as Tools > Repeaters): the screen opens on a first level, "All"
 * (the whole list as above), then the provinces that have airfields in plate order ("06 Ankara (5)": the map's names),
 * then "Unknown" when some airfields have no province. OK opens that province's list (nearest first or by code, the
 * same rows, details, listen, air watch and #, all within the province); Back returns to the first level, Back there
 * leaves. # on the first level: the nearest airfield's details in "All". Which groups have airfields is a bit each (one
 * pass when the screen opens); a row's count is one pass when it is drawn. */
#include "config.h"
#if CONFIG_AIRFIELDS
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/radio.h"
#include "radio/radio_am.h"
#include "aprs/aprs.h"
#include "ui/airfield.h"
#include "ui/air_pack.h"	/* DDM32 Plus: the user's air band pack over the table */
#include "storage/ref_pack.h"
#if CONFIG_AIR_WATCH
#include "ui_svc.h"	/* UI_MW_MAX */
#endif
#if CONFIG_AIR_PROV
#include <string.h>
#include "ui/map.h"
#endif

static uint16_t sel = AIR_NONE, top = AIR_NONE;
static uint8_t fsel;
static bool info;
static char done[48];		/* the toast after tuning (or watching) */

#if CONFIG_AIR_PROV
#define GRP_UNKNOWN	(AIR_PROVS + 1u)	/* the first level's groups: 0 All, 1..81 the plates, then Unknown */
#define GRPS		(AIR_PROVS + 2u)
static uint8_t present[(GRPS + 7u) / 8u];	/* group g has airfields (bit g; All always) */
static uint8_t filt = AIR_PROV_ALL;		/* the list's province: AIR_PROV_ALL, a plate or AIR_PROV_UNKNOWN */
static uint8_t gfocus;				/* the first level's picked row */
static uint16_t gtop;				/* and its top row (ui_draw_list) */
static bool groups;				/* the first level is shown */
#define FILT	filt
#else
#define FILT	AIR_PROV_ALL
#endif

/* Where we are now (the position APRS would send, ambiguity and all) */
static void here_now(air_here_t *h, int32_t *lat, int32_t *lon)
{
	uint32_t err;
	bool dmr;
	bool ok = aprs_position_now(lat, lon, &err, &dmr);
	air_here_set(h, ok, ok ? *lat : 0, ok ? *lon : 0);
}

/* "118.100" (the channel name, 8.33 kHz ones as pilots say them: "121.580") */
static void sb_khz(sbuf_t *b, uint32_t khz)
{
	sb_u32(b, khz / 1000u, 1);
	sb_chr(b, '.');
	sb_u32(b, khz % 1000u, 3);
}

/* "TWR 118.100" */
static void sb_air_freq(sbuf_t *b, const air_freq_t *f)
{
	sb_str(b, air_type_name(f->type));
	sb_chr(b, ' ');
	sb_khz(b, air_freq_khz(f));
}

/* "LTAC Esenboğa" */
static void sb_field(sbuf_t *b, const air_field_t *a)
{
	sb_str(b, a->code);
	sb_chr(b, ' ');
	sb_str(b, a->name);
}

static void __attribute__((noinline)) row(int y, int h, uint16_t i, bool focus, const air_here_t *here, int32_t lat,
					  int32_t lon)
{
	char buf[48], fit[48];
	sbuf_t b;
	const air_field_t *a = air_get(i);	/* the one copy: used before anything else reads a record */
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	int w = gfx_width();
	if (a == NULL)
		return;
	if (focus)
		gfx_fill(0, y, w, h, ui_col.focus);
	color_t fg = focus ? ui_col.focus_fg : ui_col.fg, dim = focus ? ui_col.focus_fg : ui_col.dim;
	/* "LTAC Esenboğa", then "28 km NE  TWR 118.100" (the tower, else the first frequency; all in the details) */
	sb_init(&b, buf, sizeof buf);
	sb_field(&b, a);
	gfx_text_fit(m, buf, w - 4, fit, sizeof fit);
	gfx_text(m, 2, y + 1, fit, fg);
	sb_init(&b, buf, sizeof buf);
	if (here->ok) {
		uint32_t meters;
		uint16_t brg;
		air_way(i, lat, lon, &meters, &brg);
		sb_range(&b, meters, brg);
		sb_str(&b, "  ");
	}
	uint8_t n = air_count(i);	/* the user's air band pack over the built-in table (air_pack.h) */
	if (n == 0) {
		sb_str(&b, TR("No frequencies in the data"));
	} else {
		uint8_t k = 0, more = 0;
		while (k + 1u < n && air_freq(i, k)->type != AIR_TWR)
			k++;
		const air_freq_t *f = air_freq(i, air_freq(i, k)->type == AIR_TWR ? k : 0u);
		uint8_t type = f->type;	/* f may be the pack's one copy, which the loop below overwrites */
		sb_air_freq(&b, f);
		/* "TWR 125.000 +1": the service has more frequencies (all of them in the details) */
		for (k = 0; k < n; k++)
			more = (uint8_t)(more + (air_freq(i, k)->type == type));
		if (more > 1u) {
			sb_str(&b, " +");
			sb_u32(&b, more - 1u, 1);
		}
	}
	gfx_text_fit(s, buf, w - 4, fit, sizeof fit);
	gfx_text(s, 2, y + m->h + 2, fit, dim);
}

static void __attribute__((noinline)) draw_list(int y, const air_here_t *h, int32_t lat, int32_t lon)
{
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	gfx_text(s, 0, y, h->ok ? TR("Nearest first") : TR("No position: by code"), h->ok ? ui_col.dim : ui_col.warn);
	y += s->h + 3;
	int rh = m->h + s->h + 4;
	int rows = (ui_layout.hint_y - y) / rh;
	if (rows < 1)
		rows = 1;
	/* Keep the picked row on the page: the top row moves only when the pick would leave it */
	uint16_t nf = air_nfields(), rs = air_rank_in(h, sel, FILT), rt = top < nf ? air_rank_in(h, top, FILT) : 0;
	if (top >= nf || rs < rt) {
		top = sel;
	} else if (rs >= rt + (uint16_t)rows) {
		top = sel;
		for (int k = 1; k < rows; k++) {
			uint16_t p = air_next_in(h, top, -1, FILT);
			if (p == AIR_NONE)
				break;
			top = p;
		}
	}
	uint16_t i = top;
	for (int k = 0; k < rows && i != AIR_NONE; k++) {
		row(y, rh, i, i == sel, h, lat, lon);
		y += rh;
		i = air_next_in(h, i, 1, FILT);
	}
}

#if CONFIG_AIR_PROV
static bool grp_has(uint8_t g)
{
	return (present[g >> 3] >> (g & 7u)) & 1u;
}

/* The province filter of group g */
static uint8_t grp_filter(uint8_t g)
{
	return g == 0 ? (uint8_t)AIR_PROV_ALL : g == GRP_UNKNOWN ? (uint8_t)AIR_PROV_UNKNOWN : g;
}

/* Which groups have airfields: one pass over the table */
static void grp_scan(void)
{
	memset(present, 0, sizeof present);
	present[0] = 1u;	/* All */
	for (uint16_t i = 0, n = air_nfields(); i < n; i++) {
		ref_key_t k;
		uint8_t p = ref_key(REF_S_AKEY, i, &k) ? k.prov : (uint8_t)AIR_PROV_UNKNOWN;
		uint8_t g = p >= 1u && p <= AIR_PROVS ? p : (uint8_t)GRP_UNKNOWN;
		present[g >> 3] |= (uint8_t)(1u << (g & 7u));
	}
}

static uint8_t grp_count(void)
{
	uint8_t n = 0;
	for (uint8_t g = 0; g < GRPS; g++)
		n += grp_has(g);
	return n;
}

/* The group on row r of the first level */
static uint8_t grp_at(uint8_t r)
{
	for (uint8_t g = 0; g < GRPS; g++)
		if (grp_has(g) && r-- == 0)
			return g;
	return 0;
}

/* "All airfields (74)", "06 Ankara (5)", "Unknown (1)" */
static void grp_item(uint16_t r, ui_item_t *it)
{
	uint8_t g = grp_at((uint8_t)r);
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	if (g == 0) {
		sb_str(&b, TR("All airfields"));
	} else if (g == GRP_UNKNOWN) {
		sb_str(&b, TR("Unknown"));
	} else {
		sb_u32(&b, g, 2);
		sb_chr(&b, ' ');
		char name[MAP_NAME_MAX];
		map_province_name((uint8_t)(g - 1u), name, sizeof name);
		sb_str(&b, name);
	}
	sb_str(&b, " (");
	sb_u32(&b, air_count_in(grp_filter(g)), 1);
	sb_chr(&b, ')');
}

static void __attribute__((noinline)) draw_groups(void)
{
	ui_list_t l = {
		.prefix = TR("Airfields"), .count = grp_count(), .focus = gfocus, .top = &gtop, .item = grp_item,
		.hint = TR("^v pick  OK list  # nearest"),
	};
	ui_draw_list(ui_view(), &ui_st, &l);
}

/* The province list's title: the province's name, "Unknown", or nothing for All */
static const char *filt_name(void)
{
	static char name[MAP_NAME_MAX];
	if (filt == AIR_PROV_ALL)
		return "";
	if (filt == AIR_PROV_UNKNOWN)
		return TR("Unknown");
	map_province_name((uint8_t)(filt - 1u), name, sizeof name);
	return name;
}
#endif

static void __attribute__((noinline)) draw_info(int y, uint16_t i, const air_here_t *h, int32_t lat, int32_t lon)
{
	static const char *const kinds[] = { TRN("Large airport"), TRN("Airport"), TRN("Small airfield") };
	char buf[48], fit[48];
	sbuf_t b;
	const air_field_t *a = air_get(i);	/* the one copy: name and kind taken before the frequencies */
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	int w = gfx_width();
	if (a == NULL)
		return;
	uint8_t kind = a->kind;
	sb_init(&b, buf, sizeof buf);
	sb_field(&b, a);
	gfx_text_fit(m, buf, w, fit, sizeof fit);
	gfx_text(m, 0, y, fit, ui_col.fg);
	y += m->h + 3;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, tr(kinds[kind < 3u ? kind : 2u]));
	if (h->ok) {
		uint32_t meters;
		uint16_t brg;
		air_way(i, lat, lon, &meters, &brg);
		sb_str(&b, "  ");
		sb_range(&b, meters, brg);
		sb_chr(&b, ' ');
		sb_u32(&b, brg, 1);
		sb_str(&b, GLYPH_DEGREE);
	}
	gfx_text_fit(s, buf, w, fit, sizeof fit);
	gfx_text(s, 0, y, fit, ui_col.dim);
	y += s->h + 4;
	uint8_t n = air_count(i);
	if (n == 0) {
		gfx_text_fit(s, TR(TEXT_ALT_OPEN "No frequencies in the data" TEXT_ALT_NEXT "No frequencies" TEXT_ALT_CLOSE), w,
			     fit, sizeof fit);	/* the long one is wider than the panel */
		gfx_text(s, 0, y, fit, ui_col.warn);
		return;
	}
	if (fsel >= n)
		fsel = 0;
	int rh = m->h + 4;
	int rows = (ui_layout.hint_y - y - 2 * (s->h + 2) - 2) / rh;
	if (rows < 1)
		rows = 1;
	int first = fsel >= rows ? fsel - rows + 1 : 0;
	for (int k = first; k < n && k < first + rows; k++) {
		const air_freq_t *f = air_freq(i, (uint8_t)k);
		bool focus = k == fsel;
		if (focus)
			gfx_fill(0, y, w, rh, ui_col.focus);
		color_t fg = focus ? ui_col.focus_fg : ui_col.fg;
		sb_init(&b, buf, sizeof buf);
		sb_air_freq(&b, f);
		gfx_text(m, 2, y + 2, buf, fg);
		if (air_freq_833(f))
			gfx_text(s, w - gfx_text_width(s, "8.33") - 2, y + 2 + (m->h - s->h) / 2, "8.33",
				 focus ? fg : ui_col.dim);
		y += rh;
	}
	gfx_text(s, 0, y + 2, TR("AM, RX only"), ui_col.rx);
	gfx_text(s, 0, y + 4 + s->h, TR("Not for navigation"), ui_col.dim);
}

static void airfield_draw(void)
{
	air_here_t h;
	int32_t lat = 0, lon = 0;
#if CONFIG_AIR_PROV
	if (groups && air_nfields() != 0) {
		draw_groups();
		return;
	}
	const char *name = filt_name();
#else
	const char *name = "";
#endif
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Airfields"), name) + 2;
	if (air_nfields() == 0) {	/* no map pack, or one without airfields */
		gfx_text(ui_layout.small, 0, y, TR("No airfield data: load the map pack"), ui_col.warn);
		ui_draw_bottom(&ui_st, TR("Back"));
		ui_draw_banner(&ui_st);
		return;
	}
	here_now(&h, &lat, &lon);
	if (sel >= air_nfields())
		sel = air_next_in(&h, AIR_NONE, 1, FILT);
	if (info)
		draw_info(y, sel, &h, lat, lon);
	else
		draw_list(y, &h, lat, lon);
#if CONFIG_AIR_WATCH
	ui_draw_bottom(&ui_st, info ? TR(TEXT_ALT_OPEN "OK listen  # watch all" TEXT_ALT_NEXT "OK listen  # all"
					 TEXT_ALT_CLOSE) : TR("^v pick  OK info  # nearest"));
#else
	ui_draw_bottom(&ui_st, info ? TR("OK listen  ^v pick  Back") : TR("^v pick  OK info  # nearest"));
#endif
	ui_draw_banner(&ui_st);
}

void ui_airfield_tune(uint16_t i, uint8_t k)
{
	if (i >= air_nfields() || k >= air_count(i))
		return;
	const air_field_t *a = air_get(i);	/* nothing below reads another record before a->code is used */
	if (a == NULL)
		return;
	const air_freq_t *f = air_freq(i, k);
	uint32_t hz = air_freq_hz(f);
	if (!radio_am_capable()) {
		ui_toast(TR("No AM receive on this radio"));
		return;
	}
	if (ui_view()->state == RS_TX) {
		ui_toast(TR("Can't tune while transmitting"));
		return;
	}
	if (!radio_am_band(hz) || !board_in_band(hz, false)) {
		ui_toast(TR("Not in the radio's bands"));
		return;
	}
	/* Tools > Air band's path: the VFO there; AM and no TX follow from the band (radio_am.h) */
	radio_req_vfo(true);
	radio_req_vfo_set(hz);
	sbuf_t b;
	sb_init(&b, done, sizeof done);
	sb_str(&b, a->code);
	sb_chr(&b, ' ');
	sb_air_freq(&b, f);
	sb_str(&b, TR(": AM, RX only"));
	ui_toast(done);
	ui_go_home();
}

#if CONFIG_AIR_WATCH
/* Whether airfield i's frequency k (hz f) is one of its frequencies before k (a channel name on two services) */
static bool __attribute__((noinline)) seen_before(uint16_t i, uint8_t k, uint32_t f)
{
	for (uint8_t j = 0; j < k; j++) {
		const air_freq_t *af = air_freq(i, j);
		if (af != NULL && air_freq_hz(af) == f)
			return true;
	}
	return false;
}

/* # in airfield i's details: all its frequencies on the multi-watch ring, each channel name once: the services in the
 * list's order, then the ATIS (the radio core holds an ATIS letter 5 s at most, then passes it over: its recording
 * never stops). More than UI_MW_MAX: the first UI_MW_MAX of that order, and the toast says how many of how many
 * ("LTAI: 6/7 frequencies watched"). One alone is tuned as OK does */
static void __attribute__((noinline)) watch_all(uint16_t i)
{
	const air_field_t *a = air_get(i);	/* air_freq below keeps its own copy, not this one */
	if (a == NULL)
		return;
	uint32_t hz[UI_MW_MAX];
	uint8_t n = 0, first = 0, atis = 0, all = 0, count = air_count(i);
	for (uint8_t pass = 0; pass < 2; pass++) {	/* 0: every service but the ATIS; 1: the ATIS */
		for (uint8_t k = 0; k < count; k++) {
			const air_freq_t *af = air_freq(i, k);	/* the user's air band pack over the table (air_pack.h) */
			if (af == NULL || (af->type == AIR_ATIS) != (pass == 1))
				continue;
			uint32_t f = air_freq_hz(af);
			bool twice = false;
			for (uint8_t j = 0; j < n; j++)
				twice = twice || hz[j] == f;
			if (twice || (n == UI_MW_MAX && seen_before(i, k, f)))
				continue;
			all++;
			if (n == UI_MW_MAX)
				continue;	/* past the ring's six: only counted */
			if (n == 0)
				first = k;
			if (pass == 1)
				atis |= (uint8_t)(1u << n);
			hz[n++] = f;
		}
	}
	if (n < 2) {
		ui_airfield_tune(i, first);
		return;
	}
	if (!radio_am_capable()) {
		ui_toast(TR("No AM receive on this radio"));
		return;
	}
	if (ui_view()->state == RS_TX) {
		ui_toast(TR("Can't tune while transmitting"));
		return;
	}
	for (uint8_t k = 0; k < n; k++) {
		if (!radio_am_band(hz[k]) || !board_in_band(hz[k], false)) {
			ui_toast(TR("Not in the radio's bands"));
			return;
		}
	}
	ui_go_home();
	if (!ui_dual_air_watch(hz, n, atis))
		return;
	sbuf_t b;
	sb_init(&b, done, sizeof done);
	sb_str(&b, a->code);
	sb_str(&b, ": ");
	if (all > n) {	/* "LTAI: 6/7 frequencies watched" */
		sb_u32(&b, n, 1);
		sb_chr(&b, '/');
		sb_u32(&b, all, 1);
		sb_chr(&b, ' ');
		sb_str(&b, TR("frequencies watched"));
	} else {
		sb_str(&b, TR("Air watch on"));
	}
	ui_toast(done);
}
#endif

#if CONFIG_AIR_PROV
/* The first level: Up / Down pick (round), OK opens the group's list, # the nearest airfield's details (All), Back
 * leaves the screen */
static bool grp_key(const input_event_t *ev, bool press)
{
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP)) {
		int dir = ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : ev->action == ACT_NAV_DOWN ? 1 : -1;
		uint8_t n = grp_count();
		gfocus = (uint8_t)((gfocus + n + dir) % n);
		ui_dirty();
		return true;
	}
	if ((ev->action == ACT_SELECT || ev->action == ACT_CONTEXT) && ev->gesture == GESTURE_PRESS) {
		bool nearest = ev->action == ACT_CONTEXT;
		if (nearest)
			gfocus = 0;
		filt = grp_filter(grp_at(gfocus));
		groups = false;
		sel = top = AIR_NONE;	/* the group's nearest (airfield_draw) */
		info = nearest;
		fsel = 0;
		if (nearest) {
			air_here_t h;
			int32_t lat, lon;
			here_now(&h, &lat, &lon);
			sel = air_nearest(&h);
		}
		ui_dirty();
		return true;
	}
	return false;
}
#endif

static bool airfield_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	if (air_nfields() == 0)
		return false;
#if CONFIG_AIR_PROV
	if (groups)
		return grp_key(ev, press);
#endif
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP)) {
		int dir = ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : ev->action == ACT_NAV_DOWN ? 1 : -1;
		if (info) {
			uint8_t n = air_count(sel);
			if (n)
				fsel = (uint8_t)((fsel + n + (dir > 0 ? 1 : n - 1)) % n);
		} else {
			air_here_t h;
			int32_t lat, lon;
			here_now(&h, &lat, &lon);
			uint16_t nx = sel < air_nfields() ? air_next_in(&h, sel, dir, FILT) : AIR_NONE;
			sel = nx != AIR_NONE ? nx : air_next_in(&h, AIR_NONE, dir, FILT);	/* round from the end to the start */
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		if (sel < air_nfields()) {
			if (!info) {
				info = true;
				fsel = 0;
			} else if (air_count(sel) == 0) {
				ui_toast(TR("No frequencies in the data"));
			} else {
				ui_airfield_tune(sel, fsel);
			}
		}
		ui_dirty();
		return true;
	}
#if CONFIG_AIR_WATCH
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS && info && sel < air_nfields()) {
		if (air_count(sel) == 0)
			ui_toast(TR("No frequencies in the data"));
		else
			watch_all(sel);
		ui_dirty();
		return true;
	}
#endif
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		air_here_t h;
		int32_t lat, lon;
		here_now(&h, &lat, &lon);
		sel = air_next_in(&h, AIR_NONE, 1, FILT);	/* the nearest (of the province) */
		info = true;
		fsel = 0;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS && info) {
		info = false;
		ui_dirty();
		return true;
	}
#if CONFIG_AIR_PROV
	/* Back from a province's list: the first level, its focus kept */
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		groups = true;
		ui_dirty();
		return true;
	}
#endif
	return false;
}

static void airfield_enter(void)
{
	sel = top = AIR_NONE;	/* the nearest first, each time the screen opens */
	info = false;
	fsel = 0;
#if CONFIG_AIR_PROV
	grp_scan();
	filt = AIR_PROV_ALL;
	groups = true;
	gfocus = 0;
	gtop = 0;
#endif
	ui_dirty();
}

const ui_scr_def_t ui_airfield_screen = {
	.name = "airfields", .flags = SF_FIELD, .key = airfield_key, .draw = airfield_draw, .enter = airfield_enter,
};
#endif
