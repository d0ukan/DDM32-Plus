/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 receive on the FM path (radio_nxdn.h, docs/nxdn.md). Radio task only, except radio_nxdn_caller.
 *
 * Structure after DM32-nxdn-p25 integration/radiofoundry/nxdn/radio_nxdn.c (GPL-3.0-or-later): the receive audio
 * read every poll into the software receiver, voice fields paced one per 20 ms into ambeVoice's sink (it primes three,
 * then plays at the audio clock; NXDN's fields come four at a time every 80 ms and the radio task polls about every
 * 25 ms). Their transmit half and bench capture are left out.
 *
 * RAM: the receiver, the pacing queue and the read buffer (about 830 bytes) come from the RTOS heap while an NXDN
 * channel listens (RADIO_NXDN_HEAP=1, the default; APRS's FM ring is allocated the same way and never runs at the same
 * time, since APRS listens only on FM channels), so static RAM grows by about 60 bytes. RADIO_NXDN_HEAP=0 makes them
 * static instead.
 *
 * dPMR (CONFIG_DPMR, core/radio/radio_dpmr.h, docs/dpmr.md) runs through the same code: its receiver shares the
 * NXDN receiver's storage (a union in nxdn_state_t) and everything else here. Only one listens at a time, by
 * construction: `kind` says which one `st` holds, and a poll for the other kind (the selected channel's mode changed)
 * stops it before starting the new one; both polls run in the radio task only.
 *
 * HARDWARE-UNVERIFIED (docs/nxdn.md "What the radio must confirm"). */
#include "config.h"
#if CONFIG_NXDN
#if !CONFIG_APRS_RX
#error "CONFIG_NXDN needs CONFIG_APRS_RX (the FM receive audio ring)"
#endif
#include <string.h>
#include "radio/radio_nxdn.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"
#include "dmr/ambe/ambeVoice.h"
#include "nxdn/nxdn_rx.h"
#if CONFIG_XBAND
#include "radio/xband.h"
#endif
#if CONFIG_DPMR
#include "radio/radio_dpmr.h"
#include "dpmr/dpmr_rx.h"
#endif

#ifndef RADIO_NXDN_HEAP
#define RADIO_NXDN_HEAP 1
#endif

#define VQ		12u		/* voice fields queued for pacing (240 ms) */
#define READ_N		64u		/* samples per rx_read */
#define HEARD_MS	300u
#define RETRY_MS	500u		/* a start that failed (the ring busy, no heap) is tried again after this */
#define FLAT_KEEP_MS	1000u		/* the flat path is checked again this often (a retune resets the band-pass) */

typedef struct {
	union {
		nxdn_receiver_t rx;	/* an NXDN channel listens (kind KIND_NXDN) */
#if CONFIG_DPMR
		dpmr_receiver_t dp;	/* or a dPMR one (KIND_DPMR): never both */
#endif
	};
	uint8_t vq[VQ][NXDN_SLOT_BYTES];
	uint8_t vq_head, vq_tail;
	int16_t buf[READ_N];
} nxdn_state_t;

#if RADIO_NXDN_HEAP
static nxdn_state_t *st;
#else
static nxdn_state_t st_static;
static nxdn_state_t *st;
#endif
#if CONFIG_DPMR
_Static_assert(DPMR_SLOT_BYTES == NXDN_SLOT_BYTES, "one pacing queue for both");
_Static_assert(sizeof(dpmr_receiver_t) <= sizeof(nxdn_receiver_t), "dPMR fits NXDN's receiver storage");
#endif
enum { KIND_NXDN, KIND_DPMR };
static uint8_t kind;		/* which receiver st holds while listening */
static bool listening, v_open, voice_used;
static uint32_t tuned_hz, v_next_ms, retry_ms, flat_ms, last_frames, last_frame_ms;
static uint8_t tuned_ran;
static radio_nxdn_caller_t caller;
static volatile uint32_t caller_seq;
#if CONFIG_DPMR
static radio_dpmr_caller_t dcaller;
static volatile uint32_t dcaller_seq;
#endif

static bool rx_playing(void)
{
#if CONFIG_DPMR
	if (kind == KIND_DPMR)
		return st->dp.rx.playing;
#endif
	return st->rx.rx.playing;
}

static uint32_t rx_frames(void)
{
#if CONFIG_DPMR
	if (kind == KIND_DPMR)
		return st->dp.rx.frames;
#endif
	return st->rx.rx.frames;
}

/* --- receiver callbacks (inside nxdn_receiver_audio, radio task) --- */
static void slot_in(void *ctx, const uint8_t slot[NXDN_SLOT_BYTES])
{
	(void)ctx;
	if ((uint8_t)(st->vq_head - st->vq_tail) >= VQ)
		return;		/* the radio task fell far behind: drop (the sink conceals) */
	memcpy(st->vq[st->vq_head % VQ], slot, NXDN_SLOT_BYTES);
	st->vq_head++;
}

static void on_event(void *ctx, unsigned ev, const nxdn_call_msg_t *m)
{
	(void)ctx;
	radio_nxdn_caller_t c;
	memset(&c, 0, sizeof c);
	if (ev == NXDN_EV_CALL && m != NULL) {
		c.active = true;
		c.group = m->call_type != NXDN_CALL_INDIVIDUAL;
		c.encrypted = m->cipher_type != 0;
		c.emergency = (m->cc_option & 0x80u) != 0;
		c.source = m->source;
		c.destination = m->destination;
		c.playing = st->rx.rx.admitted;
	}
	c.ran = st->rx.rx.last_ran;
	taskENTER_CRITICAL();
	caller = c;
	caller_seq++;
	taskEXIT_CRITICAL();
}

#if CONFIG_DPMR
static void dpmr_event(void *ctx, unsigned ev, const dpmr_call_t *m)
{
	(void)ctx;
	radio_dpmr_caller_t c;
	memset(&c, 0, sizeof c);
	c.cc = DPMR_CC_NONE;
	if (ev == DPMR_EV_CALL && m != NULL) {
		c.active = true;
		c.all_call = dpmr_all_call(m->format);
		c.data = !dpmr_mode_voice(m->mode);
		c.emergency = dpmr_emergency(m->flags);
		c.have_source = m->have_own;
		c.have_destination = m->have_called;
		c.source = m->own;
		c.destination = m->called;
		c.cc = m->cc;
		c.playing = st->dp.rx.admitted;
	}
	taskENTER_CRITICAL();
	dcaller = c;
	dcaller_seq++;
	taskEXIT_CRITICAL();
}
#endif

/* --- voice to the DMR vocoder, one field per 20 ms --- */
/* (DDM32 Plus cross-band, radio/xband.h: an NXDN side's call is recorded as it plays, each 72-bit voice field being a
 * DMR air frame; the engine takes them only while a session listens here, and never an encrypted call's) */
static void voice_out(uint32_t now_ms)
{
	const dmr_voice_sink_t *sink = ambeVoiceSink();
	uint8_t queued = (uint8_t)(st->vq_head - st->vq_tail);
	if (!v_open) {
		if (queued < 4u)
			return;		/* a frame's worth in hand: the sink primes three on top */
		v_open = voice_used = true;
		v_next_ms = now_ms;
		if (sink->begin)
			sink->begin(NULL, sink->ctx);
#if CONFIG_XBAND
		if (kind == KIND_NXDN)
			xband_rx_begin(caller.source, caller.destination, caller.group, caller.encrypted);
#endif
	}
	while ((int32_t)(now_ms - v_next_ms) >= 0 && st->vq_tail != st->vq_head) {
		sink->frame(st->vq[st->vq_tail % VQ], sink->ctx);
#if CONFIG_XBAND
		if (kind == KIND_NXDN)
			xband_rx_frame(st->vq[st->vq_tail % VQ]);
#endif
		st->vq_tail++;
		v_next_ms += 20u;
	}
	if (st->vq_tail == st->vq_head && !rx_playing() && (int32_t)(now_ms - v_next_ms) >= 0) {
		v_open = false;
		if (sink->end)
			sink->end(sink->ctx);
#if CONFIG_XBAND
		if (kind == KIND_NXDN)
			xband_rx_end();
#endif
	}
}

static void stop(void)
{
	if (!listening)
		return;
	if (board.afsk != NULL && board.afsk->rx_stop != NULL)
		board.afsk->rx_stop();
	board_nxdn_flat(false);
	if (v_open) {
		const dmr_voice_sink_t *sink = ambeVoiceSink();
		if (sink->end)
			sink->end(sink->ctx);
		v_open = false;
	}
	if (voice_used) {
		ambeVoiceStop();	/* the audio stream stops (as when leaving DMR) */
		voice_used = false;
	}
	listening = false;
	taskENTER_CRITICAL();
	memset(&caller, 0, sizeof caller);
	caller_seq++;
#if CONFIG_DPMR
	memset(&dcaller, 0, sizeof dcaller);
	dcaller.cc = DPMR_CC_NONE;
	dcaller_seq++;
#endif
	taskEXIT_CRITICAL();
#if RADIO_NXDN_HEAP
	vPortFree(st);
#endif
	st = NULL;
}

/* sel: NXDN's RAN filter, or dPMR's colour code filter */
static bool start(uint8_t k, uint32_t rx_hz, uint8_t sel, uint32_t now_ms)
{
	const hal_afsk_t *a = board.afsk;
	if (a == NULL || a->rx_start == NULL || a->rx_read == NULL || a->rx_rate_hz != NXDN_FS)
		return false;
#if RADIO_NXDN_HEAP
	st = pvPortMalloc(sizeof *st);
	if (st == NULL)
		return false;
#else
	st = &st_static;
#endif
	memset(st, 0, sizeof *st);
	kind = k;
#if CONFIG_DPMR
	if (k == KIND_DPMR) {
		/* every call on the colour code plays (an RX-only listener; docs/dpmr.md) */
		dpmr_rx_cfg_t dcfg = { sel, slot_in, dpmr_event, NULL };
		dpmr_receiver_init(&st->dp, &dcfg, 0, 0);
	} else
#endif
	{
		/* group 0: every call on the RAN plays (an RX-only listener; docs/nxdn.md) */
		nxdn_rx_cfg_t cfg = { sel, 0, 0, slot_in, on_event, NULL };
		nxdn_receiver_init(&st->rx, &cfg, 0, 0);	/* flat path: no de-emphasis / high-pass to undo (unmeasured) */
	}
	if (!board_nxdn_flat(true) || !a->rx_start()) {
		board_nxdn_flat(false);
#if RADIO_NXDN_HEAP
		vPortFree(st);
#endif
		st = NULL;
		return false;
	}
	listening = true;
	tuned_hz = rx_hz;
	tuned_ran = sel;
	flat_ms = now_ms;
	last_frames = 0;
	last_frame_ms = 0;
	return true;
}

static void poll(uint8_t k, bool want, uint32_t rx_hz, uint8_t sel, uint32_t now_ms)
{
	if (!want) {
		if (kind == k)
			stop();		/* (the other kind's poll leaves its receiver alone) */
		return;
	}
	if (listening && (k != kind || rx_hz != tuned_hz || sel != tuned_ran)) {
		stop();		/* another channel or mode: start clean (new filter, flat path after the retune) */
	}
	if (!listening) {
		if (retry_ms != 0 && (int32_t)(now_ms - retry_ms) < 0)
			return;
		retry_ms = 0;
		if (!start(k, rx_hz, sel, now_ms)) {
			retry_ms = now_ms + RETRY_MS;
			return;
		}
	}
	if (now_ms - flat_ms >= FLAT_KEEP_MS) {
		flat_ms = now_ms;
		board_nxdn_flat(true);
	}
	for (unsigned guard = 0; guard < 32u; guard++) {
		uint16_t n = board.afsk->rx_read(st->buf, READ_N);
		if (n == 0)
			break;
#if CONFIG_DPMR
		if (kind == KIND_DPMR)
			dpmr_receiver_audio(&st->dp, st->buf, n);
		else
#endif
			nxdn_receiver_audio(&st->rx, st->buf, n);
	}
	if (rx_frames() != last_frames) {
		last_frames = rx_frames();
		last_frame_ms = now_ms != 0 ? now_ms : 1u;
	}
	voice_out(now_ms);
}

void radio_nxdn_poll(bool want, uint32_t rx_hz, uint8_t ran, uint32_t now_ms)
{
	poll(KIND_NXDN, want, rx_hz, ran, now_ms);
}

bool radio_nxdn_heard(uint32_t now_ms)
{
	return listening && kind == KIND_NXDN && last_frame_ms != 0 && now_ms - last_frame_ms < HEARD_MS;
}

uint32_t radio_nxdn_caller(radio_nxdn_caller_t *out)
{
	taskENTER_CRITICAL();
	uint32_t seq = caller_seq;
	if (out != NULL)
		*out = caller;
	taskEXIT_CRITICAL();
	return seq;
}

#if CONFIG_DPMR
void radio_dpmr_poll(bool want, uint32_t rx_hz, uint8_t cc, uint32_t now_ms)
{
	poll(KIND_DPMR, want, rx_hz, cc, now_ms);
}

bool radio_dpmr_heard(uint32_t now_ms)
{
	return listening && kind == KIND_DPMR && last_frame_ms != 0 && now_ms - last_frame_ms < HEARD_MS;
}

uint32_t radio_dpmr_caller(radio_dpmr_caller_t *out)
{
	taskENTER_CRITICAL();
	uint32_t seq = dcaller_seq;
	if (out != NULL)
		*out = dcaller;
	taskEXIT_CRITICAL();
	return seq;
}

/* Appends to out at *n (kept NUL terminated) */
static void line_str(char *out, size_t cap, size_t *n, const char *s)
{
	while (*s && *n + 1u < cap)
		out[(*n)++] = *s++;
	out[*n] = '\0';
}

static void line_id(char *out, size_t cap, size_t *n, uint32_t id)
{
	if (*n + 1u < cap)
		*n += dpmr_id_text(id, out + *n, cap - *n);
}

/* Called from the UI task: no buffer of its own (the UI task's stack is tight), the caller copied field by field */
size_t radio_dpmr_caller_line(char *out, size_t cap)
{
	if (out == NULL || cap == 0)
		return 0;
	out[0] = '\0';
	taskENTER_CRITICAL();
	bool active = dcaller.active, data = dcaller.data, all = dcaller.all_call;
	bool hs = dcaller.have_source, hd = dcaller.have_destination;
	uint32_t src = dcaller.source, dst = dcaller.destination;
	uint8_t cc = dcaller.cc;
	taskEXIT_CRITICAL();
	if (!active)
		return 0;
	size_t n = 0;
	line_str(out, cap, &n, "\x1d");
	for (unsigned alt = 0; alt < 3u; alt++) {
		bool tight = alt > 0u;
		if (alt)
			line_str(out, cap, &n, "\x1f");
		if (!tight || (!hs && !hd))
			line_str(out, cap, &n, data ? "dPMR data " : "dPMR ");
		if (!hs && !hd)
			line_str(out, cap, &n, "call");
		if (hs)
			line_id(out, cap, &n, src);
		if (hd) {
			line_str(out, cap, &n, tight ? ">" : hs ? " > " : "> ");
			if (all)
				line_str(out, cap, &n, "ALL");
			else
				line_id(out, cap, &n, dst);
		}
		if (cc < DPMR_CC_COUNT && alt < 2u) {
			char num[3] = { (char)('0' + cc / 10u), (char)('0' + cc % 10u), 0 };
			line_str(out, cap, &n, tight ? " CC" : " CC ");
			line_str(out, cap, &n, cc < 10u ? num + 1 : num);
		}
	}
	line_str(out, cap, &n, "\x1e");
	return n;
}
#endif
#endif
