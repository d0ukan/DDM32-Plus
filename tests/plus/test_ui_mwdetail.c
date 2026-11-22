/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Home details (docs/plus.md "Dual watch and multi-watch: the TX target", Home's details; tools/plus_mwfix.py,
 * integration/radiofoundry/mwatch/ui_detail.inc): each channel row says its mode's own settings. Built like
 * tests/plus/test_ui_vfox.c (every digital mode, VFO_MODES, NAC_DEC, MULTI_WATCH, P25_TXNAC) with upstream's simulator,
 * on the DM-32UV's display class (240 x 320, insets 10 x 5), English and Turkish, normal and Large text, with the
 * simulator's layout checks. First the formatter itself (every mode, every form), then Home: the multi-watch rows (the
 * simulator's zone from position 3: COUNTY DMR, County fire FM T100.0, Metro DMR, Hill top DMR, North link DMR, Harbor
 * FM; DMR CC1 TS1 talk group 3100 USA), a letter made a P25 VFO with TG 99 NAC $456, dual watch's panes with B a P25
 * VFO. Each Home case runs in a child process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/ui_svc.h"
#include "ui/ui_screens.h"
#include "ui/i18n.h"
#include "radio/radio_nxdn.h"
#include "radio/radio_dpmr.h"
#include "p25/p25_lab.h"

uint32_t radio_nxdn_caller(radio_nxdn_caller_t *out)
{
	if (out)
		memset(out, 0, sizeof *out);
	return 0;
}

uint32_t radio_dpmr_caller(radio_dpmr_caller_t *out)
{
	if (out)
		memset(out, 0, sizeof *out);
	return 0;
}

size_t radio_dpmr_caller_line(char *out, size_t cap)
{
	if (out && cap)
		out[0] = '\0';
	return 0;
}

uint32_t p25_lab_ysf_caller(char src[11], char dst[11])
{
	if (src)
		src[0] = '\0';
	if (dst)
		dst[0] = '\0';
	return 0;
}

static int fails;
static const char *name;
static bool turkish, large;

/* ---- the formatter ---- */
static void expect_detail(const char *what, const channel_t *ch, uint8_t form, uint32_t talk, bool priv, const char *want)
{
	char buf[64];
	sbuf_t b;
	ui_detail_t d;
	ui_detail_of(ch, &d);
	if (talk != 0) {
		d.b = (uint16_t)talk;
		d.c = (uint16_t)(UI_DETAIL_TALK | (priv ? UI_DETAIL_PRIVATE : 0u) | ((talk >> 16) & 0xFFu));
	}
	sb_init(&b, buf, sizeof buf);
	ui_sb_detail(&b, &d, form);
	if (strcmp(buf, want) != 0) {
		printf("FAIL detail %s form %u: \"%s\", wanted \"%s\"\n", what, form, buf, want);
		fails++;
	}
}

static void formatter(void)
{
	channel_t c;
	memset(&c, 0, sizeof c);
	/* The owner's B: RX NAC Any, TX NAC inherited $293, TG 1 */
	c.mode = RADIO_MODE_P25;
	c.p25_nac = 0xF7E;
	c.p25_tg = 1;
	expect_detail("P25 Any", &c, UI_DETAIL_WIDE, 0, false, "NAC Any>$293/659 TG 1");
	expect_detail("P25 Any", &c, UI_DETAIL_TIGHT, 0, false, "NACAny>$293/659 TG1");
	expect_detail("P25 Any", &c, UI_DETAIL_TALK_ONLY, 0, false, "TG 1");
	expect_detail("P25 Any", &c, UI_DETAIL_REST, 0, false, "NAC Any>$293/659");
	c.p25_nac = 0x293;
	c.p25_tg = 42;
	expect_detail("P25 $293", &c, UI_DETAIL_WIDE, 0, false, "NAC $293/659 TG 42");
	expect_detail("P25 $293", &c, UI_DETAIL_TIGHT, 0, false, "NAC$293/659 TG42");
	c.p25_tg = 0;
	expect_detail("P25 TG Any", &c, UI_DETAIL_WIDE, 0, false, "NAC $293/659 TG Any");
	c.p25_tx_nac_override = 0x659 + 1;	/* a TX NAC of its own */
	expect_detail("P25 TX NAC", &c, UI_DETAIL_REST, 0, false, "NAC $293/659>$659/1625");
	c.p25_tx_nac_override = 0;
	c.mode = RADIO_MODE_YSF;
	c.p25_tg = 12;
	expect_detail("YSF", &c, UI_DETAIL_WIDE, 0, false, "DG-ID 12");
	expect_detail("YSF", &c, UI_DETAIL_TIGHT, 0, false, "DG12");
	expect_detail("YSF", &c, UI_DETAIL_TALK_ONLY, 0, false, "");	/* no YSF transmitter: its DG-ID is what it hears */
	expect_detail("YSF", &c, UI_DETAIL_REST, 0, false, "DG-ID 12");
	c.p25_tg = 0;
	expect_detail("YSF Any", &c, UI_DETAIL_WIDE, 0, false, "DG-ID Any");
	c.mode = RADIO_MODE_NXDN;
	c.p25_tg = 5;
	expect_detail("NXDN", &c, UI_DETAIL_WIDE, 0, false, "RAN 5");
	expect_detail("NXDN", &c, UI_DETAIL_TIGHT, 0, false, "RAN5");
	c.mode = RADIO_MODE_DPMR;
	c.p25_tg = 8;	/* colour code 7, stored + 1 */
	expect_detail("dPMR", &c, UI_DETAIL_WIDE, 0, false, "CC 7");
	expect_detail("dPMR", &c, UI_DETAIL_TALK_ONLY, 0, false, "");	/* receive only: not a talk target */
	expect_detail("dPMR", &c, UI_DETAIL_REST, 0, false, "CC 7");
	memset(&c, 0, sizeof c);
	c.mode = RADIO_MODE_DMR;
	c.color_code = 1;
	c.timeslot = 1;
	expect_detail("DMR", &c, UI_DETAIL_WIDE, 1, false, "CC1 TS1 TG 1");
	expect_detail("DMR", &c, UI_DETAIL_TIGHT, 91, false, "CC1 TS1 TG91");
	expect_detail("DMR private", &c, UI_DETAIL_WIDE, 3101234, true, "CC1 TS1 ID 3101234");
	expect_detail("DMR no target", &c, UI_DETAIL_WIDE, 0, false, "CC1 TS1");
	c.color_code = CODEPLUG_CC_ANY;
	c.timeslot = CODEPLUG_TS_ANY;
	expect_detail("DMR Any", &c, UI_DETAIL_WIDE, 0, false, "CC* TS*");
	memset(&c, 0, sizeof c);
	c.mode = RADIO_MODE_FM;
	expect_detail("FM none", &c, UI_DETAIL_WIDE, 0, false, "No tone");
	c.tx_tone = (subtone_t){ TONE_CTCSS, 885 };
	c.rx_tone = (subtone_t){ TONE_CTCSS, 1000 };
	expect_detail("FM", &c, UI_DETAIL_WIDE, 0, false, "T88.5 R100.0");
	expect_detail("FM", &c, UI_DETAIL_TIGHT, 0, false, "T88.5");
	c.tx_tone = (subtone_t){ TONE_DCS, 023 };
	c.rx_tone = c.tx_tone;
	expect_detail("FM DCS", &c, UI_DETAIL_WIDE, 0, false, "D023N");
	c.tx_tone = (subtone_t){ TONE_NONE, 0 };
	expect_detail("FM RX only tone", &c, UI_DETAIL_TIGHT, 0, false, "RD023N");
}

/* ---- Home ---- */
static bool run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s%s): %s: %s\n", name, turkish ? "tr" : "en", large ? ", large" : "", script, err);
		fails++;
		return false;
	}
	return true;
}

static void layout(void)
{
#ifdef MWD_SHOW
	run("expect:shows=SHOW_THE_SCREEN");	/* a debug build prints what Home shows */
#endif
	char issues[SIM_LAYOUT_MAX][160];
	run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int i = 0; i < n; i++)
		printf("FAIL %s (%s%s): layout: %s\n", name, turkish ? "tr" : "en", large ? ", large" : "", issues[i]);
	fails += n;
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	if (large) {
		ui_svc_setting_set(UI_SET_LARGE_TEXT, 1);
		run("reboot");	/* the setting is read at start; without it these runs were not large at all */
	}
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	run("wait:200");
}

/* The DMR rows' CC, TS and talk group, the FM row's tone */
static void ring4(void)
{
	start("ring of 4: DMR and FM details");
	run("mwatch:4 quick 7 wait:300");
	if (!turkish)	/* the watch named by its letters (Turkish: Dörtlü izleme, its letters outside the capture) */
		run("expect:toast=Quad_watch_on");
	run("wait:2200");
	if (!turkish && !large)
		run("expect:shows=QUAD");
	run("expect:mwn=4");
	if (!large)
		run("expect:shows=CC1_TS1 expect:shows=3100 expect:shows=T100.0");
	layout();
}

static void ring6(void)
{
	start("ring of 6");
	run("mwatch:6 quick 7 wait:300");
	if (!turkish)
		run("expect:toast=6x_watch_on");
	run("wait:2200");
	run("expect:mwn=6");
	layout();
}

/* C its own VFO in P25, TG 99, NAC $456 (1110 decimal): its row */
static void ring_p25(void)
{
	start("ring of 4: C a P25 VFO");
	run("mwatch:4 quick 7 wait:500 abkey abkey vfokey quick 4 4 ctx down down down ok 9 9 ok down ok 1 1 1 0 ok "
	    "back back wait:2500");
	run("expect:vmode=P25 expect:tg=99 expect:nac=456");
	if (!large)
		run("expect:shows=$456/1110");
	layout();
}

/* Dual watch (2): B a P25 VFO: its pane names what PTT talks to (TG) and the NAC, no "VFO FM" */
static void pair_p25(void)
{
	start("dual watch: B a P25 VFO");
	run("mwatch:2 quick 7 wait:500 abkey vfokey quick 4 4 ctx down down down ok 9 9 ok down ok 1 1 1 0 ok back back "
	    "wait:2500");
	run("expect:vmode=P25 expect:mwn=0");
	if (!turkish && !large)
		run("expect:shows=DUAL");
	if (!large)
		run("expect:shows=TG_99 expect:hides=VFO_FM");
	layout();
}

/* The verifier's S1: no new selection while transmitting (the radio drops it then, so the PTT pill would move away
 * from where PTT goes): refused with a toast, the pill stays; after the transmission it moves */
static void sel_during_tx(void)
{
	start("selection during TX");
	run("mwatch:4 quick 7 wait:2500 pttdown wait:200 abkey wait:200");
	run("expect:mwsel=A");
	if (!turkish)
		run("expect:toast=Not_while_transmitting");
	run("pttup wait:1500 abkey wait:300 expect:mwsel=B");
	layout();
}

/* The radio chooses another letter itself (the debug link selects A): the pill follows at once, no request back */
static void radio_selects(void)
{
	start("the radio selects another letter");
	run("mwatch:4 quick 7 wait:2500 abkey abkey wait:300 expect:mwsel=C");
	run("dlsel:a wait:300 expect:mwsel=A");
	layout();
}

typedef void (*case_fn)(void);
static const case_fn all[] = { ring4, ring6, ring_p25, pair_p25, sel_during_tx, radio_selects };

int main(void)
{
	int bad = 0, cases = 0;
	formatter();
	bad += fails;
	for (int lang = 0; lang < 2; lang++) {
		for (int lt = 0; lt < 2; lt++) {
			for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
				fflush(stdout);
				pid_t pid = fork();
				if (pid == 0) {
					turkish = lang != 0;
					large = lt != 0;
					all[i]();
					fflush(stdout);
					_exit(fails != 0);
				}
				int st = 0;
				waitpid(pid, &st, 0);
				cases++;
				bad += !WIFEXITED(st) || WEXITSTATUS(st) != 0;
			}
		}
	}
	printf("Plus Home details: the formatter, %d Home cases (English and Turkish, normal and Large text): %d failures\n",
	       cases, bad);
	return bad != 0;
}
