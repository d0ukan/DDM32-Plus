/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus cross-band store and forward, the portable part (core/radio/xband.c; tools/test_xband.py builds it with
 * -DCONFIG_XBAND=1), on a fake radio core:
 *   - the rules: the amateur bands' edges, PMR446, the repeater input / output and satellite segments, APRS, A = B,
 *     the callsign; the pairs of modes (a mode per side): DMR -> DMR, NXDN -> DMR, DMR / NXDN -> FM, P25 -> P25 built,
 *     every other pair refused (FM -> anything, anything -> NXDN, P25 <-> DMR / FM / NXDN), one-way pairs heard on
 *     their source side only;
 *   - an over heard on A recorded frame by frame and sent on B in the same order, to the same talkgroup (or unit), the
 *     transmitter let go once the frames are out; and B to A;
 *   - listening goes between A and B every XB_DWELL_MS, held while bursts are heard;
 *   - the cut: the memory full, or 3 minutes (a longer memory): one beep, the rest of the over heard, not kept, what
 *     was kept sent;
 *   - a key-up (under 0.5 s) and an encrypted call are not relayed; a refused transmission drops the over, beeps, and
 *     owes no ID;
 *   - the CW ID: on each frequency transmitted on, after the first over, then not again within 10 minutes, then again;
 *     a refused ID beeps and the other side's still goes;
 *   - the end: the stop (on the air: let go at once), a critical battery, no memory; the session's frames given back;
 *   - a mode per side: NXDN -> DMR and DMR -> FM listen on the source side only (no dwell to the other), relay frames
 *     intact and come back to the source after; P25 -> P25 keeps 11-byte IMBE frames intact (its capacity counted in
 *     them); a CW ID accepted but never keyed (a P25 channel that would not let go) beeps and does not count. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/xband.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

/* --- The fake radio core --- */
static struct {
	int tunes, keys, unkeys, cws, beeps[2];
	uint8_t tuned, keyed_side, cw_side[4];
	uint32_t key_dst;
	bool key_group, refuse_key, refuse_cw, tx;
	int req_posts;
} f;
static uint32_t now;

void radio_req_xband(bool on)
{
	(void)on;
	f.req_posts++;
}

static void io_tune(uint8_t side)
{
	f.tunes++;
	f.tuned = side;
}
static bool io_key(uint8_t side, uint32_t dst, bool group)
{
	f.keys++;
	f.keyed_side = side;
	f.key_dst = dst;
	f.key_group = group;
	if (f.refuse_key)
		return false;
	f.tx = true;
	return true;
}
static void io_unkey(void)
{
	f.unkeys++;
}
static bool io_cw(uint8_t side)
{
	if (f.cws < 4)
		f.cw_side[f.cws] = side;
	f.cws++;
	if (f.refuse_cw)
		return false;
	f.tx = true;
	return true;
}
static void io_beep(uint8_t what)
{
	if (what < 2)
		f.beeps[what]++;
}
static const xband_io_t io = { io_tune, io_key, io_unkey, io_cw, io_beep };
static xband_in_t in;

static void poll_ms(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 25) {
		now += 25;
		in.tx = f.tx;	/* the radio core's view: on the air while keyed (an over, the ID) */
		xband_poll(&io, &in, now);
	}
}

static uint8_t *mem;
static size_t mem_len;

/* A session on 145.500 / 433.500 in modes ma (A) and mb (B), CC1 TS1, with mem_bytes of memory in three pieces */
static void start_modes(size_t mem_bytes, uint8_t ma, uint8_t mb)
{
	memset(&f, 0, sizeof f);
	memset(&in, 0, sizeof in);
	free(mem);
	mem_len = mem_bytes;
	mem = mem_bytes ? malloc(mem_bytes) : NULL;
	xband_req_t *r = xband_req();
	r->hz[0] = 145500000;
	r->hz[1] = 433500000;
	r->mode[0] = ma;
	r->mode[1] = mb;
	r->cc = 1;
	r->ts = 1;
	r->power = 1;
	strcpy(r->call, "TA2DOU");
	CHECK(xband_start() && xband_on() && f.req_posts == 1, "starts");
	CHECK(xband_begin(now), "the radio core takes it");
	uint8_t *seg[3] = { mem, mem + mem_bytes / 2, mem + mem_bytes / 2 + mem_bytes / 4 };
	size_t len[3] = { mem_bytes / 2, mem_bytes / 4, mem_bytes - mem_bytes / 2 - mem_bytes / 4 };
	xband_memory(seg, len, mem_bytes ? 3 : 0, now);
	poll_ms(25);
	uint8_t first = xband_listens(r) & 1u ? 0 : 1;
	CHECK(mem_bytes == 0 || (f.tuned == first && f.tunes == 1), "listening on the first side heard first");
}

static void start(size_t mem_bytes)
{
	start_modes(mem_bytes, XB_MODE_DMR, XB_MODE_DMR);
}

/* Listening on side k (the dwell goes round to it) */
static void to_side(uint8_t k)
{
	xband_view_t v;
	for (int i = 0; i < 200; i++) {
		xband_view(&v);
		if (v.phase == XB_P_LISTEN && v.side == k)
			return;
		poll_ms(25);
	}
	CHECK(false, "never listening on %u", k);
}

/* An over of n frames heard now (frame k holds k in every byte, wrapped), then its end */
static void over(uint32_t n, uint32_t dst, bool group)
{
	in.rx = true;
	xband_rx_begin(3101234, dst, group, false);
	for (uint32_t k = 0; k < n; k++) {
		uint8_t fr[XB_FRAME_BYTES];
		memset(fr, (int)(k & 0xFF), sizeof fr);
		fr[0] = (uint8_t)(k >> 8);
		xband_rx_frame(fr);
		if (k % 3 == 2)
			poll_ms(50);	/* the stack hands three frames a 60 ms burst */
	}
	xband_rx_end();
	in.rx = false;
}

/* The call stack pulls every frame; then the transmission ends (the modem's last superframe) */
static uint32_t drain(uint8_t *first, uint8_t *last, bool *in_order)
{
	uint32_t n = 0;
	uint8_t fr[XB_FRAME_BYTES];
	*in_order = true;
	while (xband_tx_next(fr)) {
		uint32_t k = (uint32_t)fr[0] << 8 | fr[1];
		if ((k & 0xFFu) != fr[1] || (n & 0xFFFFu) != (k & 0xFFFFu) || fr[2] != (uint8_t)(n & 0xFF))
			*in_order = false;
		if (n == 0 && first)
			memcpy(first, fr, sizeof fr);
		if (last)
			memcpy(last, fr, sizeof fr);
		n++;
		if (n % 3 == 0)
			poll_ms(50);
	}
	poll_ms(100);
	f.tx = in.tx = false;
	return n;
}

static void tx_follow(void)
{
	in.tx = f.tx;
}

/* --- The rules --- */
static void test_rules(void)
{
	static const struct { uint32_t hz; uint8_t why; } t[] = {
		{ 143999999, XB_R_BAND }, { 144000000, XB_R_OK }, { 144500000, XB_R_OK }, { 144800000, XB_R_APRS },
		{ 144806250, XB_R_APRS }, { 144812500, XB_R_OK }, { 145000000, XB_R_REPEATER }, { 145187500, XB_R_REPEATER },
		{ 145212500, XB_R_OK }, { 145500000, XB_R_OK }, { 145587500, XB_R_OK }, { 145600000, XB_R_REPEATER },
		{ 145787500, XB_R_REPEATER }, { 145800000, XB_R_SATELLITE }, { 146000000, XB_R_SATELLITE },
		{ 146012500, XB_R_BAND }, { 429987500, XB_R_BAND }, { 430000000, XB_R_OK }, { 431050000, XB_R_REPEATER },
		{ 431825000, XB_R_REPEATER }, { 432500000, XB_R_OK }, { 433500000, XB_R_OK }, { 435000000, XB_R_SATELLITE },
		{ 437500000, XB_R_SATELLITE }, { 438500000, XB_R_OK }, { 438650000, XB_R_REPEATER },
		{ 439425000, XB_R_REPEATER }, { 439500000, XB_R_OK }, { 440000000, XB_R_OK }, { 440012500, XB_R_BAND },
		{ 446000000, XB_R_PMR446 }, { 446006250, XB_R_PMR446 }, { 446193750, XB_R_PMR446 }, { 446200000, XB_R_PMR446 },
		{ 446212500, XB_R_BAND }, { 162550000, XB_R_BAND }, { 0, XB_R_BAND },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
		CHECK(xband_check_hz(t[i].hz) == t[i].why, "%u Hz: %u, not %u", (unsigned)t[i].hz,
		      (unsigned)xband_check_hz(t[i].hz), (unsigned)t[i].why);
	xband_req_t r;
	memset(&r, 0, sizeof r);
	r.hz[0] = 145500000;
	r.hz[1] = 433500000;
	r.mode = XB_MODE_DMR;
	strcpy(r.call, "TA2DOU");
	CHECK(xband_check(&r) == XB_R_OK, "VHF / UHF simplex");
	r.hz[1] = 145500000;
	CHECK(xband_check(&r) == XB_R_SAME, "A = B");
	r.hz[1] = 446006250;
	CHECK(xband_check(&r) == XB_R_PMR446, "PMR446");
	r.hz[1] = 439000000;
	CHECK(xband_check(&r) == XB_R_REPEATER, "a repeater output");
	r.hz[1] = 433500000;
	/* the pairs: (A, B) -> the sides heard (bit 0 A, relayed to B; bit 1 B, relayed to A), 0 refused */
	static const struct { uint8_t a, b, listens; } pairs[] = {
		{ XB_MODE_DMR, XB_MODE_DMR, 3 }, { XB_MODE_P25, XB_MODE_P25, 3 },
		{ XB_MODE_NXDN, XB_MODE_DMR, 1 }, { XB_MODE_DMR, XB_MODE_NXDN, 2 },
		{ XB_MODE_DMR, XB_MODE_FM, 1 }, { XB_MODE_FM, XB_MODE_DMR, 2 },
		{ XB_MODE_NXDN, XB_MODE_FM, 1 }, { XB_MODE_FM, XB_MODE_NXDN, 2 },
		{ XB_MODE_FM, XB_MODE_FM, 0 }, { XB_MODE_NXDN, XB_MODE_NXDN, 0 },	/* no FM receive, no NXDN transmitter */
		{ XB_MODE_P25, XB_MODE_DMR, 0 }, { XB_MODE_DMR, XB_MODE_P25, 0 },	/* no transcoding */
		{ XB_MODE_P25, XB_MODE_FM, 0 }, { XB_MODE_FM, XB_MODE_P25, 0 },
		{ XB_MODE_P25, XB_MODE_NXDN, 0 }, { XB_MODE_NXDN, XB_MODE_P25, 0 },
		{ XB_MODES, XB_MODE_DMR, 0 }, { XB_MODE_DMR, 7, 0 },			/* (YSF is no mode here) */
	};
	for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
		r.mode[0] = pairs[i].a;
		r.mode[1] = pairs[i].b;
		uint8_t want = pairs[i].listens ? XB_R_OK : XB_R_MODE;
		CHECK(xband_check(&r) == want && (pairs[i].a >= XB_MODES || pairs[i].b >= XB_MODES ||
						  xband_listens(&r) == pairs[i].listens),
		      "pair %u -> %u: %u, listens %u", pairs[i].a, pairs[i].b, (unsigned)xband_check(&r),
		      (unsigned)xband_listens(&r));
	}
	r.mode[0] = r.mode[1] = XB_MODE_P25;
	CHECK(xband_frame_bytes(&r) == XB_P25_FRAME_BYTES, "P25 frames: 11 bytes");
	r.mode[0] = XB_MODE_NXDN;
	r.mode[1] = XB_MODE_FM;
	CHECK(xband_frame_bytes(&r) == XB_FRAME_BYTES, "AMBE+2 frames: 9 bytes");
	/* every refusal of a frequency holds whatever the modes */
	r.hz[1] = 446006250;
	CHECK(xband_check(&r) == XB_R_PMR446, "PMR446 with NXDN -> FM");
	r.hz[1] = 439000000;
	CHECK(xband_check(&r) == XB_R_REPEATER, "a repeater output with NXDN -> FM");
	r.hz[1] = 433500000;
	r.mode[0] = r.mode[1] = XB_MODE_DMR;
	strcpy(r.call, "TA2");
	CHECK(xband_check(&r) == XB_R_CALL, "no callsign");
	r.call[0] = '\0';
	CHECK(xband_check(&r) == XB_R_CALL, "no callsign at all");
	strcpy(xband_req()->call, "");
	xband_req()->hz[0] = 145500000;
	xband_req()->hz[1] = 433500000;
	xband_req()->mode[0] = xband_req()->mode[1] = XB_MODE_DMR;
	CHECK(!xband_start() && !xband_on(), "a refused request does not start");
	strcpy(xband_req()->call, "TA2DOU");
	xband_req()->mode[0] = XB_MODE_P25;
	CHECK(!xband_start() && !xband_on() && f.req_posts == 0, "P25 -> DMR does not start");
	xband_req()->mode[0] = XB_MODE_FM;
	xband_req()->mode[1] = XB_MODE_FM;
	CHECK(!xband_start() && !xband_on() && f.req_posts == 0, "FM -> FM does not start");
	CHECK(xband_power_level(1, 3) == 0 && xband_power_level(2, 3) == 1 && xband_power_level(3, 3) == 2 &&
	      xband_power_level(3, 0) == 0, "power classes");
	CHECK(xband_frames_for(17104) == 1900 && xband_frames_for(8) == 0, "frames in the P25 worker's memory");
}

/* --- Relaying --- */
static void test_relay(void)
{
	start(17104);	/* the P25 worker's voice + fifo + rp_buf */
	xband_view_t v;
	xband_view(&v);
	CHECK(v.capacity == 1900 && v.phase == XB_P_LISTEN, "1900 frames (38 s)");
	over(500, 2862, true);
	xband_view(&v);
	CHECK(v.phase == XB_P_RECORD && v.frames == 500, "recorded 500");
	poll_ms(25);
	CHECK(f.tuned == 1 && f.keys == 1 && f.keyed_side == 1 && f.key_dst == 2862 && f.key_group, "sent on B to TG 2862");
	xband_view(&v);
	CHECK(v.phase == XB_P_SEND && v.side == 1 && v.frames == 500, "sending 500 on B");
	tx_follow();
	bool order;
	uint32_t n = drain(NULL, NULL, &order);
	CHECK(n == 500 && order, "the over out whole and in order: %u", (unsigned)n);
	CHECK(f.unkeys == 1, "let go once the frames are out");
	poll_ms(50);
	/* the first over: the CW ID on B (only B transmitted) */
	CHECK(f.cws == 1 && f.cw_side[0] == 1, "the first ID on B");
	xband_view(&v);
	CHECK(v.phase == XB_P_ID && v.relayed == 1, "identifying");
	tx_follow();
	poll_ms(3000);
	f.tx = in.tx = false;
	poll_ms(50);
	xband_view(&v);
	CHECK(v.phase == XB_P_LISTEN && v.side == 1 && v.ids == 1 && f.tuned == 1, "then listening on B (the reply)");
	/* B to A, a private call: no ID within the 10 minutes, the reply goes to the unit */
	over(100, 3105555, false);
	poll_ms(25);
	CHECK(f.keys == 2 && f.keyed_side == 0 && f.key_dst == 3105555 && !f.key_group, "B to A, private");
	tx_follow();
	n = drain(NULL, NULL, &order);
	poll_ms(100);
	CHECK(n == 100 && order && f.cws == 1, "no ID yet: %d", f.cws);
	xband_view(&v);
	CHECK(v.phase == XB_P_LISTEN && v.side == 0 && v.relayed == 2, "listening on A");
	/* 10 minutes on: the next over brings the IDs owed (A from the last over, and the new one's) */
	poll_ms(XB_ID_EVERY_MS);
	/* A owes one since the last ID (the B to A over): due now, keyed while idle */
	CHECK(f.cws == 2 && f.cw_side[1] == 0, "the 10 minutes: A's ID");
	tx_follow();
	poll_ms(2000);
	f.tx = in.tx = false;
	poll_ms(100);
	xband_view(&v);
	CHECK(v.ids == 2 && v.phase == XB_P_LISTEN, "two IDs");
	xband_stop();
	poll_ms(25);
	CHECK(!xband_on() && !xband_owns_radio(), "stopped");
	xband_view(&v);
	CHECK(v.phase == XB_P_OFF && v.ended == XB_E_STOPPED, "ended: stopped");
	xband_release();
}

static void test_dwell(void)
{
	start(17104);
	poll_ms(XB_DWELL_MS - 50);
	CHECK(f.tuned == 0, "A for a dwell");
	poll_ms(100);
	CHECK(f.tuned == 1 && f.tunes == 2, "then B");
	in.activity = true;	/* bursts heard on B: held */
	poll_ms(5 * XB_DWELL_MS);
	CHECK(f.tuned == 1 && f.tunes == 2, "held while bursts are heard");
	in.activity = false;
	poll_ms(XB_DWELL_MS + 50);
	CHECK(f.tuned == 0 && f.tunes == 3, "back to A after the bursts end");
	/* a key-up: not relayed */
	over(XB_MIN_FRAMES - 1, 9, true);
	poll_ms(50);
	CHECK(f.keys == 0, "a key-up is not relayed");
	/* encrypted: not recorded */
	in.rx = true;
	xband_rx_begin(1, 9, true, true);
	for (int k = 0; k < 100; k++)
		xband_rx_frame((const uint8_t[XB_FRAME_BYTES]){ 1 });
	xband_rx_end();
	in.rx = false;
	poll_ms(XB_REC_QUIET_MS + 100);
	CHECK(f.keys == 0, "encrypted: nothing relayed");
	/* a recording whose end never comes: quiet for XB_REC_QUIET_MS ends it */
	in.rx = true;
	xband_rx_begin(1, 9, true, false);
	for (int k = 0; k < 60; k++)
		xband_rx_frame((const uint8_t[XB_FRAME_BYTES]){ 2 });
	in.rx = false;
	poll_ms(XB_REC_QUIET_MS - 100);
	CHECK(f.keys == 0, "still waiting");
	poll_ms(200);
	CHECK(f.keys == 1, "quiet: relayed");
	xband_stop();
	poll_ms(25);
	CHECK(f.unkeys == 1, "stopped on the air: let go at once");
	xband_release();
}

static void test_cut(void)
{
	/* the memory full: 1900 frames kept of 2500, one beep */
	start(17104);
	over(2500, 91, true);
	poll_ms(25);
	CHECK(f.beeps[XB_BEEP_CUT] == 1, "one beep for the cut: %d", f.beeps[XB_BEEP_CUT]);
	tx_follow();
	bool order;
	uint32_t n = drain(NULL, NULL, &order);
	CHECK(n == 1900 && order, "the memory's 1900 frames sent: %u", (unsigned)n);
	xband_stop();
	poll_ms(25);
	xband_release();
	/* 3 minutes with more memory: 9000 frames, then cut */
	start(9500u * XB_FRAME_BYTES);
	xband_view_t v;
	xband_view(&v);
	CHECK(v.capacity == 9500, "a larger memory");
	over(9600, 91, true);
	xband_view(&v);
	CHECK(v.cut && v.frames == XB_OVER_MAX_MS / XB_FRAME_MS, "cut at 3 minutes: %u", (unsigned)v.frames);
	poll_ms(25);
	CHECK(f.beeps[XB_BEEP_CUT] == 1, "one beep");
	tx_follow();
	n = drain(NULL, NULL, &order);
	CHECK(n == 9000, "3 minutes sent: %u", (unsigned)n);
	xband_stop();
	poll_ms(25);
	xband_release();
}

static void test_refusals(void)
{
	/* the transmission refused: the over dropped, an error beep, no ID owed, back on the side it came from */
	start(17104);
	f.refuse_key = true;
	over(200, 91, true);
	poll_ms(25);
	xband_view_t v;
	xband_view(&v);
	CHECK(f.keys == 1 && f.beeps[XB_BEEP_ERROR] == 1 && v.phase == XB_P_LISTEN && v.side == 0 && f.tuned == 0,
	      "refused: listening on A again");
	poll_ms(XB_ID_EVERY_MS + 1000);
	CHECK(f.cws == 0, "nothing sent: no ID");
	/* the radio cuts it on the air (its time-out): what went counts for the ID, not as relayed */
	f.refuse_key = false;
	to_side(0);
	over(400, 91, true);
	poll_ms(25);
	tx_follow();
	uint8_t fr[XB_FRAME_BYTES];
	for (int k = 0; k < 100; k++)
		xband_tx_next(fr);
	poll_ms(100);
	f.tx = in.tx = false;
	poll_ms(150);
	xband_view(&v);
	CHECK(v.relayed == 0 && f.beeps[XB_BEEP_ERROR] == 2 && f.unkeys == 1, "cut short: an error beep");
	CHECK(f.cws == 1 && f.cw_side[0] == 1, "B was on the air: its ID");
	/* the ID refused there: a beep; the session listens on */
	tx_follow();
	poll_ms(1000);
	f.tx = in.tx = false;
	poll_ms(50);
	xband_stop();
	poll_ms(25);
	xband_release();
	/* a refused ID (the radio would not key there): a beep, and the session listens on */
	start(17104);
	over(100, 91, true);	/* A to B */
	poll_ms(25);
	tx_follow();
	bool order;
	drain(NULL, NULL, &order);
	f.refuse_cw = true;
	poll_ms(50);
	CHECK(f.cws == 1 && f.beeps[XB_BEEP_ERROR] == 1, "B's ID refused: a beep");
	xband_view(&v);
	CHECK(v.phase == XB_P_LISTEN && v.ids == 0, "listening (the next is owed in 10 minutes)");
	xband_stop();
	poll_ms(25);
	xband_release();
	/* a critical battery ends it */
	start(17104);
	in.battery_critical = true;
	poll_ms(25);
	xband_view(&v);
	CHECK(!xband_owns_radio() && v.ended == XB_E_BATTERY, "the battery");
	xband_release();
	/* no memory: it never relays */
	start(0);
	poll_ms(25);
	xband_view(&v);
	CHECK(!xband_owns_radio() && v.ended == XB_E_MEMORY && v.capacity == 0, "no memory");
	xband_release();
}

/* NXDN -> DMR (A -> B), and DMR -> FM: heard on the source side only, frames intact, back to the source after */
static void test_one_way(void)
{
	static const uint8_t to[2] = { XB_MODE_DMR, XB_MODE_FM };
	for (int k = 0; k < 2; k++) {
		start_modes(17104, k ? XB_MODE_DMR : XB_MODE_NXDN, to[k]);
		poll_ms(5 * XB_DWELL_MS);
		CHECK(f.tuned == 0 && f.tunes == 1, "%d: no dwell to the side that is never heard", k);
		over(300, 2862, true);
		poll_ms(25);
		CHECK(f.keys == 1 && f.keyed_side == 1 && f.key_dst == 2862 && f.key_group, "%d: sent on B", k);
		tx_follow();
		bool order;
		uint32_t n = drain(NULL, NULL, &order);
		CHECK(n == 300 && order && f.unkeys == 1, "%d: the over whole and in order: %u", k, (unsigned)n);
		poll_ms(50);
		/* B's ID, then listening: on A (B is never heard) */
		CHECK(f.cws == 1 && f.cw_side[0] == 1, "%d: B's ID", k);
		tx_follow();
		poll_ms(2000);
		f.tx = in.tx = false;
		poll_ms(50);
		xband_view_t v;
		xband_view(&v);
		CHECK(v.phase == XB_P_LISTEN && v.side == 0 && f.tuned == 0 && v.ids == 1 && v.relayed == 1,
		      "%d: back on A", k);
		/* the same pair the other way round (B -> A): heard on B only */
		xband_stop();
		poll_ms(25);
		xband_release();
		start_modes(17104, to[k], k ? XB_MODE_DMR : XB_MODE_NXDN);
		xband_view(&v);
		CHECK(f.tuned == 1 && v.side == 1, "%d: the reverse pair listens on B", k);
		poll_ms(3 * XB_DWELL_MS);
		CHECK(f.tuned == 1 && f.tunes == 1, "%d: and stays there", k);
		xband_stop();
		poll_ms(25);
		xband_release();
	}
}

/* P25 -> P25: 11-byte IMBE frames, kept and sent intact, the capacity counted in them */
static void test_p25(void)
{
	start_modes(10188 - 180, XB_MODE_P25, XB_MODE_P25);	/* the FIFO past its ring, the call replay's frames */
	xband_view_t v;
	xband_view(&v);
	CHECK(v.capacity == 5004 / 11 + 2502 / 11 + 2502 / 11, "P25 capacity in 11-byte frames: %u", v.capacity);
	CHECK(v.capacity >= 900 && v.capacity <= 911, "about 18 s: %u", v.capacity);
	in.rx = true;
	xband_rx_begin(3101234, 655, true, false);
	for (uint32_t k = 0; k < 450; k++) {
		uint8_t fr[XB_P25_FRAME_BYTES];
		for (unsigned j = 0; j < sizeof fr; j++)
			fr[j] = (uint8_t)(k * 7u + j);
		xband_rx_frame(fr);
		if (k % 9 == 8)
			poll_ms(175);	/* a P25 LDU: nine frames each 180 ms */
	}
	in.rx = false;
	poll_ms(XB_REC_QUIET_MS + 50);
	CHECK(f.keys == 1 && f.keyed_side == 1 && f.key_dst == 655 && f.key_group, "sent on B to TG 655");
	tx_follow();
	uint32_t n = 0;
	bool same = true;
	uint8_t fr[XB_FRAME_MAX];
	while (xband_tx_next(fr)) {
		for (unsigned j = 0; j < XB_P25_FRAME_BYTES; j++)
			same = same && fr[j] == (uint8_t)(n * 7u + j);
		n++;
	}
	CHECK(n == 450 && same, "450 IMBE frames out intact: %u", (unsigned)n);
	poll_ms(100);
	f.tx = in.tx = false;
	poll_ms(100);
	xband_view(&v);
	CHECK(v.relayed == 1 && f.unkeys == 1, "relayed");
	xband_stop();
	poll_ms(25);
	xband_release();
	CHECK(xband_frames_for(18) == 2, "AMBE+2 frames again after the P25 session");
}

/* A CW ID accepted but never keyed (in.id_wait, then nothing): an error beep, not counted; one keyed counts */
static void test_id_wait(void)
{
	start(17104);
	over(100, 91, true);
	poll_ms(25);
	tx_follow();
	bool order;
	drain(NULL, NULL, &order);
	f.tx = in.tx = false;
	in.id_wait = true;	/* io_cw said yes, but the channel is not let go yet */
	poll_ms(50);
	CHECK(f.cws == 1, "B's ID asked for");
	f.tx = in.tx = false;
	poll_ms(1000);
	xband_view_t v;
	xband_view(&v);
	CHECK(v.phase == XB_P_ID && v.ids == 0, "waiting for it");
	in.id_wait = false;	/* given up: never keyed */
	poll_ms(50);
	xband_view(&v);
	CHECK(v.ids == 0 && f.beeps[XB_BEEP_ERROR] == 1 && v.phase == XB_P_LISTEN, "not counted, a beep");
	xband_stop();
	poll_ms(25);
	xband_release();
}

int main(void)
{
	test_rules();
	test_relay();
	test_dwell();
	test_cut();
	test_refusals();
	test_one_way();
	test_p25();
	test_id_wait();
	free(mem);
	printf("cross-band engine: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
