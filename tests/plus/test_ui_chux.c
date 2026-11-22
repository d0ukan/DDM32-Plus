/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus r15 "channels" (tools/plus_chux.py, docs/plus.md "Channels, zones and DMR rows") through the real UI in
 * upstream's simulator, on the DM-32UV's display class (240 x 320, insets 10 x 5) with its key map, in English and in
 * Turkish. Each case runs in a child process (the screens' static state starts over); the layout check on every
 * screen a case ends on. Covers:
 *   1. zone size: the zone list's Add channel "16/80", the channel editor's Zones "12/80" beside a zone it is not in,
 *      Zones > Create New Zone "3/68"; a full zone (80): Add channel dimmed "80/80" and refused "Zone full: max 80
 *      channels", Copy here refused the same way; a position whose channel can't be read shows;
 *   2. DMR rows: a channel made DMR gets the first group contact; the RX group list picker offers Any right after
 *      None, picking it says what it hears and the row reads "All TGs TS1", None says so too; saved; an RX only DMR
 *      channel saves without a TX contact; Direct mode on says what it does;
 *   3. digital monitor: a channel with RX list Any shows AllTG, not DMon; the monitor key's toggle shows and hides DMon
 *      and says what it does (on: every call; off on such a channel: the channel still hears all TGs);
 *   6. a new channel (Channels > New from current, the editor's Save as new channel) joins Home's zone at its end and
 *      Home steps onto it (the owner's "only up to 26"). */
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
#include "codeplug/codeplug.h"

static int fails;
static const char *name;
static bool turkish;

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

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	const char *shots = getenv("CHUX_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "chux-tr-" : "chux-en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	run("wait:200");
}

/* Down (or Up) to the row whose label starts with label; false when it isn't there */
static bool row(const char *label, const char *key)
{
	for (int k = 0; k < 40; k++) {
		if (strncmp(ui_app_focus_label(), label, strlen(label)) == 0)
			return true;
		run(key);
	}
	printf("FAIL %s (%s): no row %s (focus %s)\n", name, turkish ? "tr" : "en", label, ui_app_focus_label());
	fails++;
	return false;
}

/* ---------------- 1. zone size ---------------- */

static void case_zone_counts(void)
{
	start("zone list: Add channel 16/80");
	run("ok 4 ctx");
	for (int k = 0; k < 20 && strcmp(ui_app_focus_label(), T("Add channel", "Kanal ekle")) != 0; k++)
		run("up");
	expect(T("screen=zone focus=Add_channel shows=16/80", "screen=zone focus=Kanal_ekle shows=16/80"));
	layout();
	start("channel editor Zones: 12/80 beside Simplex");
	run("ok 5 ok");
	row(T("Zones", "B\366lgeler"), "up");
	run("ok");
	expect("screen=chzones shows=12/80 shows=12/80");
	expect(T("shows=In", "shows=Var"));
	layout();
	start("Create New Zone: 3/68");
	run("ok 4 down down down");
	expect("screen=zones shows=3/68");
	layout();
}

static void case_zone_full(void)
{
	start("full zone: Add channel 80/80, refused");
	sim_worst(true);
	run("ok 4 ctx");
	for (int k = 0; k < 90 && strcmp(ui_app_focus_label(), T("Add channel", "Kanal ekle")) != 0; k++)
		run("up");
	expect("screen=zone shows=80/80");
	layout();
	run("ok");
	expect(T("toast=Zone_full:_max_80_channels writes=0", "toast=B\366lge_dolu:_en_\347ok_80_kanal writes=0"));
	layout();
	start("full zone: Copy here refused");
	sim_worst(true);
	run("ok 4 ctx ctx 3");
	expect(T("toast=Zone_full:_max_80_channels writes=0", "toast=B\366lge_dolu:_en_\347ok_80_kanal writes=0"));
}

/* A position whose channel can't be read shows ("17 ? Ch 77", "Can't be read") instead of vanishing */
static void case_zone_unreadable(void)
{
	start("zone list: a channel that can't be read shows");
	zone_t z;
	codeplug_zone(0, &z);
	z.channels[z.count++] = 77;	/* no channel 77 in the simulator */
	codeplug_set_zone(0, &z);
	run("ok 4 ctx");
	expect("count=19");
	for (int k = 0; k < 20 && strncmp(ui_app_focus_label(), "17 ?", 4) != 0; k++)
		run("up");
	expect(T("focus=17_?_Ch_77 shows=Can't_be_read", "focus=17_?_Ch_77 shows=Okunam"));
	layout();
}

/* ---------------- 2. DMR rows ---------------- */

/* The channel in use (COUNTY DMR) in its editor */
static void dmr_channel(void)
{
	run("ok 5 ok");
	expect("screen=channel shows=DMR shows=COUNTY_DMR");
}

static void case_dmr_rows(void)
{
	start("made DMR: the first group contact");
	dmr_channel();
	row(T("Mode", "Mod"), "up");
	run("ok ok ok");	/* DMR, P25 (no contact), FM, DMR again (this build has no YSF / NXDN / dPMR) */
	row(T("TX contact", "TX ki"), "down");
	expect("shows=USA");
	layout();
	start("RX group list: Any after None, said, shown, saved");
	dmr_channel();
	row(T("RX list", "RX listesi"), "down");
	run("ok");
	expect("screen=chpick count=3 focus=County");
	run("up");
	expect(T("focus=Any:_all_TGs", "focus=T\374m\374:_t\374m_gruplar"));
	layout();
	run("ok");
	expect(T("screen=channel toast=Any:_every_TG_on_this_slot", "screen=channel toast=T\374m\374:_bu_slottaki"));
	run("wait:3000");	/* the toast gone */
	expect(T("shows=All_TGs_TS1", "shows=T\374m_TG_TS1"));
	layout();
	run("long:ok");
	expect("writes=1");
	start("RX group list: None said");
	dmr_channel();
	row(T("RX list", "RX listesi"), "down");
	run("ok up up ok");
	expect(T("toast=None:_only_the_TX_contact's_TG", "toast=Yok:_yaln\375z_TX"));
	start("RX only DMR channel saves without a TX contact");
	dmr_channel();
	row(T("TX contact", "TX ki\376isi"), "down");
	run("ok");
	row(T("None", "Yok"), "up");
	run("ok");
	row(T("RX only", "Yaln\375z RX"), "up");
	run("ok long:ok");
	expect("writes=1");
	start("Direct mode on: said");
	dmr_channel();
	row(T("Direct mode", "Direkt mod"), "down");
	run("ok");
	expect(T("toast=Direct:_TX_on_the_RX_frequency", "toast=Direkt:_TX,_RX_frekans\375nda"));
	layout();
}

/* ---------------- 3. digital monitor ---------------- */

static void case_dmon(void)
{
	start("channel Any: AllTG, the toggle says DMon on and off");
	dmr_channel();
	row(T("RX list", "RX listesi"), "down");
	run("ok up ok wait:3000 long:ok back back back up down wait:200");
	expect(T("screen=home shows=AllTG hides=DMon hearall=0", "screen=home shows=mG hides=DMon hearall=0"));
	layout();
	run("mon wait:200");
	expect(T("hearall=1 shows=DMon hides=AllTG toast=Digital_monitor_on:_every_call",
		 "hearall=1 shows=DMon hides=mG toast=Dijital_dinleme_a"));
	run("wait:3000 mon wait:200");
	expect(T("hearall=0 hides=DMon shows=AllTG toast=Monitor_off:_channel_hears_all_TGs",
		 "hearall=0 hides=DMon shows=mG toast=Dinleme_kapal"));
	layout();
	start("a channel without Any: DMon off is just off");
	run("mon wait:3000 mon wait:200");
	expect(T("hearall=0 hides=DMon hides=AllTG toast=Digital_monitor_off",
		 "hearall=0 hides=DMon hides=mG toast=Dijital_dinleme_kapal"));
	layout();
}

/* ---------------- 4. Delete channel ---------------- */

static void case_delete(void)
{
	start("zone channel menu: Delete asks, No keeps it");
	run("ok 4 ctx down down");
	expect("screen=zone focus=05_Metro_DMR count=18");
	run("ctx up");
	expect(T("screen=zchannel focus=Delete_channel", "screen=zchannel focus=Kanal"));
	layout();
	run("ok");
	expect(T("screen=confirm focus=No shows=Metro_DMR shows=Leaves_zones:_1", "screen=confirm focus=Hay shows=Metro_DMR"));
	layout();
	run("shot:delete-confirm");
	run("ok");
	expect("screen=zone writes=0 count=18");
	start("zone channel menu: Delete, Yes: gone from the zone");
	run("ok 4 ctx down down ctx up ok down ok wait:200");
	expect(T("screen=zone writes=1 count=17 toast=Deleted_Metro_DMR", "screen=zone writes=1 count=17 toast=Silindi_Metro_DMR"));
	run("wait:3000");
	expect("hides=Metro_DMR");
	layout();
	start("the channel in use: refused");
	run("ok 4 ctx ctx up ok");
	expect(T("screen=zone writes=0 toast=In_use:_switch", "screen=zone writes=0 toast=Kullan"));
	start("channel editor: Delete channel, the last row; Yes closes the editor");
	run("ok 5");
	row("5 Metro", "up");
	run("ok");
	expect("screen=channel shows=Metro_DMR");
	run("up");
	expect(T("focus=Delete_channel", "focus=Kanal"));
	layout();
	run("ok down ok wait:200");
	expect(T("screen=channels writes=1 toast=Deleted_Metro_DMR", "screen=channels writes=1 toast=Silindi_Metro_DMR"));
	layout();
	start("a new channel's editor has no Delete row");
	run("ok 5");
	row(T("New from current", "Mevcuttan yeni"), "up");
	run("ok up");
	expect("screen=channel hides=Delete_channel writes=0");
}

/* ---------------- 6. a new channel joins Home's zone ---------------- */

/* Home steps (Down) until position pos of its zone; false when it never gets there */
static bool home_reaches(uint16_t pos)
{
	for (int k = 0; k < 2 * CODEPLUG_ZONE_MAX; k++) {
		if (ui_view()->zone_pos + 1u == pos)
			return true;
		run("down wait:100");
	}
	printf("FAIL %s (%s): Home never reached position %u (at %u)\n", name, turkish ? "tr" : "en", pos,
	       ui_view()->zone_pos + 1u);
	fails++;
	return false;
}

/* The owner's case: a channel made after the zones were stored was in no zone, so Home stopped at the zone's old last
 * channel. Channels > New from current, saved: it is the zone's last channel and Home steps onto it */
static void case_new_joins(void)
{
	start("new channel: joins Home's zone, Home steps onto it");
	zone_t z;
	uint16_t zi = ui_view()->zone_index;
	codeplug_zone(zi, &z);
	uint16_t before = z.count;
	run("ok 5");
	row(T("New from current", "Mevcuttan yeni"), "up");
	run("ok long:ok wait:200");
	expect(T("toast=Saved_as_channel", "toast=Kaydedildi,_yeni_kanal"));
	codeplug_zone(zi, &z);
	if (z.count != before + 1u) {
		printf("FAIL %s (%s): zone %u has %u channels, want %u\n", name, turkish ? "tr" : "en", zi, z.count,
		       before + 1u);
		fails++;
		return;
	}
	uint16_t num = z.channels[z.count - 1u];
	channel_t c;
	if (!codeplug_channel(num, &c)) {
		printf("FAIL %s (%s): the new channel %u can't be read\n", name, turkish ? "tr" : "en", num);
		fails++;
	}
	run("back back back wait:200");
	expect("screen=home");
	if (home_reaches(z.count) && ui_view()->channel_number != num) {
		printf("FAIL %s (%s): Home's last position is channel %u, want %u\n", name, turkish ? "tr" : "en",
		       ui_view()->channel_number, num);
		fails++;
	}
	layout();
	start("Save as new channel: joins Home's zone too");
	codeplug_zone(zi, &z);
	before = z.count;
	run("ok 5 ok");
	row(T("Save as new channel", "Yeni kanal olarak kaydet"), "up");
	run("ok wait:200");
	codeplug_zone(zi, &z);
	if (z.count != before + 1u) {
		printf("FAIL %s (%s): Save as new: zone %u has %u channels, want %u\n", name, turkish ? "tr" : "en", zi,
		       z.count, before + 1u);
		fails++;
	}
}

/* ---------------- 5. P25 NAC in decimal ---------------- */

/* COUNTY DMR made P25 (NAC $293), on its RX NAC row */
static void p25_nac_row(void)
{
	run("ok 5 ok");
	row(T("Mode", "Mod"), "up");
	run("ok");
	row("RX NAC", "down");
}

static void case_nac(void)
{
	start("NAC: decimal first, typed in decimal, hex in brackets");
	p25_nac_row();
	expect("shows=659_($293)");
	layout();
	run("shot:nac-decimal");
	run("ok 2 9 3");
	expect("shows=[293]_($125)");
	run("ok");
	expect("shows=293_($125)");
	layout();
	start("NAC: past 4095 refused, Any, hex digits");
	p25_nac_row();
	run("ok 4 0 9 6");
	expect(T("toast=NAC_goes_0_to_4095 shows=[409]_($199)", "toast=NAC_0_ile_4095 shows=[409]_($199)"));
	run("ok wait:3000");
	expect("shows=409_($199)");
	run("ok ctx ok wait:3000");
	expect("shows=3966_($F7E)");
	run("ok ctx 1 2 5 ok wait:3000");
	expect("shows=293_($125)");
	layout();
	start("TX NAC: 3966 refused, a value typed");
	p25_nac_row();
	run("down");
	expect(T("focus=TX_NAC shows=Same_as_RX", "focus=TX_NAC"));
	run("ok 3 9 6 6 ok");
	expect(T("toast=TX_NAC_can't_be_3966", "toast=TX_NAC_3966"));
	run("wait:3000");
	expect("shows=[3966]_($F7E)");
	run("back back back back 1 6 2 5 ok");
	expect("shows=1625_($659)");
	layout();
}

typedef void (*case_fn)(void);

static int run_case(case_fn fn)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		fails = 0;
		fn();
		fflush(stdout);
		_exit(fails ? 1 : 0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}

int main(void)
{
	static const case_fn cases[] = { case_zone_counts, case_zone_full, case_zone_unreadable, case_dmr_rows, case_dmon, case_delete, case_nac,
					 case_new_joins };
	int bad = 0, n = 0;
	for (int lang = 0; lang < 2; lang++) {
		turkish = lang == 1;
		for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++, n++)
			bad += run_case(cases[i]);
	}
	printf("Plus r15 channels UI: %d cases (English and Turkish), %d failed\n", n, bad);
	return bad != 0;
}
