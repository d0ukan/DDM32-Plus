/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus SSTV send (core/ui/ui_sstvtx.c, docs/plus.md "SSTV send") through the real UI in upstream's simulator,
 * on the DM-32UV's display class (240 x 320, insets 10 x 5, key map M), in English and in Turkish. The radio task's
 * side (radio_req_sstv_tx, radio_sstvtx_info) is a fake the test drives as the radio core would; the band plan is the
 * real one (radio/xband.c, ui/ui_xband.c built for SSTV send alone), the card and the receive screen too.
 *
 * Covers: Tools > SSTV says "OK send" and OK opens the page; the page (the preview, the mode with its length, the TX
 * frequency with the callsign, the hint) for each mode (Left / Right); every refusal the page makes itself (no
 * callsign, not FM, a repeater channel, a repeater / satellite / APRS frequency, outside the amateur bands, the
 * time-out timer too short) with nothing asked of the radio; a start (the mode asked), the progress while it sends
 * (keys other than Back refused), Back stops (the stop asked, the page stays), "Picture sent", the radio core's own
 * refusals told, leaving the page another way stops it; the layout of every page; no PTT transmission anywhere. Each
 * case runs in a child process (the screens' static state starts over). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "radio/radio.h"
#include "radio/radio_msg.h"
#include "radio/radio_sstv.h"
#include "radio/radio_sstvtx.h"
#define ART_HOST_TEST
#include "ui/art_data.c"	/* the card's logo (CONFIG_PLUS_ART's tables, built here for the test alone) */

/* --- The radio task's side, faked --- */
static radio_sstvtx_info_t f_info;
static int f_starts, f_stops;
static uint8_t f_mode = 0xFF;
void radio_req_sstv_tx(uint8_t mode)
{
	if (mode < SSTV_TX_MODES) {
		f_starts++;
		f_mode = mode;
	} else {
		f_stops++;
	}
}
void radio_sstvtx_info(radio_sstvtx_info_t *out) { *out = f_info; }
uint16_t radio_sstvtx_seq(void) { return f_info.seq; }
static void f_set(uint8_t state, uint8_t why, uint8_t deny, uint16_t line)
{
	f_info.state = state;
	f_info.why = why;
	f_info.deny = deny;
	f_info.line = line;
	f_info.mode = f_mode < SSTV_TX_MODES ? f_mode : 0;
	f_info.lines = sstv_tx_lines(f_info.mode);
	f_info.seq++;
}
/* The receive screen's and the pager's (ui_sstv.c, ui_msg.c) */
void radio_msg_want(uint8_t kind) { (void)kind; }
uint8_t radio_msg_status(void) { return RADIO_MSG_ST_ON; }
uint32_t radio_msg_seq(void) { return 1; }
uint8_t radio_msg_count(uint8_t kind) { (void)kind; return 0; }
const radio_pager_msg_t *radio_msg_pager(uint8_t i) { (void)i; return NULL; }
const radio_acars_msg_t *radio_msg_acars(uint8_t i) { (void)i; return NULL; }
void radio_sstv_info(radio_sstv_info_t *out) { memset(out, 0, sizeof *out); out->mode = SSTV_NONE; }
uint32_t radio_sstv_seq(void) { return 1; }
bool radio_sstv_row(uint8_t image, uint8_t *row, uint8_t px[RADIO_SSTV_ROW_BYTES])
{
	(void)image, (void)row, (void)px;
	return false;
}

/* --- The harness --- */
static int fails;
static const char *name;
static bool turkish;

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, turkish ? "tr" : "en", script, err);
		fails++;
	}
}

/* key=the text in the language in use (spaces as _), as the screen has it */
static void expect_tr(const char *key, const char *en)
{
	char buf[200];
	int n = snprintf(buf, sizeof buf, "expect:%s=%s", key, tr(en));
	for (int i = 7; i < n && i < (int)sizeof buf; i++)
		if (buf[i] == ' ')
			buf[i] = '_';
	ok_run(buf);
}

/* The first part of a bottom line hint in the language in use (the bottom line draws its parts apart, split at the
 * double space) */
static void expect_hint(const char *en)
{
	char part[80];
	snprintf(part, sizeof part, "%s", tr(en));
	char *gap = strstr(part, "  ");
	if (gap != NULL)
		*gap = '\0';
	char buf[120];
	int n = snprintf(buf, sizeof buf, "expect:shows=%s", part);
	for (int i = 7; i < n && i < (int)sizeof buf; i++)
		if (buf[i] == ' ')
			buf[i] = '_';
	ok_run(buf);
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

/* The channel in use as the radio core would have it (the simulator's VFO), FM unless said */
static void channel(uint32_t rx, uint32_t tx, uint8_t mode)
{
	radio_req_vfo_set(rx);
	radio_view_t *v = (radio_view_t *)sim_view();	/* (the simulator's own view: the test is its radio core) */
	v->ch.mode = mode;
	v->ch.tx_hz = tx;
	v->ch.rx_only = false;
	v->am = false;
	radio_req_bandwidth(RADIO_BW_12K5);	/* the simulator's "changed": the UI takes the view again */
	ok_run("wait:100");
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
	const char *shots = getenv("SSTVTX_SHOTS");	/* a directory: the pages as PNG files */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "tr-" : "en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	ui_svc_name_set(UI_NAME_RADIO, "TA2DOU");
	channel(145500000, 145500000, RADIO_MODE_FM);
	/* Tools > SSTV, found by its name; OK opens the send page */
	ui_push(SCR_TOOLS, 0);
	ok_run("wait:100");
	for (int i = 0; i < 60 && strcmp(ui_app_focus_label(), "SSTV") != 0; i++)
		ok_run("down");
	check(strcmp(ui_app_focus_label(), "SSTV") == 0, "Tools has SSTV");
	ok_run("ok wait:200 expect:screen=sstv");
	expect_hint("OK send  Back");
	layout();
	ok_run("shot:sstv-rx");
	ok_run("ok wait:200 expect:screen=sstv-tx");
}

/* --- The cases --- */

static void page(void)
{
	start("the page");
	expect_tr("shows", "SSTV send");
	ok_run("expect:shows=<_Robot_36_>__37_s expect:shows=145.500 expect:shows=TA2DOU");
	expect_tr("shows", "Test picture: OK sends");
	expect_hint("OK send  <> mode");
	layout();
	ok_run("shot:sstvtx-page");
	ok_run("right wait:100 expect:shows=<_Martin_M1_>__1:56");
	layout();
	ok_run("right wait:100 expect:shows=<_PD120_>__2:08");
	layout();
	ok_run("right wait:100 expect:shows=<_Robot_36_> left wait:100 expect:shows=<_PD120_>");
	ok_run("back wait:200 expect:screen=sstv back wait:200 expect:screen=tools expect:tx=0");
	check(f_starts == 0 && f_stops == 0, "nothing asked of the radio");
}

static void refusals(void)
{
	start("refusals");
	static const struct { uint32_t rx, tx; uint8_t mode; const char *why; } t[] = {
		{ 145500000, 145500000, RADIO_MODE_DMR, "SSTV: tune an FM channel" },
		{ 145600000, 145000000, RADIO_MODE_FM, "Repeater channel (RX and TX differ)" },
		{ 145600000, 145600000, RADIO_MODE_FM, "Repeater frequency" },
		{ 145800000, 145800000, RADIO_MODE_FM, "Satellite segment" },
		{ 144800000, 144800000, RADIO_MODE_FM, "APRS frequency" },
		{ 162550000, 162550000, RADIO_MODE_FM, "Not an amateur band" },
		{ 446006250, 446006250, RADIO_MODE_FM, "PMR446: no repeaters allowed" },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
		channel(t[i].rx, t[i].tx, t[i].mode);
		ok_run("ok wait:100 expect:screen=sstv-tx");
		expect_tr("toast", t[i].why);
		layout();
		ok_run("wait:3000");
	}
	channel(144500000, 144500000, RADIO_MODE_FM);	/* (Region 1's SSTV calling frequency: allowed) */
	((radio_view_t *)sim_view())->tot_s = 30;	/* Robot 36 takes 37 s */
	ok_run("ok wait:100");
	expect_tr("toast", "TX time-out shorter than the picture");
	ok_run("wait:3000");
	((radio_view_t *)sim_view())->tot_s = 180;
	ui_svc_name_set(UI_NAME_RADIO, "Dogan");
	ok_run("ok wait:100");
	expect_tr("toast", "Set your callsign (the radio name) in qdmr.");
	layout();
	check(f_starts == 0, "nothing asked of the radio");
	ok_run("expect:tx=0");
}

static void send(void)
{
	start("sending");
	ok_run("right wait:100 ok wait:100");
	check(f_starts == 1 && f_mode == SSTV_TX_MARTIN1, "Martin M1 asked");
	/* the radio core keys, sends: the progress */
	f_set(SSTVTX_SENDING, 0, 0, 128);
	ok_run("wait:200");
	char s[64];
	snprintf(s, sizeof s, "%s 128/256", tr("Sending"));
	expect_tr("shows", s);
	expect_hint("Back stops");
	layout();
	ok_run("shot:sstvtx-sending");
	/* other keys do nothing while it sends */
	ok_run("right wait:100 ok wait:100 expect:screen=sstv-tx expect:shows=Martin_M1");
	check(f_starts == 1, "OK while sending asks nothing");
	/* Back stops it, the page stays */
	ok_run("back wait:100 expect:screen=sstv-tx");
	check(f_stops == 1, "the stop asked");
	f_set(SSTVTX_STOPPED, 0, 0, 130);
	ok_run("wait:200");
	expect_tr("toast", "Sending stopped");
	ok_run("wait:3000");
	expect_hint("OK send  <> mode");
	layout();
	/* again, to the end */
	ok_run("wait:3000 ok wait:100");
	check(f_starts == 2, "asked again");
	f_set(SSTVTX_KEYUP, 0, 0, 0);
	ok_run("wait:200");
	f_set(SSTVTX_SENDING, 0, 0, 255);
	ok_run("wait:200");
	f_set(SSTVTX_SENT, 0, 0, 256);
	ok_run("wait:200");
	expect_tr("toast", "Picture sent");
	expect_tr("shows", "Picture sent");
	layout();
	ok_run("shot:sstvtx-sent");
	ok_run("expect:tx=0");
}

static void radio_refusals(void)
{
	start("the radio core's refusals");
	static const struct { uint8_t why, deny; const char *text; } t[] = {
		{ SSTVTX_R_BUSY, 0, "Not while transmitting" },
		{ SSTVTX_R_DENY, TXDENY_RX_ONLY, "This channel is RX only" },
		{ SSTVTX_R_DENY, TXDENY_BUSY, "Channel busy" },
		{ SSTVTX_R_DENY, TXDENY_INHIBIT, "Can't transmit there" },
		{ SSTVTX_R_NOMEM, 0, "Not enough memory for SSTV" },
		{ SSTVTX_R_TOT, 0, "TX time-out shorter than the picture" },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
		ok_run("ok wait:100");
		f_set(SSTVTX_REFUSED, t[i].why, t[i].deny, 0);
		ok_run("wait:200");
		expect_tr("toast", t[i].text);
		layout();
		ok_run("wait:3000");
	}
	check(f_starts == (int)(sizeof t / sizeof t[0]), "each asked once");
}

static void leave(void)
{
	start("leaving the page");
	ok_run("ok wait:100");
	f_set(SSTVTX_SENDING, 0, 0, 10);
	ok_run("wait:200");
	ui_go_home();	/* not by Back */
	ok_run("wait:200 expect:screen=home");
	f_set(SSTVTX_SENDING, 0, 0, 11);	/* the next line */
	ok_run("wait:200");
	check(f_stops == 1, "the stop asked once the page was left");
	f_set(SSTVTX_STOPPED, 0, 0, 12);
	ok_run("wait:500");
	check(f_stops == 1, "asked once");
}

typedef void (*case_fn)(void);

int main(void)
{
	static const case_fn all[] = { page, refusals, send, radio_refusals, leave };
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
	printf("Plus SSTV send screens: %d flows (English and Turkish), %d failed\n", run, bad);
	return bad != 0;
}
