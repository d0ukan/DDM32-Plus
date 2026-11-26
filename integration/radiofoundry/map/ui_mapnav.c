/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus map navigation, the UI task's side (mapnav.h, docs/map.md). Installed as core/ui/ui_mapnav.c.
 * Every UI tick (ui_app.c), at most once a second:
 *   - the GPS fix goes into the track (CONFIG_MAP_TRACK; Tools > Map draws it, 7 held clears it), and with a point kept
 *     the signal of the channel or VFO in use beside it (DDM32 Plus CONFIG_MAP_COVERAGE, ui_mapnav_cov);
 *   - with Tools > Auto repeater on (CONFIG_AUTO_RPT; RAM only, off after power off), the check (every 30 s at most):
 *     a repeater to offer becomes a banner over any screen for 8 s, "Nearer: <call> <MHz> <km> <dir>" and
 *     "# tunes the VFO"; # (the banner key) tunes it through Tools > Repeaters' own tune (ui_rpt_tune: the same
 *     requests, its checks and its toast); Back hides it. Nothing is tuned without that key, nothing transmits.
 *     No offer while another banner is up or while transmitting: it waits for the next tick. */
#include "config.h"
#if CONFIG_MAP_TRACK || CONFIG_AUTO_RPT
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "ui/mapnav.h"

#define POLL_MS		1000u

#if CONFIG_MAP_TRACK
track_t ui_mapnav_track;
#endif
#if CONFIG_MAP_COVERAGE
uint8_t ui_mapnav_cov[TRACK_POINTS];	/* DDM32 Plus coverage: cov_byte of each track point, by ring slot */
#endif
#if CONFIG_AUTO_RPT
static arpt_t arpt = { .on = false, .cur = RPT_NONE, .next_ms = 0 };
static uint16_t offered = RPT_NONE;	/* the banner's repeater */
#endif
static uint32_t polled_ms;

#if CONFIG_AUTO_RPT
static bool in_band(uint32_t hz, bool need_tx)
{
	return board_in_band(hz, need_tx);
}

/* "Nearer: SIVRITEPE 145.700 12.3 km NE" / "# tunes the VFO" */
static void __attribute__((noinline)) auto_check(bool fix, int32_t lat, int32_t lon, uint32_t now)
{
	const radio_view_t *v = ui_view();
	if (ui_st.banner.kind != UI_BANNER_NONE || v->state == RS_TX)
		return;
	bool first = arpt.cur == RPT_NONE;
	uint16_t i = arpt_check(&arpt, fix, lat, lon, v->ch.mode == RADIO_MODE_DMR, in_band, v->ch.rx_hz, now);
	if (i == RPT_NONE)
		return;
	const rpt_entry_t *e = rpt_get(i);
	if (e == NULL)
		return;
	char title[48];
	sbuf_t b;
	uint32_t m;
	uint16_t brg;
	sb_init(&b, title, sizeof title);
	sb_str(&b, first ? TR("Nearest: ") : TR("Nearer: "));
	sb_str(&b, e->name);
	sb_chr(&b, ' ');
	sb_freq(&b, e->rx_hz);
	if (rpt_way(e, lat, lon, &m, &brg)) {
		sb_chr(&b, ' ');
		sb_range(&b, m, brg);
	}
	offered = i;
	ui_banner_offer(UI_BANNER_RPT, title, TR("# tunes the VFO"));
}

bool ui_mapnav_auto_on(void)
{
	return arpt.on;
}

void ui_mapnav_auto_toggle(void)
{
	ui_position_t p;
	if (!arpt.on && rpt_count() == 0u) {	/* no map pack (or one without repeaters): stays off */
		ui_toast(TR("No repeater data: load the map pack"));
		return;
	}
	arpt_set(&arpt, !arpt.on, ui_st.now_ms);
	offered = RPT_NONE;
	if (ui_st.banner.kind == UI_BANNER_RPT)
		ui_st.banner.kind = UI_BANNER_NONE;	/* an offer still up goes with it */
	ui_toast(!arpt.on ? TR("Auto repeater off") : ui_svc_position(&p) && p.fix ? TR("Auto repeater on")
		 : TR("Auto repeater on: waiting for GPS"));
}

void ui_mapnav_banner_open(void)
{
	if (offered < rpt_count())
		ui_rpt_tune(offered);
}
#endif

static bool __attribute__((noinline)) poll(uint32_t now)
{
	ui_position_t p;
	bool fix = ui_svc_position(&p) && p.fix, redraw = false;
#if CONFIG_MAP_TRACK
	if (fix && track_add(&ui_mapnav_track, p.lat_e7, p.lon_e7, now)) {
#if CONFIG_MAP_COVERAGE
		/* the point just kept is the newest: the signal of the channel or VFO in use now beside it */
		ui_mapnav_cov[track_slot(&ui_mapnav_track, (uint8_t)(ui_mapnav_track.n - 1u))] = cov_byte(ui_view()->rssi_dbm);
#endif
		redraw = ui_frame()->scr == SCR_MAP;
	}
#endif
#if CONFIG_AUTO_RPT
	if (arpt.on)
		auto_check(fix, p.lat_e7, p.lon_e7, now);
#endif
	return redraw;
}

bool ui_mapnav_tick(uint32_t now_ms)
{
	if (now_ms - polled_ms < POLL_MS)
		return false;
	polled_ms = now_ms;
	return poll(now_ms);
}
#endif
