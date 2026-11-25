/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 15 scan job UI flows (tests/plus/test_ui_scan.c): upstream's UI simulator (tests/host/sim/ui_sim.c)
 * as it is, built in place of it, with the P25 / DMR search lists the test sets and their requests recorded, each
 * start decoded as the radio core decodes it (radio/srange.h: the step rides on the start). */
#include "sim/ui_sim.c"
#include "radio/srange.h"
#include "ui_sim_scan.h"

sc_sim_t sc_sim;

void sc_sim_clear(void)
{
	memset(&sc_sim, 0, sizeof sc_sim);
}

uint32_t sc_sim_freq(void)
{
	return view.ch.rx_hz;
}

static void search_req(const char *what, uint32_t lo_hz, uint32_t hi_hz)
{
	size_t n = strlen(sc_sim.req);
	if (lo_hz == 0)
		snprintf(sc_sim.req + n, sizeof sc_sim.req - n, "%s%s:stop", n ? " " : "", what);
	else
		snprintf(sc_sim.req + n, sizeof sc_sim.req - n, "%s%s:%lu-%lu/%lu", n ? " " : "", what,
			 (unsigned long)(srange_lo_of(lo_hz) / 1000u), (unsigned long)(hi_hz / 1000u),
			 (unsigned long)srange_step_hz(srange_tag_of(lo_hz)));
	sc_sim.requests++;
}

bool p25s_snapshot(p25s_view_t *out)
{
	*out = sc_sim.p25;
	return true;
}

void radio_req_p25_search(uint32_t lo_hz, uint32_t hi_hz)
{
	search_req("p25s", lo_hz, hi_hz);
	sc_sim.p25.phase = lo_hz != 0 ? P25S_SWEEP : P25S_OFF;
	sc_sim.p25.at_hz = srange_lo_of(lo_hz);
}

bool dmrs_snapshot(dmrs_view_t *out)
{
	*out = sc_sim.dmr;
	return true;
}

void radio_req_dmr_search(uint32_t lo_hz, uint32_t hi_hz)
{
	search_req("dmrs", lo_hz, hi_hz);
	sc_sim.dmr.phase = lo_hz != 0 ? DMRS_SWEEP : DMRS_OFF;
	sc_sim.dmr.at_hz = srange_lo_of(lo_hz);
}

void radio_req_mode(uint8_t mode)
{
	view.ch.mode = mode;
	changed();
}

void radio_req_dmr_color_code(uint8_t cc)
{
	view.dmr.cc = cc;
	changed();
}

void radio_req_dmr_timeslot(uint8_t ts)
{
	view.dmr.ts = ts;
	changed();
}

void sc_sim_changed(void)
{
	changed();	/* the radio's view changed: the screen on top draws again (as the search's own updates do) */
}

bool sc_sim_text(const char *want, int *y0, int *y1, bool *focus)
{
	for (int i = 0; i < live_n; i++) {
		if (strstr(live[i].s, want) == NULL)
			continue;
		*y0 = live[i].y0;
		*y1 = live[i].y1;
		/* the cell's first pixel or the row's last (either may be a glyph's) */
		*focus = gfx_get_pixel(live[i].x0, live[i].y0) == ui_col.focus ||
			 gfx_get_pixel(gfx_width() - 1, live[i].y0) == ui_col.focus;
		return true;
	}
	return false;
}

bool sc_sim_text_is(const char *want)
{
	for (int i = 0; i < live_n; i++)
		if (strcmp(live[i].s, want) == 0)
			return true;
	return false;
}

int sc_sim_hint_y(void)
{
	return ui_layout.hint_y;
}

#if CONFIG_ALL_SEARCH
/* The all digital search (srchfix): its view is the DMR view's, its requests recorded as "alls:" */
bool dmrs_snapshot_all(dmrs_view_t *out)
{
	*out = sc_sim.dmr;
	return true;
}

void radio_req_all_search(uint32_t lo_hz, uint32_t hi_hz)
{
	search_req("alls", lo_hz, hi_hz);
	sc_sim.dmr.phase = lo_hz != 0 ? DMRS_SWEEP : DMRS_OFF;
	sc_sim.dmr.at_hz = srange_lo_of(lo_hz);
}

const char *dmrs_mode_name(uint8_t mode)
{
	static const char names[DMRS_MODES][5] = { "DMR", "P25", "YSF", "NXDN" };
	return mode < DMRS_MODES ? names[mode] : "?";
}

void dmrs_call_unpack(uint32_t hi, uint32_t lo, char out[11])
{
	(void)hi;
	(void)lo;
	memcpy(out, "TA2ABC/P", 9);
}
#endif
