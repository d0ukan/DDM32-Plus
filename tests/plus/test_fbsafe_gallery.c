// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus: the safe-area frame buffer draws what the whole panel's drew (tools/plus_ramsave.py). Every screen of the
// UI simulator's gallery (in each theme), its audit screens and its worst-case screens, at the DM-32UV's class
// (240x320, insets 10 / 5) and the DM-1701's (160x120, insets 5), run twice from a reset: with the frame buffer of the
// whole panel and with the safe area's only, as the radio now builds it. gfx_flush runs after every tick, as the UI
// task's loop does, into an emulated panel (ui_sim.c sim_panel: flush_indexed rows, fill_insets). The panel's states
// (a hash each time it changed), the panel at the end, the RGB565 capture of the whole panel and every visible pixel
// must be the same both ways. Built from uiSim.c (its gallery tables) by tools/test_ramsave.py.
#define main uisim_main
#include "uiSim.c"
#undef main

#define MAX_STATES	4096
static uint64_t states[2][MAX_STATES];
static int nstates[2], side, overflow;
static int pw_, ph_;

static uint64_t panel_hash(void)
{
	const uint16_t *p = sim_panel();
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < pw_ * ph_; i++)
		h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

static void on_tick(uint32_t now_ms)
{
	(void)now_ms;
	gfx_flush();
	uint64_t h = panel_hash();
	int *n = &nstates[side];
	if (*n == 0 || states[side][*n - 1] != h) {
		if (*n == MAX_STATES) {
			overflow++;
			return;
		}
		states[side][(*n)++] = h;
	}
}

static uint16_t end_panel[2][320 * 320];
static uint8_t end_rgb[2][320 * 320 * 2];
static uint16_t end_view[2][320 * 320];

static int run_both(const sim_geom_t *base, const char *name, const char *script, int *compared)
{
	static sim_geom_t g[2];
	char err[256];
	bool ok[2];
	for (side = 0; side < 2; side++) {
		g[side] = *base;
		g[side].safe = side == 1;
		nstates[side] = 0;
		pw_ = base->w;
		ph_ = base->h;
		sim_reset(&g[side], SIM_MAP_K);
		on_tick(0);	// the first frame after the reset
		ok[side] = sim_run(script, err, sizeof err);
		on_tick(0);
		memcpy(end_panel[side], sim_panel(), sizeof end_panel[side]);
		gfx_read_rgb565(0, end_rgb[side], (size_t)base->w * base->h * 2u);
		for (int y = 0; y < gfx_height(); y++)
			for (int x = 0; x < gfx_width(); x++)
				end_view[side][y * gfx_width() + x] = gfx_get_pixel(x, y);
	}
	side = 0;
	int fails = 0;
	size_t px = (size_t)base->w * base->h;
	if (ok[0] != ok[1]) {
		printf("FAIL %s %s: the script ran one way only\n", base->name, name);
		fails++;
	}
	if (nstates[0] != nstates[1] || memcmp(states[0], states[1], sizeof states[0][0] * (size_t)nstates[0]) != 0) {
		printf("FAIL %s %s: the panel went through other states (%d / %d)\n", base->name, name, nstates[0], nstates[1]);
		fails++;
	}
	if (memcmp(end_panel[0], end_panel[1], px * 2u) != 0) {
		printf("FAIL %s %s: the panel differs at the end\n", base->name, name);
		fails++;
	}
	if (memcmp(end_rgb[0], end_rgb[1], px * 2u) != 0) {
		printf("FAIL %s %s: the RGB565 capture differs\n", base->name, name);
		fails++;
	}
	if (memcmp(end_view[0], end_view[1], sizeof end_view[0]) != 0) {
		printf("FAIL %s %s: the frame buffer differs\n", base->name, name);
		fails++;
	}
	// The panel is what the capture says (the frame buffer reached the panel whole, the insets in the clear color)
	for (size_t i = 0; i < px && fails == 0; i++)
		if (end_panel[1][i] != (uint16_t)(end_rgb[1][2 * i] | end_rgb[1][2 * i + 1] << 8)) {
			printf("FAIL %s %s: the panel is not the capture at pixel %zu\n", base->name, name, i);
			fails++;
		}
	(*compared)++;
	return fails;
}

int main(void)
{
	char name[64], script[512];
	int fails = 0, compared = 0, states_total = 0;
	const char *classes[] = { "240x320i10x5", "160x120i5" };
	gal_ncolors = 0;	// the themes, not the highlight colors
	sim_set_tick_hook(on_tick);
	for (size_t c = 0; c < sizeof classes / sizeof classes[0]; c++) {
		const sim_geom_t *g = sim_geom_find(classes[c]);
		if (g == NULL || g->bpp != 4) {
			printf("FAIL no display class %s\n", classes[c]);
			return 1;
		}
		for (size_t i = 0; i < sizeof gallery / sizeof gallery[0]; i++)
			for (int k = 0; k < GAL_SHOTS; k++)
				if (gallery_shot(i, k, g, name, sizeof name, script, sizeof script)) {
					fails += run_both(g, name, script, &compared);
					states_total += nstates[1];
				}
		for (size_t i = 0; i < sizeof gallery / sizeof gallery[0]; i++) {	// the longest names, IDs, aliases
			snprintf(script, sizeof script, "worst %s", gallery[i].script);
			fails += run_both(g, gallery[i].name, script, &compared);
		}
		for (size_t i = 0; i < sizeof audit_extra / sizeof audit_extra[0]; i++)
			fails += run_both(g, audit_extra[i].name, audit_extra[i].script, &compared);
		for (size_t i = 0; i < sizeof worst_screens / sizeof worst_screens[0]; i++) {
			snprintf(script, sizeof script, "worst %s", worst_screens[i].script);
			fails += run_both(g, worst_screens[i].name, script, &compared);
		}
	}
	if (overflow) {
		printf("FAIL more than %d panel states in a run\n", MAX_STATES);
		fails++;
	}
	if (fails) {
		printf("test_fbsafe_gallery: %d failed\n", fails);
		return 1;
	}
	printf("test_fbsafe_gallery: %d screens, %d panel states: the safe-area frame buffer shows what the whole panel's "
	       "showed\n", compared, states_total);
	return 0;
}
