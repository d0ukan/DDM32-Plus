/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer's screens (core/ui/ui_dota.c, docs/dota.md 12) through the real UI in upstream's simulator,
 * on the DM-32UV's display class (240 x 320, insets 10 x 5, key map M: Left / Right), in English and in Turkish. The
 * session engine (radio/dota.c, tests/plus/test_dota_session.c) is faked here: what the screen asks it for is recorded,
 * and the view it shows is set by the test. Covers the form (rows by role and method, digits, the power and tone
 * steps), the frequency typed or stepped (the amateur bands only, the cross-band's band plan refusals: the real
 * radio/xband.c and ui_xband.c), the checks before a start, the questions in their order, the start's request, the
 * picker, the progress
 * page, Back's "Stop the transfer?", the PTT prompt (No: the transfer goes on; Yes: it stops), the results, and that
 * every page fits in both languages. Each case runs in a child process (the screens' static state starts over). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "radio/dota.h"
#include "aprs/aprs.h"
#include "aprs/aprs_plus.h"
#include "storage/storage.h"
#include "codeplug/codeplug.h"

/* --- The engine, faked --- */
static dota_req_t got;
static bool on;
static int starts, stops, clears;
static dota_view_t fv;
static uint8_t sel[CODEPLUG_CHANNELS_MAX / 8u];
static uint32_t scratch[1024 / 4];

bool dota_on(void)
{
	return on;
}

uint8_t *dota_selection(void)
{
	return sel;
}

void *dota_scratch(void)
{
	return on ? NULL : (void *)scratch;
}

bool dota_start(const dota_req_t *r)
{
	if (on)
		return false;
	got = *r;
	on = true;
	starts++;
	memset(&fv, 0, sizeof fv);
	fv.role = r->role;
	fv.method = r->method;
	fv.code = r->code;
	fv.phase = r->role == DOTA_SEND ? DOTA_P_PREP : DOTA_P_WAIT;
	fv.serial = 1;
	return true;
}

void dota_stop(void)
{
	stops++;
	fv.phase = DOTA_P_FAILED;
	fv.result = DOTA_RES_STOPPED;
	fv.ptt_prompt = false;
	fv.serial++;
	on = false;
}

void dota_view(dota_view_t *v)
{
	*v = fv;
}

void dota_ptt_prompt_clear(void)
{
	clears++;
	fv.ptt_prompt = false;
	fv.serial++;
}

bool dota_job_wanted(void)
{
	return false;
}

void dota_job(void *ctx)
{
	(void)ctx;
}

/* --- Data packs (CONFIG_DATAPACK, docs/dota.md 20): a RadioID pack in force, none of the air band; the offer the
 * receiver's view asks about, the user's answer --- */
static dpk_hdr_t rid = { .magic = { 'D', 'P', 'K', '1' }, .type = DPK_RADIOID, .fmt = 1, .seq = 3, .len = 100000,
			 .date = { '2', '0', '2', '6', '1', '1', '1', '7' }, .name = "RadioID TR" };
static dpk_info_t rid_info = { .seq = 3, .len = 100000 };
static int answers, answered_yes;

const dpk_info_t *dpk_active(uint8_t type)
{
	return type == DPK_RADIOID ? &rid_info : NULL;
}

bool dpk_header(uint8_t type, dpk_hdr_t *h)
{
	if (type != DPK_RADIOID)
		return false;
	*h = rid;
	return true;
}

void dpk_describe(const dpk_hdr_t *h, char *out, size_t len)
{
	snprintf(out, len, "%.16s, %.4s-%.2s-%.2s, %u KB", h->name, h->date, h->date + 4, h->date + 6,
		 (unsigned)((h->len + 1023u) / 1024u));
}

/* Settings > About's lines (ui_menus.c) */
uint8_t dpk_state(uint8_t type)
{
	return type == DPK_RADIOID ? DPK_OK : DPK_NONE;
}

bool dpk_about(uint8_t type, char *out, size_t len)
{
	if (type != DPK_RADIOID)
		return false;
	dpk_describe(&rid, out, len);
	return true;
}

bool dota_pack_offer(dpk_hdr_t *h)
{
	if (!fv.asking)
		return false;
	*h = rid;
	return true;
}

void dota_pack_answer(bool yes)
{
	answers++;
	answered_yes += yes;
	fv.asking = false;
	fv.serial++;
}

/* --- What the screen reads besides: APRS's checks (aprs.c is not in the simulator), the storage jobs --- */
static bool callsign_ok = true;
static uint32_t aprs_hz = 144800000u;	/* the APRS frequency set in APRS's settings */

void aprs_get_cfg(aprs_cfg_t *c)
{
	memset(c, 0, sizeof *c);
}

uint32_t aprs_cfg_freq(const aprs_cfg_t *c)
{
	(void)c;
	return aprs_hz;
}

bool aprs_amateur_hz(uint32_t hz)
{
	return (hz >= 144000000u && hz <= 146000000u) || (hz >= 430000000u && hz <= 440000000u);
}

bool aprs_callsign_ok(const char *call)
{
	return callsign_ok && call[0] != '\0';
}

/* The APRS message engine (the text entry comes with CONFIG_APRS_MSG): nothing waiting, nothing heard, nothing sent */
void aprs_plus_state(aprs_plus_t *s)
{
	memset(s, 0, sizeof *s);
	s->block = APRS_OK;
}

uint8_t aprs_sos_set(bool sos)
{
	(void)sos;
	return APRS_OK;
}

void aprs_sos_heard(char *call)
{
	call[0] = '\0';
}

uint8_t aprs_msg_count(void) { return 0; }
bool aprs_msg(uint8_t i, aprs_msg_t *out) { (void)i; (void)out; return false; }
bool aprs_msg_peek(uint8_t i, aprs_msg_t *out) { (void)i; (void)out; return false; }
bool aprs_rx_listening(void) { return true; }
bool aprs_send_message(const char *to, const char *text) { (void)to; (void)text; return false; }

/* The radio name is the callsign (the simulator's codeplug has none) */
void codeplug_radio_name(char out[CODEPLUG_NAME_LEN + 1])
{
	strcpy(out, "TA1BBB");
}

bool storage_job_busy(void)
{
	return false;
}

bool storage_job_post(storage_job_fn fn, void *ctx)
{
	(void)fn;
	(void)ctx;
	return true;
}

/* --- The checks --- */
static int fails, cases;
static const char *name;

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s: %s: %s\n", name, script, err);
		fails++;
	}
}

static void expect(const char *e)
{
	char buf[600], *o = buf;
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

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s: %s\n", name, what);
		fails++;
	}
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int k = 0; k < n; k++)
		printf("FAIL %s: layout: %s\n", name, issues[k]);
	fails += n != 0;
}

static bool turkish;

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
	const char *shots = getenv("DOTA_SHOTS");	/* a directory: the pages as PNG files */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "tr-" : "en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	ok_run("wait:100");
	ui_push(SCR_DOTA, 0);
	ok_run("wait:100");
}

/* The view moved: the screen's tick draws it */
static void moved(void)
{
	fv.serial++;
	ok_run("wait:300");
}

/* English or Turkish (Turkish: an ASCII part of the translation, as the screen draws it) */
#define T(en, tr) (turkish ? (tr) : (en))

/* --- The form --- */

static void form_send_fm(void)
{
	start("form: sender, FM");
	expect("screen=dota");
	expect(T("shows=Role shows=Send shows=Method shows=FM shows=What shows=One_channel shows=Frequency shows=Default "
		 "shows=433.450 shows=88.5 "
		 "shows=Power shows=Low shows=Code shows=About hides=Import hides=Slot",
		 "shows=Rol shows=Frekans shows=Ton shows=Kod shows=Yakla hides=Slot"));
	layout();
	ok_run("shot:form-send");
	/* Power: Left from Low wraps to High, Right twice from there: Mid */
	ok_run("down down down down down down down");	/* Role, Method, What, item, source, Frequency, Tone, Power */
	ok_run("left");
	expect(T("shows=High", "shows=Y"));
	ok_run("right right");
	expect(T("shows=Mid", "shows=Orta"));
	/* the tone steps through the CTCSS list */
	ok_run("up right");
	expect("hides=88.5 shows=100.0");	/* the short list: 88.5, 100.0, 123.0, 131.8, 162.2 */
	ok_run("right right right right");
	expect("shows=88.5");
}

static void form_receive_dmr(void)
{
	start("form: receiver, DMR");
	ok_run("right down right");	/* Role: Receive; Method: DMR */
	expect(T("shows=Receive shows=DMR shows=Import shows=New_zone shows=Zone_name shows=May_transmit shows=Off "
		 "shows=Ready hides=What hides=Power hides=About hides=Color_code hides=Slot hides=CC",
		 "shows=Al shows=DMR shows=Yeni shows=nderebilir hides=Color_code hides=Slot"));
	layout();
	/* Import: Current zone takes the zone name row away */
	ok_run("down down down right");	/* source, Frequency, Import */
	expect(T("shows=Current_zone hides=Zone_name", "shows=Ge"));
	layout();
}

static void form_sender_dmr(void)
{
	start("form: sender, DMR");
	ok_run("down right");
	expect(T("shows=Receiver_ID hides=Color_code hides=Slot hides=88.5", "shows=ID hides=Color_code hides=88.5"));
	layout();
	/* more rows than fit: the last ones scroll into view */
	ok_run("down down down down down down down down down down down");
	expect(T("shows=Code hides=Role", "shows=Kod hides=Rol_"));
	layout();
}

static void type_code(const char *digits);

/* The frequency's source: Default (433.450) on both roles; the current channel only when simplex, may transmit and
 * amateur; the last session's; typed (Manual) */
static void form_freq_source(void)
{
	start("form: the frequency's source");
	ok_run("down down down down");	/* Role, Method, What, item: the source */
	expect(T("shows=Frequency shows=Default shows=433.450", "shows=Frekans shows=Varsay shows=433.450"));
	layout();
	ok_run("shot:freq-source");
	/* the radio is on a DMR channel outside the amateur bands, no session yet: Default, then Manual */
	ok_run("right");
	expect(T("shows=Manual shows=433.450 hides=Current_channel hides=Last_used", "shows=Elle"));
	ok_run("right");
	expect(T("shows=Default", "shows=Varsay"));
	/* typing a frequency makes it Manual */
	ok_run("down 4 3 3 6 0 0 up");
	expect(T("shows=Manual shows=433.600", "shows=Elle shows=433.600"));
	/* the receiver starts with the same source and frequency as a sender */
	ok_run("up up up up right");
	expect(T("shows=Receive shows=Manual shows=433.600", "shows=Al shows=433.600"));
	ok_run("down down right");	/* the source: Default again */
	expect("shows=433.450");
	layout();
}

/* The current channel when it is a simplex amateur one; the last session's frequency afterwards */
static void form_freq_channel_last(void)
{
	start("form: current channel, last used");
	ok_run("dlpos:8 wait:300");	/* channel 8: 145.230 MHz simplex FM */
	ui_push(SCR_DOTA, 0);
	ok_run("wait:100 down down down down right");
	expect(T("shows=Current_channel shows=145.230", "shows=Mevcut shows=145.230"));
	ok_run("left");
	expect("shows=433.450");
	type_code("1 2 3 4");
	ok_run("down ok ok");
	check(starts == 1 && got.hz == DOTA_DEFAULT_HZ && got.cc == 1 && got.ts == 1, "started on the default");
	on = false;
	fv.phase = DOTA_P_FAILED;
	moved();
	ok_run("back");
	ui_push(SCR_DOTA, 0);
	ok_run("wait:100 up up up up up up up up up up down down down down right right");
	expect(T("shows=Last_used shows=433.450", "shows=Son shows=433.450"));
}

/* --- Starting --- */

static void type_code(const char *digits)
{
	/* the code row: Role, Method, What, item, source, Frequency, Tone, Power, Code */
	ok_run("up up up up up up up up up up down down down down down down down down");
	ok_run(digits);
	ok_run("ok");
}

static void start_needs_code(void)
{
	start("start: the code first");
	ok_run("down down down down down down down down down ok");	/* Send without a code */
	expect(T("toast=Code_/_ID_needed", "toast=Kod_/_ID_gerekli"));
	check(starts == 0, "started without a code");
}

static void start_asks_ready(void)
{
	start("start: receiver ready? then the request");
	type_code("1 2 3 4");
	expect("shows=1234");
	ok_run("down ok");	/* Send */
	expect(T("shows=Receiver_ready?", "shows=haz"));
	layout();
	check(starts == 0, "started before the question");
	ok_run("ok");
	check(starts == 1, "not started after Yes");
	check(got.role == DOTA_SEND && got.method == DOTA_FM && got.code == 1234 && got.power == 0 &&
	      got.ctcss == DOTA_CTCSS_DEFAULT && got.kind == DOTA_KIND_CHANNEL && got.channel != 0 && got.hz != 0,
	      "the request");
	expect(T("screen=dota shows=Send shows=FM shows=Back_stop", "screen=dota shows=Geri"));
}

static void start_question_no(void)
{
	start("start: No on the question");
	type_code("1 2 3 4");
	ok_run("down ok back");
	check(starts == 0, "started after No");
	expect(T("shows=Method shows=Send", "shows=ntem shows=nder"));
}

static void start_battery(void)
{
	start("start: battery, ready in turn");
	ok_run("battery:5");
	ok_run("down down down down down 1 4 5 5 0 0 up");	/* 145.500 MHz */
	expect("shows=145.500");
	ok_run("down down down down 1 2 3 4 down ok");
	expect(T("shows=Battery_low", "shows=Pil"));
	layout();
	ok_run("shot:question");
	ok_run("ok");
	expect(T("shows=Receiver_ready?", "shows=haz"));
	ok_run("ok");
	check(starts == 1 && got.hz == 145500000u, "started on 145.500 MHz after the two questions");
}

/* A frequency typed: the amateur bands only (144-146, 430-440 MHz), and in them not where the cross-band refuses
 * (the band plan's PMR446, repeater, satellite and APRS segments): its toast, the frequency and its source stay */
static void freq_typed(void)
{
	static const struct {
		const char *keys;
		uint32_t hz;		/* 0: refused */
		const char *en, *tr;
	} t[] = {
		{ "4 4 6 0 0 0", 0, "toast=PMR446", "toast=PMR446" },
		{ "4 4 6 1 0 0 0", 0, "toast=PMR446", "toast=PMR446" },
		{ "1 6 2 5 5 0", 0, "toast=Not_an_amateur_band", "toast=Amat" },
		{ "1 4 3 9 8 7 5", 0, "toast=Not_an_amateur_band", "toast=Amat" },
		{ "4 2 9 9 9 9 0", 0, "toast=Not_an_amateur_band", "toast=Amat" },
		{ "4 4 0 0 1 2 5", 0, "toast=Not_an_amateur_band", "toast=Amat" },
		{ "1 4 6 0 1 2 5", 0, "toast=Not_an_amateur_band", "toast=Amat" },
		{ "1 4 5 6 0 0", 0, "toast=Repeater_frequency", "toast=R" },
		{ "1 4 5 1 0 0", 0, "toast=Repeater_frequency", "toast=R" },
		{ "4 3 1 0 5 0", 0, "toast=Repeater_frequency", "toast=R" },
		{ "4 3 8 6 5 0", 0, "toast=Repeater_frequency", "toast=R" },
		{ "4 3 5 5 0 0", 0, "toast=Satellite_segment", "toast=Uydu" },
		{ "1 4 5 8 2 5", 0, "toast=Satellite_segment", "toast=Uydu" },
		{ "1 4 4 8 0 0", 0, "toast=APRS_frequency", "toast=APRS" },
		{ "1 4 4 0 0 0", 144000000u, "shows=144.000", "shows=144.000" },
		{ "1 4 5 5 0 0", 145500000u, "shows=145.500", "shows=145.500" },
		{ "4 3 0 0 0 0", 430000000u, "shows=430.000", "shows=430.000" },
		{ "4 3 3 6 0 0", 433600000u, "shows=433.600", "shows=433.600" },
		{ "4 4 0 0 0 0", 440000000u, "shows=440.000", "shows=440.000" },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
		/* each in a child of its own: the screen's state starts over */
		fflush(stdout);
		pid_t pid = fork();
		if (pid != 0) {
			int st = 0;
			waitpid(pid, &st, 0);
			fails += !WIFEXITED(st) || WEXITSTATUS(st) != 0;
			continue;
		}
		start("frequency: typed");
		ok_run("down down down down down");	/* Role, Method, What, item, source: the frequency */
		ok_run(t[i].keys);
		ok_run("ok");
		expect(T(t[i].en, t[i].tr));
		if (t[i].hz == 0)
			expect(T("shows=Default shows=433.450", "shows=Varsay shows=433.450"));
		else
			expect(T("shows=Manual", "shows=Elle"));
		layout();
		if (i == 0)
			ok_run("shot:freq-refused");
		/* the frequency is the one sent */
		type_code("1 2 3 4");
		ok_run("down ok ok");
		uint32_t want = t[i].hz != 0 ? t[i].hz : DOTA_DEFAULT_HZ;
		if (starts != 1 || got.hz != want) {
			printf("FAIL %s: %s: started %d on %u Hz, not on %u\n", name, t[i].keys, starts, (unsigned)got.hz,
			       (unsigned)want);
			fails++;
		}
		fflush(stdout);
		_exit(fails != 0);
	}
}

/* Left / Right on the frequency: not out of the amateur bands (a toast, it stays); into a refused segment inside them
 * it goes, and Send names it */
static void freq_stepped(void)
{
	start("frequency: stepped");
	ok_run("down down down down down 4 4 0 0 0 0 ok");
	expect("shows=440.000");
	ok_run("right");
	expect(T("toast=Not_an_amateur_band shows=440.000", "toast=Amat shows=440.000"));
	ok_run("left");
	expect("shows=439.987");
	ok_run("1 4 4 0 0 0 ok left");	/* 144.000, then down */
	expect(T("toast=Not_an_amateur_band shows=144.000", "toast=Amat shows=144.000"));
	/* 434.9875 up to 435.000: the satellite segment, refused at Send */
	ok_run("4 3 4 9 8 7 5 ok right");
	expect("shows=435.000");
	type_code("1 2 3 4");
	ok_run("down ok");
	expect(T("toast=Satellite_segment", "toast=Uydu"));
	check(starts == 0, "started in the satellite segment");
	/* the receiver too */
	ok_run("up up up up up up up up up right");
	expect(T("shows=Receive", "shows=Al"));
	ok_run("down down down down down down down down ok");
	expect(T("toast=Satellite_segment", "toast=Uydu"));
	check(starts == 0, "receiver started in the satellite segment");
}

static void start_refusals(void)
{
	start("start: no callsign, APRS frequency");
	callsign_ok = false;
	type_code("1 2 3 4");
	ok_run("down ok");
	if (!turkish)
		expect("toast=Set_your_callsign");
	callsign_ok = true;
	/* APRS's own frequency set to one the band plan allows: still refused */
	aprs_hz = 145500000u;
	ok_run("up up up up 1 4 5 5 0 0 ok");	/* 145.500 MHz */
	ok_run("down down down down ok");
	expect(T("toast=APRS_frequency", "toast=APRS"));
	check(starts == 0, "started on APRS");
}

static void receiver_start(void)
{
	start("receiver: the zone name, the code and the wait");
	ok_run("right down down down down down down down down ok");	/* Ready: a new zone without a name */
	expect(T("toast=Zone_name_needed", "toast=B"));
	check(starts == 0, "started without a zone name");
	ok_run("up up up right");	/* Import: Current zone (the name row goes) */
	ok_run("down down ok");	/* May transmit, Ready */
	check(starts == 1 && got.role == DOTA_RECEIVE && got.code < 10000u, "the receiver's start");
	char e[64];
	snprintf(e, sizeof e, "shows=%04u", got.code);
	expect(e);
	expect(T("shows=Receive shows=Waiting_for_sender", "shows=Al shows=bekleniyor"));
	layout();
}

static void receiver_dmr_id(void)
{
	start("receiver: DMR shows our ID");
	ok_run("right down right");
	ok_run("down down down right down down ok");	/* Import: Current zone; Ready */
	check(starts == 1 && got.method == DOTA_DMR && got.cc == 1 && got.ts == 1, "DMR receiver started, CC 1 TS 1");
	expect("shows=3101276");
	ok_run("shot:dmr-wait");
}

/* --- The transfer --- */

static void run_page(void)
{
	start("transfer: progress");
	type_code("1 2 3 4");
	ok_run("down ok ok");
	fv.phase = DOTA_P_RUN;
	strcpy(fv.peer, "TA1AAA");
	fv.channels = 10;
	fv.pct = 50;
	fv.keyed = true;
	moved();
	expect(T("shows=TA1AAA shows=5/10_channels shows=On_air", "shows=TA1AAA shows=5/10 shows=Yay"));
	layout();
	ok_run("shot:run");
	fv.keyed = false;
	fv.asking = true;
	moved();
	expect(T("shows=Waiting_for_answer", "shows=bekleniyor"));
}

static void back_stop_no(void)
{
	start("transfer: Back, then No");
	type_code("1 2 3 4");
	ok_run("down ok ok");
	fv.phase = DOTA_P_RUN;
	moved();
	ok_run("back");
	expect(T("screen=dota shows=Stop_the_transfer? shows=OK_yes", "screen=dota shows=durdurulsun"));
	layout();
	ok_run("back");
	check(stops == 0, "stopped on No");
	expect(T("hides=Stop_the_transfer?", "hides=durdurulsun"));
}

static void back_stop_yes(void)
{
	start("transfer: Back, then Yes");
	type_code("1 2 3 4");
	ok_run("down ok ok");
	fv.phase = DOTA_P_RUN;
	moved();
	ok_run("back ok");
	check(stops == 1, "not stopped on Yes");
	expect(T("screen=dota shows=Nothing_changed shows=Back", "screen=dota shows=Hi"));
	ok_run("back");
	expect("screen=home");
}

static void ptt_prompt(void)
{
	start("transfer: the PTT prompt, No then Yes");
	type_code("1 2 3 4");
	ok_run("down ok ok");
	fv.phase = DOTA_P_RUN;
	moved();
	fv.ptt_prompt = true;	/* the radio core refused PTT (its deny tone) and asks */
	moved();
	expect(T("shows=Stop_the_transfer? shows=OK_yes shows=Back_no", "shows=durdurulsun shows=OK_evet"));
	layout();
	ok_run("shot:ptt");
	ok_run("back");
	check(clears == 1 && stops == 0 && on, "No: the transfer goes on");
	expect(T("hides=Stop_the_transfer?", "hides=durdurulsun"));
	fv.ptt_prompt = true;
	moved();
	ok_run("ok");
	check(stops == 1, "Yes: stopped");
}

static void writing_holds(void)
{
	start("transfer: writing can't be stopped");
	ok_run("right down down down down down right down down ok");
	fv.phase = DOTA_P_WRITING;
	moved();
	expect(T("shows=Writing", "shows=Yaz"));
	ok_run("back");
	expect(T("hides=Stop_the_transfer?", "hides=durdurulsun"));
	check(stops == 0, "stopped while writing");
}

static void results(void)
{
	static const struct {
		uint8_t phase, result, why;
		const char *en, *tr;
	} r[] = {
		{ DOTA_P_DONE, DOTA_RES_SENT, 0, "shows=Sent_12_channels,_2 shows=zones", "shows=12_kanal,_2_b shows=nderildi" },
		{ DOTA_P_FAILED, DOTA_RES_NO_ANSWER, 0, "shows=No_answer", "shows=Yan" },
		{ DOTA_P_FAILED, DOTA_RES_REFUSED, DOTA_R_VERSION, "shows=Other_version", "shows=Farkl" },
		{ DOTA_P_FAILED, DOTA_RES_REFUSED, DOTA_R_NOT_READY, "shows=Not_ready", "shows=Haz" },
		{ DOTA_P_FAILED, DOTA_RES_REFUSED, DOTA_R_CHANNELS, "shows=Codeplug_full", "shows=Codeplug_dolu" },
		{ DOTA_P_FAILED, DOTA_RES_PEER_STOPPED, 0, "shows=Receiver_stopped", "shows=durdurdu" },
		{ DOTA_P_FAILED, DOTA_RES_UNSURE, 0, "shows=No_final_answer", "shows=Son" },
		{ DOTA_P_FAILED, DOTA_RES_BUSY, 0, "shows=Channel_busy", "shows=Kanal" },
		{ DOTA_P_FAILED, DOTA_RES_NOTHING, 0, "shows=Nothing_to_send", "shows=bir" },
	};
	for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++) {
		start("transfer: results");
		type_code("1 2 3 4");
		ok_run("down ok ok");
		fv.phase = r[i].phase;
		fv.result = r[i].result;
		fv.why = r[i].why;
		fv.ch_added = 12;
		fv.zones_added = 2;
		on = false;
		moved();
		expect(T(r[i].en, r[i].tr));
		layout();
		if (i == 0)
			ok_run("shot:result");
	}
}

static void receiver_done(void)
{
	start("receiver: done");
	ok_run("right down down down down down right down down ok");
	fv.phase = DOTA_P_DONE;
	fv.result = DOTA_RES_DONE;
	fv.ch_added = 1;
	fv.zones_added = 1;
	on = false;
	moved();
	/* one of each: singular in English; Turkish puts the verb last */
	expect(T("shows=Imported_1_channel,_1 shows=zone hides=channels hides=zones", "shows=1_kanal,_1_b shows=aktar"));
	layout();
	/* no zone: the count of zones is left out */
	fv.ch_added = 5;
	fv.zones_added = 0;
	moved();
	expect(T("shows=Imported_5_channels hides=zone", "shows=5_kanal hides=lge"));
}

/* --- The picker --- */

/* --- Data packs --- */

/* What: the RadioID pack in force (its name, date and size), the air band pack none (greyed, nothing to send) */
static void pack_send(void)
{
	start("data pack: send the RadioID pack");
	ok_run("down down left left left");	/* What: One channel, then back round past the map pack and the air band to the RadioID pack */
	expect(T("shows=RadioID_pack shows=RadioID_TR shows=2026-11-17 shows=About", "shows=RadioID_paketi shows=RadioID_TR "
		 "shows=2026-11-17 shows=Yakla"));
	layout();
	ok_run("shot:pack-send");
	ok_run("right");		/* the air band pack: none */
	expect(T("shows=Air_band_pack shows=none hides=RadioID_TR", "shows=Hava shows=yok hides=RadioID_TR"));
	layout();
	ok_run("right");		/* the map pack (firmware 1.1): none here either */
	expect(T("shows=Map_pack shows=none hides=RadioID_TR", "shows=Harita shows=yok hides=RadioID_TR"));
	layout();
	ok_run("left");			/* back to the air band pack */
	type_code("1 2 3 4");
	ok_run("down ok");
	expect(T("toast=Nothing_to_send", "toast=G"));
	check(starts == 0, "started without a pack");
	ok_run("up up up up up up up up up up down down left");	/* What: the RadioID pack */
	ok_run("down down down down down down down ok ok");	/* Send, Receiver ready? */
	check(starts == 1 && got.kind == DOTA_KIND_PACK && got.pack == DPK_RADIOID && got.code == 1234, "the pack's request");
	/* its progress in KB, its result */
	fv.phase = DOTA_P_RUN;
	fv.pack = true;
	fv.channels = 834;	/* 100,000 B in 120-byte frames */
	fv.pct = 50;
	strcpy(fv.peer, "TA1BBB");
	moved();
	expect("shows=TA1BBB shows=49/98_KB");
	layout();
	fv.phase = DOTA_P_DONE;
	fv.result = DOTA_RES_SENT;
	on = false;
	moved();
	expect(T("shows=Sent_data_pack", "shows=veri_paketi_g"));
	layout();
}

/* The receiver: "Data pack: RadioID TR, 2026-11-17, 98 KB. Accept?" while it waits; OK yes, Back no */
static void pack_question(int yes)
{
	start(yes ? "data pack: the receiver's Yes" : "data pack: the receiver's No");
	ok_run("right down down down down down right down down ok");	/* Receive, Import: Current zone, Ready */
	check(starts == 1, "receiver started");
	fv.asking = true;
	moved();
	expect(T("shows=Data_pack:_RadioID_TR, shows=2026-11-17, shows=Accept? shows=OK_yes shows=Back_no",
		 "shows=Veri_paketi:_RadioID shows=2026-11-17, shows=Kabul? shows=OK_evet"));
	layout();
	if (yes)
		ok_run("shot:pack-question");
	ok_run(yes ? "ok" : "back");
	check(answers == 1 && answered_yes == yes, "the answer");
	check(stops == 0 && on, "still waiting (no stop question, no stop)");
	expect(T("hides=Accept? shows=Waiting_for_sender", "hides=Kabul? shows=bekleniyor"));
}

static void pack_yes(void)
{
	pack_question(1);
}

static void pack_no(void)
{
	pack_question(0);
}

/* Done: "Imported data pack"; the sender's "Not accepted" */
static void pack_results(void)
{
	start("data pack: the results");
	ok_run("right down down down down down right down down ok");
	fv.phase = DOTA_P_DONE;
	fv.result = DOTA_RES_DONE;
	fv.pack = true;
	on = false;
	moved();
	expect(T("shows=Imported_data_pack", "shows=veri_paketi_i"));
	layout();
	start("data pack: not accepted");
	type_code("1 2 3 4");
	ok_run("down ok ok");
	for (uint8_t why = DOTA_R_WAIT; why <= DOTA_R_DECLINED; why++) {
		fv.phase = DOTA_P_FAILED;
		fv.result = DOTA_RES_REFUSED;
		fv.why = why;
		on = false;
		moved();
		expect(T("shows=Not_accepted", "shows=Kabul_edilmedi"));
		layout();
	}
}

static void picker_zone(void)
{
	start("picker: a zone");
	ok_run("down down right right");	/* What: A zone */
	expect(T("shows=A_zone", "shows=lge"));	/* "Bölge" */
	ok_run("down ok");
	expect("screen=dota-pick");
	layout();
	ok_run("down ok");
	expect("screen=dota");
	type_code("1 2 3 4");
	ok_run("down ok ok");
	check(starts == 1 && got.kind == DOTA_KIND_ZONE && got.zone == 1, "zone 1 sent");
}

static void picker_selected(void)
{
	start("picker: channels ticked in two zones");
	ok_run("down down right");	/* What: Selected channels */
	ok_run("down ok");
	expect("screen=dota-pick");
	ok_run("ok down ok right ok back");	/* two in this zone, one in the next */
	expect("screen=dota shows=3");
	unsigned n = 0;
	for (unsigned i = 0; i < sizeof sel * 8u; i++)
		n += (sel[i >> 3] >> (i & 7u)) & 1u;
	check(n == 3, "three channels ticked");
}

typedef void (*case_fn)(void);
static const case_fn all[] = {
	form_send_fm, form_receive_dmr, form_sender_dmr, form_freq_source, form_freq_channel_last, start_needs_code,
	start_asks_ready, start_question_no,
	start_battery, freq_typed, freq_stepped, start_refusals, receiver_start, receiver_dmr_id, run_page, back_stop_no, back_stop_yes,
	ptt_prompt, writing_holds, results, receiver_done, picker_zone, picker_selected, pack_send, pack_yes, pack_no,
	pack_results,
};

int main(void)
{
	int bad = 0, run = 0;
	for (int l = 0; l < 2; l++) {
		for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				turkish = l == 1;
				all[i]();
				cases++;
				fflush(stdout);
				_exit(fails != 0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			run++;
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
				bad++;
				if (!WIFEXITED(st))
					printf("FAIL case %u (%s): crashed\n", i, l ? "tr" : "en");
			}
		}
	}
	printf("DOTA screens: %d cases (English and Turkish), %d failed\n", run, bad);
	return bad != 0;
}
