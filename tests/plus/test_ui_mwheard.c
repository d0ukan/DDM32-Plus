/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus multi-watch: the heard call's talk group on Home (docs/plus.md "Multi-watch: the heard talk group"; tools/plus_mwfix.py
 * seventh round, integration/radiofoundry/mwatch/ui_mwatch.inc mw_heard_short, ui_mwatch_draw.inc). Built like
 * tests/plus/test_ui_mwdetail.c plus CONFIG_P25_CALLER / CONFIG_P25_EXTRAS with the P25 worker's caller queries stubbed here
 * (tests/plus/test_p25x_worker.c checks them against the real worker). A call on a watch letter says its own talk group
 * (the receiver's report, not the channel's settings): DMR group and private calls on a letter in a pane (A: the dual
 * watch tick's caller, the text that fills in after the call's start) and outside the panes (C), a P25 call on a P25 VFO
 * letter, 4 and 6 letters, normal and Large text (one line per letter), with the simulator's layout checks. Each case runs
 * in a child process. */
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


/* The call history (CONFIG_CALL_LOG, the UI's Recent) is not under test: no call logged */
uint32_t p25_lab_call(p25_lab_call_t *out)
{
	memset(out, 0, sizeof *out);
	return 0;
}

/* The P25 worker's caller, one call at a time: tg 0 = the link control has not named it yet */
static uint16_t stub_tg;
static uint32_t stub_src = 77285, stub_target;
static bool stub_private;
uint32_t p25_lab_p25_caller(uint16_t *tg, uint32_t *src)
{
	if (tg)
		*tg = stub_tg;
	if (src)
		*src = stub_src;
	return stub_tg;
}
unsigned p25_lab_p25_kind(uint32_t *target)
{
	if (target)
		*target = stub_target;
	return stub_private ? (unsigned)P25_LAB_PRIVATE : 0u;
}
size_t p25_lab_p25_caller_line(char *out, size_t cap, bool pane)
{
	(void)pane;
	if (stub_tg == 0) {
		out[0] = '\0';
		return 0;
	}
	/* the worker's pane form: "TG 101 ID 77285 NAC $293/659" | "TG101 ID77285" */
	return (size_t)snprintf(out, cap, "\x1d" "TG %u ID %u NAC $293/659\x1f" "TG%u ID%u\x1e", stub_tg, stub_src, stub_tg,
				stub_src);
}

static int fails;
static const char *name;
static bool large;

static bool run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, large ? "large" : "normal", script, err);
		fails++;
		return false;
	}
	return true;
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int i = 0; i < n; i++)
		printf("FAIL %s (%s): layout: %s\n", name, large ? "large" : "normal", issues[i]);
	fails += n;
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	if (large) {
		ui_svc_setting_set(UI_SET_LARGE_TEXT, 1);
		run("reboot");	/* the setting is read at start */
	}
	run("wait:200");
}

/* A on a pane: the call starts with no name and no group yet (the receiver names them later), then they come in: the row
 * must follow (before: it kept the first text, "DMR call", for the whole call) */
static void dmr_pane_fills_in(void)
{
	start("A (in a pane): the group comes in after the start");
	run("mwatch:4 quick 7 wait:2500 tune:a rx:0,0,g wait:300 expect:mwrx=A");
	if (!large)
		run("expect:shows=DMR_call");
	run("rx:3101234,91,g wait:300");
	run("expect:shows=TG_91 expect:hides=DMR_call");
	layout();
	run("rxend wait:2000 expect:mwrx=-");
	run("expect:hides=TG_91");	/* back to the channel details (and how long ago) */
	layout();
}

/* C outside the panes: its own tick */
static void dmr_outside(void)
{
	start("C (outside the panes): a group call");
	run("mwatch:4 quick 7 wait:2500 tune:c rx:3101234,91,g wait:300 expect:mwrx=C expect:shows=TG_91");
	layout();
}

/* A private call: the short form "Pvt <caller>" (a row with a name says "to you" where it fits) */
static void dmr_private(void)
{
	start("A: a private call");
	run("mwatch:4 quick 7 wait:2500 tune:a rx:3101234,2345,p wait:300 expect:mwrx=A");
	run(large ? "expect:shows=Pvt" : "expect:shows=to_you");
	layout();
}

/* 6 letters: shorter rows (the simulator's zone: A COUNTY DMR, B County fire FM, C Metro DMR) */
static void dmr_six(void)
{
	start("6 letters: a group call on C");
	run("mwatch:6 quick 7 wait:2500 tune:c rx:3101234,91,g wait:300 expect:mwrx=C expect:shows=TG_91");
	layout();
}

/* Dual watch (2): the pane says the group */
static void dmr_pair(void)
{
	start("dual watch: a group call on A");
	run("mwatch:2 quick 7 wait:2500 tune:a rx:0,0,g wait:300 rx:3101234,91,g wait:300");
	if (!large)	/* Large text: the panes name the letters only (nothing under them) */
		run("expect:shows=TG_91");
	layout();
}

/* C a P25 VFO (TG 99, NAC $456 as its settings): the heard call is TG 101 (another group), first not named yet */
static void p25_letter(void)
{
	start("C (a P25 VFO): the heard group, not the channel's");
	run("mwatch:4 quick 7 wait:500 abkey abkey vfokey quick 4 4 ctx down down down ok 9 9 ok down ok 1 1 1 0 ok back back "
	    "wait:2500 expect:vmode=P25");
	stub_tg = 0;
	run("expect:vmode=P25 expect:mwat=C rx:0,0,g wait:300 expect:mwrx=C");
#ifdef MWH_SHOW
	run("expect:shows=SHOW_THE_SCREEN");
#endif
	if (!large)
		run("expect:shows=P25_call");
	stub_tg = 101;
	run("wait:300 expect:shows=101");
	if (!large)
		run("expect:shows=77285");
	layout();
	stub_private = true;
	stub_target = 1234;
	run("wait:300");
	if (large)
		run("expect:shows=Pvt");
	layout();
}

typedef void (*case_fn)(void);
static const case_fn all[] = { dmr_pane_fills_in, dmr_outside, dmr_private, dmr_six, dmr_pair, p25_letter };

int main(void)
{
	int bad = 0, cases = 0;
	for (int lt = 0; lt < 2; lt++) {
		for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
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
	printf("Plus multi-watch heard talk group: %d Home cases (normal and Large text): %d failures\n", cases, bad);
	return bad != 0;
}
