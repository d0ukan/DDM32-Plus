/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer in the real radio core (radio_sm.c with CONFIG_DOTA=1: radio_dota.inc, the session engine
 * dota.c and its protocol), on radioCoreTest's fake board with a fake AFSK transmitter, after upstream's and Classic's
 * radio suites (tests/classic/test_radio.inc) have run again with it built in. Included after them by
 * tools/test_classic.py (test_radio_dota), linked with -Wl,--wrap=beep_play so the beeps played are seen.
 *
 * Checked: a session takes the radio onto its own channel (frequency, 12.5 kHz, the CTCSS both ways, the power class
 * mapped to the board's lowest level; DMR: the colour code and slot) and keys it only through the session; PTT during
 * a session is refused with the deny tone, never keys, and raises the prompt; requests that would move the radio
 * (channel, zone, VFO, scan) are dropped while it runs and work again after it; the user's channel or VFO comes back
 * exactly as it was (frequency, tones, mode, power) when the session ends by itself ("No answer"), by the user's
 * stop, or on a DMR session; no keyed span goes past the session's 10 s limit (the radio's time-out is 15 s at the
 * least). The receiving side is not on the air here: tests/plus/test_dota_session.c and test_dota_fm.c run two
 * sessions against each other, and test_dota_proto.c checks the FM rest after each keyed minute. */
#include "radio/dota.h"
#include "radio/beep.h"

/* --- The beeps the radio core plays (the real beep.c behind) --- */
static uint32_t dr_beeps[BEEP_COUNT];
void __real_beep_play(beep_t b, uint8_t level, bool shared);
void __wrap_beep_play(beep_t b, uint8_t level, bool shared)
{
	if ((unsigned)b < BEEP_COUNT)
		dr_beeps[b]++;
	__real_beep_play(b, level, shared);
}

/* --- The fake AFSK transmitter: busy for the tones' length at 1200 baud --- */
static uint32_t dr_busy_until, dr_afsk_starts, dr_afsk_bits;
static bool dr_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t m, uint16_t s, uint8_t l)
{
	(void)t, (void)m, (void)s, (void)l;
	CHECK(baud == 1200 && sm.view.state == RS_TX);
	dr_afsk_starts++;
	dr_afsk_bits += n;
	dr_busy_until = now + n * 1000u / 1200u;
	return true;
}
static bool dr_more(const uint8_t *t, uint16_t n)
{
	(void)t;
	dr_afsk_bits += n;
	dr_busy_until += n * 1000u / 1200u;
	return true;
}
static bool dr_busy(void) { return (int32_t)(dr_busy_until - now) > 0; }
static bool dr_queued(void) { return false; }
static void dr_stop(void) { dr_busy_until = now; }
const hal_afsk_t dota_afsk = { .tx_start = dr_start, .tx_busy = dr_busy, .tx_stop = dr_stop, .tx_more = dr_more,
			       .tx_queued = dr_queued };

/* The UI's request reaches the radio task (radio_task.c's radio_req_dota) */
void radio_req_dota(bool on)
{
	req(RREQ_DOTA, on ? 1 : 0);
}

/* --- The receiver's import is not used here (a sender, or a receiver nobody sends to): codeplug.c's staging --- */
bool codeplug_stage_channel(uint16_t number, const channel_t *ch) { (void)number, (void)ch; CHECK(false); return false; }
uint16_t codeplug_free_nth(uint16_t k) { (void)k; return 0; }
uint16_t codeplug_free_channels(void) { return 0; }
bool codeplug_commit_staged(const uint8_t *ranks, uint16_t n) { (void)ranks, (void)n; CHECK(false); return false; }
uint16_t codeplug_contact_index(uint32_t id, uint8_t type) { (void)id, (void)type; return 0; }
uint8_t codeplug_free_zone_slots(void) { return 0; }
bool codeplug_stage_zone(uint8_t k, const zone_t *z) { (void)k, (void)z; CHECK(false); return false; }
bool codeplug_staged_zone(uint8_t k, zone_t *z) { (void)k, (void)z; return false; }
bool codeplug_set_listen_list(uint16_t i, const listen_list_t *l) { (void)i, (void)l; CHECK(false); return false; }
bool codeplug_delete_contact(uint16_t i) { (void)i; CHECK(false); return false; }
bool codeplug_delete_listen_list(uint16_t i) { (void)i; CHECK(false); return false; }
bool codeplug_tag_remove(uint16_t number) { (void)number; CHECK(false); return false; }

/* --- Watching the transmitter --- */
static uint32_t dr_key_at, dr_longest, dr_keyed_ms, dr_spans;
static int dr_tx0;
static bool dr_was_tx;

static void dr_watch(void)
{
	bool tx = sm.view.state == RS_TX;
	if (tx && !dr_was_tx) {
		dr_key_at = now;
	}
	if (!tx && dr_was_tx) {
		uint32_t span = now - dr_key_at;
		dr_spans++;
		dr_keyed_ms += span;
		if (span > dr_longest)
			dr_longest = span;
	}
	dr_was_tx = tx;
}

/* ms of polls, watching every keyed span; the receiver's channel is clear and carries no tone */
static void dr_run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) {
		poll_for(RADIO_POLL_MS);
		dr_watch();
	}
}

static void dr_reset(void)
{
	memset(dr_beeps, 0, sizeof dr_beeps);
	dr_busy_until = now;
	dr_afsk_starts = dr_afsk_bits = 0;
	dr_longest = dr_keyed_ms = dr_spans = 0;
	dr_was_tx = false;
	dr_tx0 = hw.tx_calls;
}

static dota_req_t dr_req(uint8_t role, uint8_t method)
{
	dota_req_t r;
	memset(&r, 0, sizeof r);
	r.role = role;
	r.method = method;
	r.hz = 433500000;
	r.ctcss = 885;
	r.cc = 3;
	r.ts = 2;
	r.peer_id = 3101234;
	r.power = 0;	/* Low */
	r.code = 4271;
	strcpy(r.call, "TA1AAA");
	r.kind = DOTA_KIND_CHANNEL;
	r.channel = 1;
	r.imp.mode = DOTA_IMP_ADD;
	return r;
}

/* The user's place as the radio core holds it */
typedef struct {
	bool vfo;
	uint16_t number;
	uint32_t rx_hz, tx_hz;
	subtone_t rx_tone, tx_tone;
	uint8_t mode, power;
} dr_place_t;
static dr_place_t dr_place(void)
{
	dr_place_t p = { sm.view.vfo, sm.view.channel_number, sm.view.ch.rx_hz, sm.view.ch.tx_hz, sm.view.ch.rx_tone,
			 sm.view.ch.tx_tone, sm.view.ch.mode, sm.view.power };
	return p;
}
static bool dr_same(const dr_place_t *a, const dr_place_t *b)
{
	return a->vfo == b->vfo && a->number == b->number && a->rx_hz == b->rx_hz && a->tx_hz == b->tx_hz &&
	       a->rx_tone.type == b->rx_tone.type && a->rx_tone.value == b->rx_tone.value &&
	       a->tx_tone.type == b->tx_tone.type && a->tx_tone.value == b->tx_tone.value && a->mode == b->mode &&
	       a->power == b->power;
}

/* A sender nobody answers: the session channel, PTT refused, requests dropped, "No answer", the channel back */
static void test_dota_sender_fm(void)
{
	setup_codeplug();
	start();
	signal(120, false);		/* a clear channel, no tone heard */
	poll_for(1000);
	CHECK(sm.view.channel_number == 1 && hw.rx_hz == 146520000);
	dr_place_t before = dr_place();
	dr_reset();
	dota_req_t r = dr_req(DOTA_SEND, DOTA_FM);
	CHECK(dota_start(&r));
	dr_run(100);
	/* on the session's channel: 433.500, 12.5 kHz, CTCSS 88.5 both ways */
	CHECK(dota_owns_radio() && hw.rx_hz == 433500000 && hw.rx_bw == RADIO_BW_12K5);
	CHECK(hw.rx_tone.type == TONE_CTCSS && hw.rx_tone.value == 885);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && !sm.view.ch.rx_only);
	/* the first window keys with the tone, at the lowest power */
	dr_run(3000);
	CHECK(hw.tx_calls > dr_tx0 && hw.tx_hz == 433500000 && hw.tx_tone.type == TONE_CTCSS && hw.tx_tone.value == 885);
	CHECK(hw.tx_power == 0 && dr_afsk_starts > 0);
	CHECK(dr_beeps[BEEP_DOTA_MARK] == 1);	/* the start tones */

	/* PTT while it runs: the deny tone, the prompt, no keying of its own */
	while (sm.view.state == RS_TX)
		dr_run(RADIO_POLL_MS);
	int tx_before = hw.tx_calls;
	uint32_t deny = dr_beeps[BEEP_TX_DENY];
	req(RREQ_PTT, 1);
	dr_run(200);
	dota_view_t v;
	dota_view(&v);
	CHECK(dr_beeps[BEEP_TX_DENY] == deny + 1 && v.ptt_prompt && !sm.ptt);
	CHECK(hw.tx_calls == tx_before || hw.tx_hz == 433500000);	/* anything keyed is the session's own */
	req(RREQ_PTT, 0);
	dota_ptt_prompt_clear();	/* the user's No */

	/* requests that would move the radio are dropped */
	req(RREQ_CH_STEP, 1);
	req(RREQ_VFO, 1);
	req(RREQ_ZONE, 1);
	dr_run(200);
	CHECK(dota_owns_radio() && !sm.view.vfo && hw.rx_hz == 433500000 && sm.view.channel_number == 1);

	/* nobody answers: the HELLO goes again and again, then "No answer" */
	for (int i = 0; i < 1200 && dota_owns_radio(); i++)
		dr_run(100);
	dota_view(&v);
	CHECK(!dota_owns_radio() && !dota_on() && v.result == DOTA_RES_NO_ANSWER);
	CHECK(dr_beeps[BEEP_TX_DENY] == deny + 2);	/* the failure's tone */
	dr_run(200);
	/* the channel exactly as it was, tuned again */
	dr_place_t after = dr_place();
	CHECK(dr_same(&before, &after) && hw.rx_hz == 146520000 && hw.rx_tone.type == TONE_NONE);
	CHECK(sm.view.state != RS_TX && dr_spans > 2);
	/* no keyed span past the session's limit (10 s), well under the radio's time-out (180 s by default, 15 s at
	 * the least a channel can set) */
	CHECK(dr_longest > 0 && dr_longest <= 10000u + RADIO_POLL_MS);
	/* requests work again */
	req(RREQ_CH_STEP, 1);
	dr_run(100);
	CHECK(sm.view.channel_number == 2 && hw.rx_hz == 162550000);
	printf("Plus DOTA radio core: FM sender alone: %u keyed spans, the longest %u ms, %u tone bits, then \"No answer\" "
	       "and channel 1 back\n", dr_spans, dr_longest, dr_afsk_bits);
}

/* The user stops part way: one ABORT, the end, the channel back */
static void test_dota_stop(void)
{
	setup_codeplug();
	start();
	signal(120, false);
	poll_for(1000);
	req(RREQ_CH_STEP, 1);	/* channel 2, receive only: the session transmits anyway on its own channel */
	poll_for(200);
	dr_place_t before = dr_place();
	dr_reset();
	dota_req_t r = dr_req(DOTA_SEND, DOTA_FM);
	CHECK(dota_start(&r));
	dr_run(4000);
	CHECK(dota_owns_radio() && hw.tx_calls > dr_tx0);
	dota_stop();
	for (int i = 0; i < 200 && dota_owns_radio(); i++)
		dr_run(100);
	dota_view_t v;
	dota_view(&v);
	CHECK(!dota_owns_radio() && v.result == DOTA_RES_STOPPED);
	dr_run(200);
	dr_place_t after = dr_place();
	CHECK(dr_same(&before, &after) && hw.rx_hz == 162550000 && sm.view.state != RS_TX);
}

/* A DMR receiver waiting from the VFO: tuned to DMR with the colour code and slot; the user leaves; the VFO back */
static void test_dota_receiver_dmr_vfo(void)
{
	setup_codeplug();
	start();
	signal(120, false);
	poll_for(1000);
	req(RREQ_VFO, 1);
	poll_for(200);
	req(RREQ_VFO_SET, 145500000);
	poll_for(200);
	CHECK(sm.view.vfo && hw.rx_hz == 145500000);
	dr_place_t before = dr_place();
	dr_reset();
	dota_req_t r = dr_req(DOTA_RECEIVE, DOTA_DMR);
	CHECK(dota_start(&r));
	dr_run(500);
	CHECK(dota_owns_radio() && sm.view.ch.mode == RADIO_MODE_DMR && hw.rx_hz == 433500000);
	CHECK(sm.view.ch.color_code == 3 && sm.view.ch.timeslot == 2 && sm.view.ch.dmo);
	dr_run(5000);
	CHECK(hw.tx_calls == dr_tx0);	/* a receiver nobody calls never transmits */
	dota_stop();
	for (int i = 0; i < 200 && dota_owns_radio(); i++)
		dr_run(100);
	CHECK(!dota_owns_radio());
	dr_run(200);
	dr_place_t after = dr_place();
	CHECK(dr_same(&before, &after) && sm.view.vfo && hw.rx_hz == 145500000);
}

/* The radio core repeats the band plan before keying (hole hunt A1: every self-keying feature checks in the core, not
 * only on its page): a session on PMR446 or a repeater input never keys, FM or DMR; stopped, the channel comes back */
static void test_dota_band_plan(void)
{
	static const struct { uint32_t hz; uint8_t method; } cases[] = {
		{ 446006250u, DOTA_FM }, { 145000000u, DOTA_FM }, { 146520000u, DOTA_FM }, { 446006250u, DOTA_DMR },
	};
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		setup_codeplug();
		start();
		signal(120, false);
		poll_for(1000);
		dr_place_t before = dr_place();
		dr_reset();
		dota_req_t r = dr_req(DOTA_SEND, cases[i].method);
		r.hz = cases[i].hz;
		CHECK(dota_start(&r));
		dr_run(100);
		CHECK(dota_owns_radio() && hw.rx_hz == cases[i].hz);	/* on the session's channel ... */
		dr_run(6000);	/* (an FM sender gives up when its first key-up is refused) */
		CHECK(hw.tx_calls == dr_tx0 && dr_spans == 0 && sm.view.state != RS_TX);	/* ... never keyed */
		dota_stop();
		for (int k = 0; k < 200 && dota_owns_radio(); k++)
			dr_run(100);
		CHECK(!dota_owns_radio() && hw.tx_calls == dr_tx0);
		dr_run(200);
		dr_place_t after = dr_place();
		CHECK(dr_same(&before, &after));
	}
	printf("Plus DOTA radio core: the band plan again before keying: PMR446, a repeater input, 146.520 never keyed\n");
}

int main(void)
{
	int prior = classic_radio_main();
	int before = failures;
	test_dota_sender_fm();
	test_dota_stop();
	test_dota_receiver_dmr_vfo();
	test_dota_band_plan();
	printf("Plus DOTA in the radio core: %d failures (upstream's and Classic's suites with it built in: %s)\n",
	       failures - before, prior ? "FAILED" : "passed");
	return prior != 0 || failures != before;
}
