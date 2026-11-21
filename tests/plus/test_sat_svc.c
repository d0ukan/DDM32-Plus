/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus satellites, the service (radio/sat_svc.c) with the shipped elements, a fake GPS (Ankara, its UTC
 * running with the test's clock) and a fake radio core that records the requests:
 *  - the pass alarm: one satellite and any satellite (with a lowest top elevation) against a one-second brute force
 *    scan; the warning lead_min before the rise and the tone at the rise, each within a second; armed late, under way,
 *    before the GPS time, turned off; no SGP4 a second while it only waits (the live look never runs);
 *  - the tracking: the first request sequence (VFO RX, FM, wide, uplink CTCSS, no RX tone, the split), the step
 *    hysteresis (sat_follow_hz) and the VFO never more than the hold plus a second's drift off the true Doppler, an
 *    uplink move alone not touching RX, no request at all while PTT is held, the nominal frequencies back after the
 *    set, a hand change ending it with the VFO left alone.
 * The fake radio core has no PTT or keying call: the service links only against the VFO, mode, bandwidth and tone
 * requests below, so it cannot key the transmitter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "radio/sat.h"
#include "radio/radio.h"
#include "aprs/aprs.h"
#include "aprs/aprs_fmt.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define LAT	399334000	/* Ankara */
#define LON	328597000
#define ALT	900
#define POLL_MS	25u

/* --- Fakes: the clock, the critical section, the GPS --- */
static uint32_t now_ms = 1000u;
static uint32_t poll_ms = POLL_MS;	/* 100 ms while only the alarm waits: the same once-a-second checks, faster */
static uint32_t micros_calls;
uint32_t hal_millis(void) { return now_ms; }
uint32_t hal_micros(void) { micros_calls++; return now_ms * 1000u; }
void mock_enter(void) {}
void mock_exit(void) {}

static int32_t gps_t0;		/* GPS UTC (seconds since 2000) at now_ms = 0 */
static bool gps_on;

bool aprs_gps(aprs_gps_t *g, uint32_t now)
{
	memset(g, 0, sizeof *g);
	if (!gps_on)
		return false;
	time_t tt = (time_t)(gps_t0 + (int32_t)(now / 1000u)) + 946684800;
	struct tm tm;
	gmtime_r(&tt, &tm);
	g->fix = true;
	g->lat = LAT;
	g->lon = LON;
	g->alt_m = ALT;
	g->utc_hms = (uint32_t)(tm.tm_hour * 10000 + tm.tm_min * 100 + tm.tm_sec);
	g->date = (uint32_t)(tm.tm_mday * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_year % 100);
	return true;
}

static int32_t utc_now(void) { return gps_t0 + (int32_t)(now_ms / 1000u); }

/* --- Fake radio core: the VFO and a log of the requests --- */
typedef struct {
	char k;			/* S vfo_set, T vfo_tx, M mode, B bandwidth, C tone */
	uint32_t v;
	uint32_t ms;
} req_t;
static req_t reqs[4096];
static int nreq;
static bool vfo;
static uint32_t rx_hz, tx_hz;

static void log_req(char k, uint32_t v)
{
	if (nreq < (int)(sizeof reqs / sizeof reqs[0]))
		reqs[nreq++] = (req_t){ k, v, now_ms };
}

void radio_req_vfo_set(uint32_t hz)
{
	log_req('S', hz);
	int32_t off = vfo ? (int32_t)(tx_hz - rx_hz) : 0;	/* the VFO keeps a split's offset */
	vfo = true;
	rx_hz = hz;
	tx_hz = (uint32_t)((int32_t)hz + off);
}

void radio_req_vfo_tx(uint32_t hz)
{
	log_req('T', hz);
	if (vfo)
		tx_hz = hz ? hz : rx_hz;
}

void radio_req_mode(uint8_t mode) { log_req('M', mode); }
void radio_req_bandwidth(radio_bw_t bw) { log_req('B', (uint32_t)bw); }
void radio_req_tone(uint8_t which, subtone_t tone) { log_req('C', (uint32_t)which << 16 | tone.value); }

/* The radio core's hold for an auto track (radio_sathold.inc): taken unless refused (busy), the user's VFO state
 * noted; the end puts it back. Logged as H 1 / H 0 */
static bool held, hold_refuse;
static struct { bool vfo; uint32_t rx, tx; } user;
void radio_req_sat_hold(bool on)
{
	log_req('H', on);
	if (on && !held && !hold_refuse) {
		held = true;
		user.vfo = vfo;
		user.rx = rx_hz;
		user.tx = tx_hz;
	} else if (!on && held) {
		held = false;
		vfo = user.vfo;
		rx_hz = user.rx;
		tx_hz = user.tx;
	}
}
bool radio_sat_held(void) { return held; }

/* --- Running the radio task --- */
typedef struct {
	sat_alarm_event_t ev;
	int32_t at;		/* UTC seconds */
	uint32_t ms;
} got_t;
static got_t got[32];
static int ngot;
static bool screen;		/* the Satellites screen draws (sat_seen each poll) */
static uint8_t focus;
static bool ptt;

static void poll_once(void)
{
	now_ms += poll_ms;
	if (screen)
		sat_seen(focus);
	sat_service(vfo, rx_hz, tx_hz, ptt, false, now_ms);
	sat_alarm_event_t ev;
	while (sat_alarm_take(&ev))
		if (ngot < 32)
			got[ngot++] = (got_t){ ev, utc_now(), now_ms };
}

static void run_s(int32_t s)
{
	for (uint32_t end = now_ms + (uint32_t)s * 1000u; now_ms < end;)
		poll_once();
}

static sat_view_t view_now(void)
{
	sat_view_t v;
	sat_view(&v);
	return v;
}

/* Until the alarm is no longer looking for its pass: the polls it took */
static int run_until_found(void)
{
	int polls = 0;
	do {
		poll_once();
		polls++;
	} while (view_now().alarm.state == SAT_AL_FINDING && polls < 200000);
	return polls;
}

static uint8_t sat_index(uint32_t catnum)
{
	for (uint8_t i = 0; i < sat_count; i++)
		if (sat_table[i].catnum == catnum)
			return i;
	return SAT_NONE;
}

/* The reference: a one-second scan for satellite k's first pass rising after from with its top at least min_el
 * (rounded as the search rounds it), up to SAT_SEARCH_S */
static sat_pass_t brute_next(uint8_t k, int32_t from, int min_el)
{
	sgp4_t p;
	sat_obs_t o;
	sat_pass_t out = { 0, 0, 0, SAT_PASS_NONE };
	sat_observer(LAT, LON, ALT, &o);
	if (sgp4_init(&p, &sat_table[k].el) != SGP4_OK)
		return out;
	bool up = false;
	float top = 0;
	int32_t aos = 0;
	for (int32_t t = from - 1200; t < from + SAT_SEARCH_S + 1200; t++) {
		sat_look_t l;
		if (sat_eval(&p, &sat_table[k], (double)t, &o, &l) != SGP4_OK)
			return out;
		if (l.el >= 0.0f && !up) {
			up = true;
			aos = t;
			top = l.el;
		} else if (l.el >= 0.0f) {
			top = l.el > top ? l.el : top;
		} else if (up) {
			up = false;
			if (aos > from && (int)(top + 0.5f) >= min_el) {
				if (aos - from > SAT_SEARCH_S)
					return out;
				out = (sat_pass_t){ aos, t - 1, (int8_t)(top + 0.5f), SAT_PASS_OK };
				return out;
			}
		}
	}
	return out;
}

/* A fresh service: the static state of sat_svc.c is reset by including it once per scenario would be cleaner, but
 * the service is meant to run for ever; each scenario starts later in time and turns off what the last one left */
static void scenario(const char *name, int32_t utc)
{
	printf("  %s\n", name);
	sat_alarm_set(SAT_NONE, 0, 0, 0);
	sat_track(SAT_NONE);
	screen = false;
	ptt = false;
	hold_refuse = false;
	sat_ui_idle(false);
	gps_t0 = utc - (int32_t)(now_ms / 1000u);
	run_s(2);
	ngot = 0;
	nreq = 0;
}

static void alarm_one(uint8_t k, uint8_t lead, int min_el, int32_t start)
{
	char name[64];
	snprintf(name, sizeof name, "alarm: %s, %u min before, top %d deg or more", sat_table[k].name, lead, min_el);
	scenario(name, start);
	sat_pass_t want = brute_next(k, utc_now(), min_el);
	CHECK(want.state == SAT_PASS_OK);
	sat_alarm_set(k, lead, (uint8_t)min_el, 0);
	int polls = run_until_found();
	poll_ms = 100;
	sat_view_t v = view_now();
	CHECK(v.alarm.state == SAT_AL_ARMED && v.alarm.sat == k && v.alarm.target == k);
	CHECK(abs(v.alarm.aos - want.aos) <= 3);
	CHECK(abs(v.alarm.max_el - want.max_el) <= 1);
	printf("    found in %d polls (one propagation each): rise %+d s against the scan, top %d deg\n", polls,
	       (int)(v.alarm.aos - want.aos), v.alarm.max_el);
	uint32_t micros0 = micros_calls;
	int32_t to_soon = v.alarm.aos - lead * 60 - utc_now();
	CHECK(to_soon > 0);
	run_s(to_soon - 2);
	CHECK(ngot == 0);
	CHECK(micros_calls == micros0);		/* waiting: no live look (no SGP4) once a second */
	run_s(4);
	CHECK(ngot == 1 && got[0].ev.kind == SAT_ALARM_SOON && got[0].ev.sat == k);
	CHECK(got[0].at >= v.alarm.aos - lead * 60 && got[0].at <= v.alarm.aos - lead * 60 + 1);
	CHECK(got[0].ev.in_s >= lead * 60 - 1 && got[0].ev.in_s <= lead * 60);
	CHECK(got[0].ev.max_el == v.alarm.max_el);
	CHECK(view_now().alarm.soon);
	run_s(lead * 60 - 4);
	CHECK(ngot == 1);
	run_s(4);
	CHECK(ngot == 2 && got[1].ev.kind == SAT_ALARM_AOS && got[1].ev.sat == k);
	CHECK(got[1].at >= v.alarm.aos && got[1].at <= v.alarm.aos + 1);
	v = view_now();
	CHECK(v.alarm.target == SAT_NONE && v.alarm.state == SAT_AL_OFF);
	run_s(3600);
	CHECK(ngot == 2);			/* once: off after the rise */
	CHECK(micros_calls == micros0);
	CHECK(nreq == 0);			/* the alarm never touches the radio */
}

static void alarm_any(int min_el, int32_t start)
{
	char name[64];
	snprintf(name, sizeof name, "alarm: any satellite, top %d deg or more", min_el);
	scenario(name, start);
	int32_t now = utc_now();
	sat_pass_t best = { 0, 0, 0, SAT_PASS_NONE };
	uint8_t bk = SAT_NONE;
	for (uint8_t k = 0; k < sat_count; k++) {
		sat_pass_t p = brute_next(k, now, min_el);
		if (p.state == SAT_PASS_OK && (best.state != SAT_PASS_OK || p.aos < best.aos)) {
			best = p;
			bk = k;
		}
	}
	CHECK(bk != SAT_NONE);
	sat_alarm_set(SAT_ALARM_ANY, 2, (uint8_t)min_el, 0);
	int polls = run_until_found();
	poll_ms = 100;
	sat_view_t v = view_now();
	CHECK(v.alarm.state == SAT_AL_ARMED && v.alarm.target == SAT_ALARM_ANY);
	CHECK(v.alarm.sat == bk && abs(v.alarm.aos - best.aos) <= 3);
	printf("    %s at %+d s from now, top %d deg (scan: %s, %d deg); %d polls\n", v.alarm.sat < sat_count ?
	       sat_table[v.alarm.sat].name : "?", (int)(v.alarm.aos - now), v.alarm.max_el, sat_table[bk].name, best.max_el, polls);
	run_s(v.alarm.aos - utc_now() + 2);
	CHECK(ngot == 2 && got[0].ev.kind == SAT_ALARM_SOON && got[1].ev.kind == SAT_ALARM_AOS && got[1].ev.sat == bk);
	CHECK(got[0].at - (v.alarm.aos - 120) <= 1 && got[0].at >= v.alarm.aos - 120);
}

static void alarm_edges(uint8_t iss, int32_t start)
{
	/* Armed 3 minutes before the rise with 10 minutes' lead: the warning at once, "3 min" */
	scenario("alarm: armed inside the lead time", start);
	sat_pass_t want = brute_next(iss, utc_now(), 0);
	run_s(want.aos - utc_now() - 180);
	sat_alarm_set(iss, 10, 0, 0);
	run_until_found();
	run_s(2);
	CHECK(ngot == 1 && got[0].ev.kind == SAT_ALARM_SOON);
	printf("    the warning at once: in %d s (the rise %d s after the arming)\n", (int)got[0].ev.in_s, 180);
	CHECK(got[0].ev.in_s >= 170 && got[0].ev.in_s <= 180);
	run_s(180);
	CHECK(ngot == 2 && got[1].ev.kind == SAT_ALARM_AOS);

	/* Armed while that pass is under way: the next one */
	scenario("alarm: armed during a pass: the next pass", want.aos + 60);
	sat_pass_t next = brute_next(iss, want.aos + 60, 0);
	CHECK(next.state == SAT_PASS_OK && next.aos > want.los);
	sat_alarm_set(iss, 5, 0, 0);
	run_until_found();
	CHECK(view_now().alarm.state == SAT_AL_ARMED && abs(view_now().alarm.aos - next.aos) <= 3);

	/* The screen open: its own search result is taken (no search of the alarm's own) */
	scenario("alarm: from the screen's result", start);
	screen = true;
	focus = iss;
	for (int i = 0; i < 20000 && view_now().pass[iss].state != SAT_PASS_OK; i++)
		poll_once();
	CHECK(view_now().pass[iss].state == SAT_PASS_OK);
	sat_alarm_set(iss, 5, 0, 0);
	int polls = run_until_found();
	CHECK(polls <= 2);
	CHECK(view_now().alarm.state == SAT_AL_ARMED && view_now().alarm.aos == view_now().pass[iss].aos);
	screen = false;

	/* Turned off: nothing more */
	scenario("alarm: off", start);
	sat_alarm_set(iss, 2, 0, 0);
	run_until_found();
	CHECK(view_now().alarm.state == SAT_AL_ARMED);
	sat_alarm_set(SAT_NONE, 0, 0, 0);
	run_s(1);
	CHECK(view_now().alarm.state == SAT_AL_OFF && view_now().alarm.target == SAT_NONE);
	poll_ms = 100;
	run_s(want.los - utc_now() + 600 > 0 ? want.los - utc_now() + 600 : 600);
	CHECK(ngot == 0);

	/* Too high to happen: none in the span */
	scenario("alarm: no pass that high", start);
	sat_alarm_set(iss, 5, 95, 0);
	run_until_found();
	CHECK(view_now().alarm.state == SAT_AL_NONE);
}

/* At power-on the alarm is off; armed before the GPS has the time it shows as looking, and finds its pass once the
 * time and position come */
static void alarm_before_gps(uint8_t iss, int32_t start)
{
	printf("  alarm: armed before the GPS time\n");
	gps_on = false;
	gps_t0 = start;
	CHECK(view_now().alarm.target == SAT_NONE && view_now().alarm.state == SAT_AL_OFF);
	sat_alarm_set(iss, 5, 0, 0);
	run_s(5);
	sat_view_t v = view_now();
	CHECK(v.clock == SAT_CLOCK_NONE && v.alarm.target == iss && v.alarm.state == SAT_AL_FINDING);
	CHECK(v.alarm.lead_min == 5 && v.alarm.min_el == 0);
	gps_on = true;
	run_until_found();
	v = view_now();
	CHECK(v.clock == SAT_CLOCK_OK && v.alarm.state == SAT_AL_ARMED && v.alarm.aos > utc_now());
	sat_alarm_set(SAT_NONE, 0, 0, 0);
	run_s(1);
}

/* --- Tracking --- */

static int count(char k, int from)
{
	int n = 0;
	for (int i = from; i < nreq; i++)
		n += reqs[i].k == k;
	return n;
}

static void tracking(uint8_t k, int32_t start)
{
	const sat_info_t *s = &sat_table[k];
	char name[64];
	snprintf(name, sizeof name, "tracking %s: requests, hysteresis, PTT, the set", s->name);
	scenario(name, start);
	sat_pass_t want = brute_next(k, utc_now(), 10);
	CHECK(want.state == SAT_PASS_OK);
	run_s(want.aos - utc_now() - 400);
	vfo = false;
	rx_hz = tx_hz = 0;
	screen = true;
	focus = k;
	run_s(60);			/* the screen finds the pass */
	CHECK(view_now().pass[k].state == SAT_PASS_OK);
	nreq = 0;
	sat_track(k);
	run_s(2);
	/* The first second: into the VFO */
	CHECK(nreq >= 6);
	CHECK(reqs[0].k == 'S' && reqs[1].k == 'M' && reqs[1].v == RADIO_MODE_FM && reqs[2].k == 'B' &&
	      reqs[2].v == RADIO_BW_25K && reqs[3].k == 'C' && reqs[3].v == (RADIO_TONE_TX << 16 | s->ctcss10) &&
	      reqs[4].k == 'C' && reqs[4].v == (RADIO_TONE_RX << 16 | 0) && reqs[5].k == 'T');
	CHECK(reqs[0].v % sat_step_hz(s->down_hz) == s->down_hz % sat_step_hz(s->down_hz));
	CHECK(view_now().track == k && view_now().track_state == SAT_TRK_ON);
	screen = false;			/* the screen closed: the tracking goes on */
	/* The pass: every second the VFO against the true Doppler of now */
	sgp4_t p;
	sat_obs_t o;
	sat_observer(LAT, LON, ALT, &o);
	sgp4_init(&p, &s->el);
	int32_t hold_rx = (int32_t)sat_step_hz(s->down_hz) * SAT_HOLD_8THS / 8;
	int32_t hold_tx = (int32_t)sat_step_hz(s->up_hz) * SAT_HOLD_8THS / 8;
	int32_t worst_rx = 0, worst_tx = 0, drift_rx = 0, drift_tx = 0, prev_drx = 0, prev_dtx = 0;
	int first = nreq, ptt_from = 0, tx_only_moves = 0, retunes = 0;
	uint32_t last_rx = rx_hz, last_tx = tx_hz;
	int32_t mid = (want.aos + want.los) / 2;
	while (utc_now() < want.los) {
		run_s(1);
		if (utc_now() < want.aos)
			continue;
		sat_look_t l;
		sat_eval(&p, s, (double)now_ms / 1000.0 + gps_t0, &o, &l);
		int32_t drx = sat_doppler_rx(s->down_hz, l.rate_kms), dtx = sat_doppler_tx(s->up_hz, l.rate_kms);
		if (prev_drx || prev_dtx) {
			drift_rx = abs(drx - prev_drx) > drift_rx ? abs(drx - prev_drx) : drift_rx;
			drift_tx = abs(dtx - prev_dtx) > drift_tx ? abs(dtx - prev_dtx) : drift_tx;
		}
		prev_drx = drx;
		prev_dtx = dtx;
		int32_t er = (int32_t)(rx_hz - s->down_hz) - drx, et = (int32_t)(tx_hz - s->up_hz) - dtx;
		if (!ptt && (utc_now() < mid - 20 || utc_now() > mid + 23)) {	/* not while the VFO is held */
			worst_rx = abs(er) > worst_rx ? abs(er) : worst_rx;
			worst_tx = abs(et) > worst_tx ? abs(et) : worst_tx;
		}
		if (rx_hz != last_rx || tx_hz != last_tx) {
			retunes++;
			/* a move is a whole number of steps, at least one */
			CHECK(rx_hz == last_rx || (abs((int32_t)(rx_hz - last_rx)) % (int32_t)sat_step_hz(s->down_hz)) == 0);
			CHECK(tx_hz == last_tx || (abs((int32_t)(tx_hz - last_tx)) % (int32_t)sat_step_hz(s->up_hz)) == 0);
		}
		last_rx = rx_hz;
		last_tx = tx_hz;
		/* PTT held for 40 s across the middle of the pass, where the Doppler moves fastest: nothing at all asked */
		if (utc_now() == mid - 20) {
			ptt = true;
			ptt_from = nreq;
		}
		if (utc_now() == mid + 20) {
			CHECK(nreq == ptt_from);
			ptt = false;
			run_s(2);
			CHECK(nreq > ptt_from);	/* moved again right after (40 s at the top moves it several steps) */
			last_rx = rx_hz;
			last_tx = tx_hz;
		}
	}
	/* An uplink move alone: only T asked (the receiver left alone); every S is followed by its T */
	for (int i = first; i < nreq; i++) {
		if (reqs[i].k == 'T' && reqs[i - 1].k != 'S')
			tx_only_moves++;
		if (reqs[i].k == 'S')
			CHECK(i + 1 < nreq && reqs[i + 1].k == 'T');
	}
	printf("    %d retunes over the pass (%d S, %d T; %d uplink only); VFO off the true Doppler at most %d Hz RX, %d Hz TX "
	       "(hold %d / %d Hz, a second's drift at most %d / %d Hz)\n", retunes, count('S', first), count('T', first),
	       tx_only_moves, (int)worst_rx, (int)worst_tx, (int)hold_rx, (int)hold_tx, (int)drift_rx, (int)drift_tx);
	CHECK(retunes >= 3 && retunes <= 40);
	/* off by at most the hold plus the Doppler's drift in the second before the check (it moves once a second) */
	CHECK(worst_rx <= hold_rx + drift_rx + 20);
	CHECK(worst_tx <= hold_tx + drift_tx + 20);
	CHECK(view_now().track == k);
	/* The set: 30 s after it the nominal frequencies again, then nothing */
	run_s(want.los + 25 - utc_now());
	CHECK(view_now().track == k);
	int before = nreq;
	ptt = true;			/* keyed right then: the restore waits */
	run_s(10);
	CHECK(nreq == before && view_now().track == k);
	ptt = false;
	run_s(2);
	CHECK(view_now().track == SAT_NONE && view_now().track_state == SAT_TRK_LOS);
	CHECK(nreq == before + 2 && reqs[before].k == 'S' && reqs[before].v == s->down_hz && reqs[before + 1].k == 'T' &&
	      reqs[before + 1].v == s->up_hz);
	CHECK(rx_hz == s->down_hz && tx_hz == s->up_hz);
	run_s(120);
	CHECK(nreq == before + 2);
	for (int i = 0; i < nreq; i++)
		CHECK(strchr("STMBC", reqs[i].k) != NULL);
}

static void tracking_hand(uint8_t k, int32_t start)
{
	const sat_info_t *s = &sat_table[k];
	scenario("tracking: a hand change ends it, the VFO stays", start);
	sat_pass_t want = brute_next(k, utc_now(), 10);
	run_s(want.aos - utc_now() - 120);
	screen = true;
	focus = k;
	run_s(30);
	sat_track(k);
	run_s(10);
	CHECK(view_now().track == k);
	rx_hz = 145500000u;		/* the user turns the VFO */
	tx_hz = 145500000u;
	int before = nreq;
	run_s(2);
	CHECK(view_now().track == SAT_NONE && view_now().track_state == SAT_TRK_LEFT);
	run_s(want.los - utc_now() + 60);
	CHECK(nreq == before && rx_hz == 145500000u);
	/* OK again (sat_track(SAT_NONE)) ends it with the VFO where it is */
	scenario("tracking: stopped by the user, the VFO stays", start);
	want = brute_next(k, utc_now(), 10);
	run_s(want.aos - utc_now() + 30);
	screen = true;
	focus = k;
	run_s(30);
	sat_track(k);
	run_s(5);
	uint32_t at = rx_hz;
	CHECK(view_now().track == k && at != s->down_hz);
	before = nreq;
	sat_track(SAT_NONE);
	run_s(2);
	CHECK(view_now().track == SAT_NONE && view_now().track_state == SAT_TRK_OFF);
	CHECK(nreq == before && rx_hz == at);
}

static void follow_steps(void)
{
	printf("  sat_follow_hz: hold and steps\n");
	const uint32_t u = 437800000u, h = 145990000u;
	/* nothing tuned yet: rounded */
	CHECK(sat_follow_hz(u, 1300, 0) == 437802500u);
	/* UHF: 2.5 kHz steps, hold 5/8 = 1562 Hz */
	CHECK(sat_follow_hz(u, 1300, 437800000u) == 437800000u);
	CHECK(sat_follow_hz(u, 1562, 437800000u) == 437800000u);
	CHECK(sat_follow_hz(u, 1563, 437800000u) == 437802500u);
	CHECK(sat_follow_hz(u, -1563, 437800000u) == 437797500u);
	CHECK(sat_follow_hz(u, 9000, 437800000u) == 437810000u);	/* a jump: to the nearest step at once */
	/* VHF: 1 kHz steps, hold 625 Hz */
	CHECK(sat_follow_hz(h, -625, 145990000u) == 145990000u && sat_follow_hz(h, -626, 145990000u) == 145989000u);
	/* Back and forth over a step's edge (the clock corrected by a second): no move back */
	uint32_t t = 437800000u, moves = 0;
	int32_t walk[] = { 1100, 1200, 1260, 1240, 1300, 1249, 1400, 1251, 1500, 1350, 1562, 1450 };
	for (unsigned i = 0; i < sizeof walk / sizeof walk[0]; i++) {
		uint32_t n = sat_follow_hz(u, walk[i], t);
		moves += n != t;
		t = n;
	}
	CHECK(moves == 0 && t == 437800000u);
	int32_t on[] = { 1600, 1500, 1300, 1700, 2000, 2600, 2400, 3000, 3700, 3600, 4000 };
	moves = 0;
	for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++) {
		uint32_t n = sat_follow_hz(u, on[i], t);
		moves += n != t;
		t = n;
	}
	CHECK(moves == 1 && t == 437802500u);
	CHECK(sat_follow_hz(u, 4100, t) == 437805000u);
}

/* --- Every pass and auto track (ISS notifications) --- */

/* Every pass: three qualifying passes in a row, each announced (the warning lead minutes before, the rise), the alarm
 * armed again at once for the next; the radio never touched without auto track */
static void every_pass(uint8_t iss, int32_t start)
{
	scenario("every pass: the ISS 5 min before, 10 deg or more, three passes in a row", start);
	sat_alarm_set(iss, 5, 10, SAT_OPT_EVERY);
	run_until_found();
	poll_ms = 100;
	int32_t from = utc_now();
	for (int n = 0; n < 3; n++) {
		sat_pass_t want = brute_next(iss, from, 10);
		CHECK(want.state == SAT_PASS_OK);
		sat_view_t v = view_now();
		CHECK(v.alarm.state == SAT_AL_ARMED && v.alarm.target == iss && (v.alarm.opts & SAT_OPT_EVERY));
		CHECK(abs(v.alarm.aos - want.aos) <= 3 && v.alarm.max_el >= 10);
		int g = ngot;
		run_s(v.alarm.aos - utc_now() + 2);
		CHECK(ngot == g + 2 && got[g].ev.kind == SAT_ALARM_SOON && got[g + 1].ev.kind == SAT_ALARM_AOS);
		CHECK(got[g].at >= v.alarm.aos - 300 && got[g].at <= v.alarm.aos - 299);
		CHECK(got[g].ev.in_s >= 299 && got[g].ev.in_s <= 300);
		CHECK(got[g + 1].at >= v.alarm.aos && got[g + 1].at <= v.alarm.aos + 1);
		int polls = run_until_found();
		sat_view_t nv = view_now();
		CHECK(nv.alarm.target == iss && nv.alarm.state == SAT_AL_ARMED && nv.alarm.aos > want.los);
		printf("    pass %d: rise %+d s against the scan, top %d deg; the next found in %d polls, %d min later\n",
		       n + 1, (int)(v.alarm.aos - want.aos), v.alarm.max_el, polls, (int)(nv.alarm.aos - v.alarm.aos) / 60);
		from = want.los + 5;
	}
	CHECK(nreq == 0 && !held);
}

/* The VFO against the true Doppler of the downlink each second through an auto track: simplex, within the hold */
static void auto_follow(uint8_t k, uint32_t down, int32_t until, int32_t *worst)
{
	const sat_info_t *s = &sat_table[k];
	sgp4_t p;
	sat_obs_t o;
	sat_observer(LAT, LON, ALT, &o);
	sgp4_init(&p, &s->el);
	int32_t prev = 0, drift = 0;
	*worst = 0;
	while (utc_now() < until) {
		run_s(1);
		sat_look_t l;
		sat_eval(&p, s, (double)now_ms / 1000.0 + gps_t0, &o, &l);
		int32_t d = sat_doppler_rx(down, l.rate_kms);
		if (prev)
			drift = abs(d - prev) > drift ? abs(d - prev) : drift;
		prev = d;
		int32_t er = abs((int32_t)(rx_hz - down) - d);
		*worst = er > *worst ? er : *worst;
		CHECK(vfo && tx_hz == rx_hz);
	}
	CHECK(*worst <= (int32_t)sat_step_hz(down) * SAT_HOLD_8THS / 8 + drift + 20);
}

/* Auto track from a channel: at the rise the hold, then the VFO on the ISS's downlink (voice 145.800 or APRS
 * 145.825) with Doppler, simplex, FM wide, no tones; "tracking" after the rise's event; 30 s after the set the radio
 * back on the channel and the alarm armed for the next pass */
static void auto_track(uint8_t iss, uint8_t mode, int32_t start)
{
	scenario(mode == SAT_AUTO_APRS ? "auto track: the ISS's APRS 145.825 from a channel, back after the set" :
		 "auto track: the ISS's voice 145.800 from a channel, back after the set", start);
	vfo = false;
	rx_hz = tx_hz = 438500000u;
	sat_ui_idle(true);
	sat_alarm_set(iss, 2, 10, (uint8_t)(SAT_OPT_EVERY | mode << SAT_OPT_AUTO_SHIFT));
	run_until_found();
	poll_ms = 100;
	sat_view_t v = view_now();
	sat_pass_t want = brute_next(iss, utc_now(), 10);
	CHECK(v.alarm.state == SAT_AL_ARMED && abs(v.alarm.aos - want.aos) <= 3);
	int8_t top = v.alarm.max_el;
	run_s(v.alarm.aos - utc_now() - 3);
	CHECK(nreq == 0 && !held && !sat_auto_on());
	run_s(6);
	uint32_t down = mode == SAT_AUTO_APRS ? SAT_ISS_APRS_HZ : SAT_ISS_VOICE_HZ;
	CHECK(held && nreq >= 7);
	CHECK(reqs[0].k == 'H' && reqs[0].v == 1 && reqs[1].k == 'S' && reqs[2].k == 'M' && reqs[2].v == RADIO_MODE_FM &&
	      reqs[3].k == 'B' && reqs[3].v == RADIO_BW_25K && reqs[4].k == 'C' && reqs[4].v == (RADIO_TONE_TX << 16 | 0) &&
	      reqs[5].k == 'C' && reqs[5].v == (RADIO_TONE_RX << 16 | 0) && reqs[6].k == 'T' && reqs[6].v == reqs[1].v);
	CHECK(abs((int32_t)(reqs[1].v - down)) <= 4000 && reqs[1].v % 1000u == 0);
	CHECK(reqs[1].ms - reqs[0].ms >= 900 && reqs[1].ms - reqs[0].ms <= 2100);	/* the hold first, a second later */
	v = view_now();
	CHECK(v.track == iss && v.track_state == SAT_TRK_ON && sat_auto_on());
	CHECK(ngot >= 3 && got[ngot - 2].ev.kind == SAT_ALARM_AOS && got[ngot - 1].ev.kind == SAT_ALARM_TRACK &&
	      got[ngot - 1].ev.sat == iss && got[ngot - 1].ev.max_el == top);
	int32_t worst;
	auto_follow(iss, down, want.los, &worst);
	run_s(want.los + 25 - utc_now());
	CHECK(held && view_now().track == iss);
	run_s(10);
	v = view_now();
	CHECK(!held && !vfo && rx_hz == 438500000u && tx_hz == 438500000u && !sat_auto_on());
	CHECK(v.track == SAT_NONE && v.track_state == SAT_TRK_LOS);
	CHECK(v.alarm.target == iss && (v.alarm.state == SAT_AL_ARMED || v.alarm.state == SAT_AL_FINDING));
	int moves = 0;
	for (int i = 0; i < nreq; i++) {
		CHECK(strchr("HSTMBC", reqs[i].k) != NULL);
		moves += reqs[i].k == 'S';
	}
	CHECK(reqs[nreq - 1].k == 'H' && reqs[nreq - 1].v == 0 && count('H', 0) == 2);
	printf("    top %d deg: %d retunes, the VFO off the true Doppler at most %d Hz, back on the channel\n", top, moves,
	       (int)worst);
	run_until_found();
	CHECK(view_now().alarm.state == SAT_AL_ARMED && view_now().alarm.aos > want.los);
}

/* A key (sat_auto_cancel) ends it at once, PTT once released; the radio back as it was (a split VFO here) */
static void auto_cancel(uint8_t iss, int32_t start)
{
	scenario("auto track: a key ends it at once, the VFO back", start);
	vfo = true;
	rx_hz = 145500000u;
	tx_hz = 145000000u;
	sat_ui_idle(true);
	sat_alarm_set(iss, 2, 10, SAT_AUTO_DOWN << SAT_OPT_AUTO_SHIFT);	/* one pass */
	run_until_found();
	poll_ms = 100;
	sat_pass_t want = brute_next(iss, utc_now(), 10);
	run_s(want.aos - utc_now() + 60);
	CHECK(held && sat_auto_on() && rx_hz != 145500000u);
	int before = nreq;
	sat_auto_cancel();
	poll_once();
	sat_view_t v = view_now();
	CHECK(!held && !sat_auto_on() && vfo && rx_hz == 145500000u && tx_hz == 145000000u);
	CHECK(v.track == SAT_NONE && v.track_state == SAT_TRK_OFF);
	CHECK(nreq == before + 1 && reqs[before].k == 'H' && reqs[before].v == 0);
	run_s(want.los - utc_now() + 60);
	CHECK(nreq == before + 1 && view_now().alarm.target == SAT_NONE);	/* nothing more; one pass: off */

	scenario("auto track: PTT ends it once released, the channel back", want.los + 600);
	vfo = false;
	rx_hz = tx_hz = 438500000u;
	sat_ui_idle(true);
	sat_alarm_set(iss, 2, 10, SAT_OPT_EVERY | SAT_AUTO_DOWN << SAT_OPT_AUTO_SHIFT);
	run_until_found();
	want = brute_next(iss, utc_now(), 10);
	run_s(want.aos - utc_now() + 60);
	CHECK(held && vfo);
	before = nreq;
	ptt = true;			/* refused by the radio core while it holds; nothing moves while held */
	run_s(5);
	CHECK(held && nreq == before);
	ptt = false;
	run_s(2);
	v = view_now();
	CHECK(!held && !vfo && rx_hz == 438500000u && v.track == SAT_NONE && v.track_state == SAT_TRK_OFF);
	CHECK(nreq == before + 1 && reqs[before].k == 'H' && reqs[before].v == 0);
	run_s(want.los - utc_now() + 60);
	CHECK(nreq == before + 1 && !sat_auto_on());	/* not again in the same pass */
}

/* Not started: off Home or busy (the UI's word), PTT at the rise, the radio core refusing, a pass tracked by hand */
static void auto_busy(uint8_t iss, int32_t start)
{
	scenario("auto track: not while busy (the UI's word, PTT, the radio core's refusal, tracking by hand)", start);
	vfo = false;
	rx_hz = tx_hz = 438500000u;
	sat_ui_idle(false);
	sat_alarm_set(iss, 2, 10, SAT_OPT_EVERY | SAT_AUTO_DOWN << SAT_OPT_AUTO_SHIFT);
	run_until_found();
	poll_ms = 100;
	sat_view_t v = view_now();
	run_s(v.alarm.aos - utc_now() + 30);
	CHECK(nreq == 0 && !held && view_now().track == SAT_NONE && !sat_auto_on());
	CHECK(ngot >= 2 && got[ngot - 1].ev.kind == SAT_ALARM_AOS);
	/* the next pass: PTT held across the rise */
	run_until_found();
	v = view_now();
	sat_ui_idle(true);
	run_s(v.alarm.aos - utc_now() - 3);
	ptt = true;
	run_s(8);
	ptt = false;
	run_s(20);
	CHECK(nreq == 0 && !held && view_now().track == SAT_NONE);
	/* the next: the radio core refuses (a scan, a search ...): asked, then let go; nothing moved */
	run_until_found();
	v = view_now();
	hold_refuse = true;
	run_s(v.alarm.aos - utc_now() + 30);
	CHECK(count('H', 0) == 2 && reqs[0].v == 1 && reqs[1].v == 0 && nreq == 2);
	CHECK(!held && view_now().track == SAT_NONE && !sat_auto_on() && rx_hz == 438500000u && !vfo);
	hold_refuse = false;
	/* the next: the pass tracked by hand from the screen before its rise: no auto track over it */
	run_until_found();
	v = view_now();
	run_s(v.alarm.aos - utc_now() - 120);
	screen = true;
	focus = iss;
	run_s(30);
	sat_track(iss);
	run_s(120);
	CHECK(view_now().track == iss && !sat_auto_on() && count('H', 0) == 2);
	sat_track(SAT_NONE);
	screen = false;
	run_s(5);
}

int main(void)
{
	uint8_t iss = sat_index(SAT_ISS_CATNUM);
	CHECK(iss != SAT_NONE);
	if (iss == SAT_NONE)
		return 1;
	int32_t t0 = (int32_t)sat_table[iss].el.epoch + 3600;
	follow_steps();
	alarm_before_gps(iss, t0);
	alarm_one(iss, 5, 0, t0);
	alarm_one(iss, 2, 0, t0 + 20000);
	alarm_one(iss, 10, 30, t0 + 40000);
	alarm_any(0, t0 + 50000);
	alarm_any(40, t0 + 90000);
	alarm_edges(iss, t0 + 130000);
	for (uint8_t k = 0; k < sat_count; k++)
		tracking(k, t0 + 210000 + k * 20000);
	tracking_hand(iss, t0 + 300000);
	every_pass(iss, t0 + 340000);
	auto_track(iss, SAT_AUTO_DOWN, t0 + 380000);
	auto_track(iss, SAT_AUTO_APRS, t0 + 420000);
	auto_cancel(iss, t0 + 460000);
	auto_busy(iss, t0 + 520000);
	if (failures) {
		printf("test_sat_svc: %d failures\n", failures);
		return 1;
	}
	printf("test_sat_svc: ok\n");
	return 0;
}
