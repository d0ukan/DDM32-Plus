/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 15 scan job (docs/plus.md "Search ranges and speed") through the real UI in upstream's simulator
 * (tests/plus/ui_sim_scan.c wraps it), on the DM-32UV's display class (240 x 320, insets 10 x 5), in English and in
 * Turkish. Each case runs in a child process (the screens' static state starts over). Covers item 1, CONFIG_SEARCH_RANGE,
 * on Tools > P25 search and DMR search:
 *   - the range line with the step; Left / Right through the presets; * opens the editor, a digit opens it typing Lower;
 *   - typing Lower and Upper (OK goes on, OK applies), Back deleting a digit, Back leaving without a change;
 *   - the step row (every step, both ways), Left / Right on Lower / Upper / Move by one step, held by 1 MHz;
 *   - refusals: Upper not above Lower, a range across two bands, an end moved out of the band;
 *   - the request: the range and its step as the radio core decodes them; a running search starts again on an
 *     applied range, a stopped one waits for OK;
 *   - the range and step kept when the screen is left and opened again, the manual range then among the presets;
 *   - the layout of the screen and the editor.
 * Also srchfix (the lines fit the safe area) and searchlist (16 finds, P25 / DMR / all digital): Down through all 16
 * keeps the picked find whole on screen and the only one highlighted, Up back to none shows the newest again, the "^" /
 * "v" marker, new finds arriving on top while one far down is picked, a short list unchanged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "ui_sim_scan.h"

static int fails;
static const char *name;
static bool turkish;

/* English or Turkish (Turkish: an ASCII part of the translation, as the screen draws it) */
#define T(en, tr) (turkish ? (tr) : (en))

static bool run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, turkish ? "tr" : "en", script, err);
		fails++;
		return false;
	}
	return true;
}

static void expect(const char *e)
{
	char buf[600], *o = buf;
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
	run(buf);
}

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s (%s): %s\n", name, turkish ? "tr" : "en", what);
		fails++;
	}
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int i = 0; i < n; i++)
		printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[i]);
	fails += n;
}

/* The requests recorded since the last call, compared */
static void requests(const char *want)
{
	if (strcmp(sc_sim.req, want) != 0) {
		printf("FAIL %s (%s): requests \"%s\", expected \"%s\"\n", name, turkish ? "tr" : "en", sc_sim.req, want);
		fails++;
	}
	sc_sim.req[0] = 0;
}

static void start(const char *n, ui_scr_t scr)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);	/* Left / Right, * and #; held Left / Right repeat (the DM-32 sends USER_1 / USER_4) */
	sc_sim_clear();
	const char *shots = getenv("SCAN_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "scan-tr-" : "scan-en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	run("wait:200");
	ui_push(scr, 0);
	run("wait:200");
}

/* The DM-32's held Left / Right (its long actions; the simulator's maps have none) */
static void user_long(uint8_t action)
{
	input_event_t ev = { action, GESTURE_LONG, 0 };
	ui_app_key(&ev);
	run("wait:100");
}

/* The editor's rows: Lower, Upper, Step, Move */
static const char *ed_title(void) { return T("shows=Search_range,_MHz", "shows=Arama_aral"); }

/* --- P25 search --- */

/* The line names the preset and the step; Left / Right go through the presets (no manual one yet) */
static void p25_presets(void)
{
	start("P25 presets and the range line", SCR_P25SEARCH);
	expect(T("screen=p25search shows=Range_+-1_MHz__12.5k shows=*_", "screen=p25search shows=+-1_MHz__12.5k"));
	layout();
	run("right");
	expect("shows=+-2.5_MHz__12.5k");
	run("right right");
	expect("shows=144-146_MHz__12.5k");
	run("right");	/* round to the first: no manual range yet */
	expect("shows=+-500_kHz__12.5k");
	run("left");
	expect("shows=144-146_MHz");
	run("ok");
	requests("p25s:144000-146000/12500");
}

/* Typing: a digit opens the editor typing Lower; OK goes on to Upper; OK applies; the search waits for OK */
static void p25_type(void)
{
	start("P25 typed range", SCR_P25SEARCH);
	run("4 3 3");
	expect(T("shows=Search_range shows=433. shows=Lower", "shows=Arama_aral shows=433. shows=Alt"));
	layout();
	run("5 ok");
	expect("shows=433.5 hides=433.5-");
	run("4 3 4 ok");
	expect(T("screen=p25search shows=Range_433.5-434__12.5k hides=Search_range",
		 "screen=p25search shows=433.5-434__12.5k hides=Arama_aral"));
	layout();
	check(sc_sim.requests == 0, "nothing asked of the radio before OK");
	run("ok");
	requests("p25s:433500-434000/12500");
	run("shot:p25-typed");
}

/* Back deletes the last digit, then leaves without a change */
static void p25_back(void)
{
	start("P25 editor Back", SCR_P25SEARCH);
	run("quick");
	expect(ed_title());
	run("1 4 5 back");
	expect("shows=14");
	run("back back back");	/* the two digits, then the editor */
	expect(T("screen=p25search shows=Range_+-1_MHz__12.5k hides=Search_range", "screen=p25search shows=+-1_MHz"));
	check(sc_sim.requests == 0, "nothing asked");
}

/* The step row: every step in order, round both ways; the applied step reaches the request */
static void p25_step(void)
{
	static const char *const steps[10] = { "20_kHz", "25_kHz", "50_kHz", "100_kHz", "1_MHz", "2.5_kHz", "5_kHz",
					       "6.25_kHz", "10_kHz", "12.5_kHz" };
	start("P25 steps", SCR_P25SEARCH);
	run("quick down down");
	expect("shows=12.5_kHz");
	layout();
	char e[64];
	for (int i = 0; i < 10; i++) {
		run("right");
		snprintf(e, sizeof e, "shows=%s", steps[i]);
		expect(e);
	}
	run("left left");	/* 12.5 -> 10 -> 6.25 */
	expect("shows=6.25_kHz");
	run("up up 1 4 4 ok 1 4 6 ok");
	expect("screen=p25search shows=144-146__6.25k");
	run("ok");
	requests("p25s:144000-146000/6250");
	/* 2.5 kHz and 1 MHz both ride on the start */
	run("ok quick down down left left left left left");	/* 6.25 -> 5 -> 2.5 -> 1 MHz -> 100 -> 50 */
	expect("shows=50_kHz");
	run("right right ok");	/* 1 MHz; the search was stopped: it waits */
	requests("p25s:stop");
	run("ok");
	requests("p25s:144000-146000/1000000");
}

/* Left / Right: one step on Lower, Upper and Move; held (the DM-32's long Left / Right): 1 MHz */
static void p25_move(void)
{
	start("P25 moving the range", SCR_P25SEARCH);
	run("quick 4 3 0 ok 4 4 0");	/* Upper typed, not applied yet */
	run("down right");		/* Upper taken; Step 20 kHz */
	run("down right");		/* Move: +20 kHz */
	expect("shows=430.02 shows=440.02 shows=10_MHz");
	run("long:right");		/* +1 MHz */
	expect("shows=431.02 shows=441.02");
	user_long(ACT_USER_1);	/* the DM-32's held Left: -1 MHz */
	run("long:left left");	/* -1 MHz, -20 kHz */
	expect("shows=429 shows=439 hides=429.02");
	run("up up up right");		/* Lower +20 kHz */
	expect("shows=429.02 shows=439");
	run("down");
	user_long(ACT_USER_1);	/* Upper -1 MHz */
	expect("shows=438 hides=439");
	layout();
	run("shot:p25-editor");
	run("ok");
	expect("shows=429.02-438__20k");
	run("ok");
	requests("p25s:429020-438000/20000");
}

/* Refusals: Upper not above Lower, a range across two bands, an end moved out of its band */
static void p25_refuse(void)
{
	start("P25 refusals", SCR_P25SEARCH);
	run("quick 4 3 5 ok 4 3 4 ok");
	expect(T("toast=Upper_must_be_above_Lower", "toast=*"));
	expect(ed_title());
	run("up 1 7 3 ok 4 0 1 ok");	/* 173-401 MHz: two bands */
	expect(T("toast=Range_must_lie_in_one_band", "toast=Aral"));
	expect(ed_title());
	run("up 4 7 9 ok 4 8 0 ok");	/* 479-480 */
	expect("screen=p25search shows=479-480");
	run("quick down down down");
	user_long(ACT_USER_4);	/* Move +1 MHz: Upper would leave the band */
	expect("shows=479 shows=480 shows=1_MHz");
	run("back");
	check(sc_sim.requests == 0, "nothing asked");
}

/* A running search starts again on an applied range; the manual range is kept with its step when the screen is left
 * and opened again, and is one of the presets from then on */
static void p25_running_kept(void)
{
	start("P25 running search, range kept", SCR_P25SEARCH);
	run("ok");
	sc_sim.req[0] = 0;
	run("quick 1 4 5 ok 1 4 5 5 ok");
	requests("p25s:145000-145500/12500");
	run("back");	/* stops it and leaves */
	requests("p25s:stop");
	int asked = sc_sim.requests;
	ui_push(SCR_P25SEARCH, 0);
	run("wait:200");
	expect("screen=p25search shows=145-145.5__12.5k");
	run("left");
	expect("shows=144-146_MHz");
	run("right");
	expect("shows=145-145.5");
	run("right");
	expect("shows=+-500_kHz");
	check(sc_sim.requests == asked, "Left / Right on a stopped search ask nothing");
}

/* --- DMR search: the same editor and request --- */

static void dmr_range(void)
{
	start("DMR typed range and step", SCR_DMRSEARCH);
	expect(T("screen=dmrsearch shows=Range_+-1_MHz__12.5k shows=*_", "screen=dmrsearch shows=+-1_MHz__12.5k"));
	layout();
	run("4 3 8 ok 4 3 9 ok");
	expect("shows=438-439__12.5k");
	run("quick down down right right");	/* 25 kHz */
	expect("shows=25_kHz");
	layout();
	run("ok");
	expect("shows=438-439__25k");
	layout();
	run("ok");
	requests("dmrs:438000-439000/25000");
	run("quick down down right ok");	/* running: 50 kHz, started again */
	requests("dmrs:438000-439000/50000");
	run("shot:dmr-range");
}

/* --- srchfix: the lines fit the safe area, the status line leaves nothing behind --- */

/* Worst-case content: 4-digit pass and hits, a 3-digit negative floor, the longest NAC, talkgroup, color code, source
 * and ages (58 minutes) */
static uint32_t ago(uint32_t ms)
{
	return (uint32_t)(sim_now() - ms);
}

static void sf_p25(uint8_t phase, bool playing)
{
	p25s_view_t *v = &sc_sim.p25;
	memset(v, 0, sizeof *v);
	v->phase = phase;
	v->playing = playing;
	v->at_hz = 433525000u;
	v->floor_dbm = -120;
	v->sweeps = 9998;
	v->hits = 9999;
#if CONFIG_SEARCH_SURE
	v->step_ms = 83;
#endif
	static const struct { uint32_t hz; uint16_t nac, tg; uint32_t src; uint32_t age; } f[] = {
		{ 433525000u, 0xFFF, 65535, 4294967295u, 3500000u },	/* the longest: NAC $FFF/4095, TG 65535 */
		{ 158006250u, 0x40C, 0, 0, 780000u },			/* the owner's photo: 13m ago */
		{ 159868750u, 0x44E, 612, 663147, 780000u },		/* TG612 */
		{ 145600000u, 0x293, 101, 0, 59000u },
	};
	for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
		v->found[i].hz = f[i].hz;
		v->found[i].nac = f[i].nac;
		v->found[i].tg = f[i].tg;
		v->found[i].src = f[i].src;
		v->found[i].heard_ms = ago(f[i].age);
	}
	v->found_n = 4;
	sc_sim_changed();
}

static void sf_dmr(uint8_t phase, bool playing, bool all)
{
	dmrs_view_t *v = &sc_sim.dmr;
	memset(v, 0, sizeof *v);
	v->phase = phase;
	v->playing = playing;
	v->kind = all ? DMRS_KIND_ALL : DMRS_KIND_DMR;
	v->mode = all ? DMRS_MODE_NXDN : DMRS_MODE_DMR;
	v->at_hz = 438712500u;
	v->floor_dbm = -120;
	v->sweeps = 9998;
	v->hits = 9999;
#if CONFIG_SEARCH_SURE
	v->step_ms = 83;
#endif
	static const struct {
		uint32_t hz, dst, src;
		uint16_t cc;
		uint8_t ts, sync, mode, group, enc;
		uint32_t age;
	} f[] = {
		{ 438712500u, 16777215u, 16777214u, 15, 3, 3, DMRS_MODE_DMR, 1, 1, 3500000u },	/* everything at once */
		{ 438712500u, 2620123u, 2620124u, 1, 3, 1, DMRS_MODE_DMR, 1, 0, 780000u },	/* a repeater, 13m ago */
		{ 433525000u, 0, 4294967295u, 0xFFF, 0, 0, DMRS_MODE_P25, 1, 0, 3500000u },	/* NAC $FFF/4095 */
		{ 433525000u, 612, 663147, 0x1C8, 0, 0, DMRS_MODE_P25, 1, 0, 780000u },
		{ 144800000u, 1, 3, 15, 0, 0, DMRS_MODE_YSF, 0, 0, 780000u },
		{ 144800000u, 2, 3, 63, 0, 0, DMRS_MODE_NXDN, 1, 1, 3500000u },
	};
	for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
		dmrs_found_t *o = &v->found[i];
		o->hz = f[i].hz;
		o->dst = f[i].dst;
		o->src = f[i].src;
		o->cc = f[i].cc;
		o->ts_mask = f[i].ts;
		o->sync_mask = f[i].sync;
		o->mode = all ? f[i].mode : DMRS_MODE_DMR;
		o->group = f[i].group;
		o->enc = f[i].enc;
		o->heard_ms = ago(f[i].age);
	}
	v->found_n = 6;
	sc_sim_changed();
}

static void sf_fill(ui_scr_t scr, uint8_t phase, bool playing)
{
	if (scr == SCR_P25SEARCH)
		sf_p25(phase, playing);
	else
		sf_dmr(phase, playing, scr != SCR_DMRSEARCH);
}

/* Every phase of each screen with the worst content: the layout check (nothing past the 220 px of the safe area, none
 * clipped, none on top of another) and the pass line in its shortest form */
static void sf_layouts(ui_scr_t scr)
{
	static const struct { uint8_t phase; bool playing; } ph[] = { { 1, false }, { 2, false }, { 3, true } };
	for (unsigned i = 0; i < sizeof ph / sizeof ph[0]; i++) {
		start("srchfix layout", scr);
		sf_fill(scr, ph[i].phase, ph[i].playing);
		run("wait:300");
		layout();
		char shot[40];
		snprintf(shot, sizeof shot, "shot:sf-%d-%u", (int)scr, i);	/* SCAN_SHOTS: the screens as PNGs */
		run(shot);
		expect(T("shows=P9999_h9999_fl-120", "shows=T9999_b9999_tb-120"));
		/* the lines in the shortest form that fits the 24 glyphs (and the longer one where it does) */
		if (scr == SCR_P25SEARCH) {
			expect("shows=433.5250_FFF_TG65535_58m shows=158.0062_$40C/1036_13m shows=159.8687_$44E_TG612_13m");
			expect("shows=145.6000_$293_TG101_59s");
		} else if (scr == SCR_DMRSEARCH) {
			expect("shows=438.7125_CC15_T12_RD_58m shows=438.7125_CC1_TS1+2_R_13m");
			expect(T("shows=TG_2620123_from_2620124", "shows=TG_2620123_<2620124"));
		} else {
			expect("shows=433.5250_P25_$FFF_58m shows=433.5250_P25_$1C8_13m shows=438.7125_DMR_CC1_R_13m");
			expect(T("shows=TG_2620123_from_2620124", "shows=TG_2620123_<2620124"));
			if (ph[i].phase == 2 && !ph[i].playing)
				expect(T("shows=Prb_NXDN_438.7125", "shows=Dene_NXDN_438.7125"));
		}
	}
	/* the owner's photo: pass 72, 193 hits, floor -84 keeps its words */
	start("srchfix pass words", scr);
	sf_fill(scr, 1, false);
	sc_sim.p25.sweeps = sc_sim.dmr.sweeps = 71;
	sc_sim.p25.hits = sc_sim.dmr.hits = 193;
	sc_sim.p25.floor_dbm = sc_sim.dmr.floor_dbm = -84;
	sc_sim_changed();
	run("wait:300");
	layout();
	expect(T("shows=Pass_72_hits_193_-84dBm", "shows=Tur_72_bulgu_193_-84dBm"));
}

static void sf_p25_layout(void) { sf_layouts(SCR_P25SEARCH); }
static void sf_dmr_layout(void) { sf_layouts(SCR_DMRSEARCH); }
static void sf_all_layout(void) { sf_layouts(SCR_ALLSEARCH); }

/* The same screen after a status line change (Sweep, Probe, the call) is what a fresh draw of the new state gives:
 * compared pixel for pixel in the panel as gfx_flush sent it (no old digits left over "Probe") */
static void sf_redraw(ui_scr_t scr)
{
	static const struct { uint8_t phase; bool playing; } seq[] = { { 1, false }, { 2, false }, { 3, true }, { 1, false } };
	size_t n = (size_t)240 * 320;
	uint16_t *want = malloc(n * sizeof *want);
	for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
		/* the frame as drawn from a clean start */
		start("srchfix redraw fresh", scr);
		sf_fill(scr, seq[i].phase, seq[i].playing);
		sc_sim.p25.at_hz = sc_sim.dmr.at_hz = 159868700u + i * 100u;
		sc_sim_changed();
		run("wait:300");
		memcpy(want, sim_panel(), n * sizeof *want);
		/* ... and after the other phases before it */
		start("srchfix redraw after", scr);
		for (unsigned k = 0; k <= i; k++) {
			sf_fill(scr, seq[k].phase, seq[k].playing);
			sc_sim.p25.at_hz = sc_sim.dmr.at_hz = k < i ? 438712500u + k * 12500u : 159868700u + i * 100u;
			sc_sim_changed();
			run("wait:300");
		}
		check(memcmp(want, sim_panel(), n * sizeof *want) == 0, "the status line leaves old pixels behind");
	}
	free(want);
}

static void sf_p25_redraw(void) { sf_redraw(SCR_P25SEARCH); }
static void sf_dmr_redraw(void) { sf_redraw(SCR_DMRSEARCH); }
static void sf_all_redraw(void) { sf_redraw(SCR_ALLSEARCH); }

/* --- searchlist: 16 finds, the list scrolls with the picked one --- */

/* Find i (0 the newest) at 430 + i / 10 MHz: its line shows "430.i000" / "431.0000" ...; P25: every third has a source
 * (a second line "ID 5000i"), DMR / all digital: every second has a talkgroup and source (a second line "TG 7000i") */
static uint32_t sl_hz(unsigned i)
{
	return 430000000u + 100000u * i;
}

static void sl_mhz(unsigned i, char *out, size_t cap)
{
	uint32_t hz = sl_hz(i);
	snprintf(out, cap, "%lu.%04lu", (unsigned long)(hz / 1000000u), (unsigned long)(hz / 100u % 10000u));
}

static bool sl_two(ui_scr_t scr, unsigned i)
{
	return scr == SCR_P25SEARCH ? i % 3u == 0 : i % 2u == 0;
}

static void sl_second(ui_scr_t scr, unsigned i, char *out, size_t cap)
{
	snprintf(out, cap, scr == SCR_P25SEARCH ? "ID %u" : "TG %u", (scr == SCR_P25SEARCH ? 50000u : 70000u) + i);
}

/* Find i of the list as the test sets it (the key it is followed by: frequency, NAC / colour code, talkgroup) */
static void sl_entry(ui_scr_t scr, unsigned slot, unsigned i)
{
	if (scr == SCR_P25SEARCH) {
		p25s_found_t *f = &sc_sim.p25.found[slot];
		memset(f, 0, sizeof *f);
		f->hz = sl_hz(i);
		f->nac = 0x1C8;
		f->tg = 0;
		f->src = sl_two(scr, i) ? 50000u + i : 0;
		f->heard_ms = ago(60000u);
	} else {
		dmrs_found_t *f = &sc_sim.dmr.found[slot];
		memset(f, 0, sizeof *f);
		f->hz = sl_hz(i);
		f->cc = 1;
		f->ts_mask = 1;
		f->sync_mask = DMRS_SYNC_BS;
		f->mode = DMRS_MODE_DMR;
		f->group = true;
		f->dst = sl_two(scr, i) ? 70000u + i : 0;
		f->src = sl_two(scr, i) ? 9u : 0;
		f->heard_ms = ago(60000u);
	}
}

static void sl_fill(ui_scr_t scr, unsigned n)
{
	memset(&sc_sim.p25, 0, sizeof sc_sim.p25);
	memset(&sc_sim.dmr, 0, sizeof sc_sim.dmr);
	sc_sim.p25.phase = P25S_SWEEP;
	sc_sim.dmr.phase = DMRS_SWEEP;
	sc_sim.p25.at_hz = sc_sim.dmr.at_hz = 433500000u;
	sc_sim.dmr.kind = scr == SCR_ALLSEARCH ? DMRS_KIND_ALL : DMRS_KIND_DMR;
	for (unsigned i = 0; i < n; i++)
		sl_entry(scr, i, i);
	sc_sim.p25.found_n = sc_sim.dmr.found_n = (uint8_t)n;
	sc_sim_changed();
	run("wait:300");
}

/* Find i drawn whole (both its lines above the hint line), on the highlight when picked and only then */
static void sl_drawn(ui_scr_t scr, unsigned i, bool picked)
{
	char t[24], what[96];
	int y0, y1;
	bool focus;
	sl_mhz(i, t, sizeof t);
	snprintf(what, sizeof what, "find %u (%s) drawn%s", i, t, picked ? ", picked" : "");
	check(sc_sim_text(t, &y0, &y1, &focus) && y1 <= sc_sim_hint_y() && focus == picked, what);
	if (picked && sl_two(scr, i)) {
		sl_second(scr, i, t, sizeof t);
		snprintf(what, sizeof what, "find %u's second line (%s) drawn and picked", i, t);
		check(sc_sim_text(t, &y0, &y1, &focus) && y1 <= sc_sim_hint_y() && focus, what);
	}
}

static void sl_hidden(unsigned i)
{
	char t[24], what[64];
	int y0, y1;
	bool focus;
	sl_mhz(i, t, sizeof t);
	snprintf(what, sizeof what, "find %u (%s) not drawn", i, t);
	check(!sc_sim_text(t, &y0, &y1, &focus), what);
}

/* No find but i on the highlight (-1: none) */
static void sl_only(int picked, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		char t[24], what[96];
		int y0, y1;
		bool focus;
		sl_mhz(i, t, sizeof t);
		if (sc_sim_text(t, &y0, &y1, &focus) && focus != ((int)i == picked)) {
			snprintf(what, sizeof what, "find %u (%s) %s", i, t, focus ? "highlighted, not picked" : "picked, not highlighted");
			check(false, what);
		}
	}
}

static void sl_marker(const char *want)
{
	char what[48];
	snprintf(what, sizeof what, "the list marker \"%s\"", want);
	if (*want)
		check(sc_sim_text_is(want), what);
	else
		check(!sc_sim_text_is("^") && !sc_sim_text_is("v") && !sc_sim_text_is("^v"), "no list marker");
}

static void sl_scroll(ui_scr_t scr)
{
	start("searchlist scroll", scr);
	sl_fill(scr, 16);
	layout();
	/* Nothing picked: the newest on top, the oldest not on screen, "v" */
	sl_drawn(scr, 0, false);
	sl_hidden(15);
	sl_only(-1, 16);
	sl_marker("v");
	/* Down through all 16: the picked one always whole on screen and the only one highlighted */
	for (unsigned k = 0; k < 16u; k++) {
		run("down");
		sl_drawn(scr, k, true);
		sl_only((int)k, 16);
		layout();
	}
	sl_hidden(0);
	sl_marker("^");
	char shot[32];
	snprintf(shot, sizeof shot, "shot:sl-bottom-%d", (int)scr);	/* SCAN_SHOTS: the screens as PNGs */
	run(shot);
	run("down");	/* the oldest stays picked */
	sl_drawn(scr, 15, true);
	/* ... and Up back: each on screen, then none: the top again */
	for (int k = 14; k >= 0; k--) {
		run("up");
		sl_drawn(scr, (unsigned)k, true);
		sl_only(k, 16);
	}
	sl_marker("v");
	run("up");
	sl_drawn(scr, 0, false);
	sl_only(-1, 16);
	sl_hidden(15);
	sl_marker("v");
	layout();
}

/* A new find on top while one far down is picked: the list drops its oldest, the picked one moves down a row and stays
 * whole on screen and picked */
static void sl_arrivals(ui_scr_t scr)
{
	start("searchlist arrivals", scr);
	sl_fill(scr, 16);
	for (unsigned k = 0; k < 12u; k++)
		run("down");
	sl_drawn(scr, 11, true);
	for (unsigned n = 0; n < 4u; n++) {
		/* a new find (16 + n, a new frequency) on top; the oldest drops */
		if (scr == SCR_P25SEARCH)
			memmove(&sc_sim.p25.found[1], &sc_sim.p25.found[0], sizeof sc_sim.p25.found[0] * 15u);
		else
			memmove(&sc_sim.dmr.found[1], &sc_sim.dmr.found[0], sizeof sc_sim.dmr.found[0] * 15u);
		sl_entry(scr, 0, 16u + n);
		sc_sim_changed();
		run("wait:300");
		sl_drawn(scr, 11, true);	/* the same find (its frequency, key), now row 12 + n */
		sl_only(11, 20);
		layout();
	}
	run("up");	/* Up from it: the find above it, still on screen */
	sl_drawn(scr, 10, true);
}

/* Fewer finds than the screen holds: everything drawn, no marker, Down picks as before */
static void sl_few(ui_scr_t scr)
{
	start("searchlist few", scr);
	sl_fill(scr, 3);
	for (unsigned i = 0; i < 3u; i++)
		sl_drawn(scr, i, false);
	sl_marker("");
	run("down down");
	sl_drawn(scr, 1, true);
	sl_marker("");
	layout();
}

static void sl_p25_scroll(void) { sl_scroll(SCR_P25SEARCH); }
static void sl_dmr_scroll(void) { sl_scroll(SCR_DMRSEARCH); }
static void sl_all_scroll(void) { sl_scroll(SCR_ALLSEARCH); }
static void sl_p25_arrivals(void) { sl_arrivals(SCR_P25SEARCH); }
static void sl_dmr_arrivals(void) { sl_arrivals(SCR_DMRSEARCH); }
static void sl_all_arrivals(void) { sl_arrivals(SCR_ALLSEARCH); }
static void sl_p25_few(void) { sl_few(SCR_P25SEARCH); }
static void sl_dmr_few(void) { sl_few(SCR_DMRSEARCH); }
static void sl_all_few(void) { sl_few(SCR_ALLSEARCH); }

typedef void (*case_fn)(void);
static const case_fn all[] = {
	p25_presets, p25_type, p25_back, p25_step, p25_move, p25_refuse, p25_running_kept, dmr_range,
	sf_p25_layout, sf_dmr_layout, sf_all_layout, sf_p25_redraw, sf_dmr_redraw, sf_all_redraw,
	sl_p25_scroll, sl_dmr_scroll, sl_all_scroll, sl_p25_arrivals, sl_dmr_arrivals, sl_all_arrivals,
	sl_p25_few, sl_dmr_few, sl_all_few,
};

int main(void)
{
	int bad = 0, cases = 0;
	for (int l = 0; l < 2; l++) {
		for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				turkish = l == 1;
				all[i]();
				fflush(stdout);
				_exit(fails != 0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			cases++;
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
				bad++;
				if (!WIFEXITED(st))
					printf("FAIL case %u (%s): crashed\n", i, l ? "tr" : "en");
			}
		}
	}
	printf("Plus search range UI: %d cases (English and Turkish), %d failed\n", cases, bad);
	return bad != 0;
}
