/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools menu icons (CONFIG_PLUS_ART, docs/art.md) through the real UI in upstream's simulator, on the
 * DM-32UV's display class (240 x 320, insets 10 x 5, key map M), dark and light theme. Every Tools entry is focused in
 * turn; the icon drawn for the focused row (the only one in the focus colour) is recorded through the linker's
 * --wrap of art_icon. Checks: every entry has an icon; DOTA Transfer, Cross-band, CW decoder, SOS key and CW beacon
 * each have an icon of their own; every other icon an entry shares is one of the deliberate families below; every
 * icon is drawn inside the frame buffer (the safe area); the layout check finds nothing. TOOLICON_SHOTS: a directory
 * for the Tools menu as PNG (one file per theme and page). The radio core's side is not built (the linker leaves the
 * screens' engine calls unresolved: nothing here opens a tool). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim/ui_sim.h"
#include "ui/art.h"
#include "ui/gfx.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/ui_screens.h"
#include "ui/sos.h"
#include "ui/ui_xband.h"
#include "ui/ui_dota.h"
#include "radio/xband.h"

/* The screens behind the Tools entries are not opened here: empty definitions stand in for them, and the radio core's
   side of their engines reports nothing running */
#define STUB(n) const ui_scr_def_t n = { 0 }
STUB(ui_acars_screen);
STUB(ui_beacon_screen);
STUB(ui_cw_screen);
STUB(ui_cwtrain_screen);
STUB(ui_dota_pick_screen);
STUB(ui_dota_screen);
STUB(ui_msgview_screen);
STUB(ui_pager_screen);
STUB(ui_sos_screen);
STUB(ui_sos_set_screen);
STUB(ui_xband_pick_screen);
STUB(ui_xband_screen);
bool ui_dota_tick(uint8_t top, uint32_t now) { (void)top, (void)now; return false; }
bool ui_msg_tick(uint8_t scr, uint16_t arg) { (void)scr, (void)arg; return false; }
bool ui_sos_key(const input_event_t *ev) { (void)ev; return false; }
bool ui_sos_tick(uint32_t now_ms) { (void)now_ms; return false; }
bool ui_xband_lock(const input_event_t *ev) { (void)ev; return false; }
const char *ui_xband_home(const char *bottom) { return bottom; }
bool ui_xband_tick(uint8_t top, uint32_t now) { (void)top, (void)now; return false; }
bool xband_on(void) { return false; }

#define MAXCALLS	64
#define MAXTOOLS	64

typedef struct { unsigned id; int x, y; uint16_t color; } call_t;
static call_t calls[MAXCALLS];
static int ncalls, outside, drawn_total;

void __real_art_icon(unsigned id, int x, int y, uint16_t color);
void __wrap_art_icon(unsigned id, int x, int y, uint16_t color);
void __wrap_art_icon(unsigned id, int x, int y, uint16_t color)
{
	if (ncalls < MAXCALLS)
		calls[ncalls++] = (call_t){ id, x, y, color };
	drawn_total++;
	if (x < 0 || y < 0 || x + ART_ICON_W > gfx_width() || y + ART_ICON_H > gfx_height())
		outside++;
	__real_art_icon(id, x, y, color);
}

static int fails;
static void check(bool c, const char *what, const char *arg)
{
	if (!c) {
		printf("FAIL %s%s%s\n", what, arg ? ": " : "", arg ? arg : "");
		fails++;
	}
}

static void run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s: %s\n", script, err);
		fails++;
	}
}

static const char *const icon_names[] = {
	"SCAN", "POSITION", "HOTSPOT", "USB_NET", "SFR", "ROAM", "DIAG", "APRS", "SPECTRUM", "FOX", "BEACON", "LONE",
	"ALMANAC", "GOTO", "P25SEARCH", "AIRBAND", "BATTERY", "ZONE_OFFER", "MAP", "SETTINGS", "ZONES", "CONTACTS",
	"MESSAGES", "DUAL", "LANGUAGE", "CHANNELS", "RECENT", "LAST_CALLER", "DOTA", "XBAND", "CW", "SOS",
};

/* Icons the list may repeat on purpose: a family of tools that are one idea (docs/art.md). Anything else shared fails */
static bool family(unsigned id)
{
	return id == ART_ICON_ALMANAC || id == ART_ICON_GOTO || id == ART_ICON_P25SEARCH || id == ART_ICON_AIRBAND ||
	       id == ART_ICON_RECENT || id == ART_ICON_MAP || id == ART_ICON_POSITION || id == ART_ICON_SPECTRUM;
}

typedef struct { char label[48]; unsigned icon; } entry_t;
static entry_t entries[MAXTOOLS];
static int nentries;

static uint16_t bg(const call_t *c)
{
	return gfx_get_pixel(c->x + ART_ICON_W + 2, c->y + ART_ICON_H / 2);
}

static void shot(const char *theme, int page)
{
	const char *dir = getenv("TOOLICON_SHOTS");
	if (dir == NULL)
		return;
	char path[512];
	snprintf(path, sizeof path, "%s/tools-%s-%d.png", dir, theme, page);
	check(sim_save_png(path, 2), "png", path);
}

static void walk(const char *theme)
{
	char script[64];
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
	snprintf(script, sizeof script, "theme:%s wait:100", theme);
	run(script);
	ncalls = 0;
	ui_push(SCR_TOOLS, 0);
	run("wait:200");
	/* the focus bar: the background in the gap between a row's icon and its text. Row 0 is focused to start with, so
	   its background is the focus one and the next row's is not */
	uint16_t focus = 0;
	bool have = ncalls >= 3 && bg(&calls[0]) != bg(&calls[1]) && bg(&calls[1]) == bg(&calls[2]);
	if (have)
		focus = bg(&calls[0]);
	check(have, "the focused row can be told by its background", theme);
	int n = 0;
	char prev[48] = "";
	for (int step = 0; step < MAXTOOLS && have; step++) {
		const char *label = ui_app_focus_label();
		if (step > 0 && strcmp(label, entries[0].label) == 0 && n > 1)
			break;	/* wrapped around */
		if (step > 0 && strcmp(label, prev) == 0)
			break;
		int found = -1;
		for (int i = 0; i < ncalls; i++)
			if (bg(&calls[i]) == focus)
				found = i;
		if (strcmp(theme, "dark") == 0) {
			snprintf(entries[n].label, sizeof entries[n].label, "%s", label);
			entries[n].icon = found >= 0 ? calls[found].id : 9999;
			check(found >= 0, "the focused row has an icon", label);
			n++;
		}
		else {
			check(n < nentries && strcmp(entries[n].label, label) == 0, "the same list in both themes", label);
			check(found >= 0 && n < nentries && entries[n].icon == calls[found].id, "the same icon in both themes",
			      label);
			n++;
		}
		snprintf(prev, sizeof prev, "%s", label);
		if (step == 0 || strcmp(label, "Cross-band") == 0 || strcmp(label, "Battery") == 0)
			shot(theme, step);
		ncalls = 0;
		run("down wait:60");
	}
	if (strcmp(theme, "dark") == 0)
		nentries = n;
	check(n == nentries && n > 10, "all the Tools entries walked", theme);
	char lay[SIM_LAYOUT_MAX][160];
	int issues = sim_layout_issues(lay, SIM_LAYOUT_MAX);
	for (int i = 0; i < issues; i++)
		check(false, "layout", lay[i]);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	walk("dark");
	walk("light");
	check(drawn_total > 0 && outside == 0, "every icon inside the frame buffer", NULL);

	/* the entries by name */
	static const struct { const char *label; unsigned icon; } own[] = {
		{ "DOTA Transfer", ART_ICON_DOTA }, { "Cross-band", ART_ICON_XBAND }, { "CW decoder", ART_ICON_CW },
		{ "CW beacon", ART_ICON_BEACON },
	};
	for (unsigned k = 0; k < sizeof own / sizeof own[0]; k++) {
		int at = -1;
		for (int i = 0; i < nentries; i++)
			if (strcmp(entries[i].label, own[k].label) == 0)
				at = i;
		check(at >= 0, "the Tools list has", own[k].label);
		if (at < 0)
			continue;
		check(entries[at].icon == own[k].icon, "its own icon", own[k].label);
		for (int i = 0; i < nentries; i++)
			if (i != at && entries[i].icon == entries[at].icon) {
				char m[120];
				snprintf(m, sizeof m, "%s shares its icon with %s", own[k].label, entries[i].label);
				check(false, m, NULL);
			}
	}
	/* every other repeat is a deliberate family */
	for (int i = 0; i < nentries; i++)
		for (int j = 0; j < i; j++)
			if (entries[i].icon == entries[j].icon && !family(entries[i].icon)) {
				char m[140];
				snprintf(m, sizeof m, "%s and %s share icon %u", entries[i].label, entries[j].label, entries[i].icon);
				check(false, m, NULL);
			}
	for (int i = 0; i < nentries; i++)
		printf("  %-24s %s\n", entries[i].label,
		       entries[i].icon < sizeof icon_names / sizeof icon_names[0] ? icon_names[entries[i].icon] : "none");
	printf("tool icons: %d entries, %d icon draws: %s\n", nentries, drawn_total, fails ? "FAILED" : "ok");
	return fails != 0;
}
