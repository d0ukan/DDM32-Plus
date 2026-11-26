/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 16, Tools > Airfields by province (CONFIG_AIR_PROV, docs/plus.md "Round 16: airfields by province,
 * spectrum in colour") through the real UI in upstream's simulator, wrapped by tests/plus/ui_sim_dpk.c (a radio that
 * receives AM, the test's position, the radio core's air watch, a data chip for the air band pack), with the shipped
 * airfield table and the map's province names, on the DM-32UV's display class (240 x 320, insets 10 x 5), in English
 * and Turkish. Built by tools/test_airprov.py. Each case runs in a child process. Covers: the first level (All, the
 * provinces that have airfields in plate order with their counts, none empty), every province label fitting a row; a
 * province's list (only its airfields, nearest first or by code, Up / Down round within it, # its nearest), "+N", the
 * details, listening and the air watch (#) from it; the air band pack's frequencies in a province's list; All as
 * before; Back from the details to the province's list, to the first level (its focus kept), out; # on the first
 * level; every page fits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/ui_screens.h"
#include "ui/gfx.h"
#include "ui/i18n.h"
#include "ui/map.h"
#include "ui/airfield.h"
#include "storage/storage.h"
#include "storage/datapack.h"
#include "sim/sim_flash.h"
#include "ui_sim_dpk.h"

/* Tools > Map's screen: map.c is linked for the province names only (the map itself is not built here) */
const ui_scr_def_t ui_map_screen = { .name = "map" };

static int fails;
static const char *name;
static bool turkish;

/* English or Turkish (Turkish: an ASCII part of the translation, as the screen draws it) */
#define T(en, tr) (turkish ? (tr) : (en))

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s (%s): %s\n", name, turkish ? "tr" : "en", what);
		fails++;
	}
}

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
	char buf[700], *o = buf;
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
	const char *shots = getenv("AIRPROV_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "airprov-tr-" : "airprov-en-");
	dpk_sim_setup();
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	pos(39.9208, 32.8541);		/* Ankara, Kizilay */
	run("wait:200");
}

/* Tools > Airfields as the Tools row opens it */
static void airfields(void)
{
	ui_push(SCR_AIRFIELDS, 0);
	run("wait:100");
}

/* The first level's row of plate p (0: All): Down from the top that many times, then OK */
static void open_group(uint8_t plate)
{
	char script[400] = "";
	uint8_t row = 0;
	if (plate != 0) {
		/* rows: All, then each plate that has airfields, in order */
		for (uint8_t p = 1; p < plate; p++)
			row = (uint8_t)(row + (air_count_in(p) != 0));
		row++;
	}
	for (uint8_t k = 0; k < row; k++)
		strcat(script, "down ");
	strcat(script, "ok");
	run(script);
}

/* --- The first level --- */

static void pv_list(void)
{
	start("AP the first level");
	airfields();
	char e[200];
	snprintf(e, sizeof e, "screen=airfields shows=%s_(%u) shows=01_Adana_(%u) shows=06_Ankara_(%u)",
		 T("All_airfields", "Hepsi"), (unsigned)air_field_count, (unsigned)air_count_in(1), (unsigned)air_count_in(6));
	expect(e);
	expect(T("shows=OK_list hides=Nearest_first hides=LTAB hides=08_ hides=09_ hides=(0) hides=Unknown",
		 "hides=En_yak hides=LTAB hides=08_ hides=09_ hides=(0)"));
	run("shot:provinces");
	layout();
	/* the order: All, 01 Adana, 02 Adiyaman, ...: each row opened by its place */
	run("ok");
	expect(T("shows=Nearest_first shows=LTAB shows=LTAD", "shows=En_yak shows=LTAB shows=LTAD"));
	run("back down ok");
	expect("shows=Adana shows=LTAF hides=LTAB hides=LTDB");
	run("back down ok");
	expect("shows=LTCP hides=LTAF");	/* 02 Adiyaman */
	run("back up up up ok");		/* Adiyaman, Adana, All, round to the last row: the highest plate with airfields */
	uint8_t last = 0;
	for (uint8_t p = 1; p <= AIR_PROVS; p++)
		if (air_count_in(p) != 0)
			last = p;
	check(last == 76, "the highest plate with airfields: 76 Igdir");
	expect("shows=LTCT");
	layout();
}

/* Every province's row fits in the list's font as "81 Name (123)" */
static void pv_widths(void)
{
	start("AP province labels fit");
	char buf[48];
	int w = gfx_width();
	for (uint8_t p = 0; p < AIR_PROVS; p++) {
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
}

/* --- A province's list --- */

/* Ankara: its five airfields only, nearest first from Kizilay, round within it, # its nearest */
static void pv_ankara(void)
{
	start("AP Ankara only, nearest first, #");
	airfields();
	open_group(6);
	expect(T("screen=airfields shows=Ankara shows=Nearest_first shows=LTAB shows=LTAD hides=LTAI hides=LTAF",
		 "screen=airfields shows=Ankara shows=En_yak shows=LTAB shows=LTAD hides=LTAI hides=LTAF"));
	run("shot:ankara");
	layout();
	run("ok");
	expect("shows=LTAB_G");
	run("back up ok");		/* round: the farthest of Ankara */
	expect("hides=LTAB_G hides=LTAI hides=LTAF");
	check(air_fields[air_next_in(&(air_here_t){ true, 3992080, 3285410 }, AIR_NONE, -1, 6)].prov == 6, "the last is Ankara's");
	run("back down ok");		/* and round back: the nearest */
	expect("shows=LTAB_G");
	/* # in the list: Ankara's nearest */
	pos(40.1281, 32.9951);		/* at Esenboga */
	run("back ctx");
	expect("shows=LTAC_Esenbo");
	layout();
}

/* Antalya: "+1" on its two towers, all its frequencies in the details, OK listens */
static void pv_antalya(void)
{
	start("AP Antalya: +N, details, listen");
	pos(36.8969, 30.7133);		/* Antalya (from far away the distance leaves no room for "+1") */
	airfields();
	open_group(7);
	expect("shows=Antalya shows=LTAI shows=TWR_125.000_+1 shows=LTFG hides=LTAB");
	layout();
	run("ok");
	expect("shows=ATIS_118.275 shows=GND_121.900 shows=TWR_125.000");
	layout();
	run("down down down down down down");
	expect("shows=APP_124.425");
	run("ok");
	expect("screen=home vfo=1 freq=124425000");
}

#if CONFIG_AIR_WATCH
/* # on an airfield's page in a province: the first six of its seven on the ring (the ATIS, last, left over) */
static void pv_watch(void)
{
	start("AP air watch from a province");
	pos(36.8969, 30.7133);		/* Antalya */
	airfields();
	open_group(7);
	run("ok");
	expect(T("shows=LTAI shows=#_watch_all", "shows=LTAI shows=#_t"));
	run("ctx");
	expect(T("screen=home dual=1 mwn=6 mwradion=6 mwsel=A toast=LTAI:_6/7_frequencies_watched",
		 "screen=home dual=1 mwn=6 mwradion=6 mwsel=A toast=LTAI:_6/7_frekans_izleniyor"));
	check(air_sim.vfo_calls == 6 && air_sim.hz[0] == 121900000, "six VFOs, GND first");
	run("back back wait:300");
	expect("dual=0");
	check(air_sim.end_calls == 1, "ended");
}
#endif

/* The air band pack over the table: Guvercinlik (LTAB) with two towers in the pack, seen in Ankara's list */
static void write_pack(uint8_t type, const uint8_t *p, uint32_t len, const char *pname)
{
	dpk_hdr_t h;
	dpk_writer_t w;
	memset(&h, 0, sizeof h);
	h.type = type;
	h.fmt = 1;
	h.len = len;
	h.crc = storage_crc32(0, p, len);
	memcpy(h.date, "20261121", 8);
	strncpy(h.name, pname, sizeof h.name);
	strncpy(h.tag, "test", sizeof h.tag);
	check(dpk_begin(&w, &h) && dpk_put(&w, 0, p, len) && dpk_commit(&w), "the pack written");
	dpk_scan();
}

static void pv_pack(void)
{
	static const uint32_t khz[4] = { 120200, 132000, 118250, 119500 };
	static const uint8_t type[4] = { AIR_GND, AIR_TWR, AIR_TWR, AIR_APP };
	start("AP air band pack in a province");
	uint8_t p[64];
	uint32_t n = 0;
	p[n++] = 1, p[n++] = 0, p[n++] = 0, p[n++] = 0;
	memcpy(p + n, "LTAB\0\0\0\0", 8), n += 8, p[n++] = 4;
	for (int k = 0; k < 4; k++) {
		uint16_t ch = (uint16_t)((khz[k] - AIR_CH_BASE_KHZ) / 5u);
		p[n++] = (uint8_t)ch, p[n++] = (uint8_t)(ch >> 8), p[n++] = type[k];
	}
	write_pack(DPK_AIR, p, n, "TR aerodromes");
	airfields();
	open_group(6);
	expect("shows=LTAB shows=TWR_132.000_+1 shows=LTAD shows=TWR_122.100");	/* the pack's, the table's */
	run("ok");
	expect("shows=TWR_118.250 shows=APP_119.500");
	layout();
	run("down down ok");		/* GND, TWR, TWR 118.250: only the pack has it */
	expect("screen=home vfo=1 freq=118250000");
}

/* Without a position: a province by code */
static void pv_no_position(void)
{
	start("AP no position: by code");
	air_sim.pos = false;
	airfields();
	open_group(34);
	expect(T("shows=No_position:_by_code shows=LTBA hides=LTAB", "shows=LTBA hides=LTAB"));
	run("ok");
	expect("shows=LTBA_");		/* the first by code */
	run("back up ok");
	expect("shows=LTFM_");		/* the last by code: Istanbul (LTFJ, Sabiha Gokcen, before it) */
	layout();
}

/* --- All, Back, # --- */

static void pv_all(void)
{
	start("AP All as before");
	airfields();
	run("ok");
	expect(T("shows=Nearest_first shows=LTAB shows=OK_info", "shows=En_yak shows=LTAB"));
	run("ok");
	expect("shows=LTAB_G");
	run("back down ok");
	expect("shows=LTAD_Etimesgut");
	layout();
}

static void pv_back(void)
{
	start("AP Back");
	airfields();
	open_group(6);
	run("ok");			/* LTAB's details */
	expect("shows=LTAB_G shows=GND_120.200");
	run("back");
	expect(T("screen=airfields shows=Ankara shows=Nearest_first", "screen=airfields shows=Ankara shows=En_yak"));
	run("back");
	expect(T("screen=airfields shows=06_Ankara shows=All_airfields hides=Nearest_first",
		 "screen=airfields shows=06_Ankara shows=Hepsi hides=En_yak"));
	run("ok");			/* the focus stayed on Ankara */
	expect("shows=Ankara shows=LTAB hides=LTAI");
	run("back back");
	check(ui_frame()->scr != SCR_AIRFIELDS, "Back on the first level leaves");
	/* opened again: the first level from the top (All) */
	airfields();
	expect(T("screen=airfields shows=All_airfields", "screen=airfields shows=Hepsi"));
	run("ok");
	expect("shows=LTAB shows=LTAD");
}

static void pv_hash(void)
{
	start("AP # on the first level");
	airfields();
	run("down down down ctx");	/* whatever is picked: the nearest of all */
	expect("shows=LTAB_G shows=GND_120.200");
	run("back");
	expect(T("shows=Nearest_first shows=LTAB shows=LTAD", "shows=En_yak shows=LTAB shows=LTAD"));
	run("back");
	expect(T("shows=All_airfields", "shows=Hepsi"));
	layout();
}

typedef void (*case_fn)(void);
static const case_fn all[] = {
	pv_list, pv_widths, pv_ankara, pv_antalya,
#if CONFIG_AIR_WATCH
	pv_watch,
#endif
	pv_pack, pv_no_position, pv_all, pv_back, pv_hash,
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
	printf("Plus airfields by province UI: %d cases (English and Turkish), %d failed\n", cases, bad);
	return bad != 0;
}
