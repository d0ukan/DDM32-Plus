/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus SSTV send in the real radio core (radio_sm.c with CONFIG_SSTV_TX=1: radio_sstvtx.inc, the glue
 * radio_sstvtx.c and the generator sstv_tx.c), on radioCoreTest's fake board with a fake tone path that plays the
 * generator at 8 kHz as the radio core's clock runs (board.afsk tx_pcm), after upstream's and Classic's radio suites
 * (tests/classic/test_radio.inc) have run again with it built in. Included after them by tools/test_classic.py
 * (test_radio_sstvtx).
 *
 * Checked: a picture on the FM simplex channel keys its TX frequency with its bandwidth, tone and power, plays the
 * whole picture (the very samples sstv_tx.c makes for that mode and callsign) and unkeys once, back to receive on the
 * channel; the screen's info follows (keying up, the line, sent). Each refusal before keying: no callsign, receive
 * only, a DMR channel, a repeater (RX and TX differ), the time-out timer shorter than the picture, PTT held, no tone
 * path; nothing keys then. Stopping: the stop request, PTT (that press keys no voice), any other request (a channel
 * step, which then happens), the time-out timer: each unkeys at once, stops the tones and frees the generator. */
#include "radio/radio_sstvtx.h"

/* --- The fake tone path: pulls the generator's samples as time passes, 8 a millisecond --- */
static uint16_t (*st_gen)(int16_t *, uint16_t, int16_t);
static uint32_t st_t0, st_pulled, st_starts, st_stops, st_sum;
static bool st_playing, st_refuse;
static uint8_t st_level;

static void st_pull(void)
{
	if (!st_playing)
		return;
	uint32_t owed = (now - st_t0) * 8u;
	while (st_pulled < owed) {
		int16_t buf[32];
		uint16_t want = owed - st_pulled < 32u ? (uint16_t)(owed - st_pulled) : 32u;
		uint16_t got = st_gen(buf, want, 2750);
		for (uint16_t i = 0; i < got; i++)
			st_sum = st_sum * 31u + (uint16_t)buf[i];
		st_pulled += got;
		if (got < want) {
			st_playing = false;	/* the generator's end: the FIFO plays out */
			return;
		}
	}
}
static bool st_tx_pcm(uint16_t (*gen)(int16_t *out, uint16_t max, int16_t amp), uint8_t level)
{
	CHECK(sm.view.state == RS_TX && hw.tx_calls > 0);	/* keyed before the tones */
	if (st_refuse || st_playing)
		return false;
	st_gen = gen;
	st_level = level;
	st_t0 = now;
	st_pulled = 0;
	st_sum = 0;
	st_playing = true;
	st_starts++;
	return true;
}
static bool st_busy(void)
{
	st_pull();
	return st_playing;
}
static void st_stop(void)
{
	st_playing = false;
	st_stops++;
}
static bool st_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t m, uint16_t s, uint8_t l)
{
	(void)t, (void)n, (void)baud, (void)m, (void)s, (void)l;
	return false;
}
const hal_afsk_t sstvtx_afsk = { .tx_start = st_start, .tx_busy = st_busy, .tx_stop = st_stop, .tx_pcm = st_tx_pcm };

/* The samples sstv_tx.c makes for mode and call, hashed as the fake hashes them */
static uint32_t st_expect(uint8_t mode, const char *call, uint32_t *n)
{
	static sstv_tx_t g;
	sstv_tx_init(&g, mode, call);
	uint32_t sum = 0, k = 0;
	int16_t buf[32];
	uint16_t got;
	while ((got = sstv_tx_pcm(&g, buf, 32, 2750)) != 0) {
		for (uint16_t i = 0; i < got; i++)
			sum = sum * 31u + (uint16_t)buf[i];
		k += got;
	}
	*n = k;
	return sum;
}

static radio_sstvtx_info_t st_info(void)
{
	radio_sstvtx_info_t in;
	radio_sstvtx_info(&in);
	return in;
}

/* Channel 1's frequency for these tests: 145.500 (an amateur simplex frequency the band plan allows; the fixture's
 * 146.520 is outside Region 1's 2 m band) */
static uint32_t st_hz = 145500000u;

static void st_begin(uint16_t channel_step)
{
	setup_codeplug();
	chans[1].rx_hz = chans[1].tx_hz = st_hz;
	start();
	signal(120, false);
	poll_for(500);
	for (uint16_t i = 0; i < channel_step; i++) {
		req(RREQ_CH_STEP, 1);
		poll_for(100);
	}
	strcpy(sm.radio_name, "TA2DOU");
	st_playing = st_refuse = false;
	st_starts = st_stops = 0;
	hw.tx_calls = hw.tx_off_calls = 0;
}

/* Robot 36 sent whole on channel 1 (145.500 FM simplex) */
static void test_sstvtx_sent(void)
{
	st_begin(0);
	CHECK(sm.view.channel_number == 1 && hw.rx_hz == 145500000);
	uint32_t want_n, want = st_expect(SSTV_TX_ROBOT36, "TA2DOU", &want_n);
	uint16_t seq = radio_sstvtx_seq();
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	CHECK(sm.view.state == RS_TX && hw.tx_calls == 1 && hw.tx_hz == 145500000 && hw.tx_power == sm.view.power);
	CHECK(hw.tx_bw == (radio_bw_t)sm.view.ch.bw && hw.tx_tone.type == sm.view.ch.tx_tone.type);
	CHECK(st_info().state == SSTVTX_KEYUP && st_info().lines == 240 && radio_sstvtx_seq() != seq);
	poll_for(100);
	CHECK(st_starts == 1 && st_level == 50 && st_info().state == SSTVTX_SENDING);
	poll_for(20000);
	radio_sstvtx_info_t in = st_info();
	CHECK(sm.view.state == RS_TX && in.state == SSTVTX_SENDING && in.line > 100 && in.line < 140);
	CHECK(sm.view.tx_elapsed_ms >= 19000);
	for (int i = 0; i < 400 && sm.view.state == RS_TX; i++)
		poll_for(100);
	in = st_info();
	CHECK(sm.view.state == RS_IDLE && in.state == SSTVTX_SENT && in.line == 240 && hw.tx_off_calls == 1);
	CHECK(st_pulled == want_n && st_sum == want);	/* the very picture, whole */
	CHECK(st_stops == 1 && hw.rx_hz == 145500000 && !sm.ptt);
	printf("Plus SSTV send radio core: Robot 36 on 145.500: %u samples played (%u.%03u s), unkeyed once\n",
	       (unsigned)st_pulled, (unsigned)(st_pulled / 8000u), (unsigned)(st_pulled % 8000u / 8u));
}

/* Refused before keying: nothing keys, the deny tone */
static void st_refused(uint8_t why, uint8_t deny)
{
	poll_for(100);
	radio_sstvtx_info_t in = st_info();
	CHECK(in.state == SSTVTX_REFUSED && in.why == why && (deny == 0 || in.deny == deny));
	CHECK(hw.tx_calls == 0 && sm.view.state != RS_TX && st_starts == 0);
}

static void test_sstvtx_refusals(void)
{
	st_begin(0);
	strcpy(sm.radio_name, "Dogan");
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	st_refused(SSTVTX_R_CALL, 0);
	st_begin(1);	/* channel 2: receive only */
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	st_refused(SSTVTX_R_DENY, TXDENY_RX_ONLY);
	st_begin(0);
	req(RREQ_ZONE, 2);	/* zone "Tones": a repeater, RX and TX differ */
	poll_for(200);
	hw.tx_calls = 0;
	CHECK(sm.view.ch.rx_hz != sm.view.ch.tx_hz);
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	st_refused(SSTVTX_R_SPLIT, 0);
	st_begin(2);	/* channel 4 (3 is empty, stepped over): DMR */
	CHECK(sm.view.channel_number == 4 && sm.view.ch.mode == RADIO_MODE_DMR);
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	st_refused(SSTVTX_R_MODE, 0);
	st_begin(0);
	sm.view.tot_s = 60;	/* Martin M1 takes 1:56 */
	req(RREQ_SSTV_TX, SSTV_TX_MARTIN1);
	st_refused(SSTVTX_R_TOT, 0);
	st_begin(0);
	req(RREQ_PTT, 1);
	poll_for(100);
	CHECK(sm.ptt);
	int voice = hw.tx_calls;
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	poll_for(100);
	CHECK(st_info().state == SSTVTX_REFUSED && st_info().why == SSTVTX_R_BUSY && hw.tx_calls == voice && st_starts == 0);
	req(RREQ_PTT, 0);
	poll_for(500);
	/* the board's tone path refuses: unkeyed at once */
	st_begin(0);
	st_refuse = true;
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	poll_for(200);
	CHECK(st_info().state == SSTVTX_REFUSED && st_info().why == SSTVTX_R_AUDIO && hw.tx_off_calls == 1);
	CHECK(sm.view.state != RS_TX);
	/* the board would not key */
	st_begin(0);
	tx_fail = true;
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	poll_for(200);
	tx_fail = false;
	CHECK(st_info().state == SSTVTX_REFUSED && st_info().why == SSTVTX_R_KEY && sm.view.state != RS_TX && st_starts == 0);
}

/* Each way of stopping: off the air at once */
static void test_sstvtx_stops(void)
{
	/* the stop request (the screen's Back) */
	st_begin(0);
	req(RREQ_SSTV_TX, SSTV_TX_PD120);
	poll_for(3000);
	CHECK(st_playing && sm.view.state == RS_TX);
	req(RREQ_SSTV_TX, SSTV_TX_MODES);
	CHECK(!st_playing && st_stops == 1 && hw.tx_off_calls == 1 && sm.view.state == RS_IDLE);
	CHECK(st_info().state == SSTVTX_STOPPED && st_info().why == SSTVTX_R_NONE);
	poll_for(200);
	CHECK(hw.tx_calls == 1);
	/* PTT: stops it; that press keys no voice, the next one does */
	st_begin(0);
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	poll_for(2000);
	req(RREQ_PTT, 1);
	CHECK(!st_playing && hw.tx_off_calls == 1 && sm.view.state != RS_TX && !sm.ptt && hw.tx_calls == 1);
	poll_for(200);
	req(RREQ_PTT, 0);
	poll_for(200);
	req(RREQ_PTT, 1);
	CHECK(sm.ptt && sm.view.state == RS_TX && hw.tx_calls == 2);	/* voice, as usual */
	req(RREQ_PTT, 0);
	poll_for(500);
	/* a channel step: stops it, then the step happens */
	st_begin(0);
	req(RREQ_SSTV_TX, SSTV_TX_MARTIN1);
	poll_for(2000);
	req(RREQ_CH_STEP, 1);
	poll_for(100);
	CHECK(!st_playing && hw.tx_off_calls == 1 && sm.view.state != RS_TX && sm.view.channel_number == 2);
	CHECK(st_info().state == SSTVTX_STOPPED);
	/* a beep or a setting does not stop it */
	st_begin(0);
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	poll_for(1000);
	req(RREQ_BEEP, 0);
	poll_for(100);
	CHECK(st_playing && sm.view.state == RS_TX);
	/* the time-out timer cuts it */
	sm.view.tot_s = 15;
	poll_for(15000);
	CHECK(!st_playing && sm.view.state != RS_TX && hw.tx_off_calls == 1);
	CHECK(st_info().state == SSTVTX_STOPPED && st_info().why == SSTVTX_R_TOT);
	printf("Plus SSTV send radio core: stopped by Back, PTT, a channel step and the time-out; refusals checked\n");
}

/* Hole hunt A1 / B7: the radio core is the authority. The band plan again before keying (146.520 is outside Region 1's
 * 2 m band, 446.00625 PMR446, 145.000 a repeater input, 145.825 the satellite segment); nothing moving the receiver: a
 * scan, a dual watch (before: the core moved to the selected letter and keyed it unchecked) */
static void test_sstvtx_core_checks(void)
{
	static const uint32_t refused[] = { 146520000u, 446006250u, 145000000u, 145825000u, 144800000u, 150000000u };
	for (unsigned i = 0; i < sizeof refused / sizeof refused[0]; i++) {
		st_hz = refused[i];
		st_begin(0);
		CHECK(sm.view.ch.tx_hz == refused[i]);
		req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
		st_refused(SSTVTX_R_BAND, 0);
	}
	st_hz = 433400000u;	/* the band plan allows it: keyed, then stopped */
	st_begin(0);
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 433400000u);
	req(RREQ_SSTV_TX, SSTV_TX_MODES);
	CHECK(sm.view.state != RS_TX);
	st_hz = 145500000u;
	/* a zone scan runs: refused, the scan goes on */
	st_begin(0);
	req(RREQ_SCAN, RSCAN_ZONE);
	poll_for(500);
	CHECK(scan_on(&sm.scan));
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	st_refused(SSTVTX_R_WATCH, 0);
	CHECK(scan_on(&sm.scan));
	req(RREQ_SCAN, RSCAN_STOP);
	poll_for(500);
#if CONFIG_DUAL_WATCH
	/* dual watch, A selected on 146.520 (out of the band plan), the ring on B: refused, nothing keyed anywhere */
	st_hz = 146520000u;
	st_begin(0);
	radio_dual_ref_t ref[2] = { { false, 0, 0 }, { false, 0, 1 } };
	radio_req_dual(ref, 0);
	poll_for(300);
	CHECK(scan_on(&sm.scan) && sm.scan.kind == SCAN_DUAL);
	hw.tx_calls = 0;
	for (int i = 0; i < 6; i++) {	/* wherever the ring is when the request lands */
		req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
		st_refused(SSTVTX_R_WATCH, 0);
		CHECK(sm.scan.kind == SCAN_DUAL);
		poll_for(170);
	}
	radio_req_dual(NULL, 0);
	poll_for(300);
	st_hz = 145500000u;
#endif
	printf("Plus SSTV send radio core: the band plan and a scan / dual watch refused in the core\n");
}

/* Hole hunt B16: the radio name is the station's identification only when it is shaped like an ITU callsign: a prefix
 * (a letter, two letters, letter and digit, digit and letter, or digit and two letters), one digit, 1 to 4 letters */
static void test_sstvtx_callsign(void)
{
	static const char *const good[] = { "TA2DOU", "TA2DOU/P", "YM2KDZ", "ta2dou-7", "W1AW", "N4ABC", "VE3ABC",
					    "KD2ABCD", "2E0ABC", "9A1A", "4X6TT", "A61AB", "3DA0XY", "M0ABC" };
	static const char *const bad[] = { "DM32UV", "BAOFENG", "12345", "RADIO1", "Dogan", "DM-32UV", "TA22DOU",
					   "1A2BC3", "W1", "TA2DOU1", "ABCD1EF", "1234A" };
	char cs[RADIO_CWID_CALL_MAX + 1];
	for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) {
		if (radio_cwid_callsign(good[i], cs, sizeof cs) == 0) {
			printf("  callsign refused: %s\n", good[i]);
			CHECK(false);
		}
	}
	for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		if (radio_cwid_callsign(bad[i], cs, sizeof cs) != 0) {
			printf("  not a callsign, taken: %s\n", bad[i]);
			CHECK(false);
		}
	}
	CHECK(radio_cwid_callsign("TA2DOU/P", cs, sizeof cs) == 6 && strcmp(cs, "TA2DOU") == 0);
	/* the radio core refuses the factory radio name */
	st_begin(0);
	strcpy(sm.radio_name, "DM32UV");
	req(RREQ_SSTV_TX, SSTV_TX_ROBOT36);
	st_refused(SSTVTX_R_CALL, 0);
	printf("Plus SSTV send radio core: callsigns: %u taken, %u refused (DM32UV, BAOFENG, 12345 among them)\n",
	       (unsigned)(sizeof good / sizeof good[0]), (unsigned)(sizeof bad / sizeof bad[0]));
}

int main(void)
{
	int prior = classic_radio_main();
	int before = failures;
	test_sstvtx_sent();
	test_sstvtx_refusals();
	test_sstvtx_stops();
	test_sstvtx_core_checks();
	test_sstvtx_callsign();
	printf("Plus SSTV send in the radio core: %d failures (upstream's and Classic's suites with it built in: %s)\n",
	       failures - before, prior ? "FAILED" : "passed");
	return prior != 0 || failures != before;
}
