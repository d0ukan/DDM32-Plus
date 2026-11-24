/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Sun & Moon (almanac.h): for the GPS position and date, sunrise / sunset, civil dawn / dusk, the
 * day's length, and the moon drawn as it looks with its age and lit fraction. Left / Right: the local offset from
 * UTC in half hours (+03:00 at first: Turkey). Needs a GPS fix with a date. */
#include "config.h"
#if CONFIG_ALMANAC
#include <math.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "ui/almanac.h"

static int16_t tz_min = 180;
static const char *const phase_names[8] = { TRN("New moon"), TRN("Waxing crescent"), TRN("First quarter"),
	TRN("Waxing gibbous"), TRN("Full moon"), TRN("Waning gibbous"), TRN("Last quarter"), TRN("Waning crescent") };

static void sb_hhmm(sbuf_t *b, int16_t min)
{
	if (min == ALMANAC_NONE) {
		sb_str(b, "--:--");
		return;
	}
	sb_u32(b, (uint32_t)min / 60u, 2);
	sb_chr(b, ':');
	sb_u32(b, (uint32_t)min % 60u, 2);
}

static int row(int y, const char *label, int16_t a, int16_t c)
{
	char buf[24];
	sbuf_t b;
	const font_t *f = ui_layout.medium;
	gfx_text(f, 0, y, label, ui_col.dim);	/* a short word: the two times (12 characters) own the right of the row */
	sb_init(&b, buf, sizeof buf);
	sb_hhmm(&b, a);
	sb_str(&b, "  ");
	sb_hhmm(&b, c);
	gfx_text(f, gfx_width() - gfx_text_width(f, buf), y, buf, ui_col.fg);
	return y + f->h + 3;
}

/* The moon as it looks: a disk, its lit part from the terminator to the lit limb (right while waxing) */
static void draw_moon(int cx, int cy, int r, float f)
{
	float c = cosf(2.0f * 3.14159265f * f);
	for (int dy = -r; dy <= r; dy++) {
		float w = sqrtf((float)(r * r - dy * dy));
		int x0 = cx - (int)w, len = 2 * (int)w + 1;
		gfx_hline(x0, cy + dy, len, ui_col.line);
		float a, b;
		if (f < 0.5f) {
			a = w * c;
			b = w;
		} else {
			a = -w;
			b = -w * c;
		}
		int la = cx + (int)lrintf(a), lb = cx + (int)lrintf(b);
		if (lb > la)
			gfx_hline(la, cy + dy, lb - la + 1, ui_col.warn);
	}
}

static void almanac_draw(void)
{
	char buf[32];
	sbuf_t b;
	ui_position_t p;
	almanac_t a;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Sun & Moon"), "") + 4;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	if (!ui_svc_position(&p) || !p.fix || p.date_dmy == 0 ||
	    !almanac_compute(p.date_dmy, p.lat_e7, p.lon_e7, tz_min, &a)) {
		gfx_text(s, 0, y, TR("Needs a GPS fix (and its date)"), ui_col.warn);
		gfx_text(s, 0, y + s->h + 2, TR("Settings > Location"), ui_col.dim);
		ui_draw_bottom(&ui_st, TR("Back"));
		ui_draw_banner(&ui_st);
		return;
	}
	sb_init(&b, buf, sizeof buf);
	sb_u32(&b, p.date_dmy / 10000u, 2);
	sb_chr(&b, '.');
	sb_u32(&b, p.date_dmy / 100u % 100u, 2);
	sb_str(&b, ".20");
	sb_u32(&b, p.date_dmy % 100u, 2);
	sb_str(&b, "  UTC");
	sb_chr(&b, tz_min < 0 ? '-' : '+');
	int16_t z = tz_min < 0 ? (int16_t)-tz_min : tz_min;
	sb_u32(&b, (uint32_t)z / 60u, 2);
	sb_chr(&b, ':');
	sb_u32(&b, (uint32_t)z % 60u, 2);
	gfx_text(s, 0, y, buf, ui_col.dim);
	y += s->h + 4;
	y = row(y, TR("Sun"), a.rise_min, a.set_min);
	y = row(y, TR("Twil."), a.dawn_min, a.dusk_min);
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("Day "));
	if (a.rise_min != ALMANAC_NONE && a.set_min != ALMANAC_NONE) {
		int16_t len = (int16_t)((a.set_min - a.rise_min + 1440) % 1440);
		sb_hhmm(&b, len);
	} else {
		sb_str(&b, TR("no rise/set"));
	}
	gfx_text(m, 0, y, buf, ui_col.fg);
	y += m->h + 6;
	int r = (ui_layout.hint_y - y - 4) / 2;
	if (r > 28)
		r = 28;
	if (r >= 6) {
		draw_moon(r + 2, y + r, r, (float)a.moon_age10 / 295.3f);
		int x = 2 * r + 10;
		gfx_text(s, x, y, tr(phase_names[a.moon_phase]), ui_col.fg);
		sb_init(&b, buf, sizeof buf);
		sb_u32(&b, a.moon_lit, 1);
		sb_str(&b, TR("% lit, "));
		sb_u32(&b, a.moon_age10 / 10u, 1);
		sb_str(&b, TR(" days"));
		gfx_text(s, x, y + s->h + 2, buf, ui_col.dim);
	}
	ui_draw_bottom(&ui_st, TR("<> UTC offset  Back"));
	ui_draw_banner(&ui_st);
}

static bool almanac_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
		tz_min = (int16_t)(tz_min + (ev->action == ACT_NAV_RIGHT ? 30 : -30));
		if (tz_min > 14 * 60)
			tz_min = -12 * 60;
		if (tz_min < -12 * 60)
			tz_min = 14 * 60;
		ui_dirty();
		return true;
	}
	return false;
}

static void almanac_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_almanac_screen = {
	.name = "almanac", .flags = SF_FIELD, .key = almanac_key, .draw = almanac_draw, .enter = almanac_enter,
};
#endif
