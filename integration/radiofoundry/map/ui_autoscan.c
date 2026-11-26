/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus auto airfield scan and auto repeater scan (CONFIG_AUTO_SCAN, docs/autoscan-design.md): the UI task's
 * side. Installed as core/ui/ui_autoscan.c; tools/plus_autoscan.py adds Tools > Air scan and Tools > Repeater scan
 * (and their held digits, Settings > Keys) and calls ui_autoscan_tick from the UI tick.
 *
 * On: the map pack and the GPS are needed (refused without: a toast). Once a second (only while on): with a GPS fix,
 * the first selection at once, then every AS_EVERY_MS at most and only after moving AS_*_MOVE_M (autoscan.c as_due),
 * the nearest repeaters (FM, DMR) or airfield frequencies (autoscan.c) on the multi-watch ring, each letter its own
 * VFO (ui_airwatch.inc ui_dual_auto_ring), with Home labels. Two or more: the ring; fewer: no ring (the user's state
 * as it was), the scan waits, a toast says how many. A refresh replaces only the letters that changed, never while
 * the ring holds a signal, waits after one or transmits (it waits for the next second); the radio core refuses a
 * replacement then too, and a refused one is asked again (the VFO read back). A new map pack: everything again; no
 * pack (or none of the kind): off. No fix: the letters stay. The ring stopped any way (Back twice, # 5, DOTA, ...):
 * off (ui_autoscan_ended). The Auto repeater's offers wait while it runs (ui_mapnav.c). RAM only, off at power on.
 * Nothing here transmits; PTT on the ring is the radio core's (radio_airwatch.inc air_talk_back). */
#include "config.h"
#if CONFIG_AUTO_SCAN
#include <string.h>
#include "board.h"
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "radio/radio_am.h"
#include "radio/scan.h"
#include "radio/tones.h"
#include "storage/ref_pack.h"
#include "ui/autoscan.h"
#include "ui/rpt.h"
#include "ui/airfield.h"

#define POLL_MS	1000u

static as_state_t as;
static uint32_t polled_ms;
static char as_label[18];	/* a letter's label (mw_row's name holds 16) */
static char as_msg[sizeof as_label + 1];	/* "+" and a label: the toast (ui_toast keeps the pointer) */
static as_pick_t as_cand[AS_MAX], as_fields[AS_MAX];
static uint32_t as_arg[AS_MAX];

static bool band_rpt(uint32_t hz) { return board_in_band(hz, false) && !radio_am_band(hz); }
static bool band_air(uint32_t hz) { return board_in_band(hz, false) && radio_am_band(hz) && radio_am_capable(); }

uint8_t ui_autoscan_on(void)
{
	return as.kind;
}

/* The toasts, whole sentences (the Turkish word order is another) */
enum { SAY_ON, SAY_WAIT, SAY_OFF, SAY_NONE, SAY_ONE };
static const char *say(uint8_t kind, uint8_t what)
{
	bool air = kind == AS_AIR;
	switch (what) {
	case SAY_ON: return air ? TR("Air scan on") : TR("Repeater scan on");
	case SAY_WAIT: return air ? TR("Air scan on: waiting for GPS") : TR("Repeater scan on: waiting for GPS");
	case SAY_OFF: return air ? TR("Air scan off") : TR("Repeater scan off");
	case SAY_NONE: return air ? TR("Air scan: no airfield within 100 km") : TR("Repeater scan: none within 60 km");
	default: return air ? TR("Air scan: 1 frequency only, waiting") : TR("Repeater scan: 1 repeater only, waiting");
	}
}

static const char *no_data(uint8_t kind)
{
	return kind == AS_AIR ? TR("No airfield data: load the map pack") : TR("No repeater data: load the map pack");
}

static bool have_data(uint8_t kind)
{
	return kind == AS_AIR ? air_nfields() != 0u : rpt_count() != 0u;
}

static uint8_t tone_index(uint16_t ctcss10)
{
	for (uint8_t i = 0; i < TONES_CTCSS_COUNT; i++)
		if (tones_ctcss[i] == ctcss10)
			return (uint8_t)(i + 1u);
	return 0;	/* not a tone of the table: none (the list's tone is the access tone; nothing to receive) */
}

/* Letter p: its RX, its request bits (UI_AIR_*) and its label into as_label. False: the pack doesn't give it */
static bool __attribute__((noinline)) letter(const as_pick_t *p, uint32_t *hz, uint32_t *arg)
{
	sbuf_t b;
	sb_init(&b, as_label, sizeof as_label);
	if (as.kind == AS_RPT) {
		const rpt_entry_t *e = rpt_get(p->id);
		rpt_tune_t t;
		if (e == NULL || !rpt_tune_plan(e, &t))
			return false;
		*hz = t.rx_hz;
		uint32_t off = ((uint32_t)(int32_t)e->off & 0x3FFFFu) << UI_AIR_OFF_SHIFT;
		*arg = t.dmr ? UI_AIR_DMR | (uint32_t)(t.cc & 0x0Fu) << UI_AIR_P_SHIFT | (uint32_t)(t.ts & 3u) << (UI_AIR_P_SHIFT + 4) |
			       off
			     : UI_AIR_FM | (t.narrow ? UI_AIR_NARROW : 0u) | (uint32_t)tone_index(t.ctcss10) << UI_AIR_P_SHIFT | off;
		sb_str(&b, e->name);
		sb_chr(&b, ' ');
		sb_u32(&b, p->d_m == UINT32_MAX ? 0u : (p->d_m + 500u) / 1000u, 1);
		sb_str(&b, "km");
		return true;
	}
	const air_field_t *a = air_get((uint16_t)(p->id >> 6));
	air_freq_t f;
	if (a == NULL)
		return false;
	sb_str(&b, a->code);
	if (!air_freq_get((uint32_t)a->first + (p->id & 63u), &f))
		return false;
	*hz = air_freq_hz(&f);
	*arg = 0;		/* AM: the air band */
	sb_chr(&b, ' ');
	sb_str(&b, air_type_name(f.type));
	return true;
}

/* A call comes in, the ring (ours or the user's watch, a scan) holds a signal or waits after one, or the radio
 * transmits: no refresh, no ring started over it (the next second tries again) */
static bool busy(void)
{
	const radio_view_t *v = ui_view();
	return v->state == RS_TX || v->state == RS_TX_DENIED || v->state == RS_RX ||
	       (v->scan != 0 && (v->scan_phase == SCAN_PH_HOLD || v->scan_phase == SCAN_PH_RESUME ||
				 v->scan_phase == SCAN_PH_PROBE));
}

/* Off, the ring (ours) ended without the user's toast; why: the toast, NULL none */
static void stop(const char *why)
{
	uint8_t kind = as.kind;
	as.kind = AS_OFF;	/* first: the ring's end calls nothing back */
	if (ui_dual_auto_kind() == kind && kind != AS_OFF)
		ui_dual_auto_end();
	if (why != NULL)
		ui_toast(why);
}

void ui_autoscan_ended(void)
{
	if (as.kind == AS_OFF)
		return;
	ui_toast(say(as.kind, SAY_OFF));
	as.kind = AS_OFF;
}

void ui_autoscan_toggle(uint8_t kind)
{
	ui_position_t p;
	if (as.kind == kind) {	/* the same one again: off */
		stop(say(kind, SAY_OFF));
		return;
	}
	if (!have_data(kind)) {
		ui_toast(no_data(kind));
		return;
	}
	if (!ui_svc_position(&p)) {	/* no GPS, or off in Settings: the scan doesn't switch it on */
		ui_toast(TR("GPS is off: switch it on in Settings"));
		return;
	}
	stop(NULL);	/* the other kind */
	memset(&as, 0, sizeof as);
	as.kind = kind;
	as.said = 0xFFu;
	polled_ms = ui_st.now_ms - POLL_MS;	/* the next tick looks */
	ui_toast(say(kind, p.fix ? SAY_ON : SAY_WAIT));
	ui_go_home();
}

/* Fewer than two: no ring (the user's state as it was); the toast once per count */
static void too_few(uint8_t n)
{
	if (ui_dual_auto_kind() == as.kind)
		ui_dual_auto_end();
	as.pend = 0;
	if (as.said == n)
		return;
	as.said = n;
	ui_toast(say(as.kind, n == 0 ? SAY_NONE : SAY_ONE));
}

/* The letters chosen (as.l[0..as.n)) onto the ring: a new ring (none of ours, or another count; old_n 0xFF: a new
 * pack, everything again), else only the letters whose entry changed. Then every label (the distances moved) */
static void __attribute__((noinline)) put(uint8_t old_n, const uint16_t old_id[])
{
	uint8_t n = 0;
	for (uint8_t k = 0; k < as.n; k++)	/* a row the pack doesn't give (it changed under us): left out */
		if (letter(&as.l[k], &as.hz[n], &as_arg[n]))
			as.l[n++] = as.l[k];
	as.n = n;
	if (n < 2) {
		too_few(n);
		return;
	}
	as.said = 0xFFu;
	bool ring = ui_dual_auto_kind() == as.kind;
	uint8_t first = AS_MAX;	/* the first letter that came in (its toast) */
	if (!ring || n != old_n) {
		for (uint8_t k = 0; k < n; k++)
			as_arg[k] |= ring ? UI_AIR_REPLACE : 0u;	/* the radio's air state is ours already */
		if (!ui_dual_auto_ring(as.hz, as_arg, n, as.kind)) {
			as.pend = 0;
			return;		/* (a toast said why) as_due tries again later */
		}
		as.pend = ring ? (uint8_t)((1u << n) - 1u) : 0u;
	} else {
		for (uint8_t k = 0; k < n; k++) {
			if (as.l[k].id == old_id[k])
				continue;
			ui_dual_auto_set(k, as.hz[k], as_arg[k]);
			as.pend |= (uint8_t)(1u << k);
			if (first == AS_MAX)
				first = k;
		}
	}
	for (uint8_t k = 0; k < n; k++) {
		uint32_t hz, arg;
		if (letter(&as.l[k], &hz, &arg))
			ui_dual_auto_label(k, as_label);
		if (k == first) {	/* "+TA2XYZ 12km": the one that came in */
			sbuf_t b;
			sb_init(&b, as_msg, sizeof as_msg);
			sb_chr(&b, '+');
			sb_str(&b, as_label);
			ui_toast(as_msg);
		}
	}
}

/* A replacement the radio core refused (the ring held a signal by then): asked again once it is quiet */
static void retry(void)
{
	if (as.pend == 0 || ui_dual_auto_kind() != as.kind || busy())
		return;
	for (uint8_t k = 0; k < as.n; k++) {
		if (!((as.pend >> k) & 1u))
			continue;
		uint32_t hz, arg;
		if (ui_dual_auto_has(k, as.hz[k]))
			as.pend &= (uint8_t)~(1u << k);
		else if (letter(&as.l[k], &hz, &arg))
			ui_dual_auto_set(k, hz, arg);
	}
}

/* The selection from (lat, lon): the letters kept, refreshed (full: from nothing) */
static void __attribute__((noinline)) choose(int32_t lat, int32_t lon, bool full)
{
	uint8_t n = full ? 0 : as.n;
	if (as.kind == AS_RPT) {
		for (uint8_t k = 0; k < n; k++)
			as.l[k].d_m = as_rpt_dist(as.l[k].id, lat, lon);
		uint8_t nc = as_pick_rpt(lat, lon, AS_RPT_RADIUS_M, band_rpt, as_cand, AS_MAX);
		n = as_merge(as.l, n, as_cand, nc, AS_RPT_RADIUS_M / 100u * AS_KEEP_PCT);
		as_group(as.l, n);
		as.n = n;
		return;
	}
	uint8_t nf = as_letter_fields(as.l, n, lat, lon, as_fields);
	uint8_t nc = as_pick_fields(lat, lon, AS_AIR_RADIUS_M, band_air, as_cand, AS_MAX);
	nf = as_merge(as_fields, nf, as_cand, nc, AS_AIR_RADIUS_M / 100u * AS_KEEP_PCT);
	as.n = as_air_letters(as_fields, nf, band_air, as.l, AS_MAX);
}

bool ui_autoscan_tick(uint32_t now_ms)
{
	if (as.kind == AS_OFF || now_ms - polled_ms < POLL_MS)
		return false;
	polled_ms = now_ms;
	if (!have_data(as.kind)) {	/* the map pack went (or lost the kind's sections) */
		stop(no_data(as.kind));
		return true;
	}
	retry();
	ui_position_t p;
	memset(&p, 0, sizeof p);
	bool fix = ui_svc_position(&p) && p.fix;
	uint32_t gen = ref_seq();
	bool stale = as.n >= 2 && ui_dual_auto_kind() != as.kind;	/* chosen, but no ring of ours */
	uint8_t due = as_due(&as, fix, p.lat_e7, p.lon_e7, now_ms, gen, stale);
	if (due == AS_DUE_NO || busy())
		return false;	/* busy: the next second */
	static uint16_t old_id[AS_MAX];
	uint8_t old_n = due == AS_DUE_FULL ? 0xFFu : as.n;
	for (uint8_t k = 0; k < AS_MAX; k++)
		old_id[k] = as.l[k].id;
	choose(p.lat_e7, p.lon_e7, due == AS_DUE_FULL);
	as.picked = true;
	as.lat_e7 = p.lat_e7;
	as.lon_e7 = p.lon_e7;
	as.next_ms = now_ms + AS_EVERY_MS;
	as.gen = gen;
	put(old_n, old_id);
	return true;
}
#endif
