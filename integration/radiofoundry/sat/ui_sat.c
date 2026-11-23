/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Satellites (sat.h): every satellite's next pass, soonest first (rise in local time, the highest
 * elevation, how long), and for the one in focus the live azimuth, elevation, range, range rate and the Doppler
 * corrected RX / TX with the shifts in kHz. Up / Down: focus, Left / Right: the local offset from UTC in half hours
 * (+03:00 at first), OK: track the pass in the VFO (under way or due within 10 minutes) or stop; on a satellite with
 * nothing to track OK opens the pass alarm page (0 opens it from any). #: the arming tone while tracking (SO-50); on
 * the ISS while it is above the horizon, # sends one APRS position through its digipeater (145.825 MHz corrected for
 * Doppler, path ARISS; aprs_iss_send). The radio task does the math (sat_svc.c); this screen only draws its view.
 *
 * The pass alarm page: Up / Down a row, Left / Right its value (For: a satellite or any; Before AOS: 2, 5, 10 min;
 * Min. elevation: 0 .. 45 degrees; Every pass: armed again after each rise and kept across power cycles, for the ISS
 * or any satellite; Auto track: Off, On (the downlink; the ISS 145.800) or APRS (the ISS 145.825), receive only, from
 * the rise to the set; a change while armed arms it again), OK arms it or turns it off, Back the list. When it is due
 * (ui_sat_alarm, every UI tick on any screen): a tone and a banner; a key ends an auto track (ui_sat_auto_key). With
 * auto track Off the rise's banner on Home is a pop-up for 30 s: "ISS overhead, max 47°", "OK: track", Home's hint
 * "OK track  Back"; OK asks for the held, receive-only track on its downlink (sat_auto_start: the radio back where it
 * was 30 s after the set; "ISS tracking" once the radio holds, else a toast says why not), Back hides it, other keys
 * are Home's. Elsewhere it is the plain 8 s banner.
 *
 * Tools > ISS (ui_iss_screen): the ISS alone on one page: its next pass (or "Above now"), AOS / LOS, the live
 * azimuth, elevation and the Doppler of the downlink in use, the two downlinks (145.800 FM voice / SSTV, 145.825
 * APRS), and the pass alarm page's rows from Before AOS on (always the ISS's alarm: Every pass arms its notifications
 * at once, replacing another satellite's alarm with a toast; another's is shown, not changed). OK:
 * the held track now (as the pop-up's; under way or due within 10 min, else it says when) or stop, #: APRS via ISS,
 * 1: the other downlink (switched under a track too). */
#include "config.h"
#if CONFIG_SATELLITES
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui_nav.h"
#include "ui_svc.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "radio/sat.h"
#include "radio/beep.h"
#include "aprs/aprs.h"

#define TLE_OLD_S	(14L * 86400L)	/* elements older than this: the age in the warning color */
#define SOON_S		600

static uint8_t sel;
static int16_t tz_min = 180;
/* The pass alarm page: open, its row, what it shows (the alarm's own values once armed); Every pass and Auto track
 * (kept in the settings: UI_SET_SAT_NOTE / UI_SET_SAT_AUTO), and whether the stored alarm was armed at power on */
static bool alarm_page, a_every, note_loaded, sky_page;	/* sky_page: the sky plot is open (below) */
static uint8_t a_row, a_for, a_lead = 1, a_el, a_auto;
static const uint8_t lead_min[] = { 2, 5, 10 };
static const uint8_t min_el[] = { 0, 10, 20, 30, 45 };
#define A_ROWS	5
#define ISS_ROWS	6	/* Tools > ISS: the alarm rows from Before AOS on, then Show orbit (the sky plot) */
/* Tools > ISS: the downlink it tracks is 145.825 (else 145.800); the AOS pop-up's satellite (SAT_NONE: none; bit 7:
 * its banner not made longer yet) and its title (the banner's, to know it is still up) */
static bool iss_aprs;
static uint8_t pop = SAT_NONE;
static char al_text[40];
#define POP_MORE_MS	22000u	/* the pop-up's banner: 30 s instead of 8 (an OK out of habit tracks less long) */
/* UI_SET_SAT_NOTE: bits 0-1 for whom (0 off, 1 the ISS, 2 any), bits 2-5 the lead and elevation, 0 = 5 min, 0 deg */
enum { NOTE_OFF, NOTE_ISS, NOTE_ANY };

static int32_t sort_key(const sat_view_t *v, uint8_t i)
{
	const sat_pass_t *p = &v->pass[i];
	return p->state == SAT_PASS_OK ? p->aos : INT32_MAX - 3 + p->state;
}

/* The satellites soonest first (under way first of all) */
static uint8_t order(const sat_view_t *v, uint8_t *idx)
{
	uint8_t n = sat_count;
	for (uint8_t i = 0; i < n; i++) {
		uint8_t j = i;
		int32_t k = sort_key(v, i);
		while (j > 0 && sort_key(v, idx[j - 1]) > k) {
			idx[j] = idx[j - 1];
			j--;
		}
		idx[j] = i;
	}
	return n;
}

static void sb_hhmm_at(sbuf_t *b, int32_t s2000)
{
	int32_t day = (s2000 + tz_min * 60L) % 86400L;
	if (day < 0)
		day += 86400L;
	sb_u32(b, (uint32_t)day / 3600u, 2);
	sb_chr(b, ':');
	sb_u32(b, (uint32_t)day / 60u % 60u, 2);
}

/* Minutes:seconds under an hour, else hours and minutes */
static void sb_until(sbuf_t *b, int32_t s)
{
	if (s < 0)
		s = 0;
	if (s < 3600) {
		sb_mmss(b, (uint32_t)s);
		return;
	}
	sb_u32(b, (uint32_t)s / 3600u, 1);
	sb_chr(b, 'h');
	sb_u32(b, (uint32_t)s / 60u % 60u, 2);
}

/* Signed kHz with one decimal: "+10.2" */
static void sb_khz(sbuf_t *b, int32_t hz)
{
	sb_chr(b, hz < 0 ? '-' : '+');
	uint32_t a = (uint32_t)(hz < 0 ? -hz : hz) + 50u;
	sb_u32(b, a / 1000u, 1);
	sb_chr(b, '.');
	sb_u32(b, a / 100u % 10u, 1);
}

static void sb_deg(sbuf_t *b, int v)
{
	sb_u32(b, (uint32_t)(v < 0 ? 0 : v), 1);
	sb_str(b, GLYPH_DEGREE);
}

static bool can_track(const sat_view_t *v, uint8_t i)
{
	const sat_pass_t *p = &v->pass[i];
	return v->clock == SAT_CLOCK_OK && p->state == SAT_PASS_OK && p->aos - v->now <= SOON_S && v->now <= p->los;
}

static void row(int y, int h, const sat_view_t *v, uint8_t i, bool focus)
{
	char buf[32];
	sbuf_t b;
	const font_t *f = ui_layout.medium;
	int w = gfx_width(), cw = f->w;
	const sat_pass_t *p = &v->pass[i];
	if (focus)
		gfx_fill(0, y, w, h, ui_col.focus);
	color_t fg = focus ? ui_col.focus_fg : ui_col.fg, dim = focus ? ui_col.focus_fg : ui_col.dim;
	int ty = y + (h - f->h) / 2;
	int x = gfx_text(f, 2, ty, sat_table[i].name, v->track == i ? (focus ? fg : ui_col.rx) : fg) + cw / 2;
	if (x < cw * 7)
		x = cw * 7;		/* the times in a column */
	sb_init(&b, buf, sizeof buf);
	if (p->state == SAT_PASS_UNKNOWN) {
		sb_str(&b, GLYPH_ELLIPSIS);
	} else if (p->state == SAT_PASS_NONE) {
		sb_str(&b, TR("no pass soon"));
	} else if (p->state == SAT_PASS_BAD) {
		sb_str(&b, TR("bad elements"));
	} else {
		bool now = p->aos <= v->now;
		if (now)
			sb_str(&b, TR("now"));
		else
			sb_hhmm_at(&b, p->aos);
		gfx_text(f, x, ty, buf, now ? (focus ? fg : ui_col.rx) : fg);
		sb_init(&b, buf, sizeof buf);
		sb_deg(&b, p->max_el);
		x = gfx_text(f, x + gfx_text_width(f, "00:00") + cw / 2, ty, buf, fg);
		/* how long, in the small font at the right where there is room */
		const font_t *sm = ui_layout.small;
		sb_init(&b, buf, sizeof buf);
		sb_u32(&b, (uint32_t)(p->los - p->aos + 30) / 60u, 1);
		sb_chr(&b, 'm');
		int dw = gfx_text_width(sm, buf);
		if (x + sm->w <= w - dw - 2)
			gfx_text(sm, w - dw - 2, y + (h - sm->h) / 2, buf, dim);
		return;
	}
	const font_t *sm = ui_layout.small;	/* "no pass soon" fits after a long name in this one */
	gfx_text(p->state == SAT_PASS_UNKNOWN ? f : sm, x, p->state == SAT_PASS_UNKNOWN ? ty : y + (h - sm->h) / 2, buf,
		 p->state == SAT_PASS_BAD ? ui_col.warn : dim);
}

/* The alarm in a line: "Alarm 14:27: ISS 47°" (the tone's time), or what it is doing; false when it is off */
static bool sb_alarm(sbuf_t *b, const sat_view_t *v)
{
	const sat_alarm_t *a = &v->alarm;
	if (a->target == SAT_NONE)
		return false;
	if (a->state == SAT_AL_ARMED && a->sat < sat_count) {
		sb_str(b, TR("Alarm "));
		sb_hhmm_at(b, a->aos - (int32_t)a->lead_min * 60);
		sb_str(b, ": ");
		sb_str(b, sat_table[a->sat].name);
		sb_chr(b, ' ');
		sb_deg(b, a->max_el);
	} else if (a->state == SAT_AL_NONE) {
		sb_str(b, TR("Alarm: none in 36 h"));
	} else {
		sb_str(b, TR("Alarm: searching"));
		sb_str(b, GLYPH_ELLIPSIS);
	}
	return true;
}

/* Where the position is from, for the title of the Satellites and ISS pages (and the plot's last row) */
static const char *pos_name(const sat_view_t *v)
{
	return v->pos_src == SAT_POS_NONE ? "" : v->pos_src == SAT_POS_GPS ? TR("Pos: GPS") : TR("Pos: typed");
}

/* The focused satellite (or the one tracked) now */
static int live(int y, const sat_view_t *v)
{
	char buf[40];
	sbuf_t b;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	uint8_t i = v->live;
	if (i >= sat_count || !v->live_ok)
		return y;
	const sat_info_t *t = &sat_table[i];
	const sat_pass_t *p = &v->pass[i];
	bool up = v->el10 >= 0;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, t->name);
	sb_chr(&b, ' ');
	if (p->state == SAT_PASS_OK && p->aos > v->now) {
		sb_str(&b, TR("AOS in "));
		sb_until(&b, p->aos - v->now);
	} else if (p->state == SAT_PASS_OK && v->now <= p->los) {
		sb_str(&b, TR("LOS in "));
		sb_until(&b, p->los - v->now);
	}
	gfx_text(m, 0, y, buf, up ? ui_col.rx : ui_col.fg);
	y += m->h + 1;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("Az "));
	sb_u32(&b, (uint32_t)(v->az10 + 5) / 10u % 360u, 1);
	sb_str(&b, GLYPH_DEGREE "  ");
	sb_str(&b, TR("El "));
	if (v->el10 < 0)
		sb_chr(&b, '-');
	sb_u32(&b, (uint32_t)abs(v->el10) / 10u, 1);
	sb_chr(&b, '.');
	sb_u32(&b, (uint32_t)abs(v->el10) % 10u, 1);
	sb_str(&b, GLYPH_DEGREE);
	gfx_text(m, 0, y, buf, up ? ui_col.fg : ui_col.dim);
	y += m->h + 1;
	/* Range and range rate (km/s, two decimals; positive: going away) */
	sb_init(&b, buf, sizeof buf);
	sb_u32(&b, v->range_km, 1);
	sb_str(&b, " km  ");
	uint32_t r = (uint32_t)abs(v->rate_ms) + 5u;
	sb_chr(&b, v->rate_ms < 0 ? '-' : '+');
	sb_u32(&b, r / 1000u, 1);
	sb_chr(&b, '.');
	sb_u32(&b, r / 10u % 100u, 2);
	sb_str(&b, " km/s");
	gfx_text(s, 0, y, buf, up ? ui_col.fg : ui_col.dim);
	y += s->h + 1;
	/* Doppler: what the radio hears the downlink on and must send the uplink on, and the shifts */
	float rate = (float)v->rate_ms / 1000.0f;
	int32_t dd = sat_doppler_rx(t->down_hz, rate), du = sat_doppler_tx(t->up_hz, rate);
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, "RX ");
	sb_freq(&b, v->track == i ? v->rx_hz : sat_tune_hz(t->down_hz, dd));
	sb_chr(&b, ' ');
	sb_khz(&b, dd);
	sb_str(&b, " kHz");
	gfx_text(s, 0, y, buf, ui_col.fg);
	y += s->h + 1;
	/* TX, its shift and the uplink tone (the arming tone while it is on); " kHz" where the line has room */
	for (int k = 0; k < 2; k++) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "TX ");
		sb_freq(&b, v->track == i ? v->tx_hz : sat_tune_hz(t->up_hz, du));
		sb_chr(&b, ' ');
		sb_khz(&b, du);
		if (k == 0)
			sb_str(&b, " kHz");
		if (t->ctcss10) {
			subtone_t tone = { TONE_CTCSS, v->track == i && v->arm && t->arm10 ? t->arm10 : t->ctcss10 };
			sb_chr(&b, ' ');
			sb_tone(&b, &tone);
		}
		if (gfx_text_width(s, buf) <= gfx_width())
			break;
	}
	gfx_text(s, 0, y, buf, ui_col.fg);
	y += s->h + 1;
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.dim;
	if (v->track == i) {
		sb_str(&b, TR("Tracking Doppler"));
		c = ui_col.rx;
	} else if (v->track_state == SAT_TRK_LEFT && v->track == SAT_NONE) {
		sb_str(&b, TR("Stopped: VFO changed"));
		c = ui_col.warn;
	} else if (can_track(v, i)) {
		sb_str(&b, TR("OK: track in the VFO"));
		c = ui_col.fg;
	} else if (sb_alarm(&b, v)) {
		c = v->alarm.state == SAT_AL_ARMED ? ui_col.fg : ui_col.dim;
	} else if (v->track_state == SAT_TRK_LOS && v->track == SAT_NONE) {
		sb_str(&b, TR("Over: VFO on nominal"));
	}
	gfx_text(s, 0, y, buf, c);
	return y + s->h + 1;
}

static bool is_iss(uint8_t i)
{
	return i < sat_count && sat_table[i].catnum == SAT_ISS_CATNUM;
}

/* # on the ISS: one APRS position through its digipeater, the uplink corrected for the Doppler of now. The engine
 * checks the rest (aprs.h aprs_iss_send: callsign, position, amateur frequency, 30 s apart) and keys as for any APRS
 * frame (CSMA on the frequency first). noinline: off sat_key's frame, which holds a sat_view_t */
static void __attribute__((noinline)) iss_send(const sat_view_t *v, uint8_t i)
{
	if (v->live != i || !v->live_ok) {
		ui_toast(TR("Can't send now"));
		return;
	}
	if (v->el10 <= 0) {
		ui_toast(TR("ISS below the horizon: wait for its pass"));
		return;
	}
	uint32_t hz = sat_tune_hz(SAT_ISS_APRS_HZ, sat_doppler_tx(SAT_ISS_APRS_HZ, (float)v->rate_ms / 1000.0f));
	switch (aprs_iss_send(hz)) {
	case APRS_OK: ui_toast(TR("Sent via ISS (ARISS)")); break;
	case APRS_NO_CALL: ui_toast(TR("Set your callsign (the radio name) in qdmr.")); break;
	case APRS_NO_POSITION: ui_toast(TR("No position: GPS or a fixed one")); break;
	case APRS_ISS_WAIT: ui_toast(TR("ISS: 30 s between sends")); break;
	default: ui_toast(TR("Can't send now")); break;
	}
}

/* --- The pass alarm page --- */

static bool armed(const sat_view_t *v)
{
	return v->alarm.target != SAT_NONE;
}

static uint8_t iss_index(void)
{
	for (uint8_t i = 0; i < sat_count; i++)
		if (is_iss(i))
			return i;
	return SAT_NONE;
}

/* Every pass is for the ISS or any satellite (what the settings can keep) */
static bool every_ok(uint8_t target)
{
	return target == SAT_ALARM_ANY || is_iss(target);
}

/* The page's lead and elevation as the stored index (0 = 5 min, 0 deg) and back */
static uint8_t plan_code(void)
{
	return (uint8_t)(((a_lead + 2u) % 3u) * 5u + a_el);
}

static void plan_take(uint8_t code)
{
	if (code > 14u)
		code = 0;
	a_lead = (uint8_t)((code / 5u + 1u) % 3u);
	a_el = (uint8_t)(code % 5u);
}

/* What the settings keep for Every pass: off unless it is on (and armed) */
static void note_store(bool on)
{
	uint8_t who = !on || !every_ok(a_for) ? NOTE_OFF : a_for == SAT_ALARM_ANY ? NOTE_ANY : NOTE_ISS;
	int32_t v = who == NOTE_OFF ? 0 : (int32_t)(who | plan_code() << 2);
	if (ui_svc_setting_get(UI_SET_SAT_NOTE) != v)
		ui_svc_setting_set(UI_SET_SAT_NOTE, v);
}

static uint8_t alarm_opts(void)
{
	return (uint8_t)((a_every ? SAT_OPT_EVERY : 0u) | (uint8_t)(a_auto << SAT_OPT_AUTO_SHIFT));
}

/* At power on (the first UI tick): the stored Every pass alarm armed again, the page showing it */
static void note_load(void)
{
	int32_t v = ui_svc_setting_get(UI_SET_SAT_NOTE), au = ui_svc_setting_get(UI_SET_SAT_AUTO);
	note_loaded = true;
	a_auto = au == SAT_AUTO_DOWN || au == SAT_AUTO_APRS ? (uint8_t)au : SAT_AUTO_OFF;
	uint8_t who = (uint8_t)(v & 3), iss = iss_index();
	if (who == NOTE_OFF || who > NOTE_ANY || (who == NOTE_ISS && iss == SAT_NONE))
		return;
	a_for = who == NOTE_ANY ? (uint8_t)SAT_ALARM_ANY : iss;
	plan_take((uint8_t)((v >> 2) & 15));
	a_every = true;
	sat_alarm_set(a_for, lead_min[a_lead], min_el[a_el], alarm_opts());
}

static void alarm_open(const sat_view_t *v)
{
	alarm_page = true;
	a_row = 0;
	if (armed(v)) {
		/* what is armed */
		a_for = v->alarm.target;
		a_every = (v->alarm.opts & SAT_OPT_EVERY) != 0;
		for (uint8_t k = 0; k < sizeof lead_min; k++)
			if (lead_min[k] == v->alarm.lead_min)
				a_lead = k;
		for (uint8_t k = 0; k < sizeof min_el; k++)
			if (min_el[k] == v->alarm.min_el)
				a_el = k;
	} else {
		a_for = sel;
		a_every = false;
	}
}

/* Armed (again) with the page's values; Every pass kept in the settings, anything else clears what they keep */
static void alarm_arm(void)
{
	sat_alarm_set(a_for, lead_min[a_lead], min_el[a_el], alarm_opts());
	note_store(a_every);
}

/* The page's rows from first on in font m (Tools > ISS: from Before AOS, its For the ISS; small); the y under them */
static int __attribute__((noinline)) alarm_rows(int y, uint8_t first, const font_t *m)
{
	char buf[24];
	sbuf_t b;
	int w = gfx_width(), rh = m->h + (first ? 3 : 4);	/* Tools > ISS (first > 0): a pixel tighter, Show orbit needs the row */
	static const char *const labels[A_ROWS] = { TRN("For"), TRN("Before AOS"), TRN("Min. elevation"),
						    TRN("Every pass"), TRN("Auto track") };
	for (uint8_t r = first; r < A_ROWS; r++) {
		bool focus = r == a_row;
		if (focus)
			gfx_fill(0, y, w, rh, ui_col.focus);
		color_t fg = focus ? ui_col.focus_fg : ui_col.fg;
		int ty = y + (rh - m->h) / 2;
		gfx_text(m, 2, ty, tr(labels[r]), focus ? fg : ui_col.dim);
		sb_init(&b, buf, sizeof buf);
		if (r == 0) {
			sb_str(&b, a_for < sat_count ? sat_table[a_for].name : TR("Any satellite"));
		} else if (r == 1) {
			sb_u32(&b, lead_min[a_lead], 1);
			sb_str(&b, TR(" min"));
		} else if (r == 2) {
			sb_deg(&b, min_el[a_el]);
		} else if (r == 3) {
			sb_str(&b, a_every ? TR("On") : TR("Off"));
		} else {
			sb_str(&b, a_auto == SAT_AUTO_APRS ? "APRS" : a_auto == SAT_AUTO_DOWN ? TR("On") : TR("Off"));
		}
		gfx_text(m, w - gfx_text_width(m, buf) - 2, ty, buf, fg);
		y += rh;
	}
	return y;
}

static void __attribute__((noinline)) alarm_draw(int y, const sat_view_t *v)
{
	char buf[40];
	sbuf_t b;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	y = alarm_rows(y, 0, m) + 3;
	const sat_alarm_t *a = &v->alarm;
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.dim;
	if (!armed(v)) {
		sb_str(&b, TR("Off: OK arms it"));
	} else if (v->clock != SAT_CLOCK_OK) {
		sb_str(&b, TR("Armed: waits for GPS"));
	} else if (a->state == SAT_AL_ARMED && a->sat < sat_count) {
		/* the pass: "SO-50 13:01 23°", then when the tone sounds */
		sb_str(&b, sat_table[a->sat].name);
		sb_chr(&b, ' ');
		sb_hhmm_at(&b, a->aos);
		sb_chr(&b, ' ');
		sb_deg(&b, a->max_el);
		gfx_text(m, 0, y, buf, ui_col.rx);
		y += m->h + 1;
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, a->soon ? TR("Tone again at AOS") : TR("Tone at "));
		if (!a->soon)
			sb_hhmm_at(&b, a->aos - (int32_t)a->lead_min * 60);
		c = ui_col.fg;
	} else {
		sb_alarm(&b, v);
	}
	gfx_text(s, 0, y, buf, c);
	y += s->h + 1;
	/* Kept across power cycles with Every pass (the settings), else RAM only; auto track: what it listens to */
	gfx_text(s, 0, y, a_every ? TR("Kept across power cycles") : TR("Off after a power cycle"), ui_col.dim);
	y += s->h + 1;
	if (a_auto != SAT_AUTO_OFF) {
		/* the ISS: its voice / SSTV or APRS frequency; another satellite (or any): its downlink */
		gfx_text(s, 0, y, !is_iss(a_for) ? TR("Tracks the downlink, RX") : a_auto == SAT_AUTO_APRS ?
			 TR("Tracks ISS 145.825, RX") : TR("Tracks ISS 145.800, RX"), ui_col.dim);
	} else if (v->cost_us) {
		/* the slowest propagation measured on this radio (the list's clock line has no room for it) */
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("SGP4 at most "));
		sb_u32(&b, (v->cost_us + 50u) / 1000u, 1);
		sb_chr(&b, '.');
		sb_u32(&b, (v->cost_us + 50u) / 100u % 10u, 1);
		sb_str(&b, " ms");
		gfx_text(s, 0, y, buf, ui_col.dim);
	}
}

/* Left / Right on a row */
static void alarm_change(bool up)
{
	if (a_row == 0) {
		/* the satellites, then any; Every pass only for the ISS or any */
		uint8_t n = (uint8_t)(sat_count + 1u), at = a_for < sat_count ? a_for : sat_count;
		at = (uint8_t)((at + (up ? 1u : n - 1u)) % n);
		a_for = at < sat_count ? at : (uint8_t)SAT_ALARM_ANY;
		if (!every_ok(a_for))
			a_every = false;
	} else if (a_row == 1) {
		a_lead = (uint8_t)((a_lead + (up ? 1u : sizeof lead_min - 1u)) % sizeof lead_min);
	} else if (a_row == 2) {
		a_el = (uint8_t)((a_el + (up ? 1u : sizeof min_el - 1u)) % sizeof min_el);
	} else if (a_row == 3) {
		a_every = !a_every;
		if (a_every && !every_ok(a_for)) {
			uint8_t iss = iss_index();
			a_for = iss != SAT_NONE ? iss : (uint8_t)SAT_ALARM_ANY;	/* Every pass: the ISS (or any) */
		}
	} else {
		a_auto = (uint8_t)((a_auto + (up ? 1u : 2u)) % 3u);
		ui_svc_setting_set(UI_SET_SAT_AUTO, a_auto);
	}
}

static bool alarm_key(const input_event_t *ev, const sat_view_t *v)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		alarm_page = false;
		ui_dirty();
		return true;
	}
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN)) {
		a_row = (uint8_t)((a_row + (ev->action == ACT_NAV_DOWN ? 1u : A_ROWS - 1u)) % A_ROWS);
		ui_dirty();
		return true;
	}
	if ((press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) || ev->action == ACT_ENC_STEP) {
		alarm_change(ev->action == ACT_ENC_STEP ? ev->value > 0 : ev->action == ACT_NAV_RIGHT);
		if (armed(v))
			alarm_arm();	/* a change while armed: armed again with it */
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		if (armed(v)) {
			sat_alarm_set(SAT_NONE, 0, 0, 0);
			note_store(false);
			ui_toast(TR("Pass alarm off"));
		} else {
			alarm_arm();
			ui_toast(a_every ? TR("Armed for every pass") : TR("Pass alarm armed"));
		}
		ui_dirty();
		return true;
	}
	return false;
}

/* The AOS pop-up's banner is still the one up (not hidden, timed out or replaced) */
static bool pop_up(void)
{
	return ui_st.banner.kind == UI_BANNER_INFO && strcmp(ui_st.banner.title, al_text) == 0;
}

static uint8_t iss_mode(void)
{
	return iss_aprs ? SAT_AUTO_APRS : SAT_AUTO_DOWN;
}

/* Every UI tick (ui_app.c), on any screen: the stored alarm armed at the first; whether an auto track may take the
 * radio now (Home, this screen or Tools > ISS, nothing on air, no scan); an alarm due sounds its tone here and gives its text for
 * the banner, none while transmitting (it waits for the end, then says the time left) */
const char *ui_sat_alarm(void)
{
	sat_alarm_event_t ev;
	if (!note_loaded)
		note_load();
	if (pop != SAT_NONE && (pop & 0x80u)) {
		pop &= 0x7Fu;
		if (pop_up())
			ui_st.banner.until_ms += POP_MORE_MS;	/* the banner went up a tick ago: up for 30 s */
	}
	const radio_view_t *rv = ui_view();
	bool tx = rv->state == RS_TX || rv->state == RS_TX_DENIED;
	uint8_t scr = ui_depth() == 0 ? SCR_HOME : ui_frame()->scr;
	bool em = ui_svc_emergency_active();	/* an emergency runs: no track takes the radio, no pop-up offers one */
	sat_ui_idle(rv->state == RS_IDLE && rv->scan == 0 && !rv->hotspot && !em &&
		    (scr == SCR_HOME || scr == SCR_SATELLITES || scr == SCR_ISS));
	if (tx || !sat_alarm_take(&ev) || ev.sat >= sat_count)
		return NULL;
	if (ev.kind == SAT_ALARM_NOTRACK) {
		/* the user's held track refused (the service's or the radio core's word) */
		ui_toast(ev.max_el == SAT_NO_BUSY ? TR("Can't track: radio busy") : ev.max_el == SAT_NO_MANUAL ?
			 TR("Can't track: a track runs") : TR("Can't track: no pass now"));
		return NULL;
	}
	sbuf_t b;
	sb_init(&b, al_text, sizeof al_text);
	sb_str(&b, sat_table[ev.sat].name);
	if (ev.kind == SAT_ALARM_SOON) {
		sb_str(&b, TR(" in "));
		sb_u32(&b, (uint32_t)(ev.in_s < 60 ? 1 : (ev.in_s + 30) / 60), 1);
		sb_str(&b, TR(" min, max "));
	} else if (ev.kind == SAT_ALARM_TRACK) {
		sb_str(&b, TR(" tracking, max "));	/* the rise's tone sounded a second ago */
	} else if (a_auto == SAT_AUTO_OFF && !ui_st.banner.hold && scr == SCR_HOME && !ui_st.entry && !em) {
		sb_str(&b, TR(" overhead, max "));	/* the pop-up: OK tracks it (ui_sat_auto_key) */
		pop = (uint8_t)(ev.sat | 0x80u);
	} else {
		sb_str(&b, TR(" AOS now, max "));
	}
	sb_deg(&b, ev.max_el);
	if (ev.kind != SAT_ALARM_TRACK)
		radio_req_beep(ev.kind == SAT_ALARM_SOON ? BEEP_TOT_WARN : BEEP_SCAN_HOLD);
	return al_text;
}

const char *ui_sat_pop_text(void)
{
	return pop != SAT_NONE && (pop & 0x80u) ? TR("OK: track") : NULL;
}

/* The pop-up is up on Home (nothing typed): Home's hint row; NULL otherwise (gone: forgotten) */
const char *ui_sat_pop_hint(void)
{
	if (pop != SAT_NONE && !pop_up())
		pop = SAT_NONE;
	return pop != SAT_NONE && ui_depth() == 0 && !ui_st.entry ? TR("OK track  Back") : NULL;
}

/* From ui_app_key before anything else. The AOS pop-up up: OK starts the held track, any other key hides it; either
 * way the key is used for that. While an auto track runs, a key press (or PTT / monitor down, which go on to the
 * radio: it refuses TX meanwhile) ends it, the radio back where it was; the key is used for that (on Tools > ISS the
 * keys stay the page's: its OK stops it). The emergency and SOS keys are never taken: the radio core ends the hold
 * for the emergency itself (emergency_tick), the service follows */
bool ui_sat_auto_key(const input_event_t *ev)
{
	bool pass = ev->action == ACT_PTT || ev->action == ACT_MONITOR;
	if (ev->action == ACT_EMERGENCY || ev->action == ACT_SOS)
		return false;
	if (ui_sat_pop_hint() != NULL && ev->gesture == GESTURE_PRESS &&
	    (ev->action == ACT_SELECT || ev->action == ACT_BACK)) {
		uint8_t i = pop & 0x7Fu;
		pop = SAT_NONE;
		ui_st.banner.kind = UI_BANNER_NONE;
		if (ev->action == ACT_SELECT)
			sat_auto_start(i, is_iss(i) ? iss_mode() : SAT_AUTO_DOWN);	/* the service says (ui_sat_alarm) */
		ui_dirty();
		return true;
	}
	if (!sat_auto_on() || (!pass && ui_depth() != 0 && (ui_frame()->scr == SCR_ISS ||
	    (ui_frame()->scr == SCR_SATELLITES && (ev->action == ACT_DIGIT_2 || (sky_page && ev->action == ACT_BACK))))))
		return false;
	if (pass ? ev->gesture != GESTURE_DOWN :
	    ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_LONG && ev->action != ACT_ENC_STEP)
		return !pass;		/* the rest of a key that ended it (its release, repeats) */
	sat_auto_cancel();
	ui_toast(TR("Auto track off: radio back"));
	ui_dirty();
	return !pass;
}

/* --- The sky plot (key 2 on Tools > Satellites and Tools > ISS) --- */

/* "Az 123°  El 23.4°" (the live look) */
static void sb_azel(sbuf_t *b, const sat_view_t *v)
{
	sb_str(b, TR("Az "));
	sb_u32(b, (uint32_t)(v->az10 + 5) / 10u % 360u, 1);
	sb_str(b, GLYPH_DEGREE "  ");
	sb_str(b, TR("El "));
	if (v->el10 < 0)
		sb_chr(b, '-');
	sb_u32(b, (uint32_t)abs(v->el10) / 10u, 1);
	sb_chr(b, '.');
	sb_u32(b, (uint32_t)abs(v->el10) % 10u, 1);
	sb_str(b, GLYPH_DEGREE);
}

/* The pass's track comes from the radio task (sat_sky): point k's azimuth in 360/256 degrees and elevation in degrees,
 * evenly from the rise to the set. The UI task cannot run SGP4 itself (its stack has no room: the cos in it is 900
 * bytes deep), so the points live in the service (64 x 2 B) and are drawn from there */
static uint8_t sky_idx = SAT_NONE;	/* the satellite the page shows (Satellites: the focus; Tools > ISS: the ISS) */
static uint8_t sky_n;			/* the points as drawn last: their count (the tests read it) ... */
static const uint8_t *sky_az, *sky_el;	/* ... and where they are */
static int sky_cx, sky_cy, sky_r;	/* the plot as drawn: centre and radius, pixels */

static void sky_open(uint8_t i)
{
	sky_page = true;
	sky_idx = i;
	ui_dirty();
}

/* The pixel of an azimuth (degrees x 100, from north, clockwise) and elevation (degrees x 100) */
static void sky_xy(int32_t az100, int32_t el100, int *x, int *y)
{
	double a = (double)az100 * (3.14159265358979323846 / 18000.0);
	double rr = (double)sky_r * (double)(9000 - el100) / 9000.0;
	*x = sky_cx + (int)lrint(rr * sin(a));
	*y = sky_cy - (int)lrint(rr * cos(a));
}

static void sky_line(int x0, int y0, int x1, int y1, color_t c)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, er = dx + dy;
	for (;;) {
		gfx_pixel(x0, y0, c);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * er;
		if (e2 >= dy) {
			er += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			er += dx;
			y0 += sy;
		}
	}
}

/* A circle (midpoint algorithm) */
static void sky_ring(int cx, int cy, int r, color_t c)
{
	int x = r, y = 0, er = 1 - r;
	while (x >= y) {
		gfx_pixel(cx + x, cy + y, c);
		gfx_pixel(cx - x, cy + y, c);
		gfx_pixel(cx + x, cy - y, c);
		gfx_pixel(cx - x, cy - y, c);
		gfx_pixel(cx + y, cy + x, c);
		gfx_pixel(cx - y, cy + x, c);
		gfx_pixel(cx + y, cy - x, c);
		gfx_pixel(cx - y, cy - x, c);
		y++;
		if (er < 0) {
			er += 2 * y + 1;
		} else {
			x--;
			er += 2 * (y - x) + 1;
		}
	}
}

/* A text line, wrapped at its spaces to the screen's width; the y under it */
static int sky_row(const font_t *f, int y, const char *text, color_t c)
{
	char out[48];
	while (*text) {
		strncpy(out, text, sizeof out - 1);
		out[sizeof out - 1] = 0;
		char *sp;
		while (gfx_text_width(f, out) > gfx_width() - 2 && (sp = strrchr(out, ' ')) != NULL)
			*sp = 0;
		gfx_text(f, 0, y, out, c);
		y += f->h + 1;
		text += strlen(out);
		while (*text == ' ')
			text++;
	}
	return y;
}

/* What stops the plot: the line, and what to do about it */
static void sky_note(int y, const char *what, const char *hint)
{
	const font_t *s = ui_layout.small;
	y = sky_row(s, y, what, ui_col.warn);
	if (hint != NULL)
		sky_row(s, y, hint, ui_col.dim);
}

/* The disc, the 30 and 60 degree rings, the cross, N / E / S / W; the plot's geometry into sky_cx / sky_cy / sky_r */
static void __attribute__((noinline)) sky_frame(int y, int h)
{
	const font_t *s = ui_layout.small;
	int w = gfx_width();
	int r = (h - 2 * s->h - 2) / 2, rw = (w - 2 * (s->w + 3)) / 2;
	if (r > rw)
		r = rw;
	if (r < 12)
		r = 12;
	sky_r = r;
	sky_cx = w / 2;
	sky_cy = y + s->h + 1 + r;
	gfx_hline(sky_cx - r, sky_cy, 2 * r + 1, ui_col.line);
	gfx_vline(sky_cx, sky_cy - r, 2 * r + 1, ui_col.line);
	sky_ring(sky_cx, sky_cy, r / 3, ui_col.line);
	sky_ring(sky_cx, sky_cy, r * 2 / 3, ui_col.line);
	sky_ring(sky_cx, sky_cy, r, ui_col.dim);
	const char *n = TR("N"), *e = TR("E"), *so = TR("S"), *wv = TR("W");
	gfx_text(s, sky_cx - gfx_text_width(s, n) / 2, sky_cy - r - s->h - 1, n, ui_col.fg);
	gfx_text(s, sky_cx - gfx_text_width(s, so) / 2, sky_cy + r + 2, so, ui_col.dim);
	gfx_text(s, sky_cx + r + 3, sky_cy - s->h / 2, e, ui_col.dim);
	gfx_text(s, sky_cx - r - 3 - gfx_text_width(s, wv), sky_cy - s->h / 2, wv, ui_col.dim);
}

/* The pass's track: a line from the rise to the set, the rise a filled square, the set an open one */
static void __attribute__((noinline)) sky_pass(void)
{
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0, xr = 0, yr = 0;
	for (uint8_t k = 0; k < sky_n; k++) {
		sky_xy((int32_t)sky_az[k] * 36000 / 256, (int32_t)sky_el[k] * 100, &x1, &y1);
		if (k)
			sky_line(x0, y0, x1, y1, ui_col.fg);
		else
			xr = x1, yr = y1;
		x0 = x1;
		y0 = y1;
	}
	gfx_fill(xr - 2, yr - 2, 5, 5, ui_col.rx);
	gfx_fill(x1 - 2, y1 - 2, 5, 5, ui_col.bg);
	gfx_rect(x1 - 2, y1 - 2, 5, 5, ui_col.tx);
}

/* The page for satellite i's pass, under the title at y (the screens' own; v: their view, taken after sat_seen(i)); the
 * texts under the plot: AOS / LOS times, the top and the time to the next event, the live azimuth and elevation */
static void __attribute__((noinline)) sky_draw(const sat_view_t *v, uint8_t i, int y)
{
	char buf[40];
	sbuf_t b;
	const font_t *s = ui_layout.small;
	const sat_pass_t *p = &v->pass[i < sat_count ? i : 0];
	sky_n = 0;
	if (v->clock != SAT_CLOCK_OK || i >= sat_count) {
		sky_note(y, i >= sat_count ? TR("bad elements") : v->clock == SAT_CLOCK_NONE ? TR("Waiting for the GPS time") :
			 TR("Waiting for a GPS position"), i >= sat_count ? NULL : TR("Needs a GPS fix or a typed position"));
	} else if (p->state != SAT_PASS_OK) {
		sky_note(y, p->state == SAT_PASS_NONE ? TR("no pass soon") : p->state == SAT_PASS_BAD ? TR("bad elements") :
			 TR("Looking for the pass"), NULL);
	} else if ((sky_n = sat_sky(i, p->aos, p->los, &sky_az, &sky_el)) < 2) {
		/* 0: the radio task is working it out (a moment), 1: it cannot */
		sky_note(y, sky_n ? TR("bad elements") : GLYPH_ELLIPSIS, NULL);
		sky_n = 0;
	} else {
		int th = 4 * (s->h + 1) + 2;
		sky_frame(y, ui_layout.hint_y - y - th);
		sky_pass();
		bool up = p->aos <= v->now;
		if (v->live == i && v->live_ok && v->el10 >= 0) {
			int x, yy;
			sky_xy((int32_t)v->az10 * 10, (int32_t)v->el10 * 10, &x, &yy);
			gfx_round_fill(x - 3, yy - 3, 7, 7, ui_col.warn);
		}
		y = sky_cy + sky_r + s->h + 4;
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "AOS ");
		sb_hhmm_at(&b, p->aos);
		sb_str(&b, "  LOS ");
		sb_hhmm_at(&b, p->los);
		y = sky_row(s, y, buf, ui_col.fg);
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Max "));
		sb_deg(&b, p->max_el);
		sb_str(&b, "  ");
		sb_str(&b, up ? TR("LOS in ") : TR("AOS in "));
		sb_until(&b, (up ? p->los : p->aos) - v->now);
		y = sky_row(s, y, buf, up ? ui_col.rx : ui_col.fg);
		if (v->live == i && v->live_ok) {
			sb_init(&b, buf, sizeof buf);
			sb_azel(&b, v);
			y = sky_row(s, y, buf, v->el10 >= 0 ? ui_col.warn : ui_col.dim);
		}
		if (v->pos_src != SAT_POS_NONE)
			gfx_text(s, 0, y, pos_name(v), ui_col.dim);
	}
	ui_draw_bottom(&ui_st, TR("Back"));
	ui_draw_banner(&ui_st);
}

/* Back returns; the page has no other keys (OK does not track from here) */
static bool sky_key(const input_event_t *ev)
{
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		sky_page = false;
		sat_sky_close();
		ui_dirty();
		return true;
	}
	return false;
}

/* --- The screen --- */

static void sat_draw(void)
{
	char buf[40];
	sbuf_t b;
	sat_view_t v;
	uint8_t idx[SAT_MAX];
	gfx_clear(ui_col.bg);
	if (sel >= sat_count)
		sel = 0;
	sat_seen(sel);
	sat_view(&v);
	int y = ui_draw_top(ui_view(), &ui_st, sky_page ? TR("Sky") : alarm_page ? TR("Pass alarm") : TR("Satellites"),
			    sky_page && sky_idx < sat_count ? sat_table[sky_idx].name : alarm_page ? "" : pos_name(&v)) + 2;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	if (sky_page) {
		sky_draw(&v, sky_idx, y);
		return;
	}
	if (alarm_page) {
		alarm_draw(y + 2, &v);
		ui_draw_bottom(&ui_st, armed(&v) ? TR("OK off  <> change") : TR("OK arm  ^v<> change"));
		ui_draw_banner(&ui_st);
		return;
	}
	if (v.clock != SAT_CLOCK_OK) {
		y = sky_row(s, y, v.clock == SAT_CLOCK_NONE ? TR("Waiting for the GPS time") : TR("Waiting for a GPS position"),
			    ui_col.warn);
		sky_row(s, y, TR("Needs a GPS fix or a typed position"), ui_col.dim);	/* wrapped: it is wider than the screen */
		ui_draw_bottom(&ui_st, TR("OK alarm  2 sky  Back"));	/* OK opens the alarm page here too (sat_key) */
		ui_draw_banner(&ui_st);
		return;
	}
	/* The time, the offset, the focused satellite's elements' age */
	int32_t age = v.now - (int32_t)sat_table[sel].el.epoch;
	sb_init(&b, buf, sizeof buf);
	sb_hhmm_at(&b, v.now);
	sb_str(&b, " UTC");
	sb_chr(&b, tz_min < 0 ? '-' : '+');
	int16_t z = tz_min < 0 ? (int16_t)-tz_min : tz_min;
	sb_u32(&b, (uint32_t)z / 60u, 2);
	sb_chr(&b, ':');
	sb_u32(&b, (uint32_t)z % 60u, 2);
	sb_str(&b, TR("  TLE "));
	sb_i32(&b, age / 86400L);
	sb_str(&b, TR(" d"));
	bool old = age > TLE_OLD_S || age < -86400L;
	int lw = gfx_text(s, 0, y, buf, old ? ui_col.warn : ui_col.dim);
	if (v.cost_us) {
		/* the slowest propagation, where there is room for it */
		sb_init(&b, buf, sizeof buf);
		sb_u32(&b, (v.cost_us + 50u) / 1000u, 1);
		sb_chr(&b, '.');
		sb_u32(&b, (v.cost_us + 50u) / 100u % 10u, 1);
		sb_str(&b, " ms");
		int cw = gfx_text_width(s, buf);
		if (lw + s->w <= gfx_width() - cw)
			gfx_text(s, gfx_width() - cw, y, buf, ui_col.dim);
	}
	y += s->h + 3;
	/* The passes, as many rows as leave room for the live lines */
	int rh = m->h + 4;
	int rows = (ui_layout.hint_y - y - (2 * m->h + 4 * s->h + 11)) / rh;
	uint8_t n = order(&v, idx), pos = 0;
	for (uint8_t k = 0; k < n; k++)
		if (idx[k] == sel)
			pos = k;
	if (rows < 1)
		rows = 1;
	uint8_t first = pos >= rows ? (uint8_t)(pos - rows + 1) : 0;
	for (uint8_t k = first; k < n && k < first + rows; k++) {
		row(y, rh, &v, idx[k], idx[k] == sel);
		y += rh;
	}
	gfx_hline(0, y + 2, gfx_width(), ui_col.line);
	bool can = can_track(&v, sel);
	ui_draw_bottom(&ui_st, v.track == sel && sat_table[sel].arm10 ?
		       TR("OK stop  # arm tone  2 sky")
		       : is_iss(sel) ? (v.track == sel ? TR("OK stop  # APRS  2 sky")
					: can ? TR("OK track  # APRS  2 sky")
					: TR("OK alarm  # APRS  2 sky"))
		       : v.track == sel ? TR("OK stop  2 sky  <> UTC")
		       : can ? TR("OK track  2 sky  <> UTC")
		       : TR("OK alarm  2 sky  <> UTC"));
	live(y + 5, &v);
	ui_draw_banner(&ui_st);
}

static bool sat_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	sat_view_t v;
	if (sky_page)
		return sky_key(ev);
	if (alarm_page) {
		sat_view(&v);
		return alarm_key(ev, &v);
	}
	if (ev->action == ACT_DIGIT_2 && ev->gesture == GESTURE_PRESS) {
		sky_open(sel);
		return true;
	}
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP)) {
		uint8_t idx[SAT_MAX], pos = 0;
		sat_view(&v);
		uint8_t n = order(&v, idx);
		for (uint8_t k = 0; k < n; k++)
			if (idx[k] == sel)
				pos = k;
		int dir = ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : ev->action == ACT_NAV_DOWN ? 1 : -1;
		if (n)
			sel = idx[(pos + n + dir) % n];
		sat_seen(sel);
		ui_dirty();
		return true;
	}
	if (press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) {
		tz_min = (int16_t)(tz_min + (ev->action == ACT_NAV_RIGHT ? 30 : -30));
		if (tz_min > 14 * 60)
			tz_min = -12 * 60;
		if (tz_min < -12 * 60)
			tz_min = 14 * 60;
		ui_dirty();
		return true;
	}
	if ((ev->action == ACT_SELECT || ev->action == ACT_DIGIT_0) && ev->gesture == GESTURE_PRESS) {
		sat_view(&v);
		if (ev->action == ACT_DIGIT_0 || (v.track != sel && !can_track(&v, sel))) {
			alarm_open(&v);		/* nothing to track: the alarm for its next pass */
		} else if (v.track == sel) {
			sat_track(SAT_NONE);
			ui_toast(TR("Tracking off: VFO stays"));
		} else {
			sat_track(sel);
			ui_toast(TR("VFO tracks the pass"));
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		sat_view(&v);
		if (v.track == sel && sat_table[sel].arm10) {
			sat_arm(!v.arm);
			ui_toast(v.arm ? TR("Uplink tone: the CTCSS") : TR("Uplink tone: arming"));
		} else if (is_iss(sel)) {
			iss_send(&v, sel);	/* only on this key press, never by itself */
		}
		ui_dirty();
		return true;
	}
	return false;
}

static void sat_enter(void)
{
	alarm_page = false;
	sky_page = false;
	sat_seen(sel);
	ui_dirty();
}

const ui_scr_def_t ui_sat_screen = {
	.name = "satellites", .flags = SF_FIELD, .key = sat_key, .draw = sat_draw, .enter = sat_enter,
};

/* --- Tools > ISS --- */

/* The row under Auto track: OK opens the sky plot (key 2 does too) */
static int iss_orbit_row(int y, const font_t *f)
{
	bool focus = a_row == ISS_ROWS - 1;
	int w = gfx_width(), rh = f->h + 3;
	if (focus)
		gfx_fill(0, y, w, rh, ui_col.focus);
	int ty = y + (rh - f->h) / 2;
	gfx_text(f, 2, ty, TR("Show orbit"), focus ? ui_col.focus_fg : ui_col.dim);
	gfx_text(f, w - gfx_text_width(f, ">") - 2, ty, ">", focus ? ui_col.focus_fg : ui_col.fg);
	return y + rh;
}

/* The pass and the live lines: "AOS in 1h05  47°" (or "LOS in 5:12  47°" under way), "AOS 14:27  LOS 14:38" (or
 * "Above now, max 47°"), azimuth and elevation, the two downlinks (the one in use marked) and its Doppler */
static int __attribute__((noinline)) iss_pass(int y, const sat_view_t *v, uint8_t i)
{
	char buf[40];
	sbuf_t b;
	const font_t *s = ui_layout.small, *m = ui_layout.medium;
	if (v->clock != SAT_CLOCK_OK || i >= sat_count) {
		gfx_text(s, 0, y, i >= sat_count ? TR("bad elements") : v->clock == SAT_CLOCK_NONE ?
			 TR("Waiting for the GPS time") : TR("Waiting for a GPS position"), ui_col.warn);
		return y + s->h + 2;
	}
	const sat_pass_t *p = &v->pass[i];
	bool up = p->state == SAT_PASS_OK && p->aos <= v->now;
	sb_init(&b, buf, sizeof buf);
	if (p->state != SAT_PASS_OK) {
		sb_str(&b, p->state == SAT_PASS_NONE ? TR("no pass soon") : p->state == SAT_PASS_BAD ? TR("bad elements") :
		       GLYPH_ELLIPSIS);
	} else {
		sb_str(&b, up ? TR("LOS in ") : TR("AOS in "));
		sb_until(&b, (up ? p->los : p->aos) - v->now);
		sb_str(&b, "  ");
		sb_deg(&b, p->max_el);
	}
	gfx_text(m, 0, y, buf, up ? ui_col.rx : ui_col.fg);
	y += m->h + 1;
	if (p->state == SAT_PASS_OK) {
		sb_init(&b, buf, sizeof buf);
		if (up) {
			sb_str(&b, TR("Above now, max "));
			sb_deg(&b, p->max_el);
		} else {
			sb_str(&b, "AOS ");
			sb_hhmm_at(&b, p->aos);
			sb_str(&b, "  LOS ");
			sb_hhmm_at(&b, p->los);
		}
		gfx_text(s, 0, y, buf, up ? ui_col.rx : ui_col.fg);
		y += s->h + 1;
	}
	bool live = v->live == i && v->live_ok;
	if (live) {
		sb_init(&b, buf, sizeof buf);
		sb_azel(&b, v);
		gfx_text(s, 0, y, buf, v->el10 >= 0 ? ui_col.fg : ui_col.dim);
		y += s->h + 1;
	}
	/* The downlinks, the one in use marked */
	gfx_text(s, 0, y, iss_aprs ? "  " : "> ", ui_col.rx);
	gfx_text(s, 2 * s->w, y, TR("145.800 voice / SSTV"), iss_aprs ? ui_col.dim : ui_col.fg);
	y += s->h + 1;
	gfx_text(s, 0, y, iss_aprs ? "> 145.825 APRS" : "  145.825 APRS", iss_aprs ? ui_col.fg : ui_col.dim);
	y += s->h + 1;
	if (live) {
		uint32_t hz = iss_aprs ? SAT_ISS_APRS_HZ : SAT_ISS_VOICE_HZ;
		int32_t dd = sat_doppler_rx(hz, (float)v->rate_ms / 1000.0f);
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, "RX ");
		sb_freq(&b, v->track == i && sat_auto_on() ? v->rx_hz : sat_tune_hz(hz, dd));
		sb_chr(&b, ' ');
		sb_khz(&b, dd);
		sb_str(&b, " kHz");
		gfx_text(s, 0, y, buf, ui_col.fg);
		y += s->h + 1;
	}
	return y;
}

static void iss_draw(void)
{
	char buf[40];
	sbuf_t b;
	sat_view_t v;
	gfx_clear(ui_col.bg);
	const font_t *s = ui_layout.small;
	uint8_t i = iss_index();
	if (i != SAT_NONE)
		sat_seen(i);
	sat_view(&v);
	int y = ui_draw_top(ui_view(), &ui_st, sky_page ? TR("Sky") : "ISS", sky_page ? "ISS" : pos_name(&v)) + 2;
	if (sky_page) {
		sky_draw(&v, i, y);
		return;
	}
	y = iss_pass(y, &v, i);
	gfx_hline(0, y + 1, gfx_width(), ui_col.line);
	y = iss_orbit_row(alarm_rows(y + 3, 1, s), s) + 2;
	/* What OK does now, or the alarm */
	sb_init(&b, buf, sizeof buf);
	color_t c = ui_col.dim;
	if (v.track != SAT_NONE) {
		sb_str(&b, v.track == i && sat_auto_on() ? TR("Tracking, back at LOS") : TR("Tracking Doppler"));
		c = ui_col.rx;
	} else if (armed(&v) && v.alarm.target != i) {
		/* another satellite's alarm: shown, left alone (Every pass On here replaces it) */
		sb_str(&b, TR("Alarm "));
		sb_str(&b, v.alarm.target < sat_count ? sat_table[v.alarm.target].name : TR("Any satellite"));
		sb_str(&b, TR(" armed"));
	} else if (i != SAT_NONE && can_track(&v, i)) {
		if (a_row != ISS_ROWS - 1)	/* on Show orbit OK opens the plot: the hint says so */
			sb_str(&b, TR("OK: track in the VFO"));
		c = ui_col.fg;
	} else if (!sb_alarm(&b, &v)) {
		sb_str(&b, TR("Track from AOS-10 min"));
	}
	gfx_text(s, 0, y, buf, c);
	ui_draw_bottom(&ui_st, a_row == ISS_ROWS - 1 ? TR("OK orbit  # APRS  1 freq") :
		       v.track != SAT_NONE ? TR("OK stop  # APRS  1 freq") : TR("OK track  # APRS  1 freq"));
	ui_draw_banner(&ui_st);
}

/* OK: stop a track (any), else the held track now, else when it can */
static void __attribute__((noinline)) iss_ok(const sat_view_t *v, uint8_t i)
{
	if (v->track != SAT_NONE) {
		bool held = sat_auto_on();
		sat_track(SAT_NONE);
		ui_toast(held ? TR("Auto track off: radio back") : TR("Tracking off: VFO stays"));
	} else if (i != SAT_NONE && can_track(v, i)) {
		sat_auto_start(i, iss_mode());	/* "ISS tracking" once the radio holds, else why not (ui_sat_alarm) */
	} else {
		ui_toast(TR("Track from AOS-10 min"));
	}
}

static bool iss_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT;
	sat_view_t v;
	uint8_t i = iss_index();
	if (sky_page)
		return sky_key(ev);
	if (ev->action == ACT_DIGIT_2 && ev->gesture == GESTURE_PRESS) {
		sky_open(i);
		return true;
	}
	if (press && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN)) {
		a_row = (uint8_t)(1u + (a_row - 1u + (ev->action == ACT_NAV_DOWN ? 1u : ISS_ROWS - 2u)) % (ISS_ROWS - 1u));
		ui_dirty();
		return true;
	}
	if ((press && (ev->action == ACT_NAV_LEFT || ev->action == ACT_NAV_RIGHT)) || ev->action == ACT_ENC_STEP) {
		if (a_row == ISS_ROWS - 1)
			return true;	/* Show orbit has no value to change */
		sat_view(&v);
		alarm_change(ev->action == ACT_ENC_STEP ? ev->value > 0 : ev->action == ACT_NAV_RIGHT);
		bool mine = v.alarm.target == i;	/* this page edits the ISS's alarm alone */
		if (a_row == 3 && !a_every) {
			if (mine) {
				sat_alarm_set(SAT_NONE, 0, 0, 0);	/* Every pass off: the notifications off */
				note_store(false);
			}
		} else if (a_every) {
			if (armed(&v) && !mine)
				ui_toast(TR("Every ISS pass: other alarm off"));
			alarm_arm();	/* Every pass on (the ISS), or a change while it is: armed with it */
		}
		ui_dirty();
		return true;
	}
	if (ev->gesture != GESTURE_PRESS)
		return false;
	if (ev->action == ACT_SELECT && a_row == ISS_ROWS - 1) {
		sky_open(i);	/* like key 2: a held track goes on (the keys here are the page's) */
		return true;
	}
	if (ev->action == ACT_SELECT || ev->action == ACT_CONTEXT) {
		sat_view(&v);
		if (ev->action == ACT_SELECT)
			iss_ok(&v, i);
		else if (i != SAT_NONE)
			iss_send(&v, i);	/* only on this key press, never by itself */
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_DIGIT_1) {
		iss_aprs = !iss_aprs;
		sat_auto_mode(iss_mode());	/* a held track of the ISS follows it */
		ui_dirty();
		return true;
	}
	return false;
}

static void iss_enter(void)
{
	sat_view_t v;
	uint8_t i = iss_index();
	sat_view(&v);
	if (v.alarm.target == i)
		alarm_open(&v);	/* the ISS's alarm: its values */
	else
		a_every = false;	/* another's (or none): shown, not edited (iss_draw) */
	alarm_page = false;
	sky_page = false;
	a_row = 1;
	a_for = i;
	ui_dirty();
}

const ui_scr_def_t ui_iss_screen = {
	.name = "iss", .flags = SF_FIELD, .key = iss_key, .draw = iss_draw, .enter = iss_enter,
};
#endif
