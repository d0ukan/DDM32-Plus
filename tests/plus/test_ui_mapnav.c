/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus map navigation (docs/map.md "APRS stations, auto repeater, track") through the real UI in upstream's
 * simulator (tests/plus/ui_sim_mapnav.c wraps it: the test's GPS position and APRS stations, the radio requests
 * recorded), on the DM-32UV's display class (240 x 320, insets 10 x 5) with its key map. Built like
 * tests/classic/test_ui.c with -DCONFIG_MAP=1 -DCONFIG_NAV=1 -DCONFIG_REPEATERS=1 -DCONFIG_AUTO_RPT=1
 * -DCONFIG_MAP_TRACK=1 -DCONFIG_APRS_MSG=1 and the map, Go-To, Repeaters and APRS message screens. Each case runs in
 * a child process (the screens' static state starts over). Covers:
 *   - Tools > Auto repeater: the row, on / off and its toasts (no fix: waiting for GPS);
 *   - the offer: a banner at once when turned on (DMR on a DMR channel, FM on FM), # tunes through Tools >
 *     Repeaters' tune (the request sequence, the VFO's frequencies, the toast, Home), Back hides it and nothing is
 *     tuned, it is never offered again while we stay (2 minutes), a clearly nearer one after moving, none for one
 *     barely nearer, none while another banner is up or while transmitting (it waits), nothing tuned without #;
 *   - APRS stations on the map: 0 picks the nearest the centre, again the next, round; the line under the map
 *     (callsign, distance, direction, age); OK the details (symbol, altitude, comment); OK again the message composer
 *     to it, and a canned message sent to that callsign; Back steps out; # switches to repeaters and 0 back; no
 *     station; a station that left the list;
 *   - the track: points while moving, "Back to start" (9) saves START once and opens Go-To on it, 7 / 7 held. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/mapnav.h"
#include "ui/nav.h"
#include "ui/rpt.h"
#include "ui_sim_mapnav.h"

/* The repeaters (sorted by RX as the generator makes them) */
#define P(la, lo) (int32_t)((la) * 1e5 + 0.5), (int32_t)((lo) * 1e5 + 0.5)
const rpt_entry_t rpt_table[] = {
	{ P(39.93, 32.86), 145600000, -480, 885, RPT_FM, 0, "TA2RAK", "Ankara", 6 },
	{ P(39.80, 32.60), 145625000, -480, 0, RPT_FM, 0, "TA2RGB", "Golbasi", 6 },
	{ P(39.94, 32.87), 145675000, -480, 0, RPT_C4FM, 0, "TA2YSF", "Ankara", 6 },
	{ P(39.92, 32.85), 439100000, -6080, 0, RPT_DMR, 1 | 3 << 4, "TA2DMR", "Ankara", 6 },
	{ P(39.40, 32.85), 439200000, -6080, 0, RPT_DMR, 2 | 2 << 4, "TA2DMS", "Haymana", 6 },
};
const rpt_info_t rpt_info = { sizeof rpt_table / sizeof rpt_table[0], 0, "2026-11-19", "test" };

#define TOOLS	"ok 6 "
#define AUTO	TOOLS "down down down down down down down down down down down "	/* the Auto repeater row */
#define MAP	TOOLS "down down down down down down down down down ok "		/* Tools > Map */

static int fails;
static const char *name;

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s: %s: %s\n", name, script, err);
		fails++;
	}
}

/* "a=b c=d": every check after the script, as tests/host/uiFlowTest.c's run_flow */
static void expect(const char *e)
{
	char buf[400], *o = buf;
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

static void gps(bool fix, double la, double lo)
{
	mapnav_sim.gps = true;
	mapnav_sim.fix = fix;
	mapnav_sim.lat_e7 = (int32_t)(la * 1e7 + (la < 0 ? -0.5 : 0.5));
	mapnav_sim.lon_e7 = (int32_t)(lo * 1e7 + (lo < 0 ? -0.5 : 0.5));
}

static void station(const char *call, double la, double lo, const char *text, int16_t alt)
{
	aprs_station_t *s = &mapnav_sim.st[mapnav_sim.nst++];
	memset(s, 0, sizeof *s);
	strcpy(s->call, call);
	s->has_pos = true;
	s->lat = (int32_t)(la * 1e7 + 0.5);
	s->lon = (int32_t)(lo * 1e7 + 0.5);
	s->sym_table = '/';
	s->sym_code = '>';
	strcpy(s->text, text);
	s->alt_m = alt;
	s->heard_ms = 1000;
	s->direct = true;
}

/* Start over: Home on the simulator's DMR channel, the GPS in Ankara, three stations heard */
static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	const char *shots = getenv("MAPNAV_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, "mapnav-");
	mapnav_sim_clear();
	gps(true, 39.93, 32.86);
	mapnav_sim.stations = true;
	station("TA2XYZ-7", 40.20, 32.86, "north", MAPNAV_ALT_NONE);
	station("TA1ABC-9", 39.95, 32.88, "Hello /A=001234 mobile", 376);
	station("YM2ANK", 39.93, 33.50, "Digi", 1200);
}

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

/* --- Tools > Auto repeater --- */

static void ar_row(void)
{
	ok_run(AUTO);
	expect("screen=tools focus=Auto_repeater shows=Off banner=none");
}

static void ar_no_fix(void)
{
	gps(false, 0, 0);
	ok_run(AUTO "ok");
	expect("toast=Auto_repeater_on:_waiting_for_GPS banner=none");
	ok_run("wait:3000");
	expect("shows=On banner=none");
	ok_run("wait:120000");
	expect("banner=none");
	check(mapnav_sim.req[0] == 0, "nothing tuned");
	/* the fix comes: the offer follows within a check */
	gps(true, 39.93, 32.86);
	ok_run("wait:32000");
	expect("banner=Nearest:_TA2DMR_439.100");
}

static void ar_off(void)
{
	ok_run(AUTO "ok wait:1100");
	expect("banner=Nearest:_TA2DMR");
	ok_run("ok");	/* off: the offer still up goes too */
	expect("toast=Auto_repeater_off banner=none");
	ok_run("wait:3000");
	expect("shows=Off");
	ok_run("wait:120000");
	expect("banner=none");
}

/* On a DMR channel: the nearest DMR repeater at once; # tunes it as Tools > Repeaters' OK does */
static void ar_dmr_tune(void)
{
	ok_run(AUTO "ok wait:1100 long:back shot:offer");
	expect("banner=Nearest:_TA2DMR_439.100_1.4_km_SW shows=#_tunes_the_VFO");
	check(mapnav_sim.req[0] == 0, "nothing tuned before #");
	ok_run("ctx");
	expect("screen=home toast=VFO_tuned_to_the_repeater banner=none vfo=1 freq=439100000 txfreq=431500000 mode=dmr "
	       "cc=1 ts=1");
	check(strcmp(mapnav_sim.req, "vfo_set:439100000 mode_dmr:1 cc:1 ts:1 vfo_tx:431500000") == 0,
	      "the DMR tune requests in order");
	if (strcmp(mapnav_sim.req, "vfo_set:439100000 mode_dmr:1 cc:1 ts:1 vfo_tx:431500000"))
		printf("  requests: %s\n", mapnav_sim.req);
	/* tuned there: no offer again */
	ok_run("wait:120000");
	expect("banner=none");
}

/* On FM: the nearest FM repeater (not the C4FM one), tuned with its tone and offset */
static void ar_fm_tune(void)
{
	mapnav_sim_mode(RADIO_MODE_FM);
	ok_run(AUTO "ok wait:1100");
	expect("banner=Nearest:_TA2RAK_145.600");
	ok_run("ctx");
	expect("screen=home toast=VFO_tuned_to_the_repeater freq=145600000 txfreq=145000000 mode=fm");
	const char *want = "vfo_set:145600000 mode_fm:1 bw:25 tone_tx:885 tone_rx:0 vfo_tx:145000000";
	check(strcmp(mapnav_sim.req, want) == 0, "the FM tune requests in order");
	if (strcmp(mapnav_sim.req, want))
		printf("  requests: %s\n", mapnav_sim.req);
}

/* Back hides it, nothing tuned; staying, never again; it expires by itself (8 s) */
static void ar_no_spam(void)
{
	mapnav_sim_mode(RADIO_MODE_FM);
	ok_run(AUTO "ok wait:1100 long:back");
	expect("screen=home banner=Nearest:_TA2RAK");
	ok_run("back");
	expect("banner=none");
	for (int i = 0; i < 10; i++) {
		ok_run("wait:30000");
		expect("banner=none");
	}
	check(mapnav_sim.req[0] == 0, "nothing tuned without #");
	expect("vfo=0");
	/* a second time on: offered once more, then it expires by itself */
	ok_run(AUTO "ok ok wait:1100 long:back");
	expect("banner=Nearest:_TA2RAK");
	ok_run("wait:8500");
	expect("banner=none");
	ok_run("wait:60000");
	expect("banner=none");
	check(mapnav_sim.req[0] == 0, "still nothing tuned");
}

/* Moving: barely nearer is not offered, clearly nearer is (and # tunes that one) */
static void ar_moving(void)
{
	mapnav_sim_mode(RADIO_MODE_FM);
	ok_run(AUTO "ok wait:1100 long:back back");
	expect("banner=none");
	gps(true, 39.862, 32.725);	/* TA2RGB 12.7 km, TA2RAK 13.8 km */
	ok_run("wait:31000");
	expect("banner=none");
	gps(true, 39.83, 32.66);	/* 6 against 20 km */
	ok_run("wait:31000");
	expect("banner=Nearer:_TA2RGB_145.625");
	ok_run("ctx");
	expect("freq=145625000 txfreq=145025000");
	check(strstr(mapnav_sim.req, "vfo_set:145625000") == mapnav_sim.req && strstr(mapnav_sim.req, "tone_tx:0"),
	      "TA2RGB tuned without a tone");
	/* back half way: no offer back (hysteresis) */
	gps(true, 39.866, 32.73);
	ok_run("wait:31000 wait:31000");
	expect("banner=none");
}

/* Another banner up: the offer waits for it to go; never during TX */
static void ar_waits(void)
{
	ok_run("msg:3101234,Hello_there");
	expect("banner=*");
	ok_run(AUTO "ok");
	expect("toast=Auto_repeater_on");
	check(strncmp(mapnav_sim.req, "", 1) == 0, "nothing tuned");
	ok_run("wait:9000");
	expect("banner=Nearest:_TA2DMR");
}

static void ar_tx(void)
{
	ok_run(AUTO "pttdown wait:300 ok wait:3000");
	expect("banner=none");
	ok_run("pttup wait:1500");
	expect("banner=Nearest:_TA2DMR");
}

/* --- APRS stations on the map --- */

static void st_pick(void)
{
	ok_run(MAP);
	expect("screen=map shows=#_repeater shows=0_APRS shows=06_Ankar");
	ok_run("0 shot:station");
	expect("screen=map shows=TA1ABC-9_2.8_km_NE shows=OK_details hides=06_Ankar");
	ok_run("0");
	expect("shows=TA2XYZ-7_30");
	ok_run("0");
	expect("shows=YM2ANK_5");
	ok_run("0");	/* round to the nearest */
	expect("shows=TA1ABC-9_2.8_km_NE");
}

/* --- T10 / S4 (docs/mappack-design.md C.4): the map layer is read from the chip only when something drawn inside the
 * map rectangle changed; a radio update alone repaints the top strip and the lines under the map --- */
void ui_map_rect(int *y, int *h);	/* ui_map.c, host builds */
void ui_toast(const char *text);	/* ui_nav.h */

static uint32_t draws_now(void)
{
	ui_app_status_t s;
	ui_app_status(&s);
	return s.draws;
}

static void map_cheap(void)
{
	int y, h;
	ok_run(MAP "wait:1500");
	expect("screen=map shows=06_Ankar");
	ui_map_rect(&y, &h);
	mapnav_sim_count_reads();
	ok_run("wait:1100");
	uint32_t hash = mapnav_sim_rows_hash(y, h), d0 = draws_now(), r0 = mapnav_sim_reads();
	for (int i = 0; i < 20; i++) {
		mapnav_sim_radio_update(-60 - i);	/* RSSI, a new view serial: a frame each */
		ok_run("wait:60");
	}
	check(draws_now() >= d0 + 20u, "a frame for each radio update");
	check(mapnav_sim_reads() == r0, "20 radio updates read the pack zero times");
	check(mapnav_sim_rows_hash(y, h) == hash, "the map rectangle byte-identical after 20 cheap frames");
	expect("screen=map shows=06_Ankar");		/* the line under the map still there */
	/* a toast up, and gone: a full frame each time */
	r0 = mapnav_sim_reads();
	ui_toast("Test toast");
	ok_run("wait:60");
	check(mapnav_sim_reads() > r0, "a toast forces a full frame");
	ok_run("wait:6000");
	r0 = mapnav_sim_reads();
	mapnav_sim_radio_update(-80);
	ok_run("wait:60");
	check(mapnav_sim_reads() == r0, "cheap again once the toast went (its going drew whole)");
	/* another screen on top, then back: the map draws whole */
	ok_run("ctx ok");
	expect("screen=repeaters");
	r0 = mapnav_sim_reads();
	ok_run("back wait:60");
	expect("screen=map");
	check(mapnav_sim_reads() > r0, "back from the screen on top: a full frame");
	ok_run("back wait:60");			/* the pick dropped */
	/* our position a few map pixels on (follow is on: the view moves with it): a full frame */
	ok_run("wait:1100");
	r0 = mapnav_sim_reads();
	gps(true, 39.99, 32.95);
	ok_run("wait:1100");
	check(mapnav_sim_reads() > r0, "our position moved: a full frame");
	/* a new APRS station in view: a full frame */
	ok_run("wait:1100");
	r0 = mapnav_sim_reads();
	station("TA3NEW-1", 39.94, 32.90, "new", MAPNAV_ALT_NONE);
	ok_run("wait:1100");
	check(mapnav_sim_reads() > r0, "a new station mark: a full frame");
}

static void st_details_message(void)
{
	ok_run(MAP "3 3 3 0 ok shot:details");
	expect("shows=/>_376_m_Hello_mobile shows=OK_message hides=TA1ABC-9_2.8");
	ok_run("ok");
	expect("screen=aprs-compose shows=TA1ABC-9");
	ok_run("back");
	expect("screen=map shows=/>_376_m");
	/* a canned text sent to it */
	ok_run("ok 3 ok 4");
	expect("toast=Sending_message");
	check(mapnav_sim.messages == 1 && strcmp(mapnav_sim.msg_to, "TA1ABC-9") == 0 && mapnav_sim.msg_text[0] != 0,
	      "the message to the station");
	check(mapnav_sim.req[0] == 0, "no radio request");
}

static void st_back(void)
{
	ok_run(MAP "0 ok back");
	expect("screen=map shows=TA1ABC-9_2.8_km_NE");
	ok_run("back");
	expect("screen=map shows=06_Ankar");
	ok_run("back");
	expect("screen=tools");
}

static void st_switch(void)
{
	ok_run(MAP "0 ctx shot:repeater");
	expect("shows=TA2RAK_145.600 hides=TA1ABC-9_2.8");
	ok_run("0");
	expect("shows=TA1ABC-9_2.8_km_NE");
	ok_run("ctx ok");
	expect("screen=repeaters shows=TA2RAK");
}

static void st_none(void)
{
	mapnav_sim.nst = 0;
	ok_run(MAP "0");
	expect("toast=No_APRS_station_with_a_position");
	/* only a status heard (no position): the same */
	station("TB3A", 0, 0, "status", MAPNAV_ALT_NONE);
	mapnav_sim.st[0].has_pos = false;
	ok_run("wait:3000 0");
	expect("toast=No_APRS_station_with_a_position");
}

static void st_gone(void)
{
	ok_run(MAP "0");
	expect("shows=TA1ABC-9_2.8");
	mapnav_sim.st[1] = mapnav_sim.st[2];	/* TA1ABC-9 dropped from the list */
	mapnav_sim.nst = 2;
	ok_run("3 1");	/* redraw */
	expect("shows=06_Ankar hides=TA1ABC-9");
}

/* --- Track --- */

static void tr_back_to_start(void)
{
	ok_run("wait:2000");
	check(track_count(&ui_mapnav_track) == 1, "the first fix kept at once");
	for (int i = 1; i <= 5; i++) {
		gps(true, 39.93 + 0.005 * i, 32.86);
		ok_run("wait:31000");
	}
	check(track_count(&ui_mapnav_track) == 6, "a point every 30 s while moving");
	gps(true, 39.93 + 0.025, 32.86);
	ok_run("wait:120000");
	check(track_count(&ui_mapnav_track) == 6, "standing: no more points");
	ok_run(MAP "3 3 3 3 3 3 shot:track 9 shot:start");
	expect("screen=goto shows=START toast=Start_saved_as_a_waypoint");
	nav_point_t wp;
	check(nav_count() == 1 && nav_get(0, &wp) && wp.lat_e7 == 399300000 && wp.lon_e7 == 328600000,
	      "START is the first fix, exactly");
	ok_run("back 9");
	expect("screen=goto shows=START");
	check(nav_count() == 1, "START saved once");
	ok_run("back 7");
	expect("screen=map toast=Hold_7_to_clear_the_track");
	check(track_count(&ui_mapnav_track) == 6, "7 alone keeps it");
	gps(false, 0, 0);	/* else the next fix starts a new track at once */
	ok_run("long:7");
	expect("screen=map toast=Track_cleared");
	check(track_count(&ui_mapnav_track) == 0, "cleared");
	ok_run("wait:3000 9");
	expect("screen=map toast=No_track_yet");
}

int main(void)
{
	static const struct { const char *name; case_fn fn; } cases[] = {
		{ "AR the Tools row", ar_row },
		{ "AR on without a fix", ar_no_fix },
		{ "AR off", ar_off },
		{ "AR DMR offer and tune", ar_dmr_tune },
		{ "AR FM offer and tune", ar_fm_tune },
		{ "AR no prompt spam", ar_no_spam },
		{ "AR moving, hysteresis", ar_moving },
		{ "AR waits for a banner", ar_waits },
		{ "AR waits for TX", ar_tx },
		{ "MAP APRS station pick", st_pick },
		{ "MAP cheap frames (T10)", map_cheap },
		{ "MAP APRS details and message", st_details_message },
		{ "MAP APRS Back", st_back },
		{ "MAP APRS # and 0", st_switch },
		{ "MAP APRS none", st_none },
		{ "MAP APRS station gone", st_gone },
		{ "MAP track and back to start", tr_back_to_start },
	};
	int failed = 0;
	unsigned n = sizeof cases / sizeof cases[0];
	for (unsigned i = 0; i < n; i++)
		failed += run_case(cases[i].name, cases[i].fn);
	printf("Plus map navigation UI: %u flows, %d failures\n", n, failed);
	return failed != 0;
}
