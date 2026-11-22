/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Satellites through the real UI in the upstream simulator (-DCONFIG_SATELLITES=1): the list, the
 * live lines (range rate, Doppler in kHz, "Tracking"), OK tracking and stopping, the pass alarm page (open from a
 * satellite with nothing to track or with 0, each row's values, arm, change while armed, off, Back to the list), the
 * alarm's banner and tone on another screen (Home) and its toast over a banner that waits for the user, the screen
 * without the GPS time (its hint names OK), and the layout check on the list and the page. Tools > ISS and the AOS
 * pop-up in English and Turkish (iss_checks): the page, its keys and settings, the held track asked for, the pop-up's
 * OK / Back / other keys, its 30 s, no pop-up during TX or with auto track on, layout checks.
 *
 * ui_sat.c is included here (its statics start each flow afresh); the radio task's service (sat_svc.c) is a fake
 * with sat.h's contract: a view the test sets, the requests recorded, an armed alarm due 2 s later (its warning) and
 * 10 s later (the rise). The math (sat.c, the generated table) is the real one. */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_sat.c"

/* --- The fake service --- */
static sat_view_t fv;
static int32_t t0;			/* UTC (seconds since 2000) when the screen first asks */
static uint32_t base_ms;		/* the sim time then (0: not yet) */
static uint8_t focus_seen;
static int track_calls, alarm_calls;
static uint8_t al_lead, al_el, al_opts;
static bool fake_auto, fake_idle;	/* an auto track runs; the UI's last word on idle */
static int cancels;
static uint32_t soon_ms, aos_ms;	/* sim times the armed alarm is due */
static uint32_t track_ms;		/* sim time an auto track's "tracking" event is due (0: none) */
static uint8_t refuse;			/* ... a refusal instead (SAT_NO_*), the user's held track */
static int beeps_soon, beeps_aos;
static uint8_t iss, so50, ao91, po101;

static int32_t fake_now(void)
{
	if (base_ms == 0)
		base_ms = sim_now() ? sim_now() : 1u;
	return t0 + (int32_t)((sim_now() - base_ms) / 1000u);
}

void sat_view(sat_view_t *out)
{
	*out = fv;
	out->now = fake_now();
	out->live = focus_seen;
	out->live_ok = focus_seen < sat_count;
}

void sat_seen(uint8_t focus) { focus_seen = focus; }

void sat_track(uint8_t idx)
{
	track_calls++;
	fv.track = idx;
	fv.track_state = idx == SAT_NONE ? SAT_TRK_OFF : SAT_TRK_ON;
	if (idx != SAT_NONE) {
		fv.rx_hz = sat_table[idx].down_hz + 10000u;
		fv.tx_hz = sat_table[idx].up_hz - 2000u;
	}
}

void sat_arm(bool on) { fv.arm = on; }

void sat_alarm_set(uint8_t target, uint8_t lead, uint8_t min_el_deg, uint8_t opts)
{
	alarm_calls++;
	fv.alarm.opts = al_opts = opts;
	fv.alarm.target = target;
	fv.alarm.lead_min = al_lead = lead;
	fv.alarm.min_el = al_el = min_el_deg;
	fv.alarm.state = target == SAT_NONE ? SAT_AL_OFF : SAT_AL_ARMED;
	fv.alarm.soon = false;
	fv.alarm.sat = target == SAT_ALARM_ANY ? ao91 : target;
	fv.alarm.max_el = target == SAT_ALARM_ANY ? 61 : 23;
	fv.alarm.aos = fake_now() + lead * 60;
	soon_ms = target == SAT_NONE ? 0 : sim_now() + 2000u;
	aos_ms = target == SAT_NONE ? 0 : sim_now() + 10000u;
}

bool sat_alarm_take(sat_alarm_event_t *out)
{
	uint32_t now = sim_now();
	if (soon_ms && now >= soon_ms) {
		soon_ms = 0;
		fv.alarm.soon = true;
		*out = (sat_alarm_event_t){ SAT_ALARM_SOON, fv.alarm.sat, fv.alarm.max_el, (int32_t)fv.alarm.lead_min * 60 };
		return true;
	}
	if (track_ms && now >= track_ms) {
		track_ms = 0;
		*out = (sat_alarm_event_t){ refuse ? SAT_ALARM_NOTRACK : SAT_ALARM_TRACK, iss, refuse ? (int8_t)refuse : 47, 0 };
		return true;
	}
	if (aos_ms && now >= aos_ms) {
		aos_ms = 0;
		*out = (sat_alarm_event_t){ SAT_ALARM_AOS, fv.alarm.sat, fv.alarm.max_el, 0 };
		fv.alarm.target = SAT_NONE;
		fv.alarm.state = SAT_AL_OFF;
		return true;
	}
	return false;
}

void sat_ui_idle(bool idle) { fake_idle = idle; }
bool sat_auto_on(void) { return fake_auto; }
void sat_auto_cancel(void) { cancels++; fake_auto = false; }

/* The user's held track (Tools > ISS, the AOS pop-up): recorded */
static int starts;
static uint8_t start_sat = SAT_NONE, start_mode, mode_now;
void sat_auto_start(uint8_t idx, uint8_t mode)
{
	starts++;
	start_sat = idx;
	start_mode = mode;
	track_ms = sim_now() + 1000u;	/* the service's word a second later: "tracking", or why not */
}
void sat_auto_mode(uint8_t mode) { mode_now = mode; }

uint8_t aprs_iss_send(uint32_t hz) { return APRS_OK; }

void radio_req_beep(uint8_t beep)
{
	beeps_soon += beep == BEEP_TOT_WARN;
	beeps_aos += beep == BEEP_SCAN_HOLD;
}

static uint8_t index_of(uint32_t catnum)
{
	for (uint8_t i = 0; i < sat_count; i++)
		if (sat_table[i].catnum == catnum)
			return i;
	return SAT_NONE;
}

/* A fresh radio task's view: the ISS due in 5 min (it can be tracked), SO-50 in 2 h, AO-91 in 5 h, PO-101 none */
static void fake_reset(bool clock)
{
	memset(&fv, 0, sizeof fv);
	fv.clock = clock ? SAT_CLOCK_OK : SAT_CLOCK_NONE;
	fv.track = SAT_NONE;
	fv.alarm.target = SAT_NONE;
	fv.az10 = 1234;
	fv.el10 = -12;
	fv.range_km = 2345;
	fv.rate_ms = -5432;
	fv.cost_us = 1500;
	t0 = (int32_t)sat_table[iss].el.epoch + 2 * 86400;
	fv.pass[iss] = (sat_pass_t){ t0 + 300, t0 + 900, 47, SAT_PASS_OK };
	fv.pass[so50] = (sat_pass_t){ t0 + 7200, t0 + 7800, 23, SAT_PASS_OK };
	fv.pass[ao91] = (sat_pass_t){ t0 + 18000, t0 + 18600, 61, SAT_PASS_OK };
	fv.pass[po101] = (sat_pass_t){ 0, 0, 0, SAT_PASS_NONE };
	focus_seen = SAT_NONE;
	base_ms = 0;
	track_calls = alarm_calls = 0;
	soon_ms = aos_ms = track_ms = 0;
	beeps_soon = beeps_aos = 0;
	/* ui_sat.c's own state, as after power-on */
	sel = 0;
	tz_min = 180;
	alarm_page = false;
	a_row = a_for = a_el = 0;
	a_lead = 1;
	a_every = false;
	a_auto = 0;
	note_loaded = false;
	al_opts = 0;
	fake_auto = fake_idle = false;
	cancels = 0;
	starts = 0;
	refuse = 0;
	start_sat = SAT_NONE;
	start_mode = mode_now = 0;
	pop = SAT_NONE;
	iss_aprs = false;
	ui_svc_setting_set(UI_SET_SAT_NOTE, 0);
	ui_svc_setting_set(UI_SET_SAT_AUTO, 0);
}

#define SATS "ok 6 down down down down down down down down ok "	/* Tools > Satellites */
#define PAGE SATS "down ok "		/* SO-50 (nothing to track): the alarm page */

typedef struct {
	flow_t f;
	bool clock;
	int track;		/* -1: not checked; else the satellite tracked after (SAT_NONE: none) */
	int alarm;		/* -1: not checked; else the alarm's target after */
	int lead, el;		/* with alarm: what it was armed with */
	int soon, aos;		/* -1: not checked; else the tones asked for */
} sat_flow_t;

/* One flow of the checks below, from a fresh fake (prep: what to set after it) */
static void auto_flow(const char *name, const char *setup, const char *script, const char *expect, int budget)
{
	flow_t f = { name, setup, script, expect, budget, false };
	run_flow(&f, sim_geom_find("240x320i10x5"), SIM_MAP_M, "Plus DM32");
}

/* Every pass, auto track, the stored alarm at power on, the banner held back during TX, a key ending an auto track */
static int auto_checks(void)
{
	int bad = 0;
#define AUTO_CHECK(c, name) do { if (!(c)) { printf("FAIL %s: %s\n", name, #c); bad++; } } while (0)
	const char *n;
	/* The page's new rows */
	fake_reset(true);
	auto_flow("Sat alarm page: Every pass and Auto track rows", PAGE, "",
		  "shows=Every_pass shows=Auto_track shows=Off_after_a_power_cycle", 0);
	/* Every pass on the ISS (0 opens the page on the soonest, the ISS): armed with it, stored */
	fake_reset(true);
	auto_flow(n = "Sat every pass on the ISS", SATS, "0 down down down right ok",
		  "toast=Armed_for_every_pass shows=Kept_across_power_cycles", 6);
	AUTO_CHECK(fv.alarm.target == iss && al_opts == SAT_OPT_EVERY && al_lead == 5 && al_el == 0, n);
	AUTO_CHECK(ui_svc_setting_get(UI_SET_SAT_NOTE) == 1, n);
	/* Changed while armed (10 min, 20 deg): armed again and stored again */
	fake_reset(true);
	auto_flow(n = "Sat every pass changed while armed", SATS, "0 down down down right ok up up right down right right",
		  "shows=10_min", 12);
	AUTO_CHECK(al_lead == 10 && al_el == 20 && (al_opts & SAT_OPT_EVERY), n);
	AUTO_CHECK(ui_svc_setting_get(UI_SET_SAT_NOTE) == (1 | (1 * 5 + 2) << 2), n);
	/* Off: stored off */
	fake_reset(true);
	auto_flow(n = "Sat every pass off", SATS, "0 down down down right ok ok", "toast=Pass_alarm_off", 7);
	AUTO_CHECK(fv.alarm.target == SAT_NONE && ui_svc_setting_get(UI_SET_SAT_NOTE) == 0, n);
	/* Every pass with SO-50 in For: the ISS instead; SO-50 picked again turns Every pass off */
	fake_reset(true);
	auto_flow("Sat every pass picks the ISS", PAGE, "down down down right", "shows=ISS shows=On", 4);
	fake_reset(true);
	auto_flow(n = "Sat a satellite of its own: Every pass off", PAGE, "down down down right up up up right",
		  "shows=SO-50 hides=Kept_across", 8);
	AUTO_CHECK(!a_every, n);
	/* Auto track APRS (Left from Off), stored; armed with it */
	fake_reset(true);
	auto_flow(n = "Sat auto track APRS", PAGE, "up left ok", "shows=APRS shows=Tracks_the_downlink,_RX", 3);
	AUTO_CHECK(ui_svc_setting_get(UI_SET_SAT_AUTO) == SAT_AUTO_APRS, n);
	AUTO_CHECK(al_opts == (SAT_AUTO_APRS << SAT_OPT_AUTO_SHIFT) && fv.alarm.target == so50, n);
	fake_reset(true);
	auto_flow(n = "Sat auto track APRS on the ISS", SATS, "0 up left", "shows=Tracks_ISS_145.825,_RX", 3);
	fake_reset(true);
	auto_flow(n = "Sat auto track on the ISS", SATS, "0 up right", "shows=Tracks_ISS_145.800,_RX", 3);
	AUTO_CHECK(ui_svc_setting_get(UI_SET_SAT_AUTO) == SAT_AUTO_DOWN, n);
	/* Stored Every pass (any satellite, 10 min, 30 deg) and auto track: armed at power on, on Home */
	fake_reset(true);
	ui_svc_setting_set(UI_SET_SAT_NOTE, 2 | (1 * 5 + 3) << 2);
	ui_svc_setting_set(UI_SET_SAT_AUTO, SAT_AUTO_DOWN);
	auto_flow(n = "Sat every pass armed at power on", "", "wait:500", "screen=home", 0);
	AUTO_CHECK(alarm_calls == 1 && fv.alarm.target == SAT_ALARM_ANY && al_lead == 10 && al_el == 30, n);
	AUTO_CHECK(al_opts == (SAT_OPT_EVERY | SAT_AUTO_DOWN << SAT_OPT_AUTO_SHIFT), n);
	AUTO_CHECK(fake_idle, n);	/* Home, idle: an auto track may start */
	/* Off Home (a menu): not idle */
	fake_reset(true);
	auto_flow(n = "Sat not idle in a menu", "", "ok wait:300", "", 1);
	AUTO_CHECK(!fake_idle, n);
	/* The warning while transmitting: held back (no tone, no banner) until the end */
	fake_reset(true);
	auto_flow(n = "Sat alarm during TX: held back", PAGE "ok long:back", "pttdown wait:1500", "screen=home", 1);
	AUTO_CHECK(beeps_soon == 0 && soon_ms != 0 && !fake_idle, n);
	fake_reset(true);
	auto_flow(n = "Sat alarm after TX: shown", PAGE "ok long:back", "pttdown wait:1500 pttup wait:300",
		  "screen=home banner=SO-50_in_5_min,_max_23" GLYPH_DEGREE, 1);
	AUTO_CHECK(beeps_soon == 1, n);
	/* Auto track started: "ISS tracking, max 47°", no second tone */
	fake_reset(true);
	track_ms = 1;
	auto_flow(n = "Sat auto track banner", "", "wait:200", "screen=home banner=ISS_tracking,_max_47" GLYPH_DEGREE, 0);
	AUTO_CHECK(beeps_soon == 0 && beeps_aos == 0, n);
	/* A key while an auto track runs ends it, used for that (Home stays; no menu opened) */
	fake_reset(true);
	fake_auto = true;
	auto_flow(n = "Sat a key ends an auto track", "", "ok", "screen=home toast=Auto_track_off:_radio_back", 1);
	AUTO_CHECK(cancels == 1 && !fake_auto, n);
	/* PTT too (it goes on to the radio, which refuses TX while it holds) */
	fake_reset(true);
	fake_auto = true;
	auto_flow(n = "Sat PTT ends an auto track", "", "ptt:300", "screen=home", 1);
	AUTO_CHECK(cancels == 1, n);
	/* Without an auto track a key is the screen's as before */
	fake_reset(true);
	auto_flow(n = "Sat a key without an auto track", "", "ok", "hides=Auto_track_off", 1);
	AUTO_CHECK(cancels == 0, n);
	/* Holehunt B1: the emergency key (its long press, and the rest of it) is never taken: it goes on to the emergency
	 * (the radio core ends the hold for it) */
	fake_reset(true);
	fake_auto = true;
	{
		static const uint8_t g[] = { GESTURE_DOWN, GESTURE_LONG, GESTURE_UP };
		bool taken = false;
		for (unsigned i = 0; i < sizeof g; i++) {
			input_event_t ev = { ACT_EMERGENCY, g[i], 0 };
			taken = taken || ui_sat_auto_key(&ev);
		}
		AUTO_CHECK(!taken && cancels == 0, "Sat the emergency key is not taken by an auto track");
	}
#undef AUTO_CHECK
	return bad;
}

/* --- Tools > ISS and the AOS pop-up, in English and in Turkish (ASCII parts of the translations) --- */
static bool turkish;
#define T(en, tr) (turkish ? (tr) : (en))
#define ISSP "ok 6 up up ok "	/* Tools > ISS (after All digital search, before Battery) */
#define ARMED SATS "0 ok long:back "	/* the ISS's pass alarm armed (one pass, auto track off), Home */

/* One flow: a fresh fake and language, the setup, prep (between setup and script, may be NULL), the script, the
 * expectations; layout: the screen checked for clipped or overlapping text too */
static int iss_flow(const char *name, const char *setup, void (*prep)(void), const char *script, const char *expect,
		    bool layout)
{
	char err[300], exp[400], issues[SIM_LAYOUT_MAX][160];
	int bad = 0;
	fake_reset(true);
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
	if (!lang_set(lang_find(turkish ? 't' << 8 | 'r' : 'e' << 8 | 'n'))) {
		printf("FAIL %s: no such language in this build\n", name);
		return 1;
	}
	flows_run++;
	if (!sim_run(setup, err, sizeof err)) {
		printf("FAIL %s (%s) setup: %s\n", name, T("en", "tr"), err);
		return 1;
	}
	if (prep)
		prep();
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s\n", name, T("en", "tr"), err);
		return 1;
	}
	char *o = exp;
	for (const char *p = expect; *p && o < exp + sizeof exp - 16;) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		memcpy(o, "expect:", 7);
		o += 7;
		while (*p && *p != ' ' && o < exp + sizeof exp - 2)
			*o++ = *p++;
		*o++ = ' ';
	}
	*o = 0;
	if (!sim_run(exp, err, sizeof err)) {
		printf("FAIL %s (%s): %s\n", name, T("en", "tr"), err);
		bad++;
	}
	if (layout) {
		int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
		for (int k = 0; k < n; k++)
			printf("FAIL %s (%s): layout: %s\n", name, T("en", "tr"), issues[k]);
		bad += n != 0;
	}
	return bad;
}

static void prep_late(void) { fv.pass[iss] = (sat_pass_t){ t0 + 7200, t0 + 7800, 35, SAT_PASS_OK }; }
static void prep_held(void) { fv.track = iss; fv.track_state = SAT_TRK_ON; fv.rx_hz = 145803000u; fake_auto = true; }
static void prep_up(void)
{
	fv.pass[iss] = (sat_pass_t){ t0 - 120, t0 + 420, 47, SAT_PASS_OK };
	fv.el10 = 234;
	fv.rate_ms = -4000;
}
static void prep_no_clock(void) { fv.clock = SAT_CLOCK_NONE; }
static void prep_aprs(void) { iss_aprs = true; }
static void prep_busy(void) { refuse = SAT_NO_BUSY; }
/* The fake alarm's warning and rise not coming (its banner hidden): the page's own lines in view */
static void prep_quiet(void)
{
	soon_ms = aos_ms = 0;
	ui_st.banner.kind = UI_BANNER_NONE;
}
/* A knob turn while the pop-up is up: not taken by it (ui_sat_auto_key false: Home's) */
static bool knob_taken;
static void prep_knob(void)
{
	input_event_t ev = { ACT_ENC_STEP, GESTURE_DOWN, 1 };
	knob_taken = ui_sat_auto_key(&ev);
}
static void prep_manual(void) { refuse = SAT_NO_MANUAL; }
#define ARMED_AO SATS "down down ok ok long:back "	/* AO-91's pass alarm armed (another satellite's), Home */
#define TRACKING T("banner=ISS_tracking,_max_47", "banner=ISS_izleniyor,_maks_47")

static int iss_checks(void)
{
	int bad = 0;
	const char *n;
#define ISS_CHECK(c, name) do { if (!(c)) { printf("FAIL %s (%s): %s\n", name, T("en", "tr"), #c); bad++; } } while (0)
	/* The page: the next pass, AOS / LOS, the downlinks, the rows from Before AOS on, the hint */
	bad += iss_flow("ISS page", ISSP, NULL, "",
			T("screen=iss shows=AOS_in_5:00__47 shows=AOS_ shows=LOS_ shows=145.800_voice_/_SSTV "
			  "shows=145.825_APRS shows=Before_AOS shows=Auto_track shows=Every_pass hides=For "
			  "shows=OK_track__#_APRS__1_freq shows=RX_145.80",
			  "screen=iss shows=AOS'a_5:00 shows=AOS_ shows=145.800_ses_/_SSTV shows=145.825_APRS "
			  "shows=OK_takip__#_APRS__1_frek"), true);
	/* OK within 10 min of AOS: the held track on 145.800 */
	bad += iss_flow(n = "ISS OK tracks", ISSP, NULL, "ok", "toast=none banner=none", false);
	ISS_CHECK(starts == 1 && start_sat == iss && start_mode == SAT_AUTO_DOWN, n);
	/* ... "tracking" only once the service says so; its refusals as toasts */
	bad += iss_flow("ISS OK tracks: the service's word", ISSP, NULL, "ok wait:1500", TRACKING, false);
	bad += iss_flow("ISS OK refused: radio busy", ISSP, prep_busy, "ok wait:1500",
			T("toast=Can't_track:_radio_busy banner=none", "toast=Takip_olmaz:_telsiz_me banner=none"), false);
	bad += iss_flow("ISS OK refused: a track runs", ISSP, prep_manual, "ok wait:1500",
			T("toast=Can't_track:_a_track_runs", "toast=Takip_olmaz:_takip_s"), false);
	/* Another satellite's alarm: shown, left alone by the rows; Every pass On replaces it, said */
	bad += iss_flow(n = "ISS with AO-91's alarm armed", ARMED_AO ISSP, prep_quiet, "right wait:300",
			T("shows=Alarm_AO-91_armed", "shows=Alarm_AO-91_kurulu"), true);
	ISS_CHECK(fv.alarm.target == ao91 && alarm_calls == 1 && al_lead == 5, n);
	bad += iss_flow(n = "ISS every pass replaces AO-91's alarm", ARMED_AO ISSP, prep_quiet, "down down right",
			T("toast=Every_ISS_pass:_other_alarm_off", "toast=Her_ISS_ge"), false);
	ISS_CHECK(fv.alarm.target == iss && (al_opts & SAT_OPT_EVERY), n);
	/* 1: 145.825 (marked, and the hint names 145.800), OK tracks that */
	bad += iss_flow(n = "ISS 1: the APRS downlink", ISSP, NULL, "1 ok",
			"shows=>_145.825_APRS", true);
	ISS_CHECK(starts == 1 && start_mode == SAT_AUTO_APRS && mode_now == SAT_AUTO_APRS, n);
	/* Too early: it says when, nothing started */
	bad += iss_flow(n = "ISS OK too early", ISSP, prep_late, "ok",
			T("toast=Track_from_AOS-10_min", "toast=AOS-10_dk'dan_takip"), false);
	ISS_CHECK(starts == 0, n);
	/* A held track: the keys are the page's (not the auto track's any-key end); OK stops it, the radio back */
	bad += iss_flow(n = "ISS OK stops a held track", ISSP, prep_held, "wait:1200",
			T("shows=Tracking,_back_at_LOS shows=OK_stop__#_APRS shows=RX_145.803",
			  "shows=Takipte shows=OK_durdur__#_APRS"), true);
	bad += iss_flow(n = "ISS OK stops a held track", ISSP, prep_held, "1 ok",
			T("screen=iss toast=Auto_track_off:_radio_back", "screen=iss"), false);
	ISS_CHECK(cancels == 0 && track_calls == 1 && fv.track == SAT_NONE && mode_now == SAT_AUTO_APRS, n);
	/* Above now: "Above now, LOS in", the live azimuth / elevation, the Doppler */
	bad += iss_flow("ISS above now", ISSP, prep_up, "wait:1200",
			T("shows=LOS_in_7:00 shows=Above_now,_max_47 shows=Az_123 shows=El_23.4 shows=+1.9_kHz",
			  "shows=LOS'a_7:00 shows=tepede,_maks_47 shows=Az_123 shows=El_23.4"), true);
	bad += iss_flow("ISS without the GPS time", ISSP, prep_no_clock, "wait:1200",
			T("shows=Waiting_for_the_GPS_time shows=Before_AOS", "screen=iss shows=GPS"), true);
	/* Every pass on: the ISS armed for every pass and kept; off again: off and forgotten */
	bad += iss_flow(n = "ISS every pass on", ISSP, NULL, "down down right", "screen=iss", true);
	ISS_CHECK(fv.alarm.target == iss && (al_opts & SAT_OPT_EVERY) && al_lead == 5 && al_el == 0, n);
	ISS_CHECK(ui_svc_setting_get(UI_SET_SAT_NOTE) == 1, n);
	bad += iss_flow(n = "ISS every pass changed", ISSP, NULL, "down down right up up right", "shows=10", false);
	ISS_CHECK(fv.alarm.target == iss && al_lead == 10 && ui_svc_setting_get(UI_SET_SAT_NOTE) == (1 | 5 << 2), n);
	bad += iss_flow(n = "ISS every pass off", ISSP, NULL, "down down right left", "screen=iss", false);
	ISS_CHECK(fv.alarm.target == SAT_NONE && ui_svc_setting_get(UI_SET_SAT_NOTE) == 0, n);
	/* Auto track (Up from Before AOS wraps to it): On, stored */
	bad += iss_flow(n = "ISS auto track row", ISSP, NULL, "up right", "screen=iss", false);
	ISS_CHECK(ui_svc_setting_get(UI_SET_SAT_AUTO) == SAT_AUTO_DOWN, n);
	/* #: APRS via ISS, its gates (below the horizon here) */
	bad += iss_flow("ISS # below the horizon", ISSP, NULL, "ctx",
			T("toast=ISS_below_the_horizon", "toast=ISS"), false);

	/* The AOS pop-up (auto track Off): "ISS overhead, max 23°", "OK: track"; OK tracks (145.800), Home stays */
	bad += iss_flow(n = "Pop-up at AOS", ARMED, NULL, "wait:10500",
			T("screen=home banner=ISS_overhead,_max_23" GLYPH_DEGREE " shows=OK:_track shows=OK_track hides=OK_Menu",
			  "screen=home banner=ISS_tepede,_maks_23" GLYPH_DEGREE " shows=OK:_takip shows=OK_takip"), true);
	ISS_CHECK(beeps_aos == 1, n);
	bad += iss_flow(n = "Pop-up OK tracks", ARMED, NULL, "wait:10500 ok", "screen=home banner=none toast=none", false);
	ISS_CHECK(starts == 1 && start_sat == iss && start_mode == SAT_AUTO_DOWN, n);
	bad += iss_flow("Pop-up OK: the service's word", ARMED, NULL, "wait:10500 ok wait:1500", T("screen=home banner=ISS_tracking,_max_47", "screen=home banner=ISS_izleniyor,_maks_47"),
			false);
	bad += iss_flow("Pop-up OK refused: radio busy", ARMED, prep_busy, "wait:10500 ok wait:1500",
			T("screen=home toast=Can't_track:_radio_busy", "screen=home toast=Takip_olmaz:_telsiz_me"), false);
	bad += iss_flow(n = "Pop-up OK tracks 145.825", ARMED, prep_aprs, "wait:10500 ok", "screen=home banner=none", false);
	ISS_CHECK(starts == 1 && start_mode == SAT_AUTO_APRS, n);
	bad += iss_flow(n = "Pop-up Back dismisses", ARMED, NULL, "wait:10500 back", "screen=home banner=none toast=none",
			false);
	ISS_CHECK(starts == 0, n);
	/* Other keys and the knob are Home's: the pop-up stays up, nothing tracked */
	bad += iss_flow(n = "Pop-up: another key is Home's", ARMED, NULL, "wait:10500 down wait:300",
			T("screen=home banner=ISS_overhead shows=OK_track", "screen=home banner=ISS_tepede"), false);
	ISS_CHECK(starts == 0, n);
	bad += iss_flow(n = "Pop-up: the knob is Home's", ARMED "wait:10500 ", prep_knob, "wait:300",
			T("screen=home banner=ISS_overhead", "screen=home banner=ISS_tepede"), false);
	ISS_CHECK(starts == 0 && !knob_taken, n);
	bad += iss_flow(n = "Pop-up: long OK is Home's", ARMED, NULL, "wait:10500 long:ok wait:300",
			T("banner=ISS_overhead", "banner=ISS_tepede"), false);
	ISS_CHECK(starts == 0, n);
	/* Not on Home (a menu open at the rise): the plain banner, no key taken */
	bad += iss_flow(n = "No pop-up off Home", ARMED, NULL, "ok wait:10500",
			T("screen=hub banner=ISS_AOS_now hides=OK:_track", "screen=hub hides=OK:_takip"),
			true);
	bad += iss_flow(n = "No pop-up off Home: OK and Back are the menu's", ARMED, NULL, "ok wait:10500 back ok",
			"screen=hub", false);
	ISS_CHECK(starts == 0, n);
	/* Up for 30 s (the rise ~10 s in), then gone; OK after that is Home's again */
	bad += iss_flow("Pop-up up for 30 s", ARMED, NULL, "wait:36000",
			T("banner=ISS_overhead", "banner=ISS_tepede"), false);
	bad += iss_flow(n = "Pop-up gone after 30 s", ARMED, NULL, "wait:45000 ok", "banner=none", false);
	ISS_CHECK(starts == 0, n);
	/* Not while transmitting: no tone, no pop-up; after the release it comes, and OK tracks */
	bad += iss_flow(n = "Pop-up waits for TX", ARMED, NULL, "pttdown wait:11000", "banner=none", false);
	ISS_CHECK(beeps_aos == 0 && starts == 0, n);
	bad += iss_flow(n = "Pop-up after TX", ARMED, NULL, "pttdown wait:11000 pttup wait:300 ok wait:1500",
			T("screen=home banner=ISS_tracking,_max_47", "screen=home banner=ISS_izleniyor,_maks_47"), false);
	ISS_CHECK(beeps_aos == 1 && starts == 1 && start_sat == iss, n);
	/* Auto track On: the rise's banner as before (it starts by itself), OK is Home's */
	bad += iss_flow(n = "No pop-up with auto track on", SATS "0 up right ok long:back ", NULL, "wait:10500",
			T("banner=ISS_AOS_now,_max_23" GLYPH_DEGREE " hides=OK:_track", "hides=OK:_takip"), false);
	bad += iss_flow(n = "No pop-up with auto track on: OK", SATS "0 up right ok long:back ", NULL, "wait:10500 ok",
			"hides=OK:_track", false);
	ISS_CHECK(starts == 0, n);
#undef ISS_CHECK
	return bad;
}

int main(int argc, char **argv)
{
	iss = index_of(25544u);
	so50 = index_of(27607u);
	ao91 = index_of(43017u);
	po101 = index_of(43678u);
	if (iss == SAT_NONE || so50 == SAT_NONE || ao91 == SAT_NONE || po101 == SAT_NONE) {
		printf("FAIL the shipped table lacks a satellite these flows use\n");
		return 1;
	}
	if (argc > 1) {		/* probe: run a script, print the screen's texts */
		char err[300];
		fake_reset(true);
		sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
		sim_run(argv[1], err, sizeof err);
		ui_app_status_t st;
		ui_app_status(&st);
		printf("screen=%s focus=%d toast=%s banner=%s\n", st.screen, (int)st.focus, st.toast, st.banner);
		sim_run("expect:shows=@@@", err, sizeof err);
		printf("%s\n", err);
		if (argc > 2)
			sim_save_png(argv[2], 2);
		return 0;
	}
	const sat_flow_t checks[] = {
		{ { "Sat list: soonest first, the ISS can be tracked", SATS, "",
		    "screen=satellites shows=ISS shows=SO-50 shows=AO-91 shows=OK_track__#_APRS_via_ISS", 0, false },
		  true, SAT_NONE, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat live lines: Az/El, range rate, Doppler in kHz", SATS, "",
		    "shows=Az_123 shows=El_-1.2 shows=2345_km__-5.43_km/s shows=+7.9_kHz shows=OK:_track_in_the_VFO", 0, false },
		  true, SAT_NONE, -1, 0, 0, -1, -1 },
		{ { "Sat track the ISS", SATS, "ok", "toast=VFO_tracks_the_pass", 1, false },
		  true, 0, -1, 0, 0, -1, -1 },
		{ { "Sat tracking shown", SATS, "ok wait:2500", "shows=Tracking_Doppler shows=OK_stop", 1, false },
		  true, 0, -1, 0, 0, -1, -1 },
		{ { "Sat stop tracking", SATS, "ok ok", "toast=Tracking_off:_VFO_stays", 2, false },
		  true, SAT_NONE, -1, 0, 0, -1, -1 },
		{ { "Sat nothing to track: OK alarm", SATS, "down", "shows=^v_pick__OK_alarm", 1, false },
		  true, SAT_NONE, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm page", SATS, "down ok",
		    "screen=satellites shows=Pass_alarm shows=For shows=SO-50 shows=Before_AOS shows=5_min "
		    "shows=Min._elevation shows=Off:_OK_arms_it shows=OK_arm", 2, false },
		  true, SAT_NONE, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm armed", PAGE, "ok", "toast=Pass_alarm_armed", 1, false },
		  true, -1, 1, 5, 0, -1, -1 },
		{ { "Sat alarm armed: the pass and the tone time", PAGE, "ok wait:2100",
		    "shows=SO-50 shows=Tone_again_at_AOS shows=OK_off__<>_change", 1, false },
		  true, -1, 1, 5, 0, 1, 0 },
		{ { "Sat alarm lead and elevation", PAGE, "down right down right right ok",
		    "shows=10_min shows=20" GLYPH_DEGREE " toast=Pass_alarm_armed", 6, false },
		  true, -1, 1, 10, 20, -1, -1 },
		{ { "Sat alarm lead wraps", PAGE, "down right right", "shows=2_min", 3, false },
		  true, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm any satellite", PAGE, "left left ok", "shows=Any_satellite toast=Pass_alarm_armed", 3, false },
		  true, -1, SAT_ALARM_ANY, 5, 0, -1, -1 },
		{ { "Sat alarm changed while armed", PAGE, "ok down down right", "shows=10" GLYPH_DEGREE, 4, false },
		  true, -1, 1, 5, 10, -1, -1 },
		{ { "Sat alarm off", PAGE, "ok ok", "toast=Pass_alarm_off shows=Off:_OK_arms_it", 2, false },
		  true, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm page: Back to the list", PAGE, "back", "screen=satellites shows=Satellites hides=Pass_alarm", 1, false },
		  true, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm page: Back twice leaves", PAGE, "back back", "hides=Pass_alarm hides=SO-50", 2, false },
		  true, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat 0: the alarm page on the ISS", SATS, "0", "shows=Pass_alarm shows=ISS", 1, false },
		  true, SAT_NONE, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat alarm on Home: the warning", PAGE "ok long:back", "wait:2500",
		    "screen=home banner=SO-50_in_5_min,_max_23" GLYPH_DEGREE, 0, false },
		  true, -1, 1, 5, 0, 1, 0 },
		{ { "Sat alarm on Home: the rise, a pop-up", PAGE "ok long:back", "wait:10500",
		    "screen=home banner=SO-50_overhead,_max_23" GLYPH_DEGREE " shows=OK:_track", 0, false },
		  true, -1, SAT_NONE, 5, 0, 1, 1 },
		{ { "Sat alarm over a zone offer: a toast", "zoneoffer:3101234 " PAGE "ok long:back", "wait:2500",
		    "shows=Zone_from toast=SO-50_in_5_min", 0, false },
		  true, -1, 1, 5, 0, 1, 0 },
		{ { "Sat without the GPS time: OK named", SATS, "", "shows=Waiting_for_the_GPS_time shows=OK_alarm", 0, false },
		  false, -1, SAT_NONE, 0, 0, -1, -1 },
		{ { "Sat without the GPS time: alarm armed", SATS, "0 ok", "shows=Armed:_waits_for_GPS", 2, false },
		  false, -1, 0, 5, 0, -1, -1 },
	};
	int sat_fails = 0;
	for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++) {
		const sat_flow_t *c = &checks[i];
		fake_reset(c->clock);
		int before = fails;
		run_flow(&c->f, sim_geom_find("240x320i10x5"), SIM_MAP_M, "Plus DM32");
		if (c->track >= 0 && fv.track != c->track) {
			printf("FAIL %s: tracking %d, want %d\n", c->f.name, fv.track, c->track);
			fails++;
		}
		if (c->alarm >= 0 && (fv.alarm.target != c->alarm ||
				      (c->alarm != SAT_NONE && (al_lead != c->lead || al_el != c->el)))) {
			printf("FAIL %s: alarm %d %d min %d deg, want %d %d %d\n", c->f.name, fv.alarm.target, al_lead, al_el,
			       c->alarm, c->lead, c->el);
			fails++;
		}
		if ((c->soon >= 0 && beeps_soon != c->soon) || (c->aos >= 0 && beeps_aos != c->aos)) {
			printf("FAIL %s: tones %d / %d, want %d / %d\n", c->f.name, beeps_soon, beeps_aos, c->soon, c->aos);
			fails++;
		}
		sat_fails += fails != before;
	}
	fails += auto_checks();
	for (int l = 0; l < 2; l++) {
		turkish = l == 1;
		fails += iss_checks();
	}
	turkish = false;
	lang_set(lang_find('e' << 8 | 'n'));
	/* The layout check on the list (tracking, alarm armed) and on the page (Every pass, auto track APRS) */
	static const char *const layouts[] = { SATS "ok wait:2500", PAGE "ok wait:2500", PAGE "left left ok wait:2500",
					       SATS "down down down", PAGE "down down down right up left ok wait:2500" };
	for (unsigned i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
		char err[300], issues[SIM_LAYOUT_MAX][160];
		fake_reset(true);
		sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
		if (!sim_run(layouts[i], err, sizeof err)) {
			printf("FAIL Sat layout %u: %s\n", i, err);
			fails++;
			continue;
		}
		int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
		for (int k = 0; k < n; k++)
			printf("FAIL Sat layout %u: %s\n", i, issues[k]);
		fails += n != 0;
		flows_run++;
	}
	printf("Plus Satellites screen: %d flows, %d failures\n", flows_run, fails);
	(void)sat_fails;
	return fails != 0;
}
