/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 14 air items (docs/plus.md "Round 14: air band and spectrum") through the real UI in upstream's
 * simulator (tests/plus/ui_sim_air.c wraps it: a radio that receives AM, the test's position), on the DM-32UV's display
 * class (240 x 320, insets 10 x 5), in English and in Turkish. Each case runs in a child process (the screens' static
 * state starts over). Covers:
 *   - item 44a, Tools > Airfields after the table's cleanup: "+N" on the list line when the service shown has more
 *     frequencies (Antalya's two towers), none with one; an airfield's every frequency in its details, the list
 *     scrolling to the last one and OK tuning it; every page fits;
 *   - item 44b, air watch (CONFIG_AIR_WATCH, ui_airwatch.inc; the radio core faked in ui_sim_air.c): # on an
 *     airfield's page puts all its frequencies on the multi-watch ring, A..F each its own VFO, the ATIS last and
 *     marked (Esenboga's four: four letters, owner's beta.6 report); more than six (Antalya's seven): the first six
 *     and the toast "LTAI: 6/7 frequencies watched"; Back twice, or
 *     # 5 Stop, brings the radio and the user's watch back as they were (off, or the user's ring with its selection
 *     and channels), nothing saved meanwhile; two frequencies make dual watch's pair, one is tuned as OK does (a
 *     channel name on two services counts once); refused while transmitting and without frequencies; a power off
 *     during it doesn't bring it back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "ui/ui_svc.h"
#include "ui_sim_air.h"

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

/* "a=b c=d": every check after the script, as tests/host/uiFlowTest.c's run_flow */
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

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int i = 0; i < n; i++)
		printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[i]);
	fails += n;
}

static void pos(double la, double lo)
{
	air_sim.pos = true;
	air_sim.lat_e7 = (int32_t)(la * 1e7 + (la < 0 ? -0.5 : 0.5));
	air_sim.lon_e7 = (int32_t)(lo * 1e7 + (lo < 0 ? -0.5 : 0.5));
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	const char *shots = getenv("AIR_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "air-tr-" : "air-en-");
	air_sim_setup();
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	pos(39.9208, 32.8541);		/* Ankara, Kizilay */
	run("wait:200");
}

static void airfields(void)
{
	ui_push(SCR_AIRFIELDS, 0);
	run("wait:100");
}

/* --- Item 44a: the airfield table --- */

/* Antalya: two tower frequencies, "+1" after the first; its details list all seven, the last one reached by
 * scrolling and tuned */
static void af_more(void)
{
	start("AF several frequencies on one service");
	pos(36.8969, 30.7133);		/* Antalya */
	airfields();
	expect(T("screen=airfields shows=Nearest_first shows=LTAI shows=TWR_125.000_+1",
		 "screen=airfields shows=En_yak shows=LTAI shows=TWR_125.000_+1"));
	run("shot:airfields-more");
	layout();
	run("ok");
	expect("shows=ATIS_118.275 shows=GND_121.900 shows=TWR_125.000 hides=OPS_131.450 hides=DEL_123.175");
	layout();
	run("down down down down down down");	/* ATIS, GND, TWR, TWR, APP, APP, APP 124.425 */
	expect("shows=APP_124.425");
	run("shot:airfield-last");
	layout();
	run("ok");
	expect("screen=home vfo=1 freq=124425000");
}

/* Kizilay: Guvercinlik's one tower, no "+"; Etimesgut keeps only its tower (Esenboga approach's 119.100 removed) */
static void af_one(void)
{
	start("AF one frequency on a service");
	airfields();
	expect("shows=LTAB shows=TWR_132.000 hides=TWR_132.000_+");
	run("down ok");
	expect("shows=LTAD shows=TWR_122.100 hides=APP_119.100");
	layout();
}

/* Kastamonu: none in the data; the details say so within the panel */
static void af_none(void)
{
	start("AF no frequency");
	pos(41.3142, 33.7958);
	airfields();
	run("ok");
	expect(T("shows=LTAL shows=No_frequencies", "shows=LTAL shows=rekans_yok"));
	layout();
}

/* --- Item 44b: air watch --- */

#if CONFIG_AIR_WATCH
static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s (%s): %s\n", name, turkish ? "tr" : "en", what);
		fails++;
	}
}

/* Antalya's seven: GND 121.900, TWR 125.000, TWR 126.100, APP 119.650, APP 124.350, APP 124.425 on the ring, its ATIS
 * 118.275 (last in the watch's order) the seventh: left over */
static const uint32_t lti_hz[6] = { 121900000, 125000000, 126100000, 119650000, 124350000, 124425000 };

static void antalya_watch(void)
{
	pos(36.8969, 30.7133);
	airfields();
	run("ok");
	run("ctx");
}

/* # on Antalya's page (seven frequencies): the first six on the ring, A..F each its own VFO, received there; the
 * toast says six of seven; Home shows it */
static void aw_watch(void)
{
	start("AW watch all");
	pos(36.8969, 30.7133);
	airfields();
	run("ok");
	expect(T("shows=#_watch_all", "shows=#_t"));
	layout();
	run("ctx");
	expect(T("screen=home dual=1 mwn=6 mwradion=6 mwsel=A toast=LTAI:_6/7_frequencies_watched",
		 "screen=home dual=1 mwn=6 mwradion=6 mwsel=A toast=LTAI:_6/7_frekans_izleniyor"));
	check(air_sim.vfo_calls == 6, "six VFOs asked for");
	check(air_sim.atis == 0, "no ATIS among the six");
	for (uint8_t k = 0; k < 6; k++)
		check(air_sim.hz[k] == lti_hz[k] && air_sim_vfo_hz(k) == lti_hz[k], "each letter's VFO on its frequency");
	expect("vfo=1 vfoidx=0 freq=121900000 shows=121.90 shows=AM_124.425 hides=FM_124 hides=118.27");
	run("tune:e");
	expect("vfo=1 vfoidx=4 freq=124350000");
	run("shot:home-air");
	layout();
}

/* Esenboga (owner's beta.6 report: four shown, three watched): ATIS 123.600, GND 121.900, TWR 118.100, APP 119.100
 * all on the ring: GND, TWR, APP on A..C, the ATIS last on D and marked as one */
static void aw_four(void)
{
	static const uint32_t ltac_hz[4] = { 121900000, 118100000, 119100000, 123600000 };
	start("AW Esenboga: all four");
	pos(40.1281, 32.9951);		/* Esenboga */
	airfields();
	expect("shows=LTAC");
	run("ok");
	expect("shows=ATIS_123.600 shows=GND_121.900 shows=TWR_118.100 shows=APP_119.100");
	run("ctx");
	expect(T("screen=home dual=1 mwn=4 mwradion=4 mwsel=A toast=LTAC:_Air_watch_on",
		 "screen=home dual=1 mwn=4 mwradion=4 mwsel=A toast=LTAC:_Hava_izleme_a"));
	check(air_sim.vfo_calls == 4, "four VFOs asked for");
	for (uint8_t k = 0; k < 4; k++)
		check(air_sim.hz[k] == ltac_hz[k] && air_sim_vfo_hz(k) == ltac_hz[k], "each letter's VFO on its frequency");
	check(air_sim.atis == 1u << 3, "D (the ATIS) marked, A..C not");
	expect("shows=121.90 shows=118.10 shows=119.10 shows=123.60");
	/* Owner's beta.7 report: every letter in AM, and Home says so: the rows and the status bar's mode word (it read
	 * "FM DUAL"), no "FM" anywhere */
	expect("shows=AM_121.900 shows=AM_123.600 hides=FM");
	run("shot:home-air-four");
	layout();
	run("back back wait:300");
	expect(T("dual=0 toast=Air_watch_off", "dual=0 toast=Hava_izleme_kapal"));
}

/* Back twice stops it: the radio back on the channel it was on, the watch off, nothing saved meanwhile */
static void aw_stop(void)
{
	start("AW stop: back as before");
	uint16_t pos0 = ui_view()->zone_pos;
	bool vfo0 = ui_view()->vfo;
	int32_t b0 = ui_svc_setting_get(UI_SET_DUAL_B), p0 = ui_svc_setting_get(UI_SET_MW_P1);
	antalya_watch();
	expect("dual=1 mwn=6");
	run("abkey abkey");	/* C selected: a change the user's watch never had */
	expect("mwsel=C");
	check(ui_svc_setting_get(UI_SET_DUAL_B) == b0 && ui_svc_setting_get(UI_SET_MW_P1) == p0, "nothing saved");
	run("back back wait:300");
	expect(T("dual=0 toast=Air_watch_off", "dual=0 toast=Hava_izleme_kapal"));
	check(air_sim.end_calls == 1, "the end asked for once");
	check(ui_view()->zone_pos == pos0 && ui_view()->vfo == vfo0, "the radio back where it was");
	check(ui_svc_setting_get(UI_SET_DUAL_B) == b0 && ui_svc_setting_get(UI_SET_MW_P1) == p0, "the settings as before");
	layout();
}

/* The user's ring of four, C selected: the air watch takes its place; # 5 Stop: the user's ring again as it was */
static void aw_user_ring(void)
{
	start("AW the user's ring kept");
	run("mwatch:4 quick 7 abkey abkey");
	expect("dual=1 mwn=4 mwsel=C mwposb=4 mwposc=5 mwposd=6");
	ui_go_home();
	antalya_watch();
	expect("dual=1 mwn=6 mwradion=6 mwsel=A");
	run("ctx 5 wait:300");
	expect("dual=1 mwn=4 mwradion=4 mwsel=C mwposb=4 mwposc=5 mwposd=6");
	expect(T("toast=Air_watch_off", "toast=Hava_izleme_kapal"));
	layout();
}

/* Two frequencies (Guvercinlik: GND 120.200, TWR 132.000): dual watch's pair of two VFOs */
static void aw_pair(void)
{
	start("AW two frequencies: the pair");
	airfields();
	run("ok ctx");
	expect("screen=home dual=1 mwn=0 mwradion=2 vfo=1 freq=120200000");
	/* Both panes and the status bar in AM (they read "VFO FM" and "FM DUAL") */
	expect("shows=VFO_AM hides=FM");
	check(air_sim.vfo_calls == 2 && air_sim.hz[1] == 132000000, "two VFOs");
	run("back back wait:300");
	expect("dual=0");
	check(air_sim.end_calls == 1, "ended");
}

/* One frequency (Etimesgut: TWR 122.100): tuned as OK does; one channel name on two services (Afyon: TWR and APP
 * 122.100) is one frequency too */
static void aw_one(void)
{
	start("AW one frequency: tuned");
	airfields();
	run("down ok ctx");
	expect("screen=home dual=0 vfo=1 freq=122100000");
	check(air_sim.vfo_calls == 0, "no ring");
	pos(38.7264, 30.6011);	/* Afyon */
	airfields();
	expect("shows=LTAH");
	run("ok ctx");
	expect("screen=home dual=0 freq=122100000");
	check(air_sim.vfo_calls == 0, "no ring for one channel name");
}

/* Refused while transmitting; an airfield without frequencies */
static void aw_refused(void)
{
	start("AW refused");
	pos(36.8969, 30.7133);
	airfields();
	run("ok");
	air_sim_state(RS_TX);
	run("ctx");
	expect(T("screen=airfields toast=Can't_tune_while_transmitting", "screen=airfields"));
	air_sim_state(RS_IDLE);
	check(air_sim.vfo_calls == 0, "nothing asked for");
	pos(41.3142, 33.7958);	/* Kastamonu: none in the data */
	run("back back");
	airfields();
	run("ok ctx");
	expect("screen=airfields dual=0");
	check(air_sim.vfo_calls == 0, "nothing asked for");
}

/* A power off during it: the watch doesn't come back (nothing of it was saved) */
static void aw_reboot(void)
{
	start("AW power off during it");
	int32_t b0 = ui_svc_setting_get(UI_SET_DUAL_B);
	antalya_watch();
	expect("dual=1 mwn=6");
	run("reboot wait:500");
	expect("dual=0");
	check(ui_svc_setting_get(UI_SET_DUAL_B) == b0, "the settings as before");
}
#endif

#if CONFIG_AUTO_SCAN
/* --- DDM32 Plus auto scan (docs/autoscan-design.md; ui_autoscan.c on the map pack made from the build's tables) --- */
#include "board.h"
#include "radio/radio_am.h"
#include "radio/scan.h"
#include "ui/autoscan.h"
#include "ui/rpt.h"
#include "ui/ui_screens.h"
#include "aprs/aprs_fmt.h"
#include "refpack_fixture.h"

static void gps(double la, double lo)
{
	air_sim.gps = AIR_GPS_FIX;
	air_sim.lat_e7 = (int32_t)(la * 1e7 + (la < 0 ? -0.5 : 0.5));
	air_sim.lon_e7 = (int32_t)(lo * 1e7 + (lo < 0 ? -0.5 : 0.5));
}

/* The repeaters the scan must take at the GPS position: FM / DMR, a position, RX in the radio's bands (not the air
 * band), within 60 km, the six nearest; the ring has the FM ones first. n: how many within reach */
static int want_rpt(uint16_t idx[AS_MAX], int *within)
{
	int n = 0;
	uint32_t d[AS_MAX];
	*within = 0;
	for (uint16_t i = 0; i < rpt_count(); i++) {
		const rpt_entry_t *e = rpt_get(i);
		uint8_t m = e->mode & RPT_MODE_MASK;
		if ((m != RPT_FM && m != RPT_DMR) || e->lat_e5 == RPT_NOPOS || !board_in_band(e->rx_hz, false) ||
		    radio_am_band(e->rx_hz))
			continue;
		uint32_t di = aprs_distance_m(air_sim.lat_e7, air_sim.lon_e7, e->lat_e5 * 100, e->lon_e5 * 100);
		if (di > AS_RPT_RADIUS_M)
			continue;
		(*within)++;
		int at = n;
		while (at > 0 && d[at - 1] > di)
			at--;
		if (at >= (int)AS_MAX)
			continue;
		int last = n < (int)AS_MAX ? n : (int)AS_MAX - 1;
		memmove(&idx[at + 1], &idx[at], (size_t)(last - at) * sizeof idx[0]);
		memmove(&d[at + 1], &d[at], (size_t)(last - at) * sizeof d[0]);
		idx[at] = i;
		d[at] = di;
		if (n < (int)AS_MAX)
			n++;
	}
	uint16_t g[AS_MAX];	/* FM first */
	int m = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int k = 0; k < n; k++)
			if (((rpt_get(idx[k])->mode & RPT_MODE_MASK) == RPT_DMR) == (pass == 1))
				g[m++] = idx[k];
	memcpy(idx, g, (size_t)n * sizeof g[0]);
	return n;
}

/* A position with exactly `count` repeaters within 60 km (a grid search over the country); false: none such */
static bool place_with(int count)
{
	for (double la = 36.0; la <= 42.0; la += 0.25)
		for (double lo = 26.0; lo <= 45.0; lo += 0.25) {
			uint16_t idx[AS_MAX];
			int within;
			gps(la, lo);
			if (want_rpt(idx, &within) == count && within == count)
				return true;
		}
	return false;
}

/* The ring holds the wanted repeaters, each letter's VFO on its output with its kind, Home shows the labels */
static void ring_is(const uint16_t idx[], int n)
{
	expect(n > 2 ? "screen=home dual=1" : "screen=home dual=1 mwn=0");
	for (int k = 0; k < n; k++) {
		const rpt_entry_t *e = rpt_get(idx[k]);
		char buf[64];
		check(air_sim.hz[k] == e->rx_hz && air_sim_vfo_hz((uint8_t)k) == e->rx_hz, "a letter on its repeater");
		bool dmr = (e->mode & RPT_MODE_MASK) == RPT_DMR;
		check(((air_sim.arg[k] & UI_AIR_DMR) != 0) == dmr && ((air_sim.arg[k] & UI_AIR_FM) != 0) == !dmr,
		      "the letter's mode");
		if (n > 2) {	/* the ring's rows: the label (callsign and km) */
			snprintf(buf, sizeof buf, "shows=%s", e->name);
			for (char *c = buf; *c; c++)
				if (*c == ' ')
					*c = '_';
			expect(buf);
		}
	}
}

/* Repeater scan at Kizilay: the nearest six on the ring with their labels; Back twice: off, the user's state back,
 * and it stays off (review M2: no refresh starts it again) */
static void as_rpt(void)
{
	start("AS repeater scan");
	gps(39.9208, 32.8541);
	uint16_t idx[AS_MAX];
	int within, n = want_rpt(idx, &within);
	check(n >= 3, "Ankara has three repeaters at least");
	ui_autoscan_toggle(AS_RPT);
	run("wait:1500");
	expect(T("toast=Repeater_scan_on", "toast=R"));
	check(ui_autoscan_on() == AS_RPT && air_sim.vfo_calls == n, "a VFO for each letter");
	ring_is(idx, n);
	run("shot:home-rptscan");
	layout();
	/* The pickers can't change a letter: the scan picks them (# 6 Change C, OK on a channel) */
	int calls0 = air_sim.vfo_calls;
	run("ctx 6 ok wait:200");
	expect(T("toast=The_scan_picks_its_letters", "toast=Harfleri_tarama_se"));
	check(air_sim.vfo_calls == calls0 && ui_autoscan_on() == AS_RPT, "nothing changed");
	ui_go_home();
	run("wait:200");
	run("back back wait:300");
	expect(T("dual=0 toast=Repeater_scan_off", "dual=0 toast=R"));
	check(ui_autoscan_on() == AS_OFF && air_sim.end_calls == 1, "off, the end asked for");
	int calls = air_sim.vfo_calls;
	gps(39.9208 + 0.3, 32.8541);
	run("wait:35000");
	check(air_sim.vfo_calls == calls && ui_autoscan_on() == AS_OFF, "stopped stays stopped");
	expect("dual=0");
}

/* Moving: after 30 s and 1 km a refresh; a letter replaced in place (UI_AIR_REPLACE), never while the ring holds a
 * signal (it waits, then does it) */
static void as_move(void)
{
	start("AS repeater scan moving");
	gps(39.9208, 32.8541);
	ui_autoscan_toggle(AS_RPT);
	run("wait:1500");
	int calls = air_sim.vfo_calls;
	check(calls >= 3, "a ring");
	gps(39.7767, 30.5206);	/* 200 km west: Eskisehir */
	air_sim_phase(SCAN_PH_HOLD);
	run("wait:40000");
	check(air_sim.vfo_calls == calls, "nothing while the ring holds a signal");
	air_sim_phase(SCAN_PH_LISTEN);
	run("wait:1500");
	check(air_sim.vfo_calls > calls && air_sim.replaced > 0 && air_sim.refused == 0, "the refresh once quiet");
	uint8_t ring = ui_dual_ring();
	for (uint8_t k = 0; k < (ring ? ring : 2u); k++) {	/* a repeater with that output within reach */
		bool near = false;
		for (uint16_t j = 0; j < rpt_count() && !near; j++) {
			const rpt_entry_t *e = rpt_get(j);
			near = e->rx_hz == air_sim.hz[k] && e->lat_e5 != RPT_NOPOS &&
			       aprs_distance_m(air_sim.lat_e7, air_sim.lon_e7, e->lat_e5 * 100, e->lon_e5 * 100) <=
				       AS_RPT_RADIUS_M / 100u * AS_KEEP_PCT;
		}
		check(near, "every letter within reach of the new place");
	}
	expect("dual=1");
	/* Standing still: no more refreshes */
	calls = air_sim.vfo_calls;
	run("wait:65000");
	check(air_sim.vfo_calls == calls, "no refresh without moving");
	layout();
}

/* A refresh the radio core refused (the ring held by the time it read it, after the UI saw it quiet): asked again until
 * the letter's VFO is the whole letter (RX, TX, mode, tone, colour code and slot), not only its RX: two repeaters often
 * share an output (verifier fix) */
static void as_retry(void)
{
	start("AS refused refresh asked again");
	gps(39.9208, 32.8541);
	ui_autoscan_toggle(AS_RPT);
	run("wait:1500");
	check(ui_dual_ring() >= 3, "a ring");
	air_sim.refuse = 64;	/* every refresh of the move refused */
	gps(39.7767, 30.5206);
	run("wait:31000");
	int refused = air_sim.refused;
	check(refused > 0, "the refresh refused");
	air_sim.refuse = 0;
	run("wait:3000");
	check(air_sim.replaced > 0, "asked again and taken");
	for (uint8_t k = 0; k < ui_dual_ring(); k++) {
		channel_t c;
		uint32_t a = air_sim.arg[k], hz = air_sim.hz[k];
		uint8_t p = (uint8_t)((a >> UI_AIR_P_SHIFT) & 0x3Fu);
		ui_svc_vfo_get(k, &c);
		bool dmr = (a & UI_AIR_DMR) != 0;
		check(c.rx_hz == hz && c.tx_hz == (uint32_t)((int32_t)hz + ((int32_t)a >> UI_AIR_OFF_SHIFT) * 1250) &&
			      c.mode == (dmr ? RADIO_MODE_DMR : RADIO_MODE_FM) &&
			      (dmr ? c.color_code == (p & 0x0Fu) : (p == 0) == (c.tx_tone.type == TONE_NONE)),
		      "each letter whole on its VFO");
	}
	int calls = air_sim.vfo_calls;
	run("wait:3000");
	check(air_sim.vfo_calls == calls, "nothing asked once taken");
}

/* None within 60 km: no ring, the user's state as it was, the scan waits; one only: the same; then a place with more:
 * the ring */
static void as_few(void)
{
	start("AS none or one within reach");
	if (!place_with(0)) {
		check(false, "a place with no repeater");
		return;
	}
	ui_autoscan_toggle(AS_RPT);
	run("wait:1500");
	expect(T("dual=0 toast=Repeater_scan:_none_within_60_km", "dual=0 toast=R"));
	check(ui_autoscan_on() == AS_RPT && air_sim.vfo_calls == 0, "on, waiting, no ring");
	if (place_with(1)) {
		run("wait:29000");	/* the refresh 30 s after the first, its toast fresh */
		expect(T("dual=0 toast=Repeater_scan:_1_repeater_only", "dual=0 toast=R"));
		check(air_sim.vfo_calls == 0, "one: no ring");
	}
	gps(39.9208, 32.8541);
	run("wait:31000");
	expect("dual=1");
	check(air_sim.vfo_calls >= 3, "the ring once there are more");
	/* Back to the sea: the ring ends (the scan's own end: no "off"), the scan waits */
	place_with(0);
	run("wait:31000");
	expect("dual=0");
	check(ui_autoscan_on() == AS_RPT && air_sim.end_calls == 1, "the ring ended, the scan still on");
}

/* No GPS: refused; no fix yet: on, waiting; the fix: the ring */
static void as_gps(void)
{
	start("AS GPS off, no fix");
	air_sim.gps = AIR_GPS_OFF;
	ui_autoscan_toggle(AS_RPT);
	expect(T("toast=GPS_is_off", "toast=GPS_kapal"));
	check(ui_autoscan_on() == AS_OFF, "refused without GPS");
	air_sim.gps = AIR_GPS_NOFIX;
	ui_autoscan_toggle(AS_RPT);
	run("wait:500");
	expect(T("dual=0 toast=Repeater_scan_on:_waiting_for_GPS", "dual=0 toast=R"));
	run("wait:3000");
	check(ui_autoscan_on() == AS_RPT && air_sim.vfo_calls == 0, "waiting");
	gps(39.9208, 32.8541);
	run("wait:1500");
	expect("dual=1");
	/* The same tool again: off */
	ui_autoscan_toggle(AS_RPT);
	run("wait:300");
	expect(T("dual=0 toast=Repeater_scan_off", "dual=0 toast=R"));
}

/* Air scan at Antalya: the airfield's best frequencies round robin, no ATIS (118.275), labels "LTAI TWR" */
static void as_air(void)
{
	start("AS air scan");
	gps(36.8969, 30.7133);
	ui_autoscan_toggle(AS_AIR);
	run("wait:1500");
	expect(T("screen=home dual=1 mwn=6 toast=Air_scan_on shows=LTAI_TWR", "screen=home dual=1 mwn=6 shows=LTAI_TWR"));
	check(air_sim.vfo_calls == 6, "six letters");
	for (int k = 0; k < 6; k++)
		check(air_sim.hz[k] != 118275000 && (air_sim.arg[k] & (UI_AIR_FM | UI_AIR_DMR | UI_AIR_ATIS)) == 0,
		      "AM letters, no ATIS");
	expect("shows=AM_125.000 hides=118.27");
	run("shot:home-airscan");
	layout();
	run("back back wait:300");
	expect(T("dual=0 toast=Air_scan_off", "dual=0 toast=Hava_tarama_kapal"));
}

/* Review M3: DOTA (or cross-band) takes the radio: the radio core ends the air state and counts the stop; the scan is
 * off, the user's state back, nothing asks for the ring again */
static void as_session(void)
{
	start("AS DOTA takes the radio");
	gps(39.9208, 32.8541);
	ui_autoscan_toggle(AS_RPT);
	run("wait:1500");
	expect("dual=1");
	int calls = air_sim.vfo_calls;
	air_sim_session();
	run("wait:1500");
	expect(T("dual=0 toast=Repeater_scan_off", "dual=0 toast=R"));
	check(ui_autoscan_on() == AS_OFF, "the scan off");
	gps(39.9208 + 0.3, 32.8541);
	run("wait:35000");
	check(air_sim.vfo_calls == calls, "nothing asked for again");
	expect("dual=0");
}

/* A new map pack mid-scan: everything again; one without repeaters: off */
static void as_pack(void)
{
	start("AS map pack changes");
	gps(39.9208, 32.8541);
	ui_autoscan_toggle(AS_RPT);
	run("wait:1500");
	int calls = air_sim.vfo_calls;
	check(calls >= 3, "a ring");
	check(refpack_fixture_install(REFPACK_ALL), "a new pack");
	run("wait:1500");
	check(air_sim.vfo_calls == 2 * calls, "every letter again");
	expect("dual=1");
	check(refpack_fixture_install(REFPACK_MAP | REFPACK_AIR), "a pack without repeaters");
	run("wait:1500");
	expect(T("dual=0 toast=No_repeater_data", "dual=0 toast=R"));
	check(ui_autoscan_on() == AS_OFF, "off");
}
#endif

typedef void (*case_fn)(void);
static const case_fn all[] = {
	af_more, af_one, af_none,
#if CONFIG_AIR_WATCH
	aw_watch, aw_four, aw_stop, aw_user_ring, aw_pair, aw_one, aw_refused, aw_reboot,
#endif
#if CONFIG_AUTO_SCAN
	as_rpt, as_move, as_retry, as_few, as_gps, as_air, as_pack, as_session,
#endif
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
	printf("Plus air UI: %d cases (English and Turkish), %d failed\n", cases, bad);
	return bad != 0;
}
