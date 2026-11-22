// SPDX-License-Identifier: GPL-3.0-or-later
// DDM32 Plus air watch (CONFIG_AIR_WATCH, item 44b, docs/plus.md "Round 14: air band and spectrum"): Tools >
// Airfields' "Watch all" (# on an airfield's page) puts up to UI_MW_MAX of its frequencies on the multi-watch ring,
// letters A..F each its own VFO (VFOs 0..5), and the ring runs as multi-watch does: the one receiver hops between
// them, a signal holds it, each received in AM because it is in the air band (and never transmitted on); an ATIS
// letter (the last ones) holds it 5 s at most, then is passed over 60 s (radio_airwatch.inc). Included at
// the end of core/ui/ui_dual.c, after ui_mwatch.inc (tools/plus_air.py).
//
// The user's watch waits in mw_keep (on or off, its letters, the selected one) and nothing of the air watch is saved:
// state_save and mw_save return while it runs, and the radio core notes nothing either (radio_airwatch.inc). Stopping
// it the way any watch stops (Back twice, # 5, * 7, the watch key) brings the user's watch back as it was: the radio
// core restores the position and VFOs A / B from the settings, then the kept ring is asked for again. A power cycle
// during it comes back as before it too.

static struct {
	bool on;			// the user's watch ran
	uint8_t sel, p1, cnt, valid;	// its selected pane, ring letter in pane 1, letters, letters with a channel
	ui_dual_ref_t ref[2], mw[UI_MW_MAX];
} mw_keep;

// DDM32 Plus: the air watch runs (or its end is not taken yet): pages that key or search refuse then (UI_AIR_ON)
bool ui_air_watch_on(void)
{
	return mw_air;
}

// The air watch stopped (any way the watch stops: the tick finds it off): the radio and the user's watch as they were
static bool __attribute__((noinline)) mw_air_end(void)
{
	ui_dual_t *d = dual();
	mw_air = false;
	ui_svc_air_end();	// the ring stops; VFOs A / B and the position from the settings
	int32_t v = ui_svc_setting_get(UI_SET_MWATCH);	// Watch channels, changed during it or not
	mw_n = (uint8_t)(v < 2 ? 2 : v > UI_MW_MAX ? UI_MW_MAX : v);
	mw_p1 = mw_keep.p1;
	mw_cnt = mw_keep.cnt;
	mw_valid = mw_keep.valid;
	memcpy(ref, mw_keep.ref, sizeof ref);
	memcpy(mw_ref, mw_keep.mw, sizeof mw_ref);
	mw_rx = UI_DUAL_NONE;
	memset(mw_heard, 0, sizeof mw_heard);
	d->on = false;
	d->back_to = UI_DUAL_NONE;
	if (mw_keep.on && apply(mw_keep.sel))
		watch_on(d);
	ui_toast(MW_AIR_OFF);
	return true;
}

bool ui_dual_air_watch(const uint32_t hz[], uint8_t n, uint8_t atis)
{
	ui_dual_t *d = dual();
	if (n < 2 || n > UI_MW_MAX)
		return false;
	if (!mw_air) {	// the user's watch, as it is now
		if (d->on)
			mw_sync();
		mw_keep.on = d->on;
		mw_keep.sel = d->back_to != UI_DUAL_NONE ? d->back_to : d->sel;
		mw_keep.p1 = mw_p1;
		mw_keep.cnt = mw_cnt;
		mw_keep.valid = mw_valid;
		memcpy(mw_keep.ref, ref, sizeof ref);
		memcpy(mw_keep.mw, mw_ref, sizeof mw_ref);
	}
	if (ui_st.scanning) {
		ui_svc_scan(false);
		ui_st.scanning = false;
	}
	// The radio core keeps the user's state first (radio_airwatch.inc); an ATIS letter is marked
	for (uint8_t k = 0; k < n; k++)
		ui_svc_air_vfo((uint8_t)(k | (((atis >> k) & 1u) != 0 ? UI_AIR_ATIS : 0u)), hz[k]);
	mw_air = true;
	d->on = false;
	d->back_to = UI_DUAL_NONE;
	mw_n = n;
	mw_p1 = 1;
	mw_valid = (uint8_t)(MW_BIT(n) - 1u);
	mw_rx = UI_DUAL_NONE;
	mw_busy_seen = 0;
	mw_p25_said = false;
	memset(mw_heard, 0, sizeof mw_heard);
	for (uint8_t k = 0; k < UI_MW_MAX; k++)
		mw_ref[k] = (ui_dual_ref_t){ true, 0, k };	// each letter its own VFO
	ref[0] = mw_ref[0];
	ref[1] = mw_ref[1];
	if (!apply(0)) {
		mw_air_end();
		return false;
	}
	watch_on(d);
	// The radio core takes the frequencies before the ring (its queue's order); Home shows them at once
	for (uint8_t k = 0; k < n; k++) {
		mw_row[k].rx_hz = hz[k];
		mw_row[k].mode = RADIO_MODE_FM;
	}
	d->pane[0].rx_hz = hz[0];
	d->pane[1].rx_hz = hz[1];
	return true;
}
