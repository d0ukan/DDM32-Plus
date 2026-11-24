/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 13 ideas 2 through the real UI in upstream's simulator (tests/plus/ui_sim_toolbox.c wraps it), on
 * the DM-32UV's display class (240 x 320, insets 10 x 5). Each case runs in a child process (the screens' static state
 * starts over). Covers:
 *   - the RAM reclaim: the six tool screens (spectrum, P25 / DMR search, P25 / DMR trunk, the pager's message page)
 *     share one buffer; going from one to the next and back, each draws from its own fresh copy (its own text there,
 *     none of the others'), spectrum's OK tunes to its own hit after the others drew; a find save stops cleanly when
 *     another screen takes the buffer (before it began: nothing written; under way: the channels written are whole);
 *   - item 24: Tools > Heard list fed by DMR calls (merged, counted after a pause, newest first) and P25 / YSF entries;
 *     a row opened; OK tunes the VFO (DMR: colour code, talk target; P25: NAC, talkgroup); YSF has no tune;
 *   - item 26: the waterfall's heap buffer taken on entry, rows from the sweeps, * off (buffer back) and on, given back
 *     on Back, on another screen on top and on OK; no heap: the bars alone, "No room", nothing leaks;
 *   - item 27: the map track with the signal beside each point; * on the map: the legend, the toast; quick settings
 *     elsewhere still open with *;
 *   - item 29: Tools > GPS: speed, heading, altitude, the fix; the trip from moving fixes (standing still adds none);
 *     OK held resets;
 *   - item 34: Tools > Clock: stopwatch with laps, the countdown's banner and beep on Home, the alarm's on another screen
 *     at its minute from the GPS time, no clock without GPS time. Layout checked on every new screen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "hal_time.h"
#include "ui/gfx.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/rpt.h"
#include "ui/findsave.h"
#include "ui/heard.h"
#include "ui/ui_heard.h"
#include "ui/wfall.h"
#include "ui/mapnav.h"
#include "ui/ui_gpsdash.h"
#include "ui/ui_clock.h"
#include "ui/i18n.h"
#include "ui/i18n.h"
#include "radio/beep.h"
#include "codeplug/codeplug.h"
#include "ui_sim_toolbox.h"

/* Tools > Repeaters' table (the map draws it) */
#define P(la, lo) (int32_t)((la) * 1e5 + 0.5), (int32_t)((lo) * 1e5 + 0.5)
const rpt_entry_t rpt_table[] = {
	{ P(39.93, 32.86), 145600000, -480, 885, RPT_FM, 0, "TA2RAK", "Ankara", 6 },
};
const rpt_info_t rpt_info = { sizeof rpt_table / sizeof rpt_table[0], 0, "2026-11-20", "test" };

static int fails;
static const char *name;

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s: %s: %s\n", name, script, err);
		fails++;
		if (getenv("TB_DUMP"))
			tb_sim_dump();
	}
}

static void expect(const char *e)
{
	char buf[500], *o = buf;
	while (*e && o < buf + sizeof buf - 16) {
		while (*e == ' ')
			e++;
		if (!*e)
			break;
		memcpy(o, "expect:", 7);
		o += 7;
		while (*e && *e != ' ' && o < buf + sizeof buf - 2)
			*o++ = *e++;
		*o++ = ' ';
	}
	*o = 0;
	ok_run(buf);
}

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s: %s\n", name, what);
		fails++;
	}
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int k = 0; k < n; k++)
		printf("FAIL %s: layout: %s\n", name, issues[k]);
	fails += n != 0;
}

static void key(uint8_t action, uint8_t gesture)
{
	input_event_t ev = { action, gesture, 0 };
	ui_app_key(&ev);
}

static void gps(bool fix, double la, double lo)
{
	tb_sim.gps = true;
	tb_sim.fix = fix;
	tb_sim.lat_e7 = (int32_t)(la * 1e7 + (la < 0 ? -0.5 : 0.5));
	tb_sim.lon_e7 = (int32_t)(lo * 1e7 + (lo < 0 ? -0.5 : 0.5));
}

static void start(const char *n)
{
	name = n;
	tb_sim_clear();
	tb_sim.gps = true;	/* a GPS without a fix (the simulator's own has a fix and a time) */
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);	/* the DM-32 has Left / Right, * and # (map M) */
}

/* --- the screens' content --- */

static void p25_find(uint32_t hz, uint16_t nac, uint16_t tg, uint32_t src)
{
	p25s_view_t *v = &tb_sim.p25;
	memmove(&v->found[1], &v->found[0], sizeof v->found[0] * (P25S_FOUND_MAX - 1u));
	memset(&v->found[0], 0, sizeof v->found[0]);
	v->found[0].hz = hz;
	v->found[0].nac = nac;
	v->found[0].tg = tg;
	v->found[0].src = src;
	v->found[0].heard_ms = 100;
	if (v->found_n < P25S_FOUND_MAX)
		v->found_n++;
}

static void dmr_find(uint32_t hz, uint8_t cc, uint8_t ts)
{
	dmrs_view_t *v = &tb_sim.dmr;
	memset(&v->found[0], 0, sizeof v->found[0]);
	v->found[0].hz = hz;
	v->found[0].cc = cc;
	v->found[0].ts_mask = ts;
	v->found[0].sync_mask = DMRS_SYNC_BS;
	v->found[0].heard_ms = 100;
	v->found_n = 1;
}

static void all_content(void)
{
	p25_find(433525000, 0x659, 101, 77285);
	dmr_find(438712500, 1, 3);
	p25tk_view_t *p = &tb_sim.p25tk;
	p->state = P25TK_CONTROL;
	p->have_net = true;
	p->wacn = 0xBEE00;
	p->sysid = 0x1C8;
	p->heard_nac = 0x1C8;
	p->cc_hz = 851012500;
	dmrtk_view_t *d = &tb_sim.dmrtk;
	d->state = DMRTK_CONTROL;
	d->have_sys = true;
	d->model = 1;
	d->net = 12;
	d->site = 2;
	d->cc_hz = 439000000;
	radio_pager_msg_t *m = &tb_sim.pager[0];
	memset(m, 0, sizeof *m);
	m->capcode = 1234567;
	m->id = 5;
	m->kind = RADIO_MSG_ALPHA;
	strcpy(m->text, "HELLO PAGER");
	m->ms = 100;
	tb_sim.pager_n = 1;
	tb_sim.msg_seq = 1;
}

/* The spectrum engine: n sweeps with a strong signal at point k (the floor at -120 dBm); each sweep after a frame
 * drawn (the screen's reads keep the sweep alive) */
static uint32_t fclk;
static void sweeps(int n, uint8_t k)
{
	for (int s = 0; s < n; s++) {
		tb_sim_changed();
		ok_run("wait:100");
		if (fclk < hal_millis())
			fclk = hal_millis();
		spectrum_view_t v;
		spectrum_snapshot(&v);
		for (uint8_t i = 0; i < v.points && spectrum_on(); i++) {	/* one sweep: the engine reads point i */
			fclk += 40;
			spectrum_service(i == k ? -60 : -120, fclk);
		}
	}
	tb_sim_changed();
	ok_run("wait:300");
}

/* --- RAM reclaim: one buffer for the six screens --- */

static void rr_round_trip(void)
{
	all_content();
	uint32_t c = tb_sim_freq();
	ui_push(SCR_SPECTRUM, 0);
	sweeps(2, 20);	/* 32 points of 15.625 kHz: point 20 is 4 above the center */
	char hit[40];
	snprintf(hit, sizeof hit, "shows=Hit_%u.%03u", (unsigned)((c + 62500u) / 1000000u),
		 (unsigned)((c + 62500u) / 1000u % 1000u));
	expect("screen=spectrum shows=Floor_-120dBm");
	expect(hit);
	layout();
	/* each screen on top of the spectrum and back: its own content, none of another's */
	static const struct { uint8_t scr; const char *shows, *hides; } hop[] = {
		{ SCR_P25SEARCH, "shows=433.5250_$659", "hides=Floor" },
		{ SCR_DMRSEARCH, "shows=438.7125_CC1", "hides=$659" },
		{ SCR_P25TRUNK, "shows=SYS_1C8_NAC$1C8/456", "hides=CC1" },	/* the WACN gives way on the DM-32 */
		{ SCR_DMRTRUNK, "shows=Small_Net_13", "hides=SYS_1C8" },
		{ SCR_PAGER, "shows=1234567:_HELLO_PAGER", "hides=Net_13" },
	};
	for (unsigned i = 0; i < sizeof hop / sizeof hop[0]; i++) {
		ui_push(hop[i].scr, 0);
		ok_run("wait:200");
		expect(hop[i].shows);
		expect(hop[i].hides);
		if (hop[i].scr == SCR_PAGER) {
			ok_run("ok wait:200");
			expect("screen=msgview shows=HELLO_PAGER shows=1234567");
			ok_run("back wait:100");
		}
		ok_run("back wait:200");
		sweeps(1, 20);
		expect("screen=spectrum shows=Floor_-120dBm hides=SYS_1C8 hides=$659 hides=HELLO");
		expect(hit);
	}
	/* one after another without the spectrum between */
	ui_push(SCR_P25TRUNK, 0);
	ok_run("wait:200");
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200");
	expect("shows=433.5250 hides=SYS_1C8");
	ok_run("back wait:200");
	expect("screen=p25trunk shows=SYS_1C8 hides=433.5250");
	ok_run("back wait:200");
	/* OK on the spectrum tunes to its own hit, whatever the others left in the buffer */
	ui_push(SCR_DMRTRUNK, 0);
	ok_run("wait:200 back wait:100");
	sweeps(1, 20);
	ok_run("ok wait:200");
	char f[40];
	snprintf(f, sizeof f, "screen=home vfo=1 freq=%u", (unsigned)(c + 62500u));
	expect(f);
}

static int zone_named(const char *n, zone_t *z)
{
	for (uint16_t i = 0; i < codeplug_zone_count(); i++)
		if (codeplug_zone(i, z) && strcmp(z->name, n) == 0)
			return i;
	return -1;
}

static void rr_save_before(void)
{
	p25_find(145600000, 0x293, 101, 3101234);
	p25_find(433525000, 0x659, 0, 0);
	p25_find(433500000, 0x1C8, 655, 77285);
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200");
	uint16_t zones = codeplug_zone_count();
	/* # saves all; the spectrum opens before the first step: the list is gone, nothing is written */
	key(ACT_CONTEXT, GESTURE_PRESS);
	ui_push(SCR_SPECTRUM, 0);
	ok_run("wait:1000");
	expect("screen=spectrum toast=Stopped");
	check(!ui_findsave_busy() && codeplug_zone_count() == zones, "nothing written, no zone");
	ok_run("back wait:200");
	expect("screen=p25search shows=433.5000");
}

static void rr_save_during(void)
{
	p25_find(145600000, 0x293, 101, 3101234);
	p25_find(433525000, 0x659, 0, 0);
	p25_find(433500000, 0x1C8, 655, 77285);
	ui_push(SCR_P25SEARCH, 0);
	ok_run("wait:200");
	key(ACT_CONTEXT, GESTURE_PRESS);
	ok_run("wait:100");
	ui_push(SCR_DMRTRUNK, 0);	/* the trunk screen takes the buffer while the save runs */
	ok_run("wait:1500");
	expect("toast=Stopped:");
	check(!ui_findsave_busy(), "not stuck");
	zone_t z;
	channel_t ch;
	static const char *const names[3] = { "P25 433.500", "P25 433.525", "P25 145.600" };
	check(zone_named("Bulunanlar 1", &z) >= 0 && z.count < 3, "stopped part way");
	for (uint16_t k = 0; k < z.count; k++)
		check(codeplug_channel(z.channels[k], &ch) && !strcmp(ch.name, names[k]) && ch.mode == RADIO_MODE_P25,
		      "each channel written is the find it should be");
}

/* --- item 24: heard list --- */

static void heard_dmr(void)
{
	ok_run("call:3101234,91,g wait:6000 call:3101234,91,g call:3102222,91,g wait:1000");
	const heard_list_t *l = ui_heard_list();
	check(l->n == 2 && l->e[0].id == 3102222 && l->e[1].id == 3101234 && l->e[1].count == 2 && l->e[0].count == 1 &&
	      l->e[0].tg == 91 && l->e[0].hz == tb_sim_freq(), "two stations, newest first, counted");
	/* Tools > Heard list */
	ui_push(SCR_TOOLS, 0);
	ok_run("wait:100");
	for (int i = 0; i < 12 && strncmp(ui_app_focus_label(), "Heard list", 10); i++)
		ok_run("up");
	ok_run("ok wait:200");
	expect("screen=heard shows=DMR_3102222 shows=DMR_3101234 shows=TG91 shows=x2");
	layout();
	ok_run("down ok wait:200");
	expect("screen=heard-one shows=TG_91_ID_3101234 shows=Calls_2 shows=OK_tune_VFO");
	layout();
	ok_run("ok wait:300");
	char f[60];
	snprintf(f, sizeof f, "screen=home vfo=1 freq=%u vmode=DMR talk=91", (unsigned)tb_sim_freq());
	expect(f);
	expect("toast=Talking_to");
}

static void heard_p25_ysf(void)
{
	ui_recent_t r;
	memset(&r, 0, sizeof r);
	r.mode = RADIO_MODE_P25;
	r.src = 77285;
	r.dst = 101;
	r.group = true;
	r.nac = 0x659;
	ui_heard_recent(&r, true);
	memset(&r, 0, sizeof r);
	r.mode = RADIO_MODE_YSF;
	strcpy(r.name, "TA2DOU");
	ui_heard_recent(&r, true);
	memset(&r, 0, sizeof r);
	r.mode = 0x80;		/* an FM caller ID kind: not kept */
	r.src = 5;
	ui_heard_recent(&r, true);
	memset(&r, 0, sizeof r);
	r.mode = RADIO_MODE_P25;
	r.src = 1;
	r.dst = 2;
	r.out = true;		/* a call we made: not kept */
	ui_heard_recent(&r, true);
	check(ui_heard_list()->n == 2, "P25 and YSF kept, the others not");
	ui_push(SCR_HEARD, 0);
	ok_run("wait:200");
	expect("shows=YSF_TA2DOU shows=P25_77285 shows=TG101");
	expect("shows=TG101_x1");	/* the DM-32's row: "TG101 $659/1625 x1 0s" does not fit, the NAC gives way */
	layout();
	/* YSF: no tune */
	ok_run("ok wait:200");
	expect("screen=heard-one shows=TA2DOU hides=OK_tune");
	ok_run("ok wait:100");
	expect("screen=heard-one");
	ok_run("back down ok wait:200");
	expect("screen=heard-one shows=$659/1625 shows=TG_101_ID_77285");
	ok_run("ok wait:300");
	char f[80];
	snprintf(f, sizeof f, "screen=home vfo=1 freq=%u vmode=P25 nac=659 tg=101", (unsigned)tb_sim_freq());
	expect(f);
	expect("toast=VFO");
	/* empty list after a power cycle */
	ok_run("reboot wait:200");
	ui_push(SCR_HEARD, 0);
	ok_run("wait:100");
	expect("shows=Nothing_heard_yet");
}

/* --- item 26: waterfall --- */

/* Pixels in the TX colour: on the spectrum screen only the waterfall's strongest level is drawn in it */
static int tx_pixels(void)
{
	int n = 0;
	for (int y = 0; y < gfx_height(); y++)
		for (int x = 0; x < gfx_width(); x++)
			n += gfx_get_pixel(x, y) == ui_col.tx;
	return n;
}

static void wf_flow(void)
{
	ui_push(SCR_SPECTRUM, 0);
	check(ui_spectrum_wf_held() && tb_sim.heap_allocs == 1 && tb_sim.heap_held == WF_BYTES, "the buffer taken");
	sweeps(3, 10);
	expect("screen=spectrum shows=<>_span shows=Floor_-120dBm hides=No_room");
	layout();
	check(tx_pixels() > 0, "the strong point's rows drawn in the waterfall");
	/* * off: the buffer back; on again: taken again */
	ok_run("quick wait:100");
	expect("screen=spectrum toast=Waterfall_off");
	ok_run("wait:2000");
	tb_sim_changed();
	ok_run("wait:100");
	check(tx_pixels() == 0, "no waterfall drawn when off");
	check(!ui_spectrum_wf_held() && tb_sim.heap_frees == 1 && tb_sim.heap_held == 0, "given back");
	ok_run("quick wait:100");
	expect("toast=Waterfall_on");
	check(ui_spectrum_wf_held() && tb_sim.heap_allocs == 2, "taken again");
	sweeps(1, 12);
	/* another screen on top: given back at the next tick */
	ui_push(SCR_HEARD, 0);
	ok_run("wait:200");
	check(!ui_spectrum_wf_held() && tb_sim.heap_frees == 2, "another screen on top: given back");
	ok_run("back wait:200");
	expect("screen=spectrum");
	check(ui_spectrum_wf_held() && tb_sim.heap_allocs == 3, "back on the spectrum: taken again");
	ok_run("back wait:200");
	/* Back gives it back at once */
	ui_push(SCR_SPECTRUM, 0);
	check(ui_spectrum_wf_held(), "taken");
	ok_run("back wait:100");
	check(!ui_spectrum_wf_held() && tb_sim.heap_allocs == tb_sim.heap_frees, "Back: given back");
	/* OK tunes and gives it back */
	ui_push(SCR_SPECTRUM, 0);
	sweeps(1, 5);
	ok_run("ok wait:200");
	expect("screen=home vfo=1");
	check(!ui_spectrum_wf_held() && tb_sim.heap_allocs == tb_sim.heap_frees && tb_sim.heap_held == 0, "OK: nothing held");
	/* Back from Home: quick settings still open with * elsewhere */
	ui_push(SCR_HEARD, 0);
	ok_run("wait:100 quick wait:100");
	expect("screen=quick");
}

static void wf_no_heap(void)
{
	tb_sim.heap_fail = true;
	ui_push(SCR_SPECTRUM, 0);
	sweeps(2, 10);
	expect("screen=spectrum shows=No_room shows=Floor_-120dBm");
	check(!ui_spectrum_wf_held() && tb_sim.heap_refused == 1, "refused, drawn without it");
	layout();
	ok_run("quick wait:100");
	expect("toast=Waterfall_off hides=No_room");
	ok_run("quick wait:100");
	expect("toast=Waterfall:_no_room");
	/* room again: the next try takes it */
	tb_sim.heap_fail = false;
	ok_run("back wait:100");
	ui_push(SCR_SPECTRUM, 0);
	check(ui_spectrum_wf_held(), "taken when there is room");
	ok_run("back wait:100");
	check(tb_sim.heap_allocs == tb_sim.heap_frees, "nothing leaks");
}

/* --- item 27: coverage --- */

static void cov_flow(void)
{
	/* three track points 30 s and about 1 km apart, the signal different at each */
	static const int16_t rssi[3] = { -90, -110, -125 };
	for (int i = 0; i < 3; i++) {
		gps(true, 39.93 + 0.01 * i, 32.86);
		tb_sim_rssi(rssi[i]);
		ok_run("wait:31000");
	}
	const track_t *t = &ui_mapnav_track;
	check(t->n == 3, "three points");
	check(cov_class(ui_mapnav_cov[track_slot(t, 0)]) == COV_GOOD &&
	      cov_class(ui_mapnav_cov[track_slot(t, 1)]) == COV_FAIR &&
	      cov_class(ui_mapnav_cov[track_slot(t, 2)]) == COV_POOR, "the signal beside each point");
	ui_push(SCR_MAP, 0);
	ok_run("wait:200");
	expect("hides=S7+");
	ok_run("quick wait:200");
	expect("screen=map toast=Coverage:_track_by_signal");
	ok_run("wait:4000");
	tb_sim_changed();
	ok_run("wait:200");
	expect("shows=S7+ shows=S5 shows=<S5");
	layout();
	ok_run("wait:4000 quick wait:200");
	expect("screen=map toast=Coverage_off hides=S7+");
}

/* --- item 29: GPS dashboard --- */

static void gps_flow(void)
{
	gps(false, 39.93, 32.86);
	ui_push(SCR_GPSDASH, 0);
	ok_run("wait:1200");
	expect("screen=gpsdash shows=No_fix");
	layout();
	gps(true, 39.93, 32.86);
	tb_sim.speed_kmh = 48;
	tb_sim.course = 45;
	tb_sim.alt_m = 850;
	ok_run("wait:1200");
	expect("shows=48 shows=NE_45 shows=850_m shows=Fix_3D,_8 shows=48_km/h");
	layout();
	/* standing still with the fix wandering: no trip */
	for (int i = 0; i < 20; i++) {
		gps(true, 39.93 + (i % 3) * 0.00005, 32.86);
		ok_run("wait:1000");
	}
	check(ui_gpsdash_trip()->meters == 0, "standing still adds nothing");
	/* 1.2 km north in two minutes */
	for (int i = 1; i <= 120; i++) {
		gps(true, 39.93 + i * 0.0000899, 32.86);
		ok_run("wait:1000");
	}
	uint32_t m = ui_gpsdash_trip()->meters;
	check(m > 1150 && m < 1250, "about 1.2 km");
	expect("shows=1.1 shows=km");
	/* slow: the course means nothing */
	tb_sim.speed_kmh = 1;
	ok_run("wait:1200");
	expect("hides=NE_45");
	/* OK alone says how; held resets */
	ok_run("ok wait:100");
	expect("toast=Hold_OK");
	ok_run("long:ok wait:100");
	expect("toast=Trip_reset shows=0_m shows=0_km/h");
	check(ui_gpsdash_trip()->meters == 0 && ui_gpsdash_trip()->max_kmh == 0, "reset");
}

/* --- item 34: clock tools --- */

static void clock_flow(void)
{
	tb_sim.gps = true;		/* a GPS without a fix: no time yet */
	ui_push(SCR_CLOCK, 0);
	ok_run("wait:1200");
	expect("screen=clock shows=No_clock:_needs_GPS shows=Stopwatch shows=0:00.0 shows=5:00 shows=07:00_Off shows=+03:00");
	layout();
	/* stopwatch: start, two laps, stop, reset */
	ok_run("ok wait:2500 ctx wait:1500 ctx wait:500 ok wait:300");
	const clockx_t *c = ui_clock_state();
	uint32_t lap1, lap2;
	uint8_t n1, n2;
	check(!c->sw_run && c->laps == 2 && clk_sw_lap(c, 0, &lap2, &n2) && clk_sw_lap(c, 1, &lap1, &n1) && n2 == 2 &&
	      n1 == 1 && lap1 >= 2500 && lap1 <= 2800 && lap2 >= 1500 && lap2 <= 1800 &&
	      clk_sw_ms(c, hal_millis()) >= 4500 && clk_sw_ms(c, hal_millis()) <= 5200, "two laps, stopped");
	expect("shows=Lap_2__0:01. shows=Lap_1__0:02. shows=#_reset");
	layout();
	ok_run("ctx wait:300");
	expect("shows=0:00.0 hides=Lap_1");
	/* countdown: down to 0:10, start, leave to Home: the banner and a tone there */
	ok_run("down left left left left left left left left left wait:200");
	expect("shows=0:10");
	ok_run("ok wait:200 back wait:200");
	expect("screen=home");
	int beeps = tb_sim.beeps;
	ok_run("wait:9000");
	check(tb_sim.beeps == beeps, "not yet");
	ok_run("wait:1500");
	expect("screen=home banner=Countdown_done");
	check(tb_sim.beeps == beeps + 1 && tb_sim.last_beep == BEEP_TOT_WARN, "the countdown's tone");
	/* the alarm at 07:00 local: the GPS says 03:59:40 UTC (+03:00) */
	tb_sim.gps = tb_sim.fix = tb_sim.time = true;
	tb_sim.utc_hms = 35936;	/* 06:59:36 local */
	tb_sim.utc_at = hal_millis();
	ui_push(SCR_CLOCK, 0);
	ok_run("wait:1500");
	expect("shows=06:59:3");
	ok_run("up up up down down ok wait:200");
	expect("shows=07:00_On shows=Rings_on_any_screen");
	layout();
	ui_push(SCR_HEARD, 0);		/* another screen */
	ok_run("wait:14000");
	check(tb_sim.last_beep != BEEP_SCAN_HOLD && ui_clock_state()->al_on, "not yet (06:59:5x)");
	ok_run("wait:10000");
	expect("screen=heard banner=Alarm_07:00");
	check(tb_sim.last_beep == BEEP_SCAN_HOLD, "the alarm's tone");
	check(!ui_clock_state()->al_on, "rings once, then off");
	/* the GPS lost: the clock goes on */
	tb_sim.fix = false;
	ok_run("back wait:2000");
	expect("screen=clock shows=07:00:");
}

/* --- Tools > Sun & Moon: the row labels must not run into the time columns (owner's photo: "Sun up/down" over
   07:44, "Dawn/dusk" over 07:14). Run in English and in Turkish, at every UTC offset the Left / Right keys reach. --- */

static void fit(const font_t *f, int x, const char *text, const char *what)
{
	int tw = gfx_text_width(f, text);
	if (x + tw > gfx_width()) {
		printf("FAIL %s: %s \"%s\" ends at %d px, the area is %d px\n", name, what, text, x + tw, gfx_width());
		fails++;
	}
}

static void almanac_rows(bool turkish)
{
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
		return;
	}
	const font_t *m = ui_layout.medium, *sm = ui_layout.small;
	/* the row labels left of the widest time columns ("00:00  00:00", 12 characters at the right edge) */
	int cols = gfx_text_width(m, "00:00  00:00"), w = gfx_width();
	check(gfx_text_width(m, tr("Sun")) + 6 <= w - cols && gfx_text_width(m, tr("Dawn")) + 6 <= w - cols,
	      "a row label runs into the time columns");
	/* the Day line: its longest form (the polar night / day) */
	char day[64];
	snprintf(day, sizeof day, "%s%s", tr("Day "), tr("no rise/set"));
	fit(m, 0, day, "Day line");
	/* the moon block (disk of radius 28 at most): every phase name, and the worst "100% lit, 29 days" line */
	static const char *const phases[8] = { "New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
		"Full moon", "Waning gibbous", "Last quarter", "Waning crescent" };
	int mx = 2 * 28 + 10;
	for (int i = 0; i < 8; i++)
		fit(sm, mx, tr(phases[i]), "moon phase");
	char lit[64];
	snprintf(lit, sizeof lit, "100%s29%s", tr("% lit, "), tr(" days"));
	fit(sm, mx, lit, "moon line");
	gps(true, 39.93, 32.86);
	tb_sim.time = true;
	tb_sim.utc_hms = 120000;
	tb_sim.utc_at = hal_millis();
	ui_push(SCR_ALMANAC, 0);
	ok_run("wait:300");
	expect("screen=almanac shows=UTC+03:00");
	layout();
	for (int k = 0; k < 8; k++) {		/* other UTC offsets */
		ok_run("right right right");
		layout();
	}
	gps(true, 69.65, 18.96);	/* Tromso: "--:--" in the rows, no sunrise */
	ok_run("wait:1200");
	layout();
}

static void almanac_en(void) { almanac_rows(false); }
static void almanac_tr(void) { almanac_rows(true); }

typedef void (*case_fn)(void);

static int run_case(const char *n, case_fn fn)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		start(n);
		fn();
		fflush(stdout);
		_exit(fails != 0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		if (!WIFEXITED(st))
			printf("FAIL %s: crashed\n", n);
		return 1;
	}
	return 0;
}

int main(void)
{
	static const struct { const char *name; case_fn fn; } cases[] = {
		{ "RR shared buffer round trip", rr_round_trip },
		{ "RR find save, buffer taken first", rr_save_before },
		{ "RR find save, buffer taken during", rr_save_during },
		{ "I24 heard DMR", heard_dmr },
		{ "I24 heard P25 and YSF", heard_p25_ysf },
		{ "I26 waterfall", wf_flow },
		{ "I26 waterfall without heap", wf_no_heap },
		{ "I27 coverage", cov_flow },
		{ "I29 GPS dashboard", gps_flow },
		{ "I34 clock", clock_flow },
		{ "Sun & Moon rows, English", almanac_en },
		{ "Sun & Moon rows, Turkish", almanac_tr },
	};
	int failed = 0;
	unsigned n = sizeof cases / sizeof cases[0];
	for (unsigned i = 0; i < n; i++)
		failed += run_case(cases[i].name, cases[i].fn);
	printf("Plus round 13 ideas 2 UI: %u flows, %d failures\n", n, failed);
	return failed != 0;
}
