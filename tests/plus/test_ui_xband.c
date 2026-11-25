/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > Cross-band (core/ui/ui_xband.c, docs/plus.md "Cross-band store and forward") through the real UI
 * in upstream's simulator, on the DM-32UV's display class (240 x 320, insets 10 x 5, key map M), in English and in
 * Turkish, with the real engine (core/radio/xband.c) driven by the test as the radio core would drive it. Covers the
 * form (its rows, a mode for each side, frequencies typed, the steps), every refusal before a start (PMR446, a repeater
 * segment, a repeater of the list, A = B, outside the amateur bands, a pair of modes not built, no callsign), the picker (a zone's channels, a repeater
 * channel refused, a DMR channel bringing its colour code and slot, Left / Right another zone), the attended-operation
 * question (Back: nothing starts; OK: it starts, Home), Home while it runs (CROSS in the status strip, the relay's
 * phase and the over's length on the bottom line), the lock (every key refused but Back, which stops it), and a
 * session that ends by itself (no memory). Each case runs in a child process (the screens' static state starts
 * over). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "hal_time.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/ui_svc.h"
#include "ui/i18n.h"
#include "ui/rpt.h"
#include "radio/xband.h"
#include "ui/ui_xband.h"
#include "codeplug/codeplug.h"

/* Tools > Repeaters' table: one repeater outside the band plan's segments (430.400, input 432.000) */
#define P(la, lo) (int32_t)((la) * 1e5 + 0.5), (int32_t)((lo) * 1e5 + 0.5)
const rpt_entry_t rpt_table[] = {
	{ P(39.93, 32.86), 430400000, 1280, 885, RPT_FM, 0, "TA2RAK", "Ankara", 6 },
};
const rpt_info_t rpt_info = { sizeof rpt_table / sizeof rpt_table[0], 0, "2026-11-20", "test" };

/* --- The radio task's side, faked (and what Tools > Repeaters asks of it: not used here) --- */
bool aprs_position_now(int32_t *lat, int32_t *lon, uint32_t *err_m, bool *dmr_gps)
{
	(void)lat, (void)lon, (void)err_m, (void)dmr_gps;
	return false;
}
void radio_req_mode(uint8_t mode) { (void)mode; }
void radio_req_dmr_color_code(uint8_t cc) { (void)cc; }
void radio_req_dmr_timeslot(uint8_t ts) { (void)ts; }
static char radio_name[CODEPLUG_NAME_LEN + 1] = "TA2DOU";
void codeplug_radio_name(char out[CODEPLUG_NAME_LEN + 1])
{
	memcpy(out, radio_name, sizeof radio_name);
}
static int req_on, req_off;
void radio_req_xband(bool on)
{
	if (on)
		req_on++;
	else
		req_off++;
}

/* The engine as the radio core drives it */
static int tunes, keys;
static uint8_t tuned;
static bool keyed;
static void io_tune(uint8_t side) { tunes++; tuned = side; }
static bool io_key(uint8_t side, uint32_t dst, bool group) { (void)side, (void)dst, (void)group; keys++; keyed = true; return true; }
static void io_unkey(void) { keyed = false; }
static bool io_cw(uint8_t side) { (void)side; return false; }
static void io_beep(uint8_t what) { (void)what; }
static const xband_io_t io = { io_tune, io_key, io_unkey, io_cw, io_beep };
static uint8_t mem[17104];
static void engine_take(bool memory)
{
	uint8_t *seg[1] = { mem };
	size_t len[1] = { sizeof mem };
	if (!xband_begin(hal_millis()))
		return;
	xband_memory(seg, len, memory ? 1 : 0, hal_millis());
}
static void engine_poll(void)
{
	xband_in_t in = { .tx = keyed };
	xband_poll(&io, &in, hal_millis());
}
static void ok_run(const char *script);
/* The radio core polls every 25 ms: the engine goes between A and B until it listens on side */
static void engine_to_side(uint8_t side)
{
	xband_view_t v;
	for (int i = 0; i < 200; i++) {
		engine_poll();
		xband_view(&v);
		if (v.phase == XB_P_LISTEN && v.side == side)
			return;
		ok_run("wait:25");
	}
}

/* --- The harness --- */
static int fails;
static const char *name;
static bool turkish;
#define T(en, trk) (turkish ? (trk) : (en))

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, turkish ? "tr" : "en", script, err);
		fails++;
	}
}

static void expect(const char *e)
{
	char buf[500], *o = buf;
	while (*e && o < buf + sizeof buf - 16) {
		while (*e == ' ')
			e++;
		if (!*e)
			break;
		memcpy(o, "expect:", 7);
		o += 7;
		while (*e && *e != ' ' && o < buf + sizeof buf - 2)
			*o++ = *e++;
		*o++ = ' ';
	}
	*o = 0;
	ok_run(buf);
}

/* key=the text in the language in use (spaces as _), as the screen has it */
static void expect_tr(const char *key, const char *en)
{
	char buf[160];
	int n = snprintf(buf, sizeof buf, "%s=%s", key, tr(en));
	for (int i = 0; i < n && i < (int)sizeof buf; i++)
		if (buf[i] == ' ')
			buf[i] = '_';
	expect(buf);
}

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s (%s): %s\n", name, turkish ? "tr" : "en", what);
		fails++;
	}
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int k = 0; k < n; k++)
		printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[k]);
	fails += n != 0;
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
	const char *shots = getenv("XBAND_SHOTS");	/* a directory: the pages as PNG files */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "tr-" : "en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	ok_run("wait:100");
	/* Tools > Cross-band, found by its (translated) name */
	ui_push(SCR_TOOLS, 0);
	ok_run("wait:100");
	const char *want = tr("Cross-band");
	for (int i = 0; i < 40 && strcmp(ui_app_focus_label(), want) != 0; i++)
		ok_run("down");
	check(strcmp(ui_app_focus_label(), want) == 0, "Tools has Cross-band");
	ok_run("ok wait:200");
	expect("screen=xband");
}

/* The form's first row (A) and its last (Start): the rows below B mode follow the modes (at most 18) */
#define TOP	"up up up up up up up up up up up up up up up up up up up up "
#define BOTTOM	"down down down down down down down down down down down down down down down down down down down down "

/* A valid pair on the form; then Start (the last row) */
static void pair(uint32_t a, uint32_t b)
{
	xband_req_t *r = xband_req();
	r->hz[0] = a;
	r->hz[1] = b;
	r->mode[0] = r->mode[1] = XB_MODE_DMR;
}
static void press_start(void)
{
	ok_run(BOTTOM "ok wait:100");
}

/* --- The cases --- */

static void form(void)
{
	start("the form");
	expect("shows=A shows=B shows=DMR");
	expect_tr("shows", "A mode");
	expect_tr("shows", "B mode");
	expect("shows=A_RX_CC shows=A_RX_TS");
	expect_tr("shows", "Any");
	layout();
	ok_run("shot:xband-form");
	ok_run(BOTTOM "wait:100");
	expect("shows=B_TX_TG");
	expect_tr("shows", "As heard");
	expect_tr("shows", "Start");
	expect_tr("shows", "Low");
	layout();
	ok_run(TOP "wait:100");
	/* A typed (kHz), B typed with its 100 Hz digit */
	ok_run("1 4 5 5 0 0 down down 4 3 3 5 1 2 5 down");
	expect("shows=145.500 shows=433.5125");
	check(xband_req()->hz[0] == 145500000 && xband_req()->hz[1] == 433512500, "typed");
	/* B's mode a step; then A's rows: RX CC Any -> 0 -> 2, RX TS Any -> 1 -> 2, RX TG typed 91, TX CC 1 -> 3, TX TS
	 * 1 -> 2, TX TG typed 2862; B's six rows; Power Mid */
	ok_run("right");
	expect("shows=FM");
	ok_run("left down right right right down right right down 9 1 down right right down right down 2 8 6 2 down");
	const xband_side_t *a = &xband_req()->side[0];
	check(xband_req()->mode[1] == XB_MODE_DMR && a->rx_cc == 2 && a->rx_ts == 2 && a->rx_tg == 91 && a->tx_cc == 3 &&
	      a->tx_ts == 2 && a->tx_tg == 2862, "A: RX CC 2 TS 2 TG 91, TX CC 3 TS 2 TG 2862");
	expect("shows=91 shows=2862");
	layout();
	ok_run("shot:xband-dmr-sides");
	/* B's RX CC typed 16: out of range, Any kept; a colour code typed 7 is taken */
	ok_run("1 6 down wait:100");
	expect_tr("toast", "Out of range");
	check(xband_req()->side[1].rx_cc == XB_CC_ANY, "B RX CC: Any kept");
	ok_run("wait:3000 up 7 down");
	check(xband_req()->side[1].rx_cc == 7, "B RX CC 7");
	ok_run("down down down down down right");
	check(xband_req()->power == 2, "Mid");
	expect_tr("shows", "Mid");
	/* A's mode: back one from DMR is NXDN (no rows of its own); B's on to P25: RX NAC, TX NAC, TX TG */
	ok_run(TOP "down left");
	expect("shows=NXDN hides=A_RX_CC");
	ok_run("down down right right");
	expect("shows=P25 shows=B_RX_NAC shows=B_TX_NAC shows=293");
	check(xband_req()->mode[0] == XB_MODE_NXDN && xband_req()->mode[1] == XB_MODE_P25, "A NXDN, B P25");
	/* B's NAC typed 659 (hex), TX NAC a step down 292, TX TG 70000 out of range for P25, then 101 */
	ok_run("down 6 5 9 down left down 7 0 0 0 0 down wait:100");
	expect_tr("toast", "Out of range");
	const xband_side_t *b = &xband_req()->side[1];
	check(b->rx_nac == 0x659 && b->tx_nac == 0x292 && b->tx_tg == 0, "B RX NAC 659, TX NAC 292, TX TG as heard");
	ok_run("wait:3000 up 1 0 1 down");
	check(b->tx_tg == 101, "B TX TG 101");
	expect("shows=659 shows=292 shows=101");
	layout();
	ok_run("shot:xband-modes");
}

/* FM tone rows (owner, 2026-11-25): an FM side shows an RX Tone row (the sub-tone an FM -> DMR receive side is gated by)
 * and a TX Tone row (the sub-tone a DMR -> FM transmit side adds to the carrier); both start at None and step through
 * the CTCSS / DCS picker */
static void fm_tones(void)
{
	start("fm tones");
	ok_run("down right wait:100");			/* A mode: DMR -> FM */
	expect("shows=FM");
	expect("shows=A_RX_Tone shows=A_TX_Tone");	/* the two tone rows appear for an FM side */
	expect_tr("shows", "None");
	const xband_side_t *a = &xband_req()->side[0];
	check(xband_req()->mode[0] == XB_MODE_FM && a->rx_tone_type == TONE_NONE && a->tx_tone_type == TONE_NONE,
	      "FM on A, tones start at None");
	/* step A's RX Tone up: the lowest CTCSS; then its TX Tone */
	ok_run("down down down right wait:100");	/* to A RX Tone, one step from None */
	check(a->rx_tone_type == TONE_CTCSS && a->rx_tone_value > 0, "A RX tone: a CTCSS value");
	layout();
	ok_run("shot:xband-fm-tones");
	ok_run("down right wait:100");			/* A TX Tone, one step from None */
	check(a->tx_tone_type == TONE_CTCSS && a->tx_tone_value > 0, "A TX tone: a CTCSS value");
	/* B stays DMR: it has no tone rows, its RX CC does */
	expect("shows=B_RX_CC");
}

static void refusals(void)
{
	start("refusals");
	static const struct { uint32_t b; const char *why; } t[] = {
		{ 446006250, "PMR446: no repeaters allowed" },
		{ 439000000, "Repeater frequency" },
		{ 145600000, "Repeater frequency" },
		{ 432000000, "Repeater frequency" },	/* the list's repeater input */
		{ 430400000, "Repeater frequency" },	/* and its output */
		{ 435500000, "Satellite segment" },
		{ 144800000, "APRS frequency" },
		{ 145500000, "A and B are the same" },
		{ 162550000, "Not an amateur band" },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
		pair(145500000, t[i].b);
		press_start();
		expect("screen=xband");
		expect_tr("toast", t[i].why);
		ok_run("wait:3000");
	}
	/* the pairs not built: no transcoding, no NXDN transmitter, no FM receive */
	static const uint8_t bad[][2] = { { XB_MODE_P25, XB_MODE_DMR }, { XB_MODE_DMR, XB_MODE_P25 },
					  { XB_MODE_NXDN, XB_MODE_NXDN }, { XB_MODE_FM, XB_MODE_FM },
					  { XB_MODE_P25, XB_MODE_FM }, { XB_MODE_NXDN, XB_MODE_P25 } };
	for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		pair(145500000, 433500000);
		xband_req()->mode[0] = bad[i][0];
		xband_req()->mode[1] = bad[i][1];
		press_start();
		expect("screen=xband");
		expect_tr("toast", "Mode pair not supported");
		ok_run("wait:3000");
	}
	layout();
	/* a pair built (DMR -> FM): the question, still a repeater segment refused first (NXDN: no receiver in this
	 * test's build, refused as a pair) */
	pair(145500000, 439000000);
	xband_req()->mode[0] = XB_MODE_DMR;
	xband_req()->mode[1] = XB_MODE_FM;
	press_start();
	expect_tr("toast", "Repeater frequency");
	ok_run("wait:3000");
	xband_req()->hz[1] = 446100000;
	press_start();
	expect_tr("toast", "PMR446: no repeaters allowed");
	ok_run("wait:3000");
	xband_req()->hz[1] = 433500000;
	press_start();
	expect(T("shows=Attended", "shows=Yaln"));
	ok_run("back wait:100");
	check(!xband_on() && req_on == 0, "the question: Back, nothing started");
	pair(145500000, 433500000);
	strcpy(radio_name, "Dogan");
	press_start();
	expect_tr("toast", "Set your callsign (the radio name) in qdmr.");
	check(!xband_on() && req_on == 0, "nothing started");
	layout();
}

static void picker(void)
{
	start("the picker");
	ok_run("ok wait:200");	/* A's row */
	expect("screen=xband-pick shows=Base_repeater shows=146.940");
	layout();
	ok_run("shot:xband-pick");
	ok_run("ok wait:100");	/* a repeater (RX and TX differ) */
	expect("screen=xband-pick");
	expect_tr("toast", "Repeater channel (RX and TX differ)");
	ok_run("right right wait:100");	/* zone 3, the hotspots */
	expect("shows=HS_433.5");
	ok_run("down ok wait:200");
	expect("screen=xband shows=433.500");
	check(xband_req()->hz[0] == 433500000 && xband_req()->side[0].tx_cc == 1 && xband_req()->side[0].tx_ts == 1 &&
	      xband_req()->side[0].rx_cc == XB_CC_ANY, "A from the channel: its CC and slot to send on, any heard");
	printf("  (picked HS 433.5: TX TG %u)\n", (unsigned)xband_req()->side[0].tx_tg);
	ok_run("down down ok wait:200");	/* B's row: a 2 m DMR channel of the same zone */
	ok_run("right right down down down down down down down ok wait:200");	/* 36 DMR SPX 5 */
	check(xband_req()->hz[1] == 145510000, "B from the channel");
	ok_run("back");
}

/* The owner's pair (1.0.2 on the radio, 2026-11-25): A 432.275, B 433.500, DMR -> DMR; neither is in a refused
 * segment, so Start asks the question. Typed, then both picked from a zone's channels */
static void owner_pair(void)
{
	start("432.275 / 433.500");
	ok_run("4 3 2 2 7 5 down down 4 3 3 5 0 0 down");
	expect("shows=432.275 shows=433.500");
	check(xband_req()->hz[0] == 432275000 && xband_req()->hz[1] == 433500000, "typed");
	check(xband_req()->mode[0] == XB_MODE_DMR && xband_req()->mode[1] == XB_MODE_DMR, "DMR -> DMR");
	check(xband_check_hz(432275000) == XB_R_OK && xband_check_hz(433500000) == XB_R_OK, "the band plan allows both");
	press_start();
	expect(T("shows=Attended", "shows=Yaln"));
	ok_run("back wait:100");
	check(!xband_on() && req_on == 0, "the question: Back, nothing started");
	/* B left on a satellite frequency from before, then typed short (433.50, 433.5): the typed one is set and Start
	 * asks (before: any entry but 6 or 7 digits was dropped, B stayed 435.500, Start said "Satellite segment") */
	xband_req()->hz[1] = 435500000;
	ok_run(TOP "down down 4 3 3 5 0 down");
	expect("shows=433.500");
	check(xband_req()->hz[1] == 433500000, "five digits: 433.50");
	xband_req()->hz[1] = 435500000;
	ok_run("up 4 3 3 5 down");	/* (each entry ends on B's mode row: one up is B again) */
	check(xband_req()->hz[1] == 433500000, "four digits: 433.5");
	ok_run("up 4 3 2 down");
	check(xband_req()->hz[1] == 432000000, "three digits: 432.000");
	ok_run("up 4 3 down wait:100");
	expect_tr("toast", "Type the MHz: 433500");
	check(xband_req()->hz[1] == 432000000, "two digits: told, kept");
	ok_run("wait:3000");
	xband_req()->hz[1] = 435500000;
	press_start();
	expect_tr("toast", "Satellite segment");	/* (a real satellite frequency is still refused) */
	ok_run("wait:3000");
	ok_run(TOP "down down 4 3 3 5 0 0 down");
	press_start();
	expect(T("shows=Attended", "shows=Yaln"));
	ok_run("back wait:100");
	/* the same from a zone: channel 35 (Hotspots' 7th) made a 432.275 DMR simplex channel; HS 433.5 its 2nd */
	channel_t c;
	check(codeplug_channel(35, &c), "channel 35");
	c.rx_hz = c.tx_hz = 432275000;
	c.mode = RADIO_MODE_DMR;
	check(ui_svc_write_channel(35, &c), "channel 35 written");
	xband_req()->hz[0] = xband_req()->hz[1] = 0;
	ok_run(TOP "ok wait:200");	/* A's row */
	ok_run("right right wait:100 down down down down down down ok wait:200");
	expect("screen=xband shows=432.275");
	ok_run("down down ok wait:200");		/* B's row */
	ok_run("right right wait:100 down ok wait:200");
	expect("screen=xband shows=433.500");
	check(xband_req()->hz[0] == 432275000 && xband_req()->hz[1] == 433500000, "picked");
	press_start();
	expect(T("shows=Attended", "shows=Yaln"));
	ok_run("ok wait:200");
	expect("screen=home shows=CROSS");
	check(xband_on() && req_on == 1, "started");
}

static void question_and_run(void)
{
	start("the question, the run");
	pair(145500000, 433500000);
	press_start();
	expect(T("shows=Attended", "shows=Yaln"));
	layout();
	ok_run("shot:xband-question");
	ok_run("back wait:100");
	expect("screen=xband");
	check(!xband_on() && req_on == 0, "Back: nothing starts");
	press_start();
	ok_run("ok wait:200");
	expect("screen=home shows=CROSS");
	expect_tr("toast", "Cross-band on: Back stops");
	check(xband_on() && req_on == 1, "started");
	/* the radio core takes it: listening on A */
	engine_take(true);
	engine_poll();
	ok_run("wait:4000");	/* the toast goes; the bottom line says what the relay does */
	char s[64];
	snprintf(s, sizeof s, "%s A", tr("Listen"));
	expect_tr("shows", s);
	layout();
	ok_run("shot:xband-home");
	/* an over on A: recorded, 2 s */
	engine_to_side(0);
	xband_rx_begin(3101234, 91, true, false);
	for (int i = 0; i < 100; i++)
		xband_rx_frame((const uint8_t[XB_FRAME_BYTES]){ 1 });
	engine_poll();
	ok_run("wait:300");
	snprintf(s, sizeof s, "%s A 0:02", tr("Record"));
	expect_tr("shows", s);
	layout();
	/* sent on B */
	xband_rx_end();
	engine_poll();
	ok_run("wait:300");
	snprintf(s, sizeof s, "%s B 0:02", tr("Send"));
	expect_tr("shows", s);
	check(keys == 1 && tuned == 1, "sending on B");
	layout();
	ok_run("shot:xband-sending");
	/* the lock: every key refused, nothing moves */
	ok_run("up wait:100");
	expect("screen=home");
	expect_tr("toast", "Cross-band on: Back stops");
	ok_run("ok wait:100 1 wait:100 ctx wait:100 quick wait:100");
	expect("screen=home");	/* (OK on Home opens the menu once the lock is gone, below) */
	check(xband_on(), "still on");
	/* Back stops it */
	ok_run("back wait:100");
	expect_tr("toast", "Cross-band off");
	engine_poll();
	ok_run("wait:300");
	check(!xband_on() && req_off == 1, "stopped");
	expect("screen=home hides=CROSS");
	/* the keys work again: OK opens the menu */
	ok_run("ok wait:200");
	expect("screen=hub");
}

static void ended_itself(void)
{
	start("no memory");
	pair(145500000, 433500000);
	press_start();
	ok_run("ok wait:200");
	check(xband_on(), "started");
	engine_take(false);
	engine_poll();
	ok_run("wait:300");
	check(!xband_on(), "ended");
	expect_tr("toast", "Cross-band off: no memory");
	expect("hides=CROSS");
}

/* # on the form (owner, 2026-11-25): "Segments free" lifts the band plan's segment refusals and the repeater list's;
 * the hint names # and its state; outside the bands, PMR446, A = B, a repeater channel stay refused; DOTA's refusal
 * (ui_xband_refusal) never changes; # does nothing while a session runs */
static void segs_free(void)
{
	start("segments free");
	check(!xband_segs_free(), "off when the screen opens (power-on)");
	expect(T("shows=#_segs:_off", "shows=_kapal hides=serbest"));
	expect_tr("hides", "Segments free");
	layout();
	pair(145500000, 435500000);
	press_start();
	expect_tr("toast", "Satellite segment");
	ok_run("wait:3000");
	ok_run("ctx wait:100");
	check(xband_segs_free(), "# sets it");
	expect(T("shows=#_segs:_on", "hides=_kapal shows=serbest"));
	expect_tr("shows", "Segments free");
	layout();
	ok_run("shot:xband-free");
	static const uint32_t ok[] = { 435500000, 436275000, 145800000, 144800000, 145600000, 439000000,
				       432000000 /* the list's repeater input */, 430400000 };
	for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
		pair(145500000, ok[i]);
		press_start();
		expect(T("shows=Attended", "shows=Yaln"));
		expect_tr("shows", "Segments free");	/* the question says it too */
		layout();
		ok_run("back wait:100");
		check(!xband_on(), "the question: Back");
		check(ui_xband_refusal(ok[i]) != NULL, "DOTA's band plan still refuses it");
	}
	static const struct { uint32_t b; const char *why; } t[] = {
		{ 446100000, "PMR446: no repeaters allowed" },
		{ 150000000, "Not an amateur band" },
		{ 440500000, "Not an amateur band" },
		{ 145500000, "A and B are the same" },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
		pair(145500000, t[i].b);
		press_start();
		expect("screen=xband");
		expect_tr("toast", t[i].why);
		ok_run("wait:3000");
	}
	pair(145500000, 433500000);
	strcpy(radio_name, "Dogan");
	press_start();
	expect_tr("toast", "Set your callsign (the radio name) in qdmr.");
	ok_run("wait:3000");
	strcpy(radio_name, "TA2DOU");
	/* a repeater channel from the picker: still refused */
	ok_run(TOP "ok wait:200 ok wait:100");
	expect("screen=xband-pick");
	expect_tr("toast", "Repeater channel (RX and TX differ)");
	ok_run("wait:3000 back wait:100");
	expect("screen=xband");
	/* # again: off, the satellite segment refused again */
	ok_run("ctx wait:100");
	check(!xband_segs_free(), "# clears it");
	pair(145500000, 435500000);
	press_start();
	expect_tr("toast", "Satellite segment");
	ok_run("wait:3000");
	/* free, a satellite pair started: # refused while it runs */
	ok_run("ctx wait:100");
	press_start();
	ok_run("ok wait:200");
	expect("screen=home shows=CROSS");
	check(xband_on(), "started on 435.500");
	engine_take(true);	/* (its begin checks the request again, the segments free) */
	engine_poll();
	check(xband_owns_radio(), "the radio core took it");
	ok_run("ctx wait:100");
	check(xband_segs_free(), "# on Home during the session: nothing");
	check(!xband_set_segs_free(false) && xband_segs_free(), "the setter refuses during a session");
	/* the screen stack is gone, the session stopped: the flag is kept (RAM), never saved */
	ok_run("back wait:100");
	engine_poll();
	ok_run("wait:300");
	check(!xband_on() && xband_segs_free(), "kept while the radio runs");
}

typedef void (*case_fn)(void);

int main(void)
{
	static const case_fn all[] = { form, fm_tones, refusals, picker, owner_pair, question_and_run, ended_itself,
				       segs_free };
	int bad = 0, run = 0;
	for (int lang = 0; lang < 2; lang++) {
		for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				turkish = lang == 1;
				all[i]();
				fflush(stdout);
				_exit(fails != 0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			run++;
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
				bad++;
				if (!WIFEXITED(st))
					printf("FAIL case %u (%s): crashed\n", i, lang ? "tr" : "en");
			}
		}
	}
	printf("Cross-band screens: %d flows (English and Turkish), %d failed\n", run, bad);
	return bad != 0;
}
