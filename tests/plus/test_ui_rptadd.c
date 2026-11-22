/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > Repeaters, * adds the picked repeater as a channel (CONFIG_CHAN_EXTRAS, docs/plus.md "Repeaters as
 * channels") through the real UI in upstream's simulator (wrapped by
 * tests/plus/ui_sim_mapnav.c), on the DM-32UV's display class with its key map, in English and Turkish. Built like
 * tests/plus/test_ui_rptprov.c plus -DCONFIG_CHAN_EXTRAS=1 (tools/test_rptprov.py). Each case runs in a child process.
 * Covers: an FM repeater (RX, TX = RX + offset, access tone, 25 kHz, name) joins All channels and Home's zone at the
 * end, the toast names the channel and the zone; a DMR repeater (colour code, slot Any or as listed, receive only, no
 * contact); the same repeater again says "Already in <zone>: ch N" and adds nothing; a mode the radio has no channel
 * for (C4FM) is refused; a full zone (80) and a transmission are refused cleanly; on the VFO only All channels takes
 * it; * works in the details too; on the province level * still opens quick settings. The simulator's codeplug and
 * zone editor are its own (a fixed number of channel slots, the zone job as a record), so a full codeplug and a write
 * in progress are only code-reviewed here; the storage job itself (zone_join.inc) is tests/plus/test_zone_cap.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/ui_screens.h"
#include "ui/i18n.h"
#include "ui/ui_svc.h"
#include "ui/rpt.h"
#include "codeplug/codeplug.h"
#include "radio/radio.h"
#include "ui_sim_mapnav.h"

#define P(la, lo) (int32_t)((la) * 1e5 + 0.5), (int32_t)((lo) * 1e5 + 0.5)
const rpt_entry_t rpt_table[] = {
	{ P(39.93, 32.86), 145600000, -480, 885, RPT_FM, 0, "TA2RAK", "Ankara", 6 },
	{ P(37.00, 35.32), 145612500, -480, 0, RPT_FM | RPT_NARROW, 0, "TA5ADA", "Adana", 1 },
	{ P(39.80, 32.60), 145625000, 0, 0, RPT_FM, 0, "TA2SPX", "Golbasi", 6 },
	{ P(37.03, 35.81), 145650000, -480, 1318, RPT_FM, 0, "TA5CEY", "Ceyhan", 1 },
	{ P(39.94, 32.87), 145675000, -480, 0, RPT_C4FM, 0, "TA2YSF", "Ankara", 6 },
	{ P(41.01, 28.97), 439000000, -6080, 0, RPT_DMR, 1 | 3 << 4, "TA1IST", "Istanbul", 34 },
	{ P(39.92, 32.85), 439100000, -6080, 0, RPT_DMR, 1 | 3 << 4, "TA2DMR", "Ankara", 6 },
	{ RPT_NOPOS, RPT_NOPOS, 439150000, -6080, 0, RPT_DMR, 1, "TA9XXX", "Nowhere", RPT_PROV_UNKNOWN },
	{ P(39.40, 32.85), 439200000, -6080, 0, RPT_DMR, 2 | 2 << 4, "TA2DMS", "Haymana", 6 },
};
const rpt_info_t rpt_info = { sizeof rpt_table / sizeof rpt_table[0], 0, "2026-11-19", "test" };

#define TOOLS	"ok 6 "
#define T(en, tr) (turkish ? (tr) : (en))

static int fails;
static const char *name;
static bool turkish;
static char rpt_open[200];

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, turkish ? "tr" : "en", script, err);
		fails++;
	}
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
	ok_run(buf);
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
	ok_run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	/* Only the toasts are this change's: the list's own rows (the "No position" line, the DMR rows) are wider than the
	 * screen in the tests of the screen before it, and are not checked here */
	static const char *const mine[] = { ": ch ", "Zone full", "B\366lge dolu", "No free", "Bo\376 kanal", "Can't add", "Yay\375n",
					    "Info only", "Yaln\375z", "Saving", "Kaydediliyor" };
	for (int i = 0; i < n; i++) {
		for (unsigned k = 0; k < sizeof mine / sizeof mine[0]; k++) {
			if (strstr(issues[i], mine[k]) != NULL) {
				printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[i]);
				fails++;
				break;
			}
		}
	}
}

static void gps(bool fix, double la, double lo)
{
	mapnav_sim.gps = true;
	mapnav_sim.fix = fix;
	mapnav_sim.lat_e7 = (int32_t)(la * 1e7 + (la < 0 ? -0.5 : 0.5));
	mapnav_sim.lon_e7 = (int32_t)(lo * 1e7 + (lo < 0 ? -0.5 : 0.5));
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	const char *shots = getenv("RPTADD_SHOTS");
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "rptadd-tr-" : "rptadd-en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	mapnav_sim_clear();
	gps(true, 39.93, 32.86);
	mapnav_sim.stations = true;
	mapnav_sim_mode(RADIO_MODE_FM);
	ok_run("wait:200");
}

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

static uint16_t zone_count_of(uint16_t zi)
{
	zone_t z;
	return codeplug_zone(zi, &z) ? z.count : 0xFFFFu;
}

/* zone zi holds want channels, the last one last (0: any) */
static bool zone_is(uint16_t zi, uint16_t want, uint16_t last)
{
	zone_t z;
	if (codeplug_zone(zi, &z) && z.count == want && (last == 0 || z.channels[z.count - 1u] == last))
		return true;
	printf("FAIL %s (%s): zone %u has %u channels (last %u), want %u (last %u)\n", name, turkish ? "tr" : "en", zi,
	       z.count, z.count ? z.channels[z.count - 1u] : 0, want, last);
	fails++;
	return false;
}

static void zone_name(uint16_t zi, const char *zname)
{
	zone_t z;
	codeplug_zone(zi, &z);
	memset(z.name, 0, sizeof z.name);
	strcpy(z.name, zname);
	codeplug_set_zone(zi, &z);
}

/* Tools > Repeaters > All, the list by frequency (no position): n Down presses put the focus on entry n of the table's
 * order among those with a position-less sort: the table order itself */
static void list_all_by_freq(int downs)
{
	char s[120];
	gps(false, 0, 0);
	ok_run(rpt_open);
	ok_run("ok");	/* All repeaters */
	snprintf(s, sizeof s, "wait:200");
	ok_run(s);
	for (int k = 0; k < downs; k++)
		ok_run("down");
}

/* --- FM: the full settings --- */

static void ra_fm(void)
{
	zone_name(0, "Home zone");
	uint16_t zi = ui_view()->zone_index, before = zone_count_of(zi), all = zone_count_of(0);
	uint16_t num = codeplug_free_channel();
	list_all_by_freq(3);	/* TA5CEY */
	expect("shows=TA5CEY");
	ok_run("quick wait:300");
	expect(T("screen=repeaters toast=Added:_ch_", "screen=repeaters toast=Eklendi:_kanal_"));
	zone_is(zi, (uint16_t)(before + 1u), num);
	if (zi != 0)
		zone_is(0, (uint16_t)(all + 1u), num);
	channel_t c;
	check(codeplug_channel(num, &c), "the new channel can be read");
	check(strcmp(c.name, "TA5CEY") == 0, "name TA5CEY");
	check(c.rx_hz == 145650000u && c.tx_hz == 145050000u, "RX 145.650, TX 145.050 (offset -600 kHz)");
	check(c.mode == RADIO_MODE_FM, "FM");
	check(c.bw == RADIO_BW_25K, "25 kHz");
	check(c.tx_tone.type == TONE_CTCSS && c.tx_tone.value == 1318, "TX tone CTCSS 131.8");
	check(c.rx_tone.type == TONE_NONE, "no RX tone");
	check(!c.rx_only, "can transmit");
	check(c.power == CODEPLUG_POWER_DEFAULT, "default power");
	layout();
	ok_run("shot:added");
	/* the same one again: said, nothing added */
	ok_run("wait:3000");
	ok_run("quick wait:300");
	expect(T("toast=Already_in_", "toast=Zaten_var:_"));
	zone_is(zi, (uint16_t)(before + 1u), num);
	check(codeplug_free_channel() == (uint16_t)(num + 1u), "no second channel was made");
	layout();
	/* narrow, simplex ones */
	ok_run("wait:3000 up up");	/* TA5ADA */
	expect("shows=TA5ADA");
	uint16_t num2 = codeplug_free_channel();
	ok_run("quick wait:300");
	check(codeplug_channel(num2, &c) && c.bw == RADIO_BW_12K5 && c.tx_tone.type == TONE_NONE, "narrow, no tone");
	ok_run("wait:3000 down");	/* TA2SPX simplex */
	expect("shows=TA2SPX");
	uint16_t num3 = codeplug_free_channel();
	ok_run("quick wait:300");
	check(codeplug_channel(num3, &c) && c.rx_hz == 145625000u && c.tx_hz == 145625000u, "simplex: TX = RX");
	zone_is(zi, (uint16_t)(before + 3u), num3);
}

/* * in the details works too */
static void ra_details(void)
{
	zone_name(0, "Home zone");
	uint16_t zi = ui_view()->zone_index, before = zone_count_of(zi);
	uint16_t num = codeplug_free_channel();
	list_all_by_freq(0);
	ok_run("ok");	/* TA2RAK's details */
	expect(T("shows=TA2RAK shows=OK_tune", "shows=TA2RAK shows=OK_ayarla"));
	ok_run("quick wait:300");
	expect(T("toast=Added:_ch_", "toast=Eklendi:_kanal_"));
	zone_is(zi, (uint16_t)(before + 1u), num);
	channel_t c;
	check(codeplug_channel(num, &c) && c.rx_hz == 145600000u && c.tx_hz == 145000000u && c.tx_tone.value == 885,
	      "TA2RAK: 145.600, TX 145.000, 88.5");
	layout();
}

/* --- DMR --- */

static void ra_dmr(void)
{
	zone_name(0, "Home zone");
	uint16_t zi = ui_view()->zone_index, before = zone_count_of(zi);
	uint16_t num = codeplug_free_channel();
	list_all_by_freq(6);	/* TA2DMR: CC1, both slots */
	expect("shows=TA2DMR");
	ok_run("quick wait:300");
	zone_is(zi, (uint16_t)(before + 1u), num);
	channel_t c;
	check(codeplug_channel(num, &c), "the DMR channel can be read");
	check(c.mode == RADIO_MODE_DMR && c.rx_hz == 439100000u && c.tx_hz == 431500000u, "DMR 439.100, TX 431.500");
	check(c.color_code == 1 && c.timeslot == CODEPLUG_TS_ANY, "CC1, both slots: Any");
	check(c.rx_only && c.contact == 0, "receive only, no contact (the list has no talkgroup)");
	check(c.listen_list == CODEPLUG_LIST_ANY, "hears every talkgroup");
	ok_run("wait:3000 down down");	/* TA2DMS: CC2, TS2 */
	expect("shows=TA2DMS");
	uint16_t num2 = codeplug_free_channel();
	ok_run("quick wait:300");
	check(codeplug_channel(num2, &c) && c.color_code == 2 && c.timeslot == 2, "CC2 TS2 as listed");
	layout();
}

/* --- Refusals --- */

static void ra_mode(void)
{
	zone_name(0, "Home zone");
	uint16_t zi = ui_view()->zone_index, before = zone_count_of(zi);
	uint16_t free0 = codeplug_free_channel();
	list_all_by_freq(4);	/* TA2YSF: C4FM */
	expect("shows=TA2YSF");
	ok_run("quick wait:300");
	expect(T("toast=Info_only", "toast=Yaln\375z_bilgi"));
	zone_is(zi, before, 0);
	check(codeplug_free_channel() == free0, "nothing written");
}

static void ra_zone_full(void)
{
	zone_t z;
	uint16_t zi = ui_view()->zone_index;
	codeplug_zone(zi, &z);
	z.count = CODEPLUG_ZONE_MAX;
	for (uint16_t i = 0; i < z.count; i++)
		z.channels[i] = (uint16_t)(1u + i % 12u);
	codeplug_set_zone(zi, &z);
	uint16_t free0 = codeplug_free_channel();
	list_all_by_freq(0);
	ok_run("quick wait:300");
	expect(T("toast=Zone_full:_max_80", "toast=B\366lge_dolu:_en_\347ok_80"));
	zone_is(zi, CODEPLUG_ZONE_MAX, 0);
	check(codeplug_free_channel() == free0, "nothing written");
	layout();
}

static void ra_tx(void)
{
	uint16_t zi = ui_view()->zone_index, before = zone_count_of(zi);
	list_all_by_freq(0);
	ok_run("datatx");
	expect("screen=repeaters");
	ok_run("quick wait:300");
	expect(T("toast=Can't_add_while_transmitting", "toast=Yay\375n_s\375ras\375nda_eklenemez"));
	zone_is(zi, before, 0);
	ok_run("dataend wait:200");
}

/* The first level (by province) has no pick: * is quick settings there */
static void ra_groups(void)
{
	ok_run(rpt_open);
	expect(T("screen=repeaters shows=All_repeaters", "screen=repeaters shows=Hepsi"));
	ok_run("quick");
	expect("screen=quick");
}

/* On the VFO: All channels only */
static void ra_vfo(void)
{
	zone_name(0, "All channels");
	radio_req_zone(1);
	radio_req_vfo(true);
	ok_run("wait:200");
	uint16_t h = zone_count_of(1), a = zone_count_of(0), num = codeplug_free_channel();
	list_all_by_freq(0);
	ok_run("quick wait:300");
	expect(T("toast=Added:_ch_", "toast=Eklendi:_kanal_"));
	zone_is(0, (uint16_t)(a + 1u), num);
	zone_is(1, h, 0);
}

/* Home on a Find & save zone: it never takes the channel (it holds finds only): All channels only, the toast names no
 * zone, and the duplicate check looks at All channels */
static void ra_find_zone(void)
{
	zone_name(0, "All channels");
	zone_name(1, "Bulunanlar 1");
	radio_req_zone(1);
	ok_run("wait:200");
	uint16_t h = zone_count_of(1), a = zone_count_of(0), num = codeplug_free_channel();
	list_all_by_freq(0);
	ok_run("quick wait:300");
	expect(T("toast=Added:_ch_", "toast=Eklendi:_kanal_"));
	zone_is(0, (uint16_t)(a + 1u), num);
	zone_is(1, h, 0);
	ok_run("wait:3000 quick wait:300");
	expect(T("toast=Already_in_All_channels:_ch_", "toast=Zaten_var:_All_channels:_ch_"));
	zone_is(0, (uint16_t)(a + 1u), num);
}

typedef void (*case_fn)(void);
static const struct { const char *name; case_fn fn; } cases[] = {
	{ "RA FM repeater: full settings, duplicate, narrow, simplex", ra_fm },
	{ "RA * in the details", ra_details },
	{ "RA DMR repeater: colour code, slot, receive only", ra_dmr },
	{ "RA a mode without a channel is refused", ra_mode },
	{ "RA full zone refused", ra_zone_full },
	{ "RA refused while transmitting", ra_tx },
	{ "RA the province level keeps quick settings", ra_groups },
	{ "RA on the VFO: All channels only", ra_vfo },
	{ "RA Home on a Find & save zone: All channels only", ra_find_zone },
};

static int run_case(unsigned i)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		start(cases[i].name);
		cases[i].fn();
		fflush(stdout);
		_exit(fails != 0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		if (!WIFEXITED(st))
			printf("FAIL %s: crashed\n", cases[i].name);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (!find_tools_row()) {
		printf("FAIL no Repeaters row in Tools\n");
		return 1;
	}
	int failed = 0;
	unsigned n = sizeof cases / sizeof cases[0];
	for (int lang = 0; lang < 2; lang++) {
		turkish = lang == 1;
		for (unsigned i = 0; i < n; i++)
			failed += run_case(i);
	}
	printf("Plus repeaters as channels UI: %u flows x 2 languages, %d failures\n", n, failed);
	return failed != 0;
}
