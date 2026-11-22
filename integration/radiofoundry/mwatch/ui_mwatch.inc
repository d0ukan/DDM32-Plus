// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus multi-watch (CONFIG_MULTI_WATCH, docs/plus.md "Multi-watch"): Settings > Talk & listen > Watch channels,
// 2 to 6. With 3 or more, dual watch's pair becomes a ring of channels A..F on the one receiver. Included at the end
// of core/ui/ui_dual.c (tools/plus_features.py prepare_multi_watch).
//
// How it sits on dual watch: ui_dual.c keeps its two panes. Pane 0 is always A; pane 1 holds one of B..F (mw_p1),
// the selected one whenever A isn't. So every rule of dual watch for the selected channel (PTT, Up / Down, the knob,
// quick settings, the talk target, the VFO key, Find, the zone keys) acts on the selected letter unchanged. The other
// letters are only references here (mw_ref, 6 bytes each, and a short row for Home); the radio core gets the whole
// ring (ui_svc_mwatch) and alternates through it. Selecting a letter outside the panes swaps it into pane 1. With 2
// watch channels none of this runs: dual watch exactly as upstream.

#define MW_BIT(k)	(1u << (k))

static uint8_t mw_n = 2;		// the setting: channels to watch, 2..UI_MW_MAX
static uint8_t mw_cnt;			// letters in the ring now (0: no ring, dual watch's pair)
static uint8_t mw_p1 = 1;		// the letter pane 1 holds
static uint8_t mw_valid;		// bit k: mw_ref[k] names a channel
static uint8_t mw_rx = UI_DUAL_NONE;	// the letter a call is coming in on now
static uint8_t mw_busy_seen;		// the view's dual_busy at the last tick
static uint8_t mw_p1_saved = 0xFF;	// the pane 1 letter last stored (UI_SET_MW_P1)
static bool mw_p25_said;		// the ring's P25 warning was shown for this ring
static uint32_t mw_second;		// the last whole second Home was redrawn for the ages
static uint16_t mw_delay_ms = RADIO_DUAL_DELAY_MS;	// Settings > Dual watch delay, for the P25 estimate
static ui_dual_ref_t mw_ref[UI_MW_MAX];
static uint32_t mw_heard[UI_MW_MAX];	// when the last call on each letter ended, 0: none since it got its channel
static struct {				// what Home's row shows, read when the letter gets its channel
	uint16_t number;		// codeplug channel number, 0 for the VFO
	uint8_t mode;
	uint32_t rx_hz;
	char name[CODEPLUG_NAME_LEN + 1];
#if UI_DETAIL
	uint16_t da, db, dc;		// DDM32 Plus: its mode's settings, ui_detail_t's a, b, c (4 bytes a row: padding)
#endif
} mw_row[UI_MW_MAX];
static char mw_msg[32];			// the toasts built here
// The codeplug reads here (static: not on the UI task's stack); a DMR row's talk contact reuses the zone's room
static union {
	zone_t z;
#if UI_DETAIL && CONFIG_DMR
	contact_t c;
#endif
} mw_u;
#define mw_z mw_u.z
static channel_t mw_ch;

static void mw_reset(void)
{
	mw_cnt = 0;
	mw_p1 = 1;
	mw_valid = 0;
	mw_rx = UI_DUAL_NONE;
	mw_busy_seen = 0;
	mw_p1_saved = 0xFF;
	mw_p25_said = false;
	memset(mw_heard, 0, sizeof mw_heard);
#if CONFIG_AIR_WATCH
	mw_air = false;		// DDM32 Plus air watch (ui_airwatch.inc)
#endif
}

static bool mw_ring(void) { return dual()->on && mw_cnt > 2; }
static bool mw_in_ring(void) { return mw_cnt > 2; }	// the watch runs as a ring, or did until a moment ago
static uint8_t mw_sel_letter(void) { return dual()->sel ? mw_p1 : 0; }
static char mw_char(uint8_t k) { return (char)('A' + k); }

// Pane i's letter, and a letter's pane (UI_DUAL_NONE: not in a pane)
static uint8_t mw_letter_of(uint8_t pane) { return pane ? (mw_cnt > 2 ? mw_p1 : 1) : 0; }
static uint8_t mw_pane_of(uint8_t k) { return k == 0 ? 0 : k == mw_p1 ? 1 : UI_DUAL_NONE; }

// The panes are where A and pane 1's letter change (every rule of dual watch): the ring follows them
static void mw_sync(void)
{
	mw_ref[0] = ref[0];
	mw_ref[mw_p1] = ref[1];
	mw_valid |= (uint8_t)(MW_BIT(0) | MW_BIT(mw_p1));
}

// Home's row of letter k from the codeplug (or the VFO). False: it can't be read
static bool __attribute__((noinline)) mw_row_read(uint8_t k)
{
	const ui_dual_ref_t *r = &mw_ref[k];
	memset(&mw_row[k], 0, sizeof mw_row[k]);
	if (r->vfo) {
		if (!ui_svc_vfo_get((uint8_t)r->pos, &mw_ch))
			return false;
	} else {
		if (!codeplug_zone(r->zone, &mw_z) || r->pos >= mw_z.count || !codeplug_channel(mw_z.channels[r->pos], &mw_ch))
			return false;
		mw_row[k].number = mw_z.channels[r->pos];
		str_copy(mw_row[k].name, sizeof mw_row[k].name, mw_ch.name);
	}
	mw_row[k].mode = mw_ch.mode;
	mw_row[k].rx_hz = mw_ch.rx_hz;
#if UI_DETAIL
	ui_detail_t d;
	ui_detail_of(&mw_ch, &d);
#if CONFIG_DMR
	// A DMR channel's talk target (the VFO's is the radio's: CC and TS only)
	uint16_t talk = r->vfo || mw_ch.mode != RADIO_MODE_DMR ? 0 : codeplug_channel_talk_contact(&mw_ch);
	if (talk != 0 && codeplug_contact(talk, &mw_u.c)) {
		d.b = (uint16_t)mw_u.c.id;
		d.c = (uint16_t)(UI_DETAIL_TALK | (mw_u.c.type == CONTACT_PRIVATE ? UI_DETAIL_PRIVATE : 0u) |
				 ((mw_u.c.id >> 16) & 0xFFu));
	}
#endif
	mw_row[k].da = d.a;
	mw_row[k].db = d.b;
	mw_row[k].dc = d.c;
#endif
	return true;
}

// r is the channel of a letter other than `self` (letters of the ring as it is set up, mw_n of them)
static bool mw_used(const ui_dual_ref_t *r, uint8_t self)
{
	for (uint8_t k = 0; k < mw_n && k < UI_MW_MAX; k++)
		if (k != self && (mw_valid & MW_BIT(k)) && ref_same(r, &mw_ref[k]))
			return true;
	return false;
}

// A channel for letter k: the next one after the letter before it that no other letter has, in its zone first, then
// the later zones (B's default for A, dual-watch.md 1). False: the codeplug has no other channel
static bool __attribute__((noinline)) mw_default(uint8_t k)
{
	zone_t *z = &mw_z;
	const ui_dual_ref_t *from = &mw_ref[k - 1u];
	if (from->vfo)
		from = &mw_ref[0];
	uint16_t n = codeplug_zone_count();
	uint16_t first = from->vfo ? ui_view()->zone_index : from->zone;
	for (uint16_t q = 0; q < n; q++) {
		uint16_t zi = (uint16_t)((first + q) % n);
		if (!codeplug_zone(zi, z) || z->count == 0)
			continue;
		uint16_t start = q == 0 && !from->vfo ? (uint16_t)(from->pos + 1u) : 0;
		for (uint16_t j = 0; j < z->count && j < CODEPLUG_ZONE_MAX; j++) {
			ui_dual_ref_t r = { false, zi, (uint16_t)((start + j) % z->count) };
			if (!mw_used(&r, k) && codeplug_channel(z->channels[r.pos], &mw_ch)) {
				mw_ref[k] = r;
				mw_valid |= (uint8_t)MW_BIT(k);
				mw_heard[k] = 0;
				return true;
			}
		}
	}
	return false;
}

// Back to dual watch's pair: pane 1's channel is B
static void mw_pair(void)
{
	if (mw_p1 != 1) {
		mw_ref[1] = ref[1];
		mw_heard[1] = mw_heard[mw_p1];
		mw_p1 = 1;
	}
	mw_cnt = 0;
	mw_rx = UI_DUAL_NONE;
}

// The ring for the setting: A and pane 1's letter from the panes; each other letter keeps its channel while it can be
// read and no letter before it (or in a pane) has it, else it gets the next free one. Fewer channels than letters in
// the codeplug: a shorter ring. False: a channel of the ring can't be read
static bool __attribute__((noinline)) mw_fill(void)
{
	uint8_t n = mw_n;
	if (mw_p1 >= n) {	// fewer letters: pane 1's channel becomes the last one's
		mw_heard[n - 1u] = mw_heard[mw_p1];
		mw_p1 = (uint8_t)(n - 1u);
	}
	mw_valid &= (uint8_t)(MW_BIT(n) - 1u);
	mw_sync();
	uint8_t cnt = n;
	for (uint8_t k = 1; k < n; k++) {
		if (k == mw_p1)
			continue;
		bool keep = (mw_valid & MW_BIT(k)) != 0;
		for (uint8_t j = 0; keep && j < n; j++)
			if (j != k && (j < k || mw_pane_of(j) != UI_DUAL_NONE) && (mw_valid & MW_BIT(j)) &&
			    ref_same(&mw_ref[k], &mw_ref[j]))
				keep = false;
		if (keep && (mw_ref[k].vfo || mw_row_read(k)))
			continue;
		mw_valid &= (uint8_t)~MW_BIT(k);
		if (!mw_default(k)) {
			cnt = k;
			break;
		}
	}
	if (mw_p1 >= cnt) {	// no room left for pane 1's letter: it takes the last place
		mw_ref[cnt - 1u] = ref[1];
		mw_p1 = (uint8_t)(cnt - 1u);
	}
	mw_cnt = cnt > 2 ? cnt : 0;
	if (mw_cnt == 0)
		mw_pair();
	bool ok = true;
	for (uint8_t k = 0; k < cnt; k++)
		ok = mw_row_read(k) && ok;
	return ok;
}

// Every request for the watch: dual watch's pair with 2 watch channels, the whole ring with more
static bool mw_svc(const ui_dual_ref_t r[2], uint8_t sel)
{
	if (r == NULL || mw_n <= 2) {
		if (r != NULL)
			mw_pair();
		return ui_svc_dual(r, sel);
	}
	bool ok = mw_fill();
	if (mw_cnt == 0)
		return ui_svc_dual(ref, sel);
	if (!ok)
		return false;
	return ui_svc_mwatch(mw_ref, mw_cnt, sel ? mw_p1 : 0);
}

// The radio's selected letter as a pane (a selection the UI didn't ask for, the debug link)
static uint8_t mw_view_sel(uint8_t radio_sel)
{
	if (mw_cnt <= 2)
		return radio_sel;
	return radio_sel == 0 ? 0 : radio_sel == mw_p1 ? 1 : dual()->sel;
}

// r is the channel of a ring letter that isn't in a pane (the panes' own rules check those): Up / Down, the zone keys
// and the pickers pass it over
static bool mw_taken(const ui_dual_ref_t *r, uint8_t pane)
{
	(void)pane;
	if (!mw_ring())
		return false;
	for (uint8_t k = 1; k < mw_cnt; k++)
		if (k != mw_p1 && ref_same(r, &mw_ref[k]))
			return true;
	return false;
}

// Kept across power off with dual watch's own (UI_SET_DUAL_B): pane 1's letter, so the radio comes back with the
// selected channel under its letter. The other letters are found again from the codeplug (mw_default)
static void mw_save(void)
{
#if CONFIG_AIR_WATCH
	if (mw_air)
		return;	// DDM32 Plus air watch: the user's ring stays as saved before it
#endif
	uint8_t p = mw_cnt > 2 ? mw_p1 : 1;
#if CONFIG_VFO_MODES
	if (ref[1].vfo && ref[1].pos >= 2)
		p = 1;	// a C..F VFO in pane 1 is RAM only: B comes back in its place (pane_code, mw_code_b)
#endif
	if (p != mw_p1_saved) {
		mw_p1_saved = p;
		ui_svc_setting_set(UI_SET_MW_P1, p);
	}
}

// Power on, the watch coming back: pane 1's letter as it was
static void mw_resume(void)
{
	int32_t p = ui_svc_setting_get(UI_SET_MW_P1);
	mw_p1 = (uint8_t)(p >= 1 && p < mw_n ? p : 1);
	mw_p1_saved = mw_p1;
	mw_valid = 0;
}

static void mw_toast_letter(const char *before, uint8_t k, const char *after)
{
	sbuf_t b;
	sb_init(&b, mw_msg, sizeof mw_msg);
	sb_str(&b, before);
	sb_chr(&b, mw_char(k));
	sb_str(&b, after);
	ui_toast(mw_msg);
}

// DDM32 Plus: the PTT pill follows the radio's selected letter k (the debug link chose it, or the radio kept another
// after a selection it dropped): the pane takes it as a selection here would, without asking the radio
static uint8_t mw_reask, mw_radio_sel = UI_DUAL_NONE;
static void __attribute__((noinline)) mw_adopt(ui_dual_t *d, uint8_t k)
{
	mw_reask = 0;
	if (k == 0 || mw_cnt <= 2) {
		d->sel = k ? 1 : 0;
	} else {
		if (k != mw_p1) {
			mw_sync();	// pane 1's letter keeps its channel
			ref[1] = mw_ref[k];
			mw_p1 = k;
			refill[1] = true;
		}
		d->sel = 1;
	}
	d->back_to = UI_DUAL_NONE;
	state_save();
	mw_toast_letter(TR("Talking on "), k, "");
}

#if CONFIG_DMR_EMERGENCY
// The emergency key while the watch runs (ui_svc_emergency.c): the alarm, the hot mic and its target are the channel
// the radio is on, which in the ring is whichever letter it is on now. The selected letter DMR: the watch ends there
// first; else refused (a toast says what to do)
bool ui_dual_emergency_ok(void)
{
	ui_dual_t *d = dual();
	if (!d->on)
		return true;
	if (d->pane[d->sel].mode != RADIO_MODE_DMR) {
		ui_toast(TR("Emergency: select a DMR letter"));
		return false;
	}
	stop(true);	// the radio stays on the selected letter
	return true;
}
#endif

// DDM32 Plus (owner 2026-11-22): the watch named by its letters, the ring's when it runs, else the setting's
static uint8_t mw_letters(void)
{
	uint8_t n = mw_cnt > 2 ? mw_cnt : mw_n;
	return n >= 2 && n <= UI_MW_MAX ? n : 2;
}

const char *ui_watch_name(uint8_t form)
{
	static const char *const names[3][UI_MW_MAX - 1] = {
		{ TRN("Dual watch"), TRN("Triple watch"), TRN("Quad watch"), TRN("5x watch"), TRN("6x watch") },
		{ TRN("DUAL"), TRN("TRIPLE"), TRN("QUAD"), TRN("5X"), TRN("6X") },
		{ TRN(TEXT_ALT_OPEN "Dual watch" TEXT_ALT_NEXT "Dual" TEXT_ALT_CLOSE),
		  TRN(TEXT_ALT_OPEN "Triple watch" TEXT_ALT_NEXT "Triple" TEXT_ALT_CLOSE),
		  TRN(TEXT_ALT_OPEN "Quad watch" TEXT_ALT_NEXT "Quad" TEXT_ALT_CLOSE),
		  TRN(TEXT_ALT_OPEN "5x watch" TEXT_ALT_NEXT "5x" TEXT_ALT_CLOSE),
		  TRN(TEXT_ALT_OPEN "6x watch" TEXT_ALT_NEXT "6x" TEXT_ALT_CLOSE) },
	};
	return tr(names[form < 3 ? form : 0][mw_letters() - 2]);
}

const char *ui_watch_on_text(void)
{
	if (mw_letters() == 2)
		return TR("Dual watch on");
	sbuf_t b;
	sb_init(&b, mw_msg, sizeof mw_msg);
	sb_str(&b, ui_watch_name(0));
	sb_str(&b, TR(" on"));
	return mw_msg;
}

// PTT and the keys on letter k: A or pane 1's letter as dual watch selects a pane; another letter first takes pane 1's
// place (pane 1's channel stays its letter's)
static void __attribute__((noinline)) mw_select(uint8_t k)
{
	ui_dual_t *d = dual();
	if (!mw_ring() || k >= mw_cnt || k == mw_sel_letter())
		return;
	d->back_to = UI_DUAL_NONE;
	mw_sync();
	if (k == 0 || k == mw_p1) {
		if (!apply(k == 0 ? 0 : 1))
			return;
	} else {
		ui_dual_ref_t keep = ref[1];
		uint8_t p = mw_p1;
		ref[1] = mw_ref[k];
		mw_p1 = k;
		if (!apply(1)) {
			ref[1] = keep;
			mw_p1 = p;
			mw_sync();
			return;
		}
		// Pane 1 is another letter's channel now: what the screens and quick settings know of it starts over
		snap[1].valid = false;
		chan_ok[1] = false;
		d->ended[1] = false;
		if (d->last == 1)
			d->last = UI_DUAL_NONE;
		pane_fill(1);
	}
	state_save();
	mw_toast_letter(TR("Talking on "), k, "");
}

// The A/B key, Left / Right, # 1: the next (or the previous) letter of the ring
static void mw_step_sel(int dir)
{
	uint8_t k = mw_sel_letter();
	mw_select((uint8_t)((k + mw_cnt + dir) % mw_cnt));
}

// Letter k (outside the panes) becomes channel r
static bool mw_set_letter(uint8_t k, const ui_dual_ref_t *r)
{
	mw_sync();
	if (mw_used(r, k)) {
		ui_toast(TR(TEXT_ALT_OPEN "Watched already: pick another" TEXT_ALT_NEXT "Watched already" TEXT_ALT_CLOSE));
		return false;
	}
	ui_dual_ref_t old = mw_ref[k];
	mw_ref[k] = *r;
	mw_valid |= (uint8_t)MW_BIT(k);
	if (!apply(dual()->sel)) {
		mw_ref[k] = old;
		mw_fill();
		return false;
	}
	mw_heard[k] = 0;
	if (mw_rx == k)
		mw_rx = UI_DUAL_NONE;
	return true;
}

// Settings > Watch channels (at start-up, then on each change): a watch that runs takes the new ring at once
void ui_dual_mw_set(int32_t v)
{
	uint8_t n = (uint8_t)(v < 2 ? 2 : v > UI_MW_MAX ? UI_MW_MAX : v);
#if CONFIG_AIR_WATCH
	if (mw_air)
		return;	// DDM32 Plus air watch: its own ring; the setting is read again at its end (ui_airwatch.inc)
#endif
	if (n == mw_n)
		return;
	mw_n = n;
	mw_p25_said = false;
	ui_dual_t *d = dual();
	if (!d->on) {
		if (n <= 2)
			mw_pair();
		return;
	}
	mw_sync();
	if (apply(d->sel)) {
		state_save();
		pane_fill(1);
	}
}

// The codeplug changed: the other letters' channels found again by their numbers (the panes' are found by
// find_again). One gone gets the next free channel
static void mw_find_again(void)
{
	if (!mw_ring())
		return;
	for (uint8_t k = 1; k < mw_cnt; k++) {
		if (k == mw_p1 || mw_ref[k].vfo || mw_row[k].number == 0)
			continue;
		ui_dual_ref_t r;
		if (find_code(mw_row[k].number, mw_ref[k].zone, &r))
			mw_ref[k] = r;
		else
			mw_valid &= (uint8_t)~MW_BIT(k);
	}
}

#if CONFIG_ZONE_WRITE
// The zone editor moved zone indexes: the other letters' zones follow (mw_find_again then finds the channels)
static void mw_follow_zones(uint32_t from, uint32_t to)
{
	uint16_t out;
	for (uint8_t k = 1; k < UI_MW_MAX; k++)
		if (k != mw_p1 && !mw_ref[k].vfo && zone_edit_map(from, to, mw_ref[k].zone, &out) != ZONE_MAP_LOST)
			mw_ref[k].zone = out;
}
#endif

// The letter the radio is tuned to, UI_DUAL_NONE for none of them
static uint8_t mw_tuned(const radio_view_t *v)
{
	ui_dual_ref_t here;
	ref_here(v, &here);
	for (uint8_t k = 0; k < mw_cnt; k++)
		if (ref_same(&mw_ref[k], &here))
			return k;
	return UI_DUAL_NONE;
}

static bool mw_has_p25(void)
{
	for (uint8_t k = 0; k < mw_cnt; k++)
		if (codeplug_mode_p25(mw_row[k].mode) && !codeplug_mode_ysf(mw_row[k].mode) &&
		    !codeplug_mode_nxdn(mw_row[k].mode))
			return true;
	return false;
}

// Every UI tick while the ring runs, after dual watch's own: which letter receives (green on Home, its caller), how
// long ago each letter last heard a call, the Busy marks, the P25 warning. True: redraw
static bool __attribute__((noinline)) mw_tick(ui_dual_t *d, const radio_view_t *v, uint32_t now)
{
	// DDM32 Plus (docs/plus.md "The TX target"): the PTT pill and the radio's selected letter agree. A selection is
	// refused while transmitting (apply). The radio choosing another letter itself (the debug link) is taken at once;
	// one it dropped (it raced a transmission, or refused the ring) is asked again, a second apart, twice at most and
	// never while a call comes in (a request lets the native receiver's P25 call go), then the pill takes the radio's
	if (v->scan == 3 /* SCAN_DUAL */ && !v->hotspot && v->dual_sel < (mw_ring() ? mw_cnt : 2u)) {
		bool moved = v->dual_sel != mw_radio_sel;
		mw_radio_sel = v->dual_sel;
		if (v->dual_sel == (mw_ring() ? mw_sel_letter() : d->sel)) {
			mw_reask = 0;
		} else if (moved) {
			mw_adopt(d, v->dual_sel);
		} else if (v->state != RS_TX && v->state != RS_TX_DENIED && v->state != RS_RX && now - started_ms >= 1000u) {
			if (mw_reask < 2u) {
				mw_reask++;
				(void)apply(d->sel);
			} else {
				mw_adopt(d, v->dual_sel);
			}
		}
	}
	if (!mw_ring()) {
		mw_rx = UI_DUAL_NONE;
		return false;
	}
	mw_sync();
	mw_save();
	bool dirty = false;
	uint8_t t = v->state == RS_RX ? mw_tuned(v) : UI_DUAL_NONE;
	if (t != mw_rx) {
		if (mw_rx < UI_MW_MAX) {
			mw_heard[mw_rx] = now != 0 ? now : 1;	// its call ended
			if (mw_pane_of(mw_rx) == UI_DUAL_NONE)
				d->last_ms = now;	// # 4 Caller: how long ago (the panes' calls are dual watch's)
		}
		mw_rx = t;
		dirty = true;
	}
	if (t < UI_MW_MAX && mw_pane_of(t) == UI_DUAL_NONE) {
		// The caller on a letter outside the panes (the panes' come from dual watch's tick)
#if CONFIG_P25_CALLER || CONFIG_YSF
		if (!native_caller(v, MW_ANY_PANE))
#endif
		{
#if CONFIG_DMR
			caller_text(caller_buf, sizeof caller_buf, v->ch.mode == RADIO_MODE_DMR, v->dmr.call.src,
				    v->dmr.call.src_name, v->dmr.call.dst, v->dmr.call.group);
#else
			caller_text(caller_buf, sizeof caller_buf, false, 0, "", 0, false);
#endif
		}
		if (strcmp(caller_buf, d->caller) != 0) {
			strcpy(d->caller, caller_buf);
			dirty = true;
		}
	}
	if (v->dual_busy != mw_busy_seen) {
		mw_busy_seen = v->dual_busy;
		dirty = true;
	}
	bool p25 = mw_has_p25();
	if (p25 && !mw_p25_said)
		ui_toast(TR(TEXT_ALT_OPEN "P25 slows the ring" TEXT_ALT_NEXT "P25 is slow" TEXT_ALT_CLOSE));
	mw_p25_said = p25;
	if (now / 1000u != mw_second) {	// the rows' ages count up
		mw_second = now / 1000u;
		// The wait after a signal for the ring's P25 estimate, read here: Home's drawing is the UI task's deepest path
		mw_delay_ms = (uint16_t)ui_svc_setting_get(UI_SET_DUAL_DELAY_MS);
		dirty = dirty || mw_heard[0] || mw_heard[1] || mw_heard[2] || mw_heard[3] || mw_heard[4] || mw_heard[5];
	}
	return dirty;
}

// ---- Home (ui_screens.c draws it: ui_draw_home_dual) ----

uint8_t ui_dual_ring(void) { return mw_ring() ? mw_cnt : 0; }

void ui_dual_ring_row(uint8_t k, const radio_view_t *v, ui_mw_row_t *out)
{
	const ui_dual_t *d = dual();
	memset(out, 0, sizeof *out);
	out->letter = mw_char(k);
	out->vfo = mw_ref[k].vfo;
	out->number = mw_row[k].number;
	out->mode = mw_row[k].mode;
	out->rx_hz = mw_row[k].rx_hz;
	out->name = mw_row[k].name;
#if UI_DETAIL
	out->detail = (ui_detail_t){ mw_row[k].da, mw_row[k].db, mw_row[k].dc, mw_row[k].mode };
	// A VFO letter as it is now (its mode, NAC or talk group change without a new channel): read from the radio's copy
	if (mw_ref[k].vfo && ui_svc_vfo_get((uint8_t)mw_ref[k].pos, &mw_ch)) {
		out->mode = mw_ch.mode;
		out->rx_hz = mw_ch.rx_hz;
		ui_detail_of(&mw_ch, &out->detail);
	}
#endif
	uint8_t pane = mw_pane_of(k);
	if (pane != UI_DUAL_NONE && d->pane[pane].vfo)
		out->rx_hz = d->pane[pane].rx_hz;	// the VFO steps: the pane follows it
#if UI_DETAIL && CONFIG_DMR
	// A DMR letter in a pane: the talk target PTT uses there now (a target for now, Call back, the VFO's) as dual watch
	// keeps it (snap), not only the channel's contact
	if (pane != UI_DUAL_NONE && out->mode == RADIO_MODE_DMR && snap[pane].valid && snap[pane].target_id != 0) {
		out->detail.b = (uint16_t)snap[pane].target_id;
		out->detail.c = (uint16_t)(UI_DETAIL_TALK | (snap[pane].target_private ? UI_DETAIL_PRIVATE : 0u) |
					   ((snap[pane].target_id >> 16) & 0xFFu));
	}
#endif
	out->sel = k == mw_sel_letter();
	out->rx = k == mw_rx;
	out->busy = !out->rx && (v->dual_busy & MW_BIT(k)) != 0;
	out->heard_ms = mw_heard[k];
	out->caller = out->rx ? d->caller : NULL;
}

// The ring's period with nothing heard, and what a P25 channel with a signal but no voice adds (0: no P25 letter)
uint32_t ui_dual_ring_ms(uint32_t *p25_extra_ms)
{
	uint8_t dmr = 0;	// a quiet DMR stop takes longer on the faster ring (radio_mwatch.h)
	for (uint8_t k = 0; k < mw_cnt; k++)
		dmr += mw_row[k].mode == RADIO_MODE_DMR;
	*p25_extra_ms = mw_has_p25() ? radio_mw_p25_extra_ms(mw_cnt, mw_delay_ms) : 0;
	return radio_mw_ring_ms(mw_cnt, dmr);
}

char ui_dual_ring_sel(void) { return mw_ring() ? mw_char(mw_sel_letter()) : '-'; }
char ui_dual_ring_rx(void) { return mw_ring() && mw_rx < UI_MW_MAX ? mw_char(mw_rx) : '-'; }

// ---- The actions (#) and the pickers ----
// # with the ring: 1 Talk on the next letter, 2 Change A, 3 Change B, 4 Caller, 5 Stop (as dual watch), then
// 6 Change C, 7 Change D, 8 Change E, 9 Change F. A long OK on a Change row talks on that letter

static uint16_t mw_actions_count(void) { return (uint16_t)(DA_COUNT + (mw_ring() ? mw_cnt - 2u : 0u)); }

// The picker's arg for letter k: its pane, or MW_LETTER_ARG | k outside the panes
static uint16_t mw_pick_arg(uint8_t k)
{
	uint8_t pane = mw_ring() ? mw_pane_of(k) : k;
	return pane != UI_DUAL_NONE ? pane : (uint16_t)(MW_LETTER_ARG | k);
}

static uint8_t mw_arg_letter(uint16_t arg) { return (arg & MW_LETTER_ARG) ? (uint8_t)(arg & 7u) : mw_letter_of((uint8_t)arg); }

static uint8_t mw_action_letter(uint16_t i)
{
	return i == DA_CHANGE_A ? 0 : i == DA_CHANGE_B ? 1 : (uint8_t)(i - DA_COUNT + 2u);
}

static void mw_value(sbuf_t *b, uint8_t k)
{
	if (mw_ref[k].vfo) {
		sb_str(b, "VFO ");
		sb_freq(b, mw_row[k].rx_hz);
		return;
	}
	sb_str(b, TEXT_ALT_OPEN);
	sb_u32(b, mw_row[k].number, 1);
	sb_chr(b, ' ');
	sb_str(b, mw_row[k].name);
	sb_str(b, TEXT_ALT_NEXT);
	sb_str(b, mw_row[k].name);
	sb_str(b, TEXT_ALT_CLOSE);
}

// True: the ring's own row (the label and the value filled)
static bool mw_actions_item(uint16_t i, ui_item_t *it)
{
	if (!mw_ring() || i == DA_CALLER)
		return false;
	sbuf_t b, vb;
	sb_init(&b, it->label, sizeof it->label);
	sb_init(&vb, it->value, sizeof it->value);
	item_num_set(it, (uint16_t)(i + 1u));
	if (i == DA_SELECT) {
		uint8_t k = (uint8_t)((mw_sel_letter() + 1u) % mw_cnt);
		sb_str(&b, TR("Talk on "));
		sb_chr(&b, mw_char(k));
		mw_value(&vb, k);
	} else if (i == DA_STOP) {
		sb_str(&b, TR("Stop, stay on "));
		sb_chr(&b, mw_char(mw_sel_letter()));
	} else {
		uint8_t k = mw_action_letter(i);
		sb_str(&b, TR("Change "));
		sb_chr(&b, mw_char(k));
		mw_value(&vb, k);
		if (k == mw_sel_letter())
			it->flags |= UI_ITEM_TEMP;	// the one PTT talks on
	}
	return true;
}

// True: done here
static bool mw_actions_select(uint16_t i, bool hold)
{
	if (!mw_ring() || i == DA_CALLER || i == DA_STOP)
		return false;
	if (i == DA_SELECT) {
		ui_go_home();
		mw_step_sel(1);
		return true;
	}
	uint8_t k = mw_action_letter(i);
	if (hold) {	// long OK: talk on it
		ui_go_home();
		mw_select(k);
		return true;
	}
	ui_push(SCR_DUAL_PICK, mw_pick_arg(k));
	return true;
}

// The picker's channel now, for its arg
static const ui_dual_ref_t *mw_pick_ref(uint16_t arg)
{
	return (arg & MW_LETTER_ARG) ? &mw_ref[arg & 7u] : &ref[arg & 1u];
}

// The letter that has r already, as the picker shows it dimmed (0: none). Without the ring, dual watch's own: the other
// pane ("A" when B is picked); the watch off, A is the channel on view
static char mw_taken_tag(const ui_dual_ref_t *r, uint16_t arg)
{
	if (!mw_ring()) {
		uint8_t pane = (uint8_t)(arg & 1u);
		ui_dual_ref_t other;
		if (dual()->on)
			other = ref[!pane];
		else
			ref_here(ui_view(), &other);
		return ref_same(r, &other) ? (pane ? 'A' : 'B') : 0;
	}
	mw_sync();
	uint8_t self = mw_arg_letter(arg);
	for (uint8_t k = 0; k < mw_cnt; k++)
		if (k != self && ref_same(r, &mw_ref[k]))
			return mw_char(k);
	return 0;
}

static void mw_pick_set(uint16_t arg, const ui_dual_ref_t *r)
{
	if (arg & MW_LETTER_ARG)
		mw_set_letter((uint8_t)(arg & 7u), r);
	else
		set_pane((uint8_t)arg, r);
}

// The VFO from the zone list. Upstream's two VFOs are A's and pane 1's letter's; with every mode in the VFO
// (CONFIG_VFO_MODES) C..F have one each too (vfo_for, mw_vfo_letter)
static void mw_pick_vfo(uint16_t arg)
{
	if (arg & MW_LETTER_ARG) {
#if CONFIG_VFO_MODES
		// DM32 Plus: a letter outside the panes gets its own VFO (C..F: VFOs 2..5, RAM only)
		ui_dual_ref_t own = { true, 0, (uint16_t)(arg & 7u) };
		ui_go_home();
		mw_set_letter((uint8_t)(arg & 7u), &own);
#else
		ui_toast(TR(TEXT_ALT_OPEN "Only a selected channel can be the VFO" TEXT_ALT_NEXT "Select it first" TEXT_ALT_CLOSE));
#endif
		return;
	}
	ui_dual_ref_t r = vfo_for((uint8_t)arg);
	ui_go_home();
	set_pane((uint8_t)arg, &r);
}

static const char *mw_title(uint16_t arg)
{
	static const char letters[] = "A\0B\0C\0D\0E\0F";
	if (!mw_ring())
		return (arg & 1u) ? "B" : "A";
	return &letters[2u * mw_arg_letter(arg)];
}

#if CONFIG_VFO_MODES
// DM32 Plus (every mode in the VFO): the letter a pane or a picker's arg means, for its VFO (vfo_for): C..F have their
// own, VFOs 2..5
static uint8_t mw_vfo_letter(uint16_t arg)
{
	if (arg & MW_LETTER_ARG)
		return (uint8_t)(arg & 7u);
	return mw_ring() ? mw_letter_of((uint8_t)(arg & 1u)) : (uint8_t)(arg & 1u);
}

// B's code for the saved state while pane 1 holds a C..F VFO (RAM only): B's channel or VFO, 0 when B has none
static int32_t mw_code_b(void)
{
	const ui_dual_ref_t *r = &mw_ref[1];
	if (!(mw_valid & MW_BIT(1)))
		return 0;
	return r->vfo ? UI_DUAL_B_VFO + (r->pos & 1u) : mw_row[1].number;
}
#endif
