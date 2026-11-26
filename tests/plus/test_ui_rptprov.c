/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Repeaters by province (CONFIG_RPT_PROV, docs/plus.md "Repeater list and nearest repeater") through
 * the real UI in upstream's simulator, wrapped by tests/plus/ui_sim_mapnav.c (the test's GPS position, the tune path's
 * radio requests recorded), on the DM-32UV's display class (240 x 320, insets 10 x 5) with its key map. Built like
 * tests/plus/test_ui_mapnav.c plus -DCONFIG_RPT_PROV=1 (tools/test_rptprov.py). Each case runs in a child process.
 * Covers: the first level (All, the provinces with repeaters in plate order with their counts, Unknown last, no empty
 * province), every province label fitting a row; a province's list (only its repeaters, nearest first or by
 * frequency, Up / Down round within it, # its nearest), details and tune from it; All as before; Back from details
 * to the province's list, to the first level, out to Tools; # on the first level; the map's pick -> details -> Back
 * to the map, details stepping through the whole list. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_screens.h"
#include "ui/gfx.h"
#include "ui/map.h"
#include "ui/rpt.h"
#include "ui_sim_mapnav.h"

/* Sorted by RX as the generator makes them; the province as it would give it */
#define P(la, lo) (int32_t)((la) * 1e5 + 0.5), (int32_t)((lo) * 1e5 + 0.5)
const rpt_entry_t rpt_table[] = {
	{ P(39.93, 32.86), 145600000, -480, 885, RPT_FM, 0, "TA2RAK", "Ankara", 6 },
	{ P(37.00, 35.32), 145612500, -480, 0, RPT_FM, 0, "TA5ADA", "Adana", 1 },
	{ P(39.80, 32.60), 145625000, -480, 0, RPT_FM, 0, "TA2RGB", "Golbasi", 6 },
	{ P(37.03, 35.81), 145650000, -480, 1318, RPT_FM, 0, "TA5CEY", "Ceyhan", 1 },
	{ P(39.94, 32.87), 145675000, -480, 0, RPT_C4FM, 0, "TA2YSF", "Ankara", 6 },
	{ P(41.01, 28.97), 439000000, -6080, 0, RPT_DMR, 1 | 3 << 4, "TA1IST", "Istanbul", 34 },
	{ P(39.92, 32.85), 439100000, -6080, 0, RPT_DMR, 1 | 3 << 4, "TA2DMR", "Ankara", 6 },
	{ RPT_NOPOS, RPT_NOPOS, 439150000, -6080, 0, RPT_DMR, 1, "TA9XXX", "Nowhere", RPT_PROV_UNKNOWN },
	{ P(39.40, 32.85), 439200000, -6080, 0, RPT_DMR, 2 | 2 << 4, "TA2DMS", "Haymana", 6 },
};
const rpt_info_t rpt_info = { sizeof rpt_table / sizeof rpt_table[0], 0, "2026-11-19", "test" };

#define TOOLS	"ok 6 "
#define MAP	TOOLS "down down down down down down down down down ok "		/* Tools > Map */

static int fails;
static const char *name;
static char rpt_open[200];	/* the script to Tools > Repeaters, found by its focus label */

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

/* Start over: Home on the simulator's DMR channel, the GPS in Ankara, no APRS stations */
static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	const char *shots = getenv("RPTPROV_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, "rptprov-");
	mapnav_sim_clear();
	gps(true, 39.93, 32.86);
	mapnav_sim.stations = true;
}

/* The Tools row of Repeaters: Down until the focus says so (the tools list depends on the build's switches) */
static bool find_tools_row(void)
{
	char err[300], script[sizeof rpt_open + 32];
	strcpy(rpt_open, TOOLS);
	for (int k = 0; k < 40; k++) {
		snprintf(script, sizeof script, "%sexpect:focus=Repeaters", rpt_open);
		start("find the Tools row");
		if (sim_run(script, err, sizeof err)) {
			strcat(rpt_open, "ok ");
			return true;
		}
		strcat(rpt_open, "down ");
	}
	return false;
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

/* --- The first level --- */

static void pv_list(void)
{
	ok_run(rpt_open);
	expect("screen=repeaters shows=All_repeaters_(9) shows=01_Adana_(2) shows=06_Ankara_(5) shows=34_ "
	       "shows=Unknown_(1) shows=OK_list hides=Nearest_first hides=TA2RAK hides=02_ hides=(0)");
	ok_run("shot:provinces");
	/* the order: All, 01, 06, 34, Unknown, round to All (each row opened by its place) */
	ok_run("ok");
	expect("shows=Nearest_first shows=TA2RAK shows=TA2YSF");
	ok_run("back down ok");
	expect("shows=Adana shows=TA5ADA hides=TA2RAK");
	ok_run("back down ok");
	expect("shows=Ankara shows=TA2RAK hides=TA5ADA hides=TA1IST");
	ok_run("back down ok");
	expect("shows=TA1IST hides=TA2RAK hides=TA5ADA");
	ok_run("back down ok");
	expect("shows=Unknown shows=TA9XXX hides=TA2RAK hides=TA1IST");
	ok_run("back down ok");	/* round: All again */
	expect("shows=Nearest_first shows=TA2RAK");
	ok_run("back up ok");	/* and back round to Unknown */
	expect("shows=TA9XXX hides=TA1IST");
}

/* Every province's row fits in the list's font as "81 Name (123)" */
static void pv_widths(void)
{
	char buf[48];
	int w = gfx_width();
	for (uint8_t p = 0; p < RPT_PROVS; p++) {
		{
			char nm[MAP_NAME_MAX];
			map_province_name(p, nm, sizeof nm);
			snprintf(buf, sizeof buf, "%02u %s (123)", (unsigned)map_province_plate(p), nm);
		}
		if (gfx_text_width(ui_layout.small, buf) > w - 2) {
			printf("FAIL %s: \"%s\" is %d px, the row %d\n", name, buf, gfx_text_width(ui_layout.small, buf), w - 2);
			fails++;
		}
	}
	check(map_province_plate(0) == 1 && map_province_plate(80) == 81, "the map's names are by plate");
}

/* --- A province's list --- */

static void pv_adana(void)
{
	ok_run(rpt_open);
	ok_run("down ok shot:adana");
	expect("screen=repeaters shows=Adana shows=Nearest_first shows=TA5ADA shows=TA5CEY hides=TA2RAK hides=TA1IST "
	       "hides=TA9XXX");
	/* nearest first from Ankara: TA5ADA, then TA5CEY; round within Adana */
	ok_run("ok");
	expect("shows=TA5ADA__FM");
	ok_run("down");
	expect("shows=TA5CEY__FM");
	ok_run("down");
	expect("shows=TA5ADA__FM");
	ok_run("up");
	expect("shows=TA5CEY__FM");
	/* # there: Adana's nearest, not Ankara's */
	ok_run("back ctx");
	expect("shows=TA5ADA__FM hides=TA2RAK");
	/* from Ceyhan, Ceyhan is nearest */
	gps(true, 37.03, 35.81);
	ok_run("back ctx");
	expect("shows=TA5CEY__FM");
}

static void pv_no_position(void)
{
	gps(false, 0, 0);
	ok_run(rpt_open);
	expect("shows=06_Ankara_(5)");
	ok_run("down down ok");
	expect("shows=No_position:_sorted_by_frequency shows=TA2RAK shows=TA2DMS hides=TA5ADA hides=TA9XXX");
	/* by frequency: TA2RAK first, TA2DMS last; round */
	ok_run("ok");
	expect("shows=TA2RAK__FM");
	ok_run("up");
	expect("shows=TA2DMS__DMR");
	ok_run("down down");
	expect("shows=TA2RGB__FM");
}

static void pv_tune(void)
{
	mapnav_sim_mode(RADIO_MODE_FM);
	ok_run(rpt_open);
	ok_run("down ok");	/* Adana */
	ok_run("down ok");	/* TA5CEY's details */
	expect("shows=TA5CEY__FM shows=OK:_tune_the_VFO");
	check(mapnav_sim.req[0] == 0, "nothing tuned before OK");
	ok_run("ok");
	expect("screen=home toast=VFO_tuned_to_the_repeater vfo=1 freq=145650000 txfreq=145050000 mode=fm");
	const char *want = "vfo_set:145650000 mode_fm:1 bw:25 tone_tx:1318 tone_rx:0 vfo_tx:145050000";
	check(strcmp(mapnav_sim.req, want) == 0, "the FM tune requests in order");
	if (strcmp(mapnav_sim.req, want))
		printf("  requests: %s\n", mapnav_sim.req);
}

static void pv_unknown(void)
{
	ok_run(rpt_open);
	ok_run("up ok");	/* Unknown: the last row */
	expect("shows=TA9XXX hides=TA2RAK");
	ok_run("ok");
	expect("shows=TA9XXX__DMR shows=Its_position_is_not_known");
}

/* --- All, as before --- */

static void pv_all(void)
{
	ok_run(rpt_open);
	ok_run("ok");
	expect("shows=Nearest_first shows=TA2RAK shows=OK_info");
	/* nearest first from Ankara: TA2RAK, TA2YSF, TA2DMR, ... every entry once, round */
	ok_run("ok");
	expect("shows=TA2RAK__FM");
	ok_run("back down ok");
	expect("shows=TA2YSF__C4FM");
	ok_run("back up up ok");
	expect("shows=TA9XXX__DMR");	/* no position: last */
	ok_run("back down ok");
	expect("shows=TA2RAK__FM");
	ok_run("back ctx");
	expect("shows=TA2RAK__FM");
}

/* --- Back --- */

static void pv_back(void)
{
	ok_run(rpt_open);
	ok_run("down down ok ok");	/* Ankara, TA2RAK's details */
	expect("shows=TA2RAK__FM");
	ok_run("back");
	expect("screen=repeaters shows=Ankara shows=Nearest_first shows=TA2DMS");
	ok_run("back");
	expect("screen=repeaters shows=06_Ankara_(5) shows=All_repeaters hides=Nearest_first");
	ok_run("ok");	/* the focus stayed on Ankara */
	expect("shows=Ankara shows=TA2DMS hides=TA5ADA");
	ok_run("back back");
	expect("screen=tools");
	/* opened again: the first level from the top (All) */
	ok_run("ok");
	expect("screen=repeaters shows=All_repeaters");
	ok_run("ok");
	expect("shows=Nearest_first shows=TA2RAK shows=TA2YSF");
}

/* # on the first level: the nearest one's details in All; Back: All's list, Back: the first level */
static void pv_hash(void)
{
	ok_run(rpt_open);
	ok_run("down down down ctx");
	expect("shows=TA2RAK__FM shows=OK:_tune_the_VFO");
	ok_run("back");
	expect("shows=Nearest_first shows=TA2RAK shows=TA2YSF");
	ok_run("back");
	expect("shows=All_repeaters_(9)");
	ok_run("ok");
	expect("shows=TA2YSF shows=Nearest_first");
}

/* --- From the map --- */

static void pv_map(void)
{
	ok_run(MAP "ctx");
	expect("screen=map shows=TA2RAK_145.600");
	ok_run("ok");
	expect("screen=repeaters shows=TA2RAK__FM hides=All_repeaters");
	ok_run("down");	/* the whole list's order, as before */
	expect("shows=TA2YSF__C4FM");
	ok_run("back");
	expect("screen=map");
	ok_run("ok");	/* the pick is still TA2RAK */
	expect("screen=repeaters shows=TA2RAK__FM");
	ok_run("ok");	/* tune */
	expect("screen=home toast=VFO_tuned_to_the_repeater freq=145600000");
}

int main(void)
{
	static const struct { const char *name; case_fn fn; } cases[] = {
		{ "RP the first level", pv_list },
		{ "RP province labels fit", pv_widths },
		{ "RP Adana only, nearest first, #", pv_adana },
		{ "RP no position: by frequency", pv_no_position },
		{ "RP tune from a province", pv_tune },
		{ "RP unknown group", pv_unknown },
		{ "RP All as before", pv_all },
		{ "RP Back", pv_back },
		{ "RP # on the first level", pv_hash },
		{ "RP map pick, details, Back", pv_map },
	};
	if (!find_tools_row()) {
		printf("FAIL no Repeaters row in Tools\n");
		return 1;
	}
	int failed = 0;
	unsigned n = sizeof cases / sizeof cases[0];
	for (unsigned i = 0; i < n; i++)
		failed += run_case(cases[i].name, cases[i].fn);
	printf("Plus repeaters by province UI: %u flows, %d failures\n", n, failed);
	return failed != 0;
}
