/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus VFO step key (CONFIG_VFO_STEPKEY, tools/plus_vfostep.py, docs/plus.md "VFO step key") through the real UI
 * in upstream's simulator, on the DM-32UV's display class (240 x 320, insets 10 x 5), English and Turkish, normal and
 * Large text, with the simulator's layout checks. Each case runs in a child process. The A/B key (SK2's short press) is
 * the script token "abkey". Covers:
 *   - the VFO on Home: the hint row names the key, the place row says the step; the key goes round Auto, 2.5, 5, 6.25,
 *     8.33, 10, 12.5, 20, 25, 50, 100 kHz and back to Auto, each press a toast and the view's step as the menu row's
 *     values give it (the same ui_svc_apply request); the frequency is not touched;
 *   - the key as the menu sets it: a step set through Settings > Talk reads on in the cycle;
 *   - not on a channel: no hint, the key starts dual watch as it did; the step stays;
 *   - not in dual watch (it selects the other letter) and not while a scan runs or a frequency is typed;
 *   - SK2 given another job in Settings > Keys: the hint is gone. */
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
#include "input/keyassign.h"

static int fails;
static const char *name;
static bool turkish, large;

#define T(en, tr) (turkish ? (tr) : (en))

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

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s (%s%s): %s\n", name, turkish ? "tr" : "en", large ? ", large" : "", what);
		fails++;
	}
}

static void layout(void)
{
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
		run("reboot");	/* the setting is read at start */
	}
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	run("wait:200");
}

#define VFO "ok 4 up ok "	/* Zones > VFO, as the simulator's flows do */

static uint32_t step_hz(void) { return ui_view()->vfo_step_hz; }

/* The hint names the key, the place row the step; the cycle goes round and comes back to Auto */
static void cycle(void)
{
	static const struct { uint32_t hz; const char *en; } want[] = {
		{ 2500, "Step_2.5_kHz" }, { 5000, "Step_5_kHz" }, { 6250, "Step_6.25_kHz" }, { 8333, "Step_8.33_kHz" },
		{ 10000, "Step_10_kHz" }, { 12500, "Step_12.5_kHz" }, { 20000, "Step_20_kHz" }, { 25000, "Step_25_kHz" },
		{ 50000, "Step_50_kHz" }, { 100000, "Step_100_kHz" }, { 25000, "Step_Auto" },
	};
	start("VFO: the step key");
	run(VFO "wait:2600");
	run(T("expect:shows=SK2_step", "expect:shows=SK2_ad"));
	if (!large)	/* Large text has no place row: the toast and the knob say it */
		run(T("expect:shows=Step_25k", "expect:shows=25k"));	/* Auto: the width's step, 25 kHz on the simulator's VFO */
	uint32_t auto_hz = step_hz();
	check(auto_hz == 25000 && ui_view()->vfo_step_auto, "starts on Auto, the width's 25 kHz");
	uint32_t rx = ui_view()->ch.rx_hz;
	for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
		run("abkey wait:100");
		char s[80];
		snprintf(s, sizeof s, "expect:toast=%s", turkish ? "Ad" : want[i].en);
		run(s);
		check(step_hz() == want[i].hz, "the step in use");
		check(ui_view()->vfo_step_auto == (i == 10), "Auto only on the last");
		check(ui_view()->vfo, "still the VFO");
		check(ui_view()->ch.rx_hz == rx, "the frequency is not touched");
		if (i == 0)
			layout();
	}
	/* the knob steps by the step the key set */
	run("abkey up wait:100");
	check(ui_view()->ch.rx_hz == rx + 2500, "Up steps by the 2.5 kHz the key set");
	layout();
	run("wait:2500");
	layout();
}

/* A step set in Settings > Talk > VFO step is where the key goes on from */
static void from_menu(void)
{
	start("VFO: the key goes on from the menu's step");
	ui_svc_apply(UI_SET_VFO_STEP, 625);
	run(VFO "wait:200");
	check(step_hz() == 6250 && !ui_view()->vfo_step_auto, "set to 6.25 kHz");
	run("abkey wait:100");
	check(step_hz() == 8333, "6.25 on to 8.33 kHz");
	ui_svc_apply(UI_SET_VFO_STEP, 10000);
	run("wait:100 abkey wait:100");
	check(ui_view()->vfo_step_auto, "100 kHz on to Auto");
	ui_svc_apply(UI_SET_VFO_STEP, 3333);	/* not one of the list's: on to Auto */
	run("wait:100 abkey wait:100");
	check(ui_view()->vfo_step_auto, "a step outside the list goes to Auto");
	layout();
}

/* On a channel there is no hint and no step: the key does what it did (starts dual watch) */
static void channel(void)
{
	start("channel: the key as before");
	run("wait:300 expect:hides=SK2_");
	check(!ui_view()->vfo, "on a channel");
	uint32_t hz = step_hz();
	run("abkey wait:300");
	check(step_hz() == hz, "the step is not touched");
	layout();
}

/* Dual watch: the A/B key selects the other letter, the step stays (a VFO letter shares the one step) */
static void dual(void)
{
	start("dual watch: the key selects");
	run(VFO "abkey abkey wait:100");	/* 2.5, 5 */
	check(step_hz() == 5000, "5 kHz set");
	run("dualkey wait:500 expect:dual=1");
	run("abkey wait:300 abkey wait:300");
	run("expect:dual=1");
	check(step_hz() == 5000, "dual watch: the step stays");
	layout();
}

/* A scan or a typed frequency: the key is not the step key then */
static void busy(void)
{
	start("scan and typing");
	run(VFO "abkey wait:100");
	check(step_hz() == 2500, "2.5 kHz set");
	run("4 4 6 wait:100");	/* a frequency being typed */
	run("abkey wait:200");
	check(step_hz() == 2500, "typing: the step stays");
	run("back back back wait:100 scankey wait:300");
	run("abkey wait:200");
	check(step_hz() == 2500, "a scan: the step stays");
	layout();
}

/* SK2 given another job (Settings > Keys): no hint */
static void reassigned(void)
{
	start("SK2 given another job");
	run(VFO "wait:2600");
	run(T("expect:shows=SK2_step", "expect:shows=SK2_ad"));
	ui_svc_setting_set(UI_SET_KEY_FIRST + 2, KEYFN_SCAN);
	run("wait:300 expect:hides=SK2_");
	layout();
}

typedef void (*case_fn)(void);
static const case_fn all[] = { cycle, from_menu, channel, dual, busy, reassigned };

int main(void)
{
	int bad = 0, cases = 0;
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
	printf("Plus VFO step key: %d cases (English and Turkish, normal and Large text): %d failures\n", cases, bad);
	return bad != 0;
}
