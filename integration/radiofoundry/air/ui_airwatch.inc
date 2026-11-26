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
//
// DDM32 Plus auto scan (CONFIG_AUTO_SCAN, ui_autoscan.c): the same ring for the nearest repeaters or airfields
// (mw_air_kind AS_RPT / AS_AIR). Its letters carry a label on Home (mw_row[k].name: "TA2XYZ 12km", "LTBA TWR"; kept
// by mw_row_read while it runs) and are replaced one by one as the radio moves (ui_dual_auto_set). Any end the user
// or another feature causes ends the scan too (ui_autoscan_ended); the scan's own end (fewer than two letters, the map
// pack gone) brings the user's watch back and leaves the scan waiting.

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

// The air watch stopped: the radio and the user's watch as they were. user: the user (or another feature) stopped it,
// the toast says so; else the auto scan's own end (it says why itself and goes on waiting)
static bool __attribute__((noinline)) mw_air_finish(bool user)
{
	ui_dual_t *d = dual();
	uint8_t kind = mw_air_kind;
	mw_air = false;
	mw_air_kind = 0;	// before the user's rows are read again: no labels
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
#if CONFIG_AUTO_SCAN
	if (kind != 0) {
		if (user)
			ui_autoscan_ended();	// off, and its toast
		return true;
	}
#else
	(void)kind;
#endif
	if (user)
		ui_toast(MW_AIR_OFF);
	return true;
}

static bool mw_air_end(void)
{
	return mw_air_finish(true);
}

// The ring of n letters, each its own VFO: hz[k] with arg[k]'s bits and tone[k] (NULL: none; ui_svc_air_vfo), for the
// airfield watch (kind 0) or the auto scan (AS_AIR, AS_RPT); sel the letter selected (the pill: PTT's when nothing is
// heard). The user's watch is kept the first time
static bool __attribute__((noinline)) mw_air_ring(const uint32_t hz[], const uint32_t arg[], const uint16_t tone[],
						  uint8_t n, uint8_t kind, uint8_t sel)
{
	ui_dual_t *d = dual();
	if (n < 2 || n > UI_MW_MAX)
		return false;
	if (sel >= n)
		sel = 0;
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
	// The radio core keeps the user's state first (radio_airwatch.inc); each letter as its bits say
	for (uint8_t k = 0; k < n; k++)
		ui_svc_air_vfo(k | arg[k], hz[k], tone != NULL ? tone[k] : 0u);
	mw_air = true;
	mw_air_kind = kind;
	d->on = false;
	d->back_to = UI_DUAL_NONE;
	mw_n = n;
	mw_p1 = sel != 0 ? sel : 1u;
	mw_valid = (uint8_t)(MW_BIT(n) - 1u);
	mw_rx = UI_DUAL_NONE;
	mw_busy_seen = 0;
	mw_p25_said = false;
	memset(mw_heard, 0, sizeof mw_heard);
	for (uint8_t k = 0; k < UI_MW_MAX; k++)
		mw_ref[k] = (ui_dual_ref_t){ true, 0, k };	// each letter its own VFO
	ref[0] = mw_ref[0];
	ref[1] = mw_ref[mw_p1];
	if (!apply(sel != 0 ? 1u : 0u)) {
		mw_air_finish(kind == 0);
		return false;
	}
	watch_on(d);
	// The radio core takes the frequencies before the ring (its queue's order); Home shows them at once
	for (uint8_t k = 0; k < n; k++) {
		mw_row[k].rx_hz = hz[k];
		mw_row[k].mode = (arg[k] & UI_AIR_DMR) != 0 ? RADIO_MODE_DMR : RADIO_MODE_FM;
	}
	d->pane[0].rx_hz = hz[0];
	d->pane[1].rx_hz = hz[mw_p1];
	return true;
}

bool ui_dual_air_watch(const uint32_t hz[], uint8_t n, uint8_t atis)
{
	uint32_t arg[UI_MW_MAX];
	if (n > UI_MW_MAX)
		return false;
#if CONFIG_AUTO_SCAN
	bool was_auto = mw_air && mw_air_kind != 0;	// an auto scan gives way to the airfield's watch
#endif
	for (uint8_t k = 0; k < n; k++)
		arg[k] = ((atis >> k) & 1u) != 0 ? UI_AIR_ATIS : 0u;
	bool ok = mw_air_ring(hz, arg, NULL, n, 0, 0);
#if CONFIG_AUTO_SCAN
	if (was_auto)
		ui_autoscan_ended();
#endif
	return ok;
}

#if CONFIG_AUTO_SCAN
bool ui_dual_auto_ring(const uint32_t hz[], const uint32_t arg[], const uint16_t tone[], uint8_t n, uint8_t kind,
		       uint8_t sel)
{
	return mw_air_ring(hz, arg, tone, n, kind, sel);
}

uint8_t ui_dual_auto_sel(void)
{
	return mw_air ? mw_sel_letter() : 0u;
}

// Letter k replaced: the radio core refuses it while the ring holds a signal or transmits (ui_autoscan.c reads the VFO
// back and asks again). What the UI knew of the letter starts over (its age, a pane's snapshot)
void ui_dual_auto_set(uint8_t k, uint32_t hz, uint32_t arg, uint16_t tone)
{
	ui_dual_t *d = dual();
	if (!mw_air || k >= UI_MW_MAX)
		return;
	ui_svc_air_vfo(k | arg | UI_AIR_REPLACE, hz, tone);
	mw_row[k].rx_hz = hz;
	mw_row[k].mode = (arg & UI_AIR_DMR) != 0 ? RADIO_MODE_DMR : RADIO_MODE_FM;
	mw_heard[k] = 0;
	if (mw_rx == k)
		mw_rx = UI_DUAL_NONE;
	uint8_t pane = mw_pane_of(k);
	if (pane != UI_DUAL_NONE) {
		snap[pane].valid = false;
		chan_ok[pane] = false;
		d->ended[pane] = false;
		d->pane[pane].rx_hz = hz;
		refill[pane] = true;
	}
}

void ui_dual_auto_label(uint8_t k, const char *label)
{
	if (mw_air && k < UI_MW_MAX)
		str_copy(mw_row[k].name, sizeof mw_row[k].name, label);
}

// The whole letter, not only its RX: two repeaters often share an output (another tone, offset or colour code), and a
// refused replacement must not pass for a taken one (its TX would stay the old repeater's under the new label)
bool ui_dual_auto_has(uint8_t k, uint32_t hz, uint32_t arg, uint16_t tone)
{
	if (!ui_svc_vfo_get(k, &mw_ch) || mw_ch.rx_hz != hz)
		return false;
	uint8_t p = (uint8_t)((arg >> UI_AIR_P_SHIFT) & 0x3Fu);
	uint32_t tx = (uint32_t)((int32_t)hz + ((int32_t)arg >> UI_AIR_OFF_SHIFT) * 1250);
	if ((arg & UI_AIR_DMR) != 0)
		return mw_ch.mode == RADIO_MODE_DMR && mw_ch.tx_hz == tx && mw_ch.color_code == (p & 0x0Fu) &&
		       mw_ch.timeslot == ((p >> 4) == 2u ? 2u : 1u);
	if ((arg & UI_AIR_FM) != 0)
		return mw_ch.mode == RADIO_MODE_FM && mw_ch.tx_hz == tx &&
		       (tone != 0 ? mw_ch.tx_tone.type == TONE_CTCSS && mw_ch.tx_tone.value == tone
				  : mw_ch.tx_tone.type == TONE_NONE);
	return mw_ch.tx_hz == hz;	// an air letter: simplex (receive only)
}

void ui_dual_auto_end(void)
{
	if (mw_air)
		mw_air_finish(false);
}

uint8_t ui_dual_auto_kind(void)
{
	return mw_air ? mw_air_kind : 0;
}
#endif
