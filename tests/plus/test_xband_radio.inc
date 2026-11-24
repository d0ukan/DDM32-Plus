/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus cross-band store and forward in the real radio core (radio_sm.c with CONFIG_XBAND=1: radio_xband.inc
 * and the engine xband.c), on radioCoreTest's fake board, its simulated DMR modem and the real DMR call stack, after
 * upstream's and Classic's radio suites (tests/classic/test_radio.inc) have run again with it built in. Included after
 * them by tools/test_classic.py (test_radio_xband), linked with -Wl,--wrap=beep_play so the beeps played are seen.
 *
 * Checked: a session takes the radio from an FM channel onto its own DMR channel (A, the colour code and slot, every
 * talkgroup heard, the lowest power) and borrows the P25 worker's memory; PTT is refused with the deny tone and never
 * keys; requests that would move the radio are dropped; a DMR call heard on A is recorded frame by frame and sent on
 * B through the call stack (the voice header to the talkgroup heard, from our own ID, every voice burst's 27 bytes
 * as heard, in order, then the terminator), the transmitter let go after; then the CW ID on B (FM, the callsign in
 * Morse, the session's DMR channel back after); the receiver goes between A and B while nothing is heard; the stop
 * puts the user's FM channel back exactly and gives the memory back; a transmission the board refuses drops the over
 * (no air, an error beep, no ID); a P25 worker that won't let go of its memory ends the session (nothing keyed).
 * A mode per side (2026-11-24): DMR -> FM (the frames decoded by the voice bridge as the over is sent, its blocks taken
 * by its mixer hook and played through the fake tone path, an FM carrier on B with no tone, let go once played, the
 * CW ID, the stop with the mixer and the channel back; a board without the tone path refuses the over); P25 -> P25 with
 * the real native receiver and transmitter (native_lab.c) and a fake worker: the session channel taken by the native
 * receiver (any NAC, our ID), PTT refused with the deny tone and never handed to the native transmitter, the frames
 * heard recorded and handed back to the worker in order on B with the heard NAC, the talkgroup heard and our ID, PTT let
 * go once the worker says sent, the CW ID on B after the native receiver let go, the stop with the channel back and the
 * memory given back after the native receiver let go. */
#include "radio/xband.h"
#include "radio/beep.h"

/* --- The beeps the radio core plays (the real beep.c behind) --- */
static uint32_t xr_beeps[BEEP_COUNT];
void __real_beep_play(beep_t b, uint8_t level, bool shared);
void __wrap_beep_play(beep_t b, uint8_t level, bool shared)
{
	if ((unsigned)b < BEEP_COUNT)
		xr_beeps[b]++;
	__real_beep_play(b, level, shared);
}

/* The UI's request reaches the radio task (radio_task.c's radio_req_xband) */
void radio_req_xband(bool on)
{
	req(RREQ_XBAND, on ? 1 : 0);
}

/* --- The P25 worker's memory (p25_lab.c): three pieces as on the radio (a P25 session: two, the FIFO past its frame
 * ring and the call replay's), refused while it runs --- */
static uint8_t xr_mem[17104] __attribute__((aligned(8)));
static int xr_lends, xr_gives;
static bool xr_lent, xr_lent_p25;
uint8_t p25_lab_lend(uint8_t *seg[], size_t len[], bool p25)
{
	if (classic_busy || xr_lent)
		return 0;
	xr_lent = true;
	xr_lent_p25 = p25;
	xr_lends++;
	if (p25) {
		seg[0] = xr_mem + 6916 + P25_LAB_XB_RING_BYTES;
		len[0] = 5788 - P25_LAB_XB_RING_BYTES;
		seg[1] = xr_mem + 6916 + 5788;
		len[1] = 4400;
		return 2;
	}
	seg[0] = xr_mem;
	len[0] = 6916;
	seg[1] = xr_mem + 6916;
	len[1] = 5788;
	seg[2] = xr_mem + 6916 + 5788;
	len[2] = 4400;
	return 3;
}
void p25_lab_give_back(void)
{
	CHECK(xr_lent);
	memset(xr_mem, 0, sizeof xr_mem);
	xr_lent = false;
	xr_gives++;
}

/* --- The P25 worker's frame ring (p25_lab.c), faked: what it heard, what it was handed to send (16 at a time, one taken
 * every 20 ms while it transmits), the link control it was given --- */
#define XR_P25_MAX 2000u
static uint8_t xr_heard[XR_P25_MAX][P25_LAB_XB_BYTES], xr_sent_fr[XR_P25_MAX][P25_LAB_XB_BYTES];
static unsigned xr_heard_n, xr_heard_taken, xr_put_n, xr_taken_n;
static bool xr_send, xr_eof, xr_was_sent;
static int xr_cancels;
static uint16_t xr_call_nac, xr_call_tg;
static uint32_t xr_call_id;
int p25_lab_xb_take(uint8_t frame[P25_LAB_XB_BYTES])
{
	CHECK(xr_lent && xr_lent_p25);
	if (xr_send || xr_heard_taken >= xr_heard_n)
		return 0;
	memcpy(frame, xr_heard[xr_heard_taken++], P25_LAB_XB_BYTES);
	return 1;
}
int p25_lab_xb_put(const uint8_t frame[P25_LAB_XB_BYTES])
{
	CHECK(xr_lent && xr_lent_p25);
	if (!xr_send || xr_eof || xr_put_n - xr_taken_n >= P25_LAB_XB_FRAMES || xr_put_n >= XR_P25_MAX)
		return 0;
	memcpy(xr_sent_fr[xr_put_n++], frame, P25_LAB_XB_BYTES);
	return 1;
}
void p25_lab_xb_heard(uint16_t *nac, uint16_t *tg, uint32_t *src)
{
	*nac = 0x293;
	*tg = 655;
	*src = 3101234;
}
void p25_lab_xb_send(int on)
{
	if (on > 0) {
		xr_send = true;
		xr_eof = xr_was_sent = false;
		xr_put_n = xr_taken_n = 0;
	} else if (on == 0) {
		xr_eof = true;
	} else {
		xr_send = xr_eof = false;
		xr_cancels++;
	}
}
int p25_lab_xb_sent(void) { return xr_was_sent; }
void p25_lab_xb_call(uint16_t nac, uint16_t tg, uint32_t id)
{
	xr_call_nac = nac;
	xr_call_tg = tg;
	xr_call_id = id;
}

/* --- The tone path (hal_afsk.h tx_pcm), faked: the generator read as the interrupt would --- */
static struct {
	uint16_t (*gen)(int16_t *out, uint16_t max, int16_t amp);
	bool busy;
	int starts, stops;
	uint8_t level;
	uint32_t samples;
	bool refuse;
} xa;
static bool xa_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t mark, uint16_t space, uint8_t level)
{
	(void)t, (void)n, (void)baud, (void)mark, (void)space, (void)level;
	CHECK(false);	/* never tones */
	return false;
}
static bool xa_busy(void) { return xa.busy; }
static void xa_stop(void) { xa.busy = false; xa.gen = NULL; xa.stops++; }
static bool xa_pcm(uint16_t (*gen)(int16_t *out, uint16_t max, int16_t amp), uint8_t level)
{
	if (xa.refuse || xa.busy || sm.view.state != RS_TX || hw_mode != RADIO_MODE_FM)
		return false;	/* (the transmitter keyed in FM first, as hrc_afsk.c needs) */
	xa.gen = gen;
	xa.level = level;
	xa.busy = true;
	xa.starts++;
	return true;
}
const hal_afsk_t xb_afsk = { .tx_start = xa_start, .tx_busy = xa_busy, .tx_stop = xa_stop, .tx_pcm = xa_pcm };
/* 4 ms of the tone path's interrupt: 32 samples asked for; fewer: the end */
static void xa_tick(void)
{
	if (!xa.busy || xa.gen == NULL)
		return;
	int16_t out[32];
	uint16_t n = xa.gen(out, 32, 16000);
	xa.samples += n;
	if (n < 32)
		xa.busy = false;
}

static void xr_reset(void)
{
	memset(xr_beeps, 0, sizeof xr_beeps);
	memset(&xa, 0, sizeof xa);
	xr_heard_n = xr_heard_taken = xr_put_n = xr_taken_n = 0;
	xr_send = xr_eof = xr_was_sent = false;
	xr_cancels = 0;
	xr_call_nac = xr_call_tg = 0;
	xr_call_id = 0;
	xr_lent_p25 = false;
	xr_lends = xr_gives = 0;
	xr_lent = false;
	classic_busy = false;
	tx_fail = false;
}

static void xr_req(uint32_t a, uint32_t b)
{
	xband_req_t *r = xband_req();
	r->hz[0] = a;
	r->hz[1] = b;
	r->mode[0] = r->mode[1] = XB_MODE_DMR;
	r->cc = 1;
	r->ts = 1;
	r->power = 1;
	strcpy(r->call, "TA2DOU");
}

/* The radio on A (the engine goes between A and B while nothing is heard) */
static bool xr_on(uint32_t hz)
{
	for (int i = 0; i < 100 && hw.rx_hz != hz; i++)
		run(25);
	return hw.rx_hz == hz;
}

/* A voice burst whose three AMBE frames hold (burst, frame) in every byte */
static void xr_voice(uint8_t index, uint8_t cc, const uint8_t *lc, uint8_t n)
{
	dmr_burst_t b;
	memset(&b, 0, sizeof b);
	b.kind = DMR_BURST_VOICE;
	b.voice_index = index;
	b.sync = index == 0 ? DMR_SYNC_MS : DMR_SYNC_NONE;
	b.cc = cc;
	b.crc_ok = true;
	b.in_step = true;
	b.rssi_dbm = -80;
	b.len = 27;
	for (int f = 0; f < 3; f++)
		memset(&b.payload[f * 9], (int)(n * 3u + (unsigned)f + 1u), 9);
	if (lc != NULL) {
		b.lc_valid = true;
		memcpy(b.lc, lc, 9);
	}
	sim_rx(&b);
}

/* A group call on the session's colour code: three headers, superframes of voice (bursts numbered from 0), the end */
static unsigned xr_call(uint32_t dst, uint32_t src, int superframes)
{
	uint8_t lc[9];
	mk_lc(lc, true, dst, src);
	for (int h = 0; h < 3; h++) {
		burst_lc(lc, 1, false);
		run(60);
	}
	unsigned n = 0;
	for (int sf = 0; sf < superframes; sf++) {
		for (int i = 0; i < 6; i++) {
			xr_voice((uint8_t)i, 1, i == 5 ? lc : NULL, (uint8_t)n++);
			run(60);
		}
	}
	burst_lc(lc, 1, true);
	return n;
}

/* The user's place as the radio core holds it */
typedef struct {
	bool vfo;
	uint16_t number;
	uint32_t rx_hz, tx_hz;
	uint8_t mode, power;
} xr_place_t;
static xr_place_t xr_place(void)
{
	xr_place_t p = { sm.view.vfo, sm.view.channel_number, sm.view.ch.rx_hz, sm.view.ch.tx_hz, sm.view.ch.mode,
			 sm.view.power };
	return p;
}
static bool xr_same(const xr_place_t *a, const xr_place_t *b)
{
	return a->vfo == b->vfo && a->number == b->number && a->rx_hz == b->rx_hz && a->tx_hz == b->tx_hz &&
	       a->mode == b->mode && a->power == b->power;
}

/* A call on A relayed on B, the CW ID, the stop */
static void test_xband_relay(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	xr_reset();
	run(500);
	CHECK(sm.view.channel_number == 1 && sm.view.ch.mode == RADIO_MODE_FM && hw.rx_hz == 146520000);
	xr_place_t before = xr_place();
	xr_req(145500000, 433500000);
	CHECK(xband_start());
	run(100);
	CHECK(xband_owns_radio() && sm.view.ch.mode == RADIO_MODE_DMR && hw.rx_hz == 145500000 && hw_mode == RADIO_MODE_DMR);
	CHECK(sm.view.ch.color_code == 1 && sm.view.ch.timeslot == 1 && sm.view.dmr.hear_all_latch && xr_lends == 1);
	xband_view_t v;
	xband_view(&v);
	CHECK(v.capacity == 1899 && v.phase == XB_P_LISTEN);	/* 768 + 643 + 488 frames, 37.98 s */

	/* PTT: the deny tone, never keyed; requests that would move the radio: dropped */
	int tx0 = hw.tx_calls;
	req(RREQ_PTT, 1);
	run(100);
	CHECK(xr_beeps[BEEP_TX_DENY] == 1 && hw.tx_calls == tx0 && sm.view.state != RS_TX);
	req(RREQ_PTT, 0);
	req(RREQ_CH_STEP, 1);
	req(RREQ_VFO, 1);
	run(100);
	CHECK(xband_owns_radio() && !sm.view.vfo && sm.view.channel_number == 1);

	/* a call to TG 2862 on A: recorded, then sent on B */
	CHECK(xr_on(145500000));
	unsigned air0 = sim.air_count;
	unsigned bursts = xr_call(2862, 3105555, 4);
	xband_view(&v);
	CHECK(v.phase == XB_P_RECORD && v.frames == 3u * bursts);
	run(200);
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 433500000 && hw_mode == RADIO_MODE_DMR && hw.tx_power == 0);
	for (int i = 0; i < 400 && sm.view.state == RS_TX; i++)
		run(25);
	CHECK(sm.view.state != RS_TX);
	/* on the air: the header to TG 2862 from us, the voice bursts as heard, the terminator */
	unsigned first = air0;
	while (first < sim.air_count && sim.air[first].kind != DMR_BURST_VOICE_LC_HEADER)
		first++;
	CHECK(first < sim.air_count);
	dmr_lc_t l;
	air_lc(first, &l);
	CHECK(l.dst == 2862 && l.src == own_dmr_id && l.flco == DMR_FLCO_GROUP_VOICE);
	unsigned voice = 0, ok = 0;
	for (unsigned k = first; k < sim.air_count; k++) {
		if (sim.air[k].kind != DMR_BURST_VOICE)
			continue;
		if (voice < bursts) {
			bool same = true;
			for (int f = 0; f < 3; f++)
				for (int j = 0; j < 9; j++)
					same = same && sim.air[k].payload[f * 9 + j] == (uint8_t)(voice * 3u + (unsigned)f + 1u);
			ok += same;
		}
		voice++;
	}
	CHECK(ok == bursts && voice >= bursts && voice < bursts + 6);	/* the superframe's rest: silence */
	CHECK(sim.air[sim.air_count - 1].kind == DMR_BURST_TERMINATOR_LC);
	xband_view(&v);
	CHECK(v.relayed == 1);

	/* the CW ID on B: FM, the callsign keyed in Morse, then the session's DMR channel on B again */
	memset(&tone, 0, sizeof tone);
	run(300);
	CHECK(sm.cw_phase != 0 && hw_mode == RADIO_MODE_FM && hw.tx_hz == 433500000 && tone.hz != 0);
	for (int i = 0; i < 400 && sm.cw_phase != 0; i++)
		run(25);
	CHECK(sm.cw_phase == 0 && tone.changes > 20);	/* TA2DOU: 20 elements */
	run(100);
	xband_view(&v);
	CHECK(v.ids == 1 && v.phase == XB_P_LISTEN && hw_mode == RADIO_MODE_DMR && hw.rx_hz == 433500000);
	/* nothing heard: back and forth */
	run(1100);
	CHECK(hw.rx_hz == 145500000);
	run(1100);
	CHECK(hw.rx_hz == 433500000);

	/* the stop: the FM channel exactly as it was, the memory back */
	xband_stop();
	run(100);
	CHECK(!xband_owns_radio() && !xband_on() && xr_gives == 1 && !xr_lent);
	xr_place_t after = xr_place();
	CHECK(xr_same(&before, &after) && hw.rx_hz == 146520000 && hw_mode == RADIO_MODE_FM && !sm.view.dmr.hear_all_latch);
	req(RREQ_CH_STEP, 1);
	run(100);
	CHECK(sm.view.channel_number == 2);	/* requests work again */
	printf("Plus cross-band radio core: %u bursts heard on A, %u voice bursts sent on B, the CW ID, channel 1 back\n",
	       bursts, voice);
}

/* The board won't key on B: the over dropped (nothing on the air), an error beep, no ID; the session listens on */
static void test_xband_refused(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	xr_reset();
	run(200);
	xr_req(145500000, 433500000);
	CHECK(xband_start());
	run(100);
	CHECK(xr_on(145500000));
	tx_fail = true;
	unsigned air0 = sim.air_count;
	xr_call(91, 3105555, 2);
	run(300);
	xband_view_t v;
	xband_view(&v);
	CHECK(sim.air_count == air0 && xr_beeps[BEEP_ERROR] >= 1 && v.relayed == 0 && xband_owns_radio());
	CHECK(sm.view.state != RS_TX && !sm.ptt);
	run(2000);
	xband_view(&v);
	CHECK(sm.cw_phase == 0 && v.ids == 0);
	xband_stop();
	run(100);
	CHECK(!xband_owns_radio() && xr_gives == 1 && hw.rx_hz == 146520000);
}

/* The P25 worker keeps its memory: the session ends (no memory), nothing keyed, the channel back */
static void test_xband_no_memory(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	xr_reset();
	run(200);
	classic_busy = true;
	int tx0 = hw.tx_calls;
	xr_req(145500000, 433500000);
	CHECK(xband_start());
	run(500);
	CHECK(xband_owns_radio() && classic_stop);	/* asked to let go */
	run(2000);
	xband_view_t v;
	xband_view(&v);
	CHECK(!xband_owns_radio() && !xband_on() && v.ended == XB_E_MEMORY && xr_lends == 0 && xr_gives == 0);
	CHECK(hw.tx_calls == tx0 && hw.rx_hz == 146520000 && sm.view.ch.mode == RADIO_MODE_FM);
	classic_busy = false;
}

/* run() with the tone path's interrupt every 4 ms */
static void xr_run_fm(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 20) {
		run(20);
		for (int k = 0; k < 5; k++)
			xa_tick();
	}
}

/* DMR on A relayed as FM on B: decoded by the voice bridge, played through the tone path, then the ID and the stop */
static void test_xband_fm(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	xr_reset();
	aud.feed = true;	/* the audio driver hands the voice bridge a block every 20 ms */
	run(500);
	xr_req(145500000, 433500000);
	xband_req()->mode[1] = XB_MODE_FM;
	CHECK(xband_start());
	run(100);
	xband_view_t v;
	xband_view(&v);
	CHECK(xband_owns_radio() && hw_mode == RADIO_MODE_DMR && hw.rx_hz == 145500000 && v.phase == XB_P_LISTEN);
	CHECK(v.capacity == (6916 - 2560) / 9 + 5788 / 9 + 4400 / 9);	/* less the decoded blocks' 160 ms */
	run(3000);
	CHECK(hw.rx_hz == 145500000);	/* B is never heard (FM): no dwell there */
	ambe_voice_stats_t s0, s1;
	unsigned bursts = xr_call(2862, 3105555, 3);
	ambeVoiceGetStats(&s0);
	run(200);
	/* on B: an FM carrier, no tone, the tone path at the nominal level */
	CHECK(sm.view.state == RS_TX && hw.tx_hz == 433500000 && hw_mode == RADIO_MODE_FM && xa.starts == 1 && xa.level == 50);
	CHECK(sm.view.ch.tx_tone.type == TONE_NONE);
	for (int i = 0; i < 400 && sm.view.state == RS_TX; i++)
		xr_run_fm(20);
	ambeVoiceGetStats(&s1);
	CHECK(sm.view.state != RS_TX && !xa.busy && xa.stops == 1);
	/* every frame handed to the bridge (its decoder ran on them in the audio task), the blocks played out */
	CHECK(s1.rx_in - s0.rx_in == 3u * bursts);
	CHECK(xa.samples >= 3u * bursts * 160u && xa.samples <= (3u * bursts + 40u) * 160u);
	xband_view(&v);
	CHECK(v.relayed == 1);
	/* the CW ID on B, then listening on A again */
	run(300);
	CHECK(sm.cw_phase != 0 && hw.tx_hz == 433500000);
	for (int i = 0; i < 400 && sm.cw_phase != 0; i++)
		run(25);
	run(200);
	xband_view(&v);
	CHECK(v.ids == 1 && v.phase == XB_P_LISTEN && v.side == 0 && hw_mode == RADIO_MODE_DMR && hw.rx_hz == 145500000);
	/* the tone path refused: the over dropped with an error beep */
	xa.refuse = true;
	int tx0 = hw.tx_calls;
	xr_call(91, 3105555, 2);
	run(300);
	xband_view(&v);
	CHECK(v.relayed == 1 && xr_beeps[BEEP_ERROR] >= 1 && sm.view.state != RS_TX && !sm.ptt && hw.tx_calls == tx0 + 1);
	xband_stop();
	run(100);
	CHECK(!xband_owns_radio() && xr_gives == 1 && hw.rx_hz == 146520000 && hw_mode == RADIO_MODE_FM);
	aud.feed = false;
	printf("Plus cross-band DMR -> FM: %u bursts heard on A, %u samples played on B\n", bursts, (unsigned)xa.samples);
}

/* --- P25: the radio task with the native receiver (classic_dispatch's order: the native service, then the core's
 * poll or its maintenance while the native receiver owns the radio) --- */
static void xr_native_tick(void)
{
	now += 25;
	if (now % RADIO_TICK_MS < 25 && !native_lab_owns())
		radio_sm_tick(&sm, now);
	if (classic_busy) {
		uint8_t state, reason, queued;
		if (classic_live->config.mode == NT_TRANSMIT)
			(void)native_lab_live_poll(classic_sid, &state, &reason, &queued);
		else
			(void)native_lab_voice_poll(classic_sid, &state, &reason);
	}
	classic_pump();
	/* the fake worker's transmission takes a frame each 20 ms, and ends once the last is taken */
	if (classic_busy && classic_live->config.mode == NT_TRANSMIT && xr_send) {
		if (xr_taken_n < xr_put_n)
			xr_taken_n++;
		if (xr_taken_n < xr_put_n && now % 100 < 25)
			xr_taken_n++;
		if (xr_eof && xr_taken_n == xr_put_n)
			xr_was_sent = true;
	}
	if (native_lab_owns())
		radio_sm_native_maintenance(&sm, now);
	else
		radio_sm_poll(&sm, now);
}
static void xr_native_run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 25)
		xr_native_tick();
}
/* A request as the radio task hands it on: the native side first */
static void xr_native_req(uint8_t type, int32_t arg)
{
	radio_req_t r = { type, arg, 0 };
	for (int i = 0; i < 100; i++) {
		int h = native_lab_request(&sm, &r);
		if (h == NATIVE_REQUEST_WAIT) {
			xr_native_tick();
			continue;
		}
		if (!h)
			radio_sm_request(&sm, &r, now);
		return;
	}
	CHECK(false);
}

static void test_xband_p25(void)
{
	setup_codeplug();
	codec_present = true;
	start();
	native_lab_init();
	fake_power.switch_on = classic_on;
	fake_power.battery_mv = classic_battery;
	classic_busy = classic_stop = classic_receiving = false;
	xr_reset();
	xr_native_run(500);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && hw.rx_hz == 146520000);
	xr_req(145500000, 433500000);
	xband_req()->mode[0] = xband_req()->mode[1] = XB_MODE_P25;
	CHECK(xband_start());
	xr_native_run(300);
	xband_view_t v;
	xband_view(&v);
	/* the session's P25 channel on A, taken by the native receiver: any NAC, any talkgroup, our ID */
	CHECK(xband_owns_radio() && xr_lends == 1 && xr_lent_p25 && v.phase == XB_P_LISTEN);
	CHECK(v.capacity == (5788 - P25_LAB_XB_RING_BYTES) / 11 + 4400 / 11);
	CHECK(sm.view.ch.mode == RADIO_MODE_P25 && sm.view.ch.p25_nac == 0xF7E && sm.view.ch.p25_id == own_dmr_id);
	CHECK(native_lab_owns() && classic_live && classic_live->state == NT_RX && classic_live->config.frequency_hz == 145500000);
	/* PTT: the deny tone; the native transmitter never sees it */
	int tx0 = hw.tx_calls;
	unsigned starts0 = classic_starts;
	xr_native_req(RREQ_PTT, 1);
	xr_native_run(100);
	xr_native_req(RREQ_PTT, 0);
	xr_native_run(100);
	CHECK(xr_beeps[BEEP_TX_DENY] == 1 && hw.tx_calls == tx0 && classic_live->config.mode != NT_TRANSMIT);
	CHECK(xband_owns_radio() && classic_starts == starts0);
	/* an over heard on A: 300 IMBE frames (an LDU's nine every 180 ms), recorded */
	for (int i = 0; i < 100 && classic_live->config.frequency_hz != 145500000; i++)
		xr_native_tick();
	classic_receiving = true;
	for (unsigned k = 0; k < 300; k++) {
		for (unsigned j = 0; j < P25_LAB_XB_BYTES; j++)
			xr_heard[xr_heard_n][j] = (uint8_t)(k * 5u + j);
		xr_heard_n++;
		if (k % 9 == 8)
			xr_native_run(175);
	}
	xr_native_run(100);
	xband_view(&v);
	CHECK(v.phase == XB_P_RECORD && v.frames == 300 && xr_heard_taken == 300);
	classic_receiving = false;
	/* sent on B: the heard NAC, the talkgroup heard, our ID; the frames in order, the native transmitter on B */
	bool keyed = false;
	for (int i = 0; i < 200 && !keyed; i++) {
		xr_native_tick();
		keyed = classic_busy && classic_live->config.mode == NT_TRANSMIT;
	}
	CHECK(keyed && classic_live->config.frequency_hz == 433500000 && xr_call_nac == 0x293 && xr_call_tg == 655 &&
	      xr_call_id == own_dmr_id && sm.view.state == RS_TX);
	for (int i = 0; i < 800 && (xband_view(&v), v.phase == XB_P_SEND); i++)
		xr_native_tick();
	bool same = xr_put_n == 300;
	for (unsigned k = 0; same && k < 300; k++)
		for (unsigned j = 0; j < P25_LAB_XB_BYTES; j++)
			same = same && xr_sent_fr[k][j] == (uint8_t)(k * 5u + j);
	CHECK(same && xr_was_sent && !sm.ptt);
	xband_view(&v);
	CHECK(v.relayed == 1);
	/* the CW ID on B: the native receiver lets go first, then FM; then the P25 channel again */
	bool cw = false;
	for (int i = 0; i < 400 && !cw; i++) {
		xr_native_tick();
		cw = sm.cw_phase != 0;
	}
	CHECK(cw && !native_lab_owns() && hw.tx_hz == 433500000 && hw_mode == RADIO_MODE_FM);
	for (int i = 0; i < 600 && (xband_view(&v), v.phase != XB_P_LISTEN); i++)
		xr_native_tick();
	xr_native_run(300);
	xband_view(&v);
	CHECK(v.ids == 1 && v.phase == XB_P_LISTEN && sm.view.ch.mode == RADIO_MODE_P25 && native_lab_owns());
	/* the stop: the native receiver lets go, the memory comes back, the user's FM channel */
	xband_stop();
	for (int i = 0; i < 400 && (xband_owns_radio() || xr_lent || native_lab_owns()); i++)
		xr_native_tick();
	xr_native_run(200);
	CHECK(!xband_on() && !xr_lent && xr_gives == 1 && !native_lab_owns() && !classic_busy);
	CHECK(sm.view.ch.mode == RADIO_MODE_FM && hw.rx_hz == 146520000 && hw_mode == RADIO_MODE_FM && !sm.ptt);
	printf("Plus cross-band P25 -> P25: 300 IMBE frames heard on A, %u sent on B to TG %u NAC %03X\n", xr_put_n,
	       (unsigned)xr_call_tg, (unsigned)xr_call_nac);
}

int main(void)
{
	int prior = classic_radio_main();
	int before = failures;
	test_xband_relay();
	test_xband_refused();
	test_xband_no_memory();
	test_xband_fm();
	test_xband_p25();
	printf("Plus cross-band in the radio core: %d failures (upstream's and Classic's suites with it built in: %s)\n",
	       failures - before, prior ? "FAILED" : "passed");
	return prior != 0 || failures != before;
}
