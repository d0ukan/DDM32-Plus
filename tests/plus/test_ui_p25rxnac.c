/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus: the NAC a P25 call was received on, on Home's caller line (integration/radiofoundry/p25_lab.c,
 * p25_lab_p25_caller_line) in the real UI with the real fonts: the simulator's 240x320 DM-32 screen (10 px hidden left
 * and right: 220 px safe width), a P25 VFO, normal and Large text. The worker is stubbed with the forms it builds for
 * a channel on NAC Any (tests/plus/test_p25rxnac.c checks the real worker's output, these strings are its forms).
 * Each call is checked for its NAC on Home and by the simulator's layout check (nothing past the safe area), at the
 * worst case (TG 65535, ID 16777214) and with short numbers, in English and Turkish. Built like
 * tests/plus/test_ui_mwheard.c. Each case runs in a child process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/gfx.h"
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

uint32_t p25_lab_call(p25_lab_call_t *out)
{
	memset(out, 0, sizeof *out);
	return 0;
}

/* The stubbed call: TG, ID, the NAC the call came on, an emergency */
static uint16_t stub_tg;
static uint32_t stub_src;
static unsigned stub_nac;
static bool stub_emergency;
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
		*target = 0;
	return stub_emergency ? (unsigned)P25_LAB_EMERGENCY : 0u;
}
/* The worker's Home forms on NAC Any (what test_p25rxnac.c checks): "P25 TG 101 ID 77285 NAC $1C8/456 BER 1.2%" |
 * "TG101 ID77285 NAC$1C8/456 BER1.2%" | "TG101 ID77285 $1C8 BER1.2%" | "TG101 ID77285 $1C8"; an emergency leads
 * every form with "EMERGENCY " (TR: "ACIL DURUM ") instead of "P25 " */
size_t p25_lab_p25_caller_line(char *out, size_t cap, bool pane)
{
	if (stub_tg == 0 || pane) {
		out[0] = '\0';
		return 0;
	}
	unsigned n = stub_nac;
	if (stub_emergency)
		return (size_t)snprintf(out, cap,
					"\x1d%s TG%u ID%u $%03X\x1f" "EMG TG%u ID%u $%03X\x1f" "EMG ID%u $%03X\x1e", TR("EMERGENCY"),
					stub_tg, stub_src, n, stub_tg, stub_src, n, stub_src, n);
	/* the four forms, the leading ones left out until the whole fits cap (as the worker does) */
	char f[4][64];
	snprintf(f[0], sizeof f[0], "P25 TG %u ID %u NAC $%03X/%u BER 1.2%%", stub_tg, stub_src, n, n);
	snprintf(f[1], sizeof f[1], "TG%u ID%u NAC$%03X/%u BER1.2%%", stub_tg, stub_src, n, n);
	snprintf(f[2], sizeof f[2], "TG%u ID%u $%03X BER1.2%%", stub_tg, stub_src, n);
	snprintf(f[3], sizeof f[3], "TG%u ID%u $%03X", stub_tg, stub_src, n);
	for (unsigned first = 0; first < 3u; first++) {
		size_t len = 0;
		char tmp[200];
		tmp[len++] = '\x1d';
		for (unsigned i = first; i < 4u; i++)
			len += (size_t)snprintf(tmp + len, sizeof tmp - len, "%s%s", i > first ? "\x1f" : "", f[i]);
		tmp[len++] = '\x1e';
		tmp[len] = '\0';
		if (len < cap || first == 2u) {
			snprintf(out, cap, "%s", tmp);
			return strlen(out);
		}
	}
	return 0;
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

static void start(const char *n, unsigned lang)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	ui_svc_setting_set(UI_SET_HINTS, 1);	/* Home's hint row carries the caller line */
	ui_svc_setting_set(UI_SET_LANGUAGE, lang ? (uint8_t)'t' << 8 | (uint8_t)'r' : 0);
	if (large)
		ui_svc_setting_set(UI_SET_LARGE_TEXT, 1);
	run("reboot");	/* the settings are read at start */
	run("wait:200");
}

typedef struct {
	const char *what;
	uint16_t tg;
	uint32_t src;
	unsigned nac;
	bool emergency;
} call_t;

static const call_t calls[] = {
	{ "worst case", 65535, 16777214u, 0x1C8, false },
	{ "worst case, NAC $659", 65535, 16777214u, 0x659, false },
	{ "worst case, NAC $F7E", 65535, 16777214u, 0xF7E, false },
	{ "worst case, NAC $000", 65535, 16777214u, 0x000, false },
	{ "TG 101 ID 77285", 101, 77285, 0x1C8, false },
	{ "TG 1 ID 1", 1, 1, 0x1C8, false },
	{ "TG 65535 ID 1234567", 65535, 1234567u, 0x1C8, false },
	{ "emergency, worst case", 65535, 16777214u, 0x1C8, true },
	{ "emergency, TG 101", 101, 77285, 0x1C8, true },
};

/* What Home's hint row (ui_draw_hint) takes of the line: the first form that fits the row in the layout's small font,
 * else the last. Its width is the real font's */
static int home_form(char *shown, size_t cap)
{
	static char line[112];
	char form[64];
	bool more = true;
	int width = 0;
	if (p25_lab_p25_caller_line(line, sizeof line, false) == 0)
		return -1;
	for (int level = 0; more; level++) {
		gfx_text_alt(line, level, form, sizeof form, &more);
		width = gfx_text_width(ui_layout.small, form);
		if (width <= gfx_width())
			break;
	}
	snprintf(shown, cap, "%s", form);
	return width;
}

static void one(unsigned i, unsigned lang)
{
	const call_t *c = &calls[i];
	char label[96], hex[16], shown[64];
	snprintf(label, sizeof label, "P25 VFO, %s%s", c->what, lang ? ", Turkish" : "");
	start(label, lang);
	/* a P25 VFO on Home (tests/plus/test_ui_p25nac.c), the call heard on it */
	run("ok 4 up ok 1 4 5 5 7 5 ok quick 4 4 back wait:300");
	run("expect:vmode=P25");
	stub_tg = c->tg;
	stub_src = c->src;
	stub_nac = c->nac;
	stub_emergency = c->emergency;
	run("rx:3101234,91,g wait:600");
	int w = home_form(shown, sizeof shown);
	snprintf(hex, sizeof hex, "$%03X", c->nac);
	printf("  %s (%s, %s) %d px of %d: \"%s\"\n", c->what, large ? "large" : "normal", lang ? "TR" : "EN", w,
	       gfx_width(), shown);
	if (w < 0 || w > gfx_width() || strstr(shown, hex) == NULL) {
		printf("FAIL %s (%s): \"%s\" is %d px of %d or has no NAC\n", name, large ? "large" : "normal", shown, w,
		       gfx_width());
		fails++;
	}
	layout();
}

int main(void)
{
	int bad = 0, cases = 0;
	for (int lt = 0; lt < 2; lt++) {
		for (unsigned lang = 0; lang < 2u; lang++) {
			for (unsigned i = 0; i < sizeof calls / sizeof calls[0]; i++) {
				fflush(stdout);
				pid_t pid = fork();
				if (pid == 0) {
					large = lt != 0;
					one(i, lang);
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
	printf("Plus P25 NAC on Home: %d cases (normal and Large text, English and Turkish): %d failures\n", cases, bad);
	return bad != 0;
}
