/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus cross-band store and forward: the radio core's side (radio/xband.h). Included in core/radio/radio_sm.c
 * before radio_sm_request (tools/plus_xband.py), after DOTA's. While a session runs the radio is on the session's own
 * channel (A or B, each in its own mode: DMR on the colour code and slot asked for with every talkgroup heard, NXDN with
 * any RAN, P25 with any NAC and talkgroup, FM for sending only), the user's channel or VFO stays where it is and comes
 * back with reload() at the end; every request but beeps, settings, the volume and power off is dropped; PTT is refused
 * with the deny tone (native_lab.c passes a P25 channel's PTT here too); the transmitter is keyed only for an over the
 * engine sends and for the CW ID (the CW beacon's keyer). Every over is keyed through the gates of a user's
 * transmission: DMR and FM through ptt_down (TX inhibit, the battery, the band, the radio's time-out), P25 through the
 * native transmitter's own PTT path (radio_sm_tx_check, the battery, the time-out; native_lab.c).
 *
 * Where the frames come from and go (xband.h has the pairs):
 *   DMR      the call stack's voice sink and source, wrapped for the session: what it plays is also recorded, what it
 *            sends comes from the recording;
 *   NXDN     radio_nxdn.c hands the engine the call's voice fields as they play;
 *   FM       sending only: the stored AMBE+2 frames go to the DMR voice bridge's sink one per 20 ms, as an NXDN call's
 *            do, so the stock codec decodes them where it always runs (the audio task, on its own stack); the decoded
 *            blocks are taken as they leave it (its mixer hook, xb_mix) into XB_PCM_SLOTS blocks of the lent memory
 *            (160 ms), and board.afsk's tx_pcm plays them into the FM modulator (its interrupt reads them: xb_gen);
 *   P25      the P25 worker's ring (p25_lab.h): the frames it would play, and the frames it sends in place of the
 *            microphone. The native receiver owns the P25 channel meanwhile (native_lab.c); the radio task polls the
 *            session from radio_sm_native_maintenance then.
 * The memory is the P25 worker's, lent for the session (p25_lab_lend) and given back before the user's channel
 * returns. */

#include "hal_time.h"
#include "p25/p25_lab.h"
#include "native_modem/native_lab.h"	/* (a P25 session: the native receiver and transmitter) */

static radio_sm_t *xb_sm;
static struct {
	bool owned : 1;		/* the radio is on the session's channel */
	bool lent : 1;		/* the P25 worker's memory is ours */
	bool asked : 1;		/* the memory was asked for */
	bool p25 : 1;		/* a P25 session (P25 pairs only with P25) */
	bool keyed : 1;		/* an FM or P25 over keyed by us: its end is ours (xb_send_poll) */
	bool eof : 1;		/* ... and its frames are all out of the store */
	bool held : 1;		/* a P25 frame taken from the store waits for room in the worker's ring */
	bool cw_wait : 1;	/* a P25 session's CW ID waits for the native receiver to let go of the channel */
	bool ending : 1;	/* a P25 session ended: the native receiver lets go first, then the user's channel */
	bool fm_open : 1;	/* an FM over's frames go to the voice bridge (its begin was called) */
	bool fm_rx : 1;		/* FM -> DMR: an FM over is being captured and encoded now (between begin and end) */
	bool fm_rx_on : 1;	/* ... the board's FM receive audio path (board.afsk rx) is running */
	bool fm_tail : 1;	/* an FM over let go: the tone path's route (the microphone muted) held until the carrier is off */
	uint8_t cw_side : 1;
	uint8_t txmode : 2;	/* XB_MODE_* being sent in (FM, P25) */
	uint8_t waits : 7;	/* polls the P25 worker took to let go of its memory so far */
	uint32_t since_ms;	/* eof, the CW wait or the ending began */
	uint32_t next_ms;	/* FM: the next frame is due to the voice bridge */
} xb;
#define XB_LEND_POLLS	(2000u / RADIO_POLL_MS)	/* it has 2 s */
#define XB_WAIT_MS	5000u			/* the native receiver lets go within this (else: given up) */
#define XB_FM_LEVEL	50u			/* the tone path's nominal deviation (hal_afsk.h, as SSTV) */
#define XB_FM_TAIL_MS	200u			/* after the last frame: the bridge's queue (3 frames) plays out */
static uint8_t xb_hold[P25_LAB_XB_BYTES];

/* --- FM: the decoded audio between the audio task and the tone path's interrupt --- */
#define XB_PCM_SLOTS	8u	/* 160 ms (power of two: the counters wrap) */
#define XB_PCM_BYTES	(XB_PCM_SLOTS * VOCODER_FRAME_SAMPLES * sizeof(int16_t))
static int16_t (*xb_pcm)[VOCODER_FRAME_SAMPLES];	/* in the lent memory (a session with an FM side) */
static volatile uint8_t xb_pcm_w, xb_pcm_r, xb_pcm_pos;
static volatile bool xb_pcm_eof;
static volatile bool xb_pcm_cut;	/* the over let go: the interrupt reads no more blocks (the route stays: xb_let_go) */

/* The voice bridge's mixer while an FM over is keyed (the audio task, every 20 ms block): the decoded block kept until
 * the last frame played (xb_pcm_eof), a block dropped when the tone path is behind; the beeps mixed as always */
static bool xb_mix(int16_t *spk)
{
	if (!xb_pcm_eof && (uint8_t)(xb_pcm_w - xb_pcm_r) < XB_PCM_SLOTS) {
		memcpy(xb_pcm[xb_pcm_w % XB_PCM_SLOTS], spk, sizeof xb_pcm[0]);
		xb_pcm_w++;
	}
	return beep_mix(spk);
}

/* board.afsk's source (its interrupt): the blocks decoded, silence while the decoder is behind, the end once all of
 * them played after the last frame */
static uint16_t xb_gen(int16_t *out, uint16_t max, int16_t amp)
{
	uint16_t n = 0;
	if (xb_pcm_cut)
		return 0;
	while (n < max) {
		if (xb_pcm_r == xb_pcm_w) {
			if (xb_pcm_eof)
				break;
			out[n++] = 0;
			continue;
		}
		const int16_t *p = xb_pcm[xb_pcm_r % XB_PCM_SLOTS];
		out[n++] = (int16_t)(((int32_t)p[xb_pcm_pos] * amp) >> 15);
		if (++xb_pcm_pos >= VOCODER_FRAME_SAMPLES) {
			xb_pcm_pos = 0;
			xb_pcm_r++;
		}
	}
	return n;
}

/* --- FM -> DMR: the FM receive audio into the stock AMBE+2 encoder (HARDWARE-UNVERIFIED) ---
 * On the DM-32 (HR_C7000) FM receive audio does not ride the 8 kHz PCM stream (hal_audio.h), so the audio task's
 * encoder would otherwise see the microphone. The radio task reads the de-emphasised FM receive audio through
 * board.afsk (the APRS / NXDN receive path, 8 kHz) into a ring in the lent memory; the audio task's encoder takes it
 * as its microphone (ambeVoiceSetMicSource, a guarded hook in ambeVoice.c) and FEC-encodes each 20 ms block into a
 * 9-byte air frame; the radio task drains those frames (ambeVoiceSource) into the engine as a DMR over. The stock
 * codec runs only on the radio, so this path is host-tested with a fake encoder and the afsk receive path faked; the
 * audio level into the encoder and the de-emphasis are unverified on the owner's unit (docs/plus-radio-test.md). */
#define XB_RX_SAMPLES	(XB_PCM_SLOTS * VOCODER_FRAME_SAMPLES)	/* 1280 samples = 160 ms at 8 kHz (shares xb_pcm) */
/* Sample indices, reset each over (an over is <= 3 min: never wraps). xb_rx_w is the radio task's, xb_rx_r the audio
 * task's; each publishes its index with a release store after its samples moved, and reads the other's with an acquire
 * load, so a sample is never read before it is written nor overwritten before it is read */
static volatile uint32_t xb_rx_w, xb_rx_r;

/* The audio task's microphone while an FM -> DMR over is captured: a 20 ms block of the FM receive ring, silence on an
 * underrun (the over keeps its 20 ms timing). Single consumer (the audio task) */
static const int16_t *xb_fm_mic(void)
{
	static int16_t blk[VOCODER_FRAME_SAMPLES];
	const int16_t *buf = (const int16_t *)xb_pcm;
	uint32_t r = xb_rx_r;
	if (buf == NULL || __atomic_load_n(&xb_rx_w, __ATOMIC_ACQUIRE) - r < (uint32_t)VOCODER_FRAME_SAMPLES) {
		memset(blk, 0, sizeof blk);
		return blk;
	}
	for (uint16_t i = 0; i < VOCODER_FRAME_SAMPLES; i++)
		blk[i] = buf[(r + i) % XB_RX_SAMPLES];
	__atomic_store_n(&xb_rx_r, r + VOCODER_FRAME_SAMPLES, __ATOMIC_RELEASE);
	return blk;
}

/* The radio task: the FM receive audio board.afsk has, into the ring, until the ring is full (no more is read: the
 * rest stays in the board's buffer for the next poll) or the board has no more (rx_read returns 0). Out of line: its
 * buffer stays off the radio task's deepest path through xb_rf_poll (tools/stack_check.py) */
static __attribute__((noinline)) void xb_fm_fill(void)
{
	const hal_afsk_t *a = board.afsk;
	int16_t *buf = (int16_t *)xb_pcm;
	int16_t tmp[64];
	if (xb_pcm == NULL || a == NULL || a->rx_read == NULL)
		return;
	uint32_t w = xb_rx_w;
	for (;;) {
		uint32_t room = (uint32_t)XB_RX_SAMPLES - (w - __atomic_load_n(&xb_rx_r, __ATOMIC_ACQUIRE));
		if (room == 0)
			break;
		uint16_t n = a->rx_read(tmp, (uint16_t)(room < sizeof tmp / sizeof tmp[0] ? room : sizeof tmp / sizeof tmp[0]));
		if (n == 0)
			break;
		for (uint16_t i = 0; i < n; i++)
			buf[w++ % XB_RX_SAMPLES] = tmp[i];
		__atomic_store_n(&xb_rx_w, w, __ATOMIC_RELEASE);	/* the samples first, then the index */
	}
}

/* The FM receive audio path and the encoder let go (an over's end, leaving the FM side, the session's end) */
static void xb_fm_rx_off(void)
{
	if (xb.fm_rx) {
		const dmr_voice_source_t *src = ambeVoiceSource();
		if (src->end != NULL)
			src->end(src->ctx);
		ambeVoiceSetMicSource(NULL);
		xband_rx_end();
		xb.fm_rx = false;
	}
	if (xb.fm_rx_on) {
		if (board.afsk != NULL && board.afsk->rx_stop != NULL)
			board.afsk->rx_stop();
		xb.fm_rx_on = false;
	}
}

/* Every poll while the session listens on an FM side (FM -> DMR): the receive audio captured and encoded, the frames
 * handed to the engine as a DMR over; the RX sub-tone (if set) gates it through the radio's own tone_ok */
static void xb_fm_rx_poll(radio_sm_t *sm)
{
	const hal_afsk_t *a = board.afsk;
	bool on_fm = xb.owned && !xb.p25 && !xb.keyed && !xb.fm_tail && !xb.cw_wait && sm->cw_phase == CW_OFF &&
		     sm->view.state != RS_TX && sm->view.ch.mode == RADIO_MODE_FM && board.vocoder != NULL &&
		     board.vocoder->present();
	if (!on_fm || xb_pcm == NULL || a == NULL || a->rx_read == NULL) {
		xb_fm_rx_off();
		return;
	}
	if (!xb.fm_rx_on) {
		if (a->rx_start == NULL || !a->rx_start())
			return;
		xb.fm_rx_on = true;
	}
	bool open = sm->view.state == RS_RX && sm->view.tone_ok;	/* a carrier with the RX tone (none: carrier squelch) */
	if (open && !xb.fm_rx) {
		xb_rx_w = xb_rx_r = 0;
		ambeVoiceSetMicSource(xb_fm_mic);
		const dmr_voice_source_t *src = ambeVoiceSource();
		if (src->begin != NULL)
			src->begin(src->ctx);	/* the encoder runs on the FM audio (the audio task) */
		xband_rx_begin(0, 0, true, false);	/* FM carries no talkgroup: the DMR side's TX talkgroup is used */
		xb.fm_rx = true;
	}
	if (xb.fm_rx) {
		uint8_t fr[XB_FRAME_BYTES];
		const dmr_voice_source_t *src = ambeVoiceSource();
		xb_fm_fill();
		while (src->next != NULL && src->next(fr, src->ctx))
			xband_rx_frame(fr);
	}
	if (!open && xb.fm_rx) {
		const dmr_voice_source_t *src = ambeVoiceSource();
		if (src->end != NULL)
			src->end(src->ctx);
		ambeVoiceSetMicSource(NULL);
		xband_rx_end();
		xb.fm_rx = false;		/* the FM receive path stays on for the next over on this side */
	}
}

/* --- The call stack's voice, wrapped --- */
/* (the audio side's sink: the over plays while it is recorded) */
#define xb_real ambeVoiceSink()

static void xb_sink_begin(const dmr_caller_t *c, void *ctx)
{
	(void)ctx;
	if (xb_real->begin != NULL)
		xb_real->begin(c, xb_real->ctx);
	xband_rx_begin(c->src, c->dst, c->group, c->encrypted);
}

static void xb_sink_frame(const uint8_t ambe72[DMR_AMBE_FRAME_BYTES], void *ctx)
{
	(void)ctx;
	xb_real->frame(ambe72, xb_real->ctx);
	xband_rx_frame(ambe72);
}

static void xb_sink_lost(void *ctx)
{
	(void)ctx;
	if (xb_real->lost != NULL)
		xb_real->lost(xb_real->ctx);
	xband_rx_frame(dmr_ambe_silence);	/* the over keeps its timing: silence where a frame was lost */
}

static void xb_sink_end(void *ctx)
{
	(void)ctx;
	if (xb_real->end != NULL)
		xb_real->end(xb_real->ctx);
	xband_rx_end();
}

static bool xb_source_next(uint8_t ambe72[DMR_AMBE_FRAME_BYTES], void *ctx)
{
	(void)ctx;
	return xband_tx_next(ambe72);
}

static const dmr_voice_sink_t xb_sink = { xb_sink_begin, xb_sink_frame, xb_sink_lost, xb_sink_end, NULL };
static const dmr_voice_source_t xb_source = { NULL, xb_source_next, NULL, NULL };

/* --- What the engine asks the radio core for --- */

/* PTT for the native P25 transmitter, as the radio task would hand it a key press (native_lab.c lets only this one
 * through while a session runs) */
static void xb_native_ptt(radio_sm_t *sm, bool down)
{
	radio_req_t r = { .type = RREQ_PTT, .arg = down ? 1 : 0 };
	xband_native_key = true;
	native_lab_request(sm, &r);
	xband_native_key = false;
}

/* A P25 session's channel let go by the native receiver (true: free, nothing of it runs): asked as any request
 * asks it (native_lab.c channel_let_go stops the worker; the FM placeholder keeps it from taking the channel again) */
static bool xb_native_free(radio_sm_t *sm)
{
	radio_req_t r = { .type = RREQ_RETUNE };
	return native_lab_request(sm, &r) == 0 && !native_lab_owns() && !p25_lab_busy();
}

/* The session channel in side's mode on its frequency. FM is a placeholder too: while a P25 session's native receiver
 * must let go (its CW ID, its end) */
_Static_assert(XB_CC_ANY == CODEPLUG_CC_ANY && XB_TS_ANY == CODEPLUG_TS_ANY, "the side's filters are the channel's");

static void xb_channel(radio_sm_t *sm, uint8_t side, uint8_t mode)
{
	channel_t *ch = &sm->view.ch;
	const xband_side_t *sd = &xband_req()->side[side & 1u];
	ch->rx_hz = ch->tx_hz = xband_req()->hz[side & 1u];
	/* DMR: heard on the side's colour code and slot (or any: the call stack filters, CONFIG_DMR_ANY); the over is sent
	 * on its TX ones (xb_key_dmr) */
	ch->color_code = sd->rx_cc;
	ch->timeslot = sd->rx_ts;
	ch->mode = mode == XB_MODE_DMR ? RADIO_MODE_DMR : mode == XB_MODE_P25 ? RADIO_MODE_P25 :
		   mode == XB_MODE_NXDN ? RADIO_MODE_NXDN : RADIO_MODE_FM;
	ch->rx_only = mode == XB_MODE_NXDN;	/* (an NXDN channel is receive only: codeplug_p25_valid) */
	/* P25: the side's NAC (or any) and any talkgroup heard; the over's own talkgroup and NAC are set when it is keyed */
	ch->p25_nac = sd->rx_nac;
	ch->p25_tg = mode == XB_MODE_P25 ? 1u : 0u;	/* (any RAN for NXDN) */
	ch->p25_tx_nac_override = 0;
	ch->p25_squelch = 0;
	ch->p25_id = sm->view.dmr.own_id;
	/* FM (owner, 2026-11-25): the side's RX sub-tone gates what an FM -> DMR receive side relays (only a carrier with it
	 * is heard; the radio's own tone_ok), and the TX sub-tone is put on a DMR -> FM carrier (ptt_down's KEY_TX reads
	 * ch->tx_tone; the DCS tail is ptt_release's). Any other mode: no tone. subtone_t and the channel's tone fields
	 * exist whatever CONFIG_SUBTONE (the picker that edits them in ui_xband.c needs it; this copy does not) */
	if (mode == XB_MODE_FM) {
		ch->rx_tone = (subtone_t){ sd->rx_tone_type, sd->rx_tone_value };
		ch->tx_tone = (subtone_t){ sd->tx_tone_type, sd->tx_tone_value };
	} else {
		ch->rx_tone = ch->tx_tone = (subtone_t){ TONE_NONE, 0 };
	}
}

/* Receive on A or B in its mode: DMR every talkgroup on the slot and colour code; a P25 channel the native receiver
 * holds is retuned by it (native_lab.c sees the channel change) */
static void xb_tune(uint8_t side)
{
	radio_sm_t *sm = xb_sm;
	uint8_t mode = xband_req()->mode[side & 1u];
	xb_channel(sm, side, mode);
	if (mode == XB_MODE_P25 && native_lab_owns()) {
		changed(sm);
		return;
	}
	tune(sm, xb_now);
	if (mode == XB_MODE_DMR)
		set_hear_all_latch(sm, true);
}

/* DMR: as a PTT would, on the side's TX colour code and slot, to dst (the side's TX talkgroup, else the one heard) */
static bool xb_key_dmr(radio_sm_t *sm, uint8_t side, uint32_t dst, bool group)
{
	const xband_side_t *sd = &xband_req()->side[side & 1u];
	if (!sm->dmr_running || dst == 0)
		return false;
	sm->view.ch.color_code = sd->tx_cc & 15u;
	sm->view.ch.timeslot = sd->tx_ts == 2 ? 2 : 1;
	sm->manual = true;	/* a target for now: the channel's (none) comes back with dmr_end */
	sm->manual_vfo = sm->view.vfo;
	sm->manual_ch = sm->view.channel_number;
	sm->manual_id = dst;
	sm->manual_private = !group;
	sm->manual_ts = 0;
	dmr_reconfigure(sm);
	ptt_down(sm, xb_now);
	if (sm->view.state != RS_TX) {
		ptt_release(sm, false, xb_now);		/* refused (TX inhibit, the battery, the band, no ID) */
		return false;
	}
	return true;
}

/* FM: as a PTT would (the carrier with the side's TX sub-tone, if set: xb_channel), then the decoded over through the
 * tone path in place of the microphone (the board mutes the microphone while the tone path is routed) */
static bool xb_key_fm(radio_sm_t *sm)
{
	const hal_afsk_t *a = board.afsk;
	if (xb_pcm == NULL || board.vocoder == NULL || !board.vocoder->present() || a == NULL || a->tx_pcm == NULL ||
	    a->tx_busy == NULL || a->tx_stop == NULL)
		return false;
	xb_pcm_w = xb_pcm_r = xb_pcm_pos = 0;
	xb_pcm_eof = xb_pcm_cut = false;
	ptt_down(sm, xb_now);
	if (sm->view.state != RS_TX) {
		ptt_release(sm, false, xb_now);		/* refused (TX inhibit, the battery, the band) */
		return false;
	}
	if (!a->tx_pcm(xb_gen, XB_FM_LEVEL)) {
		ptt_release(sm, false, xb_now);
		return false;
	}
	const dmr_voice_sink_t *sink = ambeVoiceSink();
	ambeVoiceSetMixer(xb_mix);
	if (sink->begin != NULL)
		sink->begin(NULL, sink->ctx);	/* (its stream starts: the codec decodes in the audio task) */
	xb.fm_open = true;
	xb.next_ms = xb_now - 3u * XB_FRAME_MS;	/* a burst's worth at once: the bridge primes three */
	return true;
}

/* P25: the side's TX NAC, dst (its TX talkgroup, else the one heard), our ID as the unit ID; the worker's ring as its
 * microphone */
static bool xb_key_p25(radio_sm_t *sm, uint8_t side, uint32_t dst)
{
	uint16_t nac = xband_req()->side[side & 1u].tx_nac;
	uint32_t id = sm->view.dmr.own_id;
	if (dst == 0 || dst > 0xFFFFu || id == 0 || id > 0xFFFFFEu || nac >= 0xF7Eu)
		return false;
	channel_t *ch = &sm->view.ch;
	ch->p25_tg = (uint16_t)dst;
	ch->p25_tx_nac_override = (uint16_t)(nac + 1u);
	ch->p25_id = id;
	p25_lab_xb_call(nac, (uint16_t)dst, id);
	p25_lab_xb_send(1);
	xb_native_ptt(sm, true);
	if (!sm->ptt || sm->view.state == RS_TX_DENIED) {
		xb_native_ptt(sm, false);	/* refused (radio_sm_tx_check: TX inhibit, the band, the battery ...) */
		p25_lab_xb_send(-1);
		return false;
	}
	return true;
}

/* Transmit the over on A or B in its mode (the engine tuned there just before) */
static bool xb_key(uint8_t side, uint32_t dst, bool group)
{
	radio_sm_t *sm = xb_sm;
	uint8_t mode = xband_req()->mode[side & 1u];
	bool ok = mode == XB_MODE_DMR ? xb_key_dmr(sm, side, dst, group) : mode == XB_MODE_FM ? xb_key_fm(sm) :
		  mode == XB_MODE_P25 ? xb_key_p25(sm, side, dst) : false;	/* (NXDN: never) */
	xb.keyed = ok && mode != XB_MODE_DMR;
	xb.eof = xb.held = false;
	xb.txmode = mode & 3u;
	return ok;
}

/* The over's frames are all out of the store: DMR lets go now (the call stack ends its superframe), FM and P25 once
 * what they hold is on the air (xb_send_poll) */
static void xb_unkey(void)
{
	if (!xb.keyed) {
		ptt_release(xb_sm, false, xb_now);
		return;
	}
	if (!xb.eof)
		xb.since_ms = xb_now;
	xb.eof = true;
	if (xb.txmode == XB_MODE_P25 && !xb.held)
		p25_lab_xb_send(0);
}

/* An FM over's tone path let go once the carrier is off. While it is routed the board keeps the microphone muted
 * (hrc_afsk.c route -> the DM-32's afsk_tx_route -> hrc7000_codec_mute), so it outlives the transmission: with a DCS TX
 * tone ptt_release holds the carrier RADIO_SUBTONE_TAIL_MS for the turn-off code, and no room audio may reach the
 * modulator meanwhile (an unattended relay). Every poll while it waits (xb_send_poll) */
static void xb_fm_route_off(radio_sm_t *sm)
{
	if (xb.fm_tail && sm->view.state != RS_TX) {
		board.afsk->tx_stop();
		xb.fm_tail = false;
	}
}

/* An FM or P25 over off the air (its end, the radio's time-out, a refusal, the stop) */
static void xb_let_go(radio_sm_t *sm)
{
	if (!xb.keyed)
		return;
	xb.keyed = false;
	if (xb.txmode == XB_MODE_FM) {
		xb_pcm_cut = true;	/* the tone path's interrupt reads no more blocks; its route (the mic muted) stays */
		xb_pcm_eof = true;
		ambeVoiceSetMixer(beep_mix);
		const dmr_voice_sink_t *sink = ambeVoiceSink();
		if (xb.fm_open && sink->end != NULL)
			sink->end(sink->ctx);
		xb.fm_open = false;
		ptt_release(sm, false, xb_now);	/* (a DCS TX tone: the turn-off tail starts, the carrier stays up) */
		xb.fm_tail = true;
		xb_fm_route_off(sm);		/* now, unless the tail holds the carrier */
	} else {
		xb_native_ptt(sm, false);	/* (the worker ends with the terminator: the ring says no more) */
		p25_lab_xb_send(-1);
	}
}

/* Every poll while an FM or P25 over is keyed: the next frames decoded or handed to the worker, the end */
static void xb_send_poll(radio_sm_t *sm, uint32_t now_ms)
{
	xb_fm_route_off(sm);	/* an FM over's turn-off tail ended: the tone path's route let go */
	if (!xb.keyed)
		return;
	if (xb.txmode == XB_MODE_FM) {
		/* the frames to the voice bridge one per 20 ms (its queue holds 8); after the last, its queue's time */
		const dmr_voice_sink_t *sink = ambeVoiceSink();
		uint8_t fr[XB_FRAME_BYTES];
		while (!xb.eof && (int32_t)(now_ms - xb.next_ms) >= 0 && xband_tx_next(fr)) {
			sink->frame(fr, sink->ctx);
			xb.next_ms += XB_FRAME_MS;
		}
		if (xb.eof && xb.fm_open && (int32_t)(now_ms - xb.next_ms) >= (int32_t)XB_FM_TAIL_MS) {
			xb_pcm_eof = true;	/* the blocks kept play out, then the tone path ends */
			xb.fm_open = false;
			if (sink->end != NULL)
				sink->end(sink->ctx);
		}
		bool on = sm->view.state == RS_TX && board.afsk->tx_busy();
		if (!on || (xb.eof && now_ms - xb.since_ms >= XB_SEND_SLACK_MS))
			xb_let_go(sm);	/* played out, cut by the radio (its time-out), or stuck */
		return;
	}
	/* P25: the ring topped up (a frame the ring had no room for waits) */
	while (!xb.eof || xb.held) {
		if (!xb.held && !xband_tx_next(xb_hold))
			break;
		if (!p25_lab_xb_put(xb_hold)) {
			xb.held = true;
			break;
		}
		xb.held = false;
		if (xb.eof)
			p25_lab_xb_send(0);	/* the frame that waited was the last */
	}
	if (p25_lab_xb_sent() || sm->view.state == RS_TX_DENIED || !sm->ptt ||
	    (xb.eof && now_ms - xb.since_ms >= XB_SEND_SLACK_MS))
		xb_let_go(sm);	/* sent, refused by the native transmitter, let go by the radio, or stuck */
}

/* The callsign in Morse on A or B: the CW beacon's keyer (FM carrier, the settings' speed and tone) */
static bool xb_cw_now(radio_sm_t *sm, uint8_t side)
{
	uint32_t hz = xband_req()->hz[side & 1u];
	char text[MORSE_TEXT_MAX];
	uint8_t n = 0;
	while (n < XB_CALL_MAX && n + 1u < sizeof text && xband_req()->call[n] != '\0') {
		text[n] = xband_req()->call[n];
		n++;
	}
	text[n] = '\0';
	if (n == 0 || sm->cw_phase != CW_OFF || board.radio->tx_tone == NULL || radio_sm_tx_check(sm) != TXDENY_NONE ||
	    !board_in_band(hz, true))
		return false;
	if (sm->board_mode == RADIO_MODE_DMR)
		dmr_end(sm);
	xb_cw_on = true;	/* cw_restore brings the session's channel back */
	if (cw_begin(sm, hz, sm->view.power, text, n, xb_now) == 0) {
		xb_cw_on = false;
		return false;
	}
	return true;
}

/* A P25 session waits for the native receiver to let go of the channel first (an FM placeholder on side's frequency
 * makes it let go); the engine waits meanwhile (xband_in_t.id_wait) */
static bool xb_cw(uint8_t side)
{
	radio_sm_t *sm = xb_sm;
	if (!xb.p25)
		return xb_cw_now(sm, side);
	xb_channel(sm, side, XB_MODE_FM);
	changed(sm);
	xb.cw_wait = true;
	xb.cw_side = side & 1u;
	xb.since_ms = xb_now;
	return true;
}

static void xb_beep(uint8_t what)
{
	beep(xb_sm, what == XB_BEEP_CUT ? BEEP_TOT_WARN : BEEP_ERROR);
}

/* A session to start: the user's activities stop, the session's channel is tuned (the first side heard) */
static void xb_take(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->hotspot || sm->ptt || sm->view.state == RS_TX || dota_on() || sm->cw_phase != CW_OFF ||
	    !xband_begin(now_ms))
		return;
	const xband_req_t *r = xband_req();
	xb_sm = sm;
	aprs_rf_abort(sm, now_ms);
	roam_cancel(sm, now_ms);
#if CONFIG_AIR_WATCH
	/* the air watch / auto scan ends first, as the user's stop would (its own RREQ_AIR_END is dropped during the
	 * session: sm->air stayed set, review M3); the UI follows the counted stop */
	air_session_end(sm, now_ms);
#endif
#if CONFIG_SCAN
	/* dual / multi-watch: it ends on the selected letter (the one with PTT; reload() puts it back after the session),
	 * not the letter the ring is on */
	if (scan_on(&sm->scan))
		scan_stop(sm, dual_running(sm), now_ms);	/* dual watch and multi-watch too: the session has one channel */
#endif
#if CONFIG_CW_BEACON
	if (beacon.on || beacon.keying) {
		beacon_now_ms = now_ms;
		beacon_stop(sm);
	}
#endif
	/* RAM only: reload() puts the user's channel back. A plain channel: no talker alias, privacy or SFR role */
	channel_t *ch = &sm->view.ch;
	memset(ch, 0, sizeof *ch);
	strcpy(ch->name, "CROSS");
	ch->bw = RADIO_BW_12K5;
	ch->power = CODEPLUG_POWER_DEFAULT;
	ch->dmo = true;
	ch->no_eco = true;	/* battery saving never sleeps the receiver on an FM receive side (save_allowed) */
#if CONFIG_DMR_PRIVACY
	sm->privacy_type = DMR_PRIVACY_OFF;
#endif
	sm->view.power = xband_power_level(r->power, board.power_count);
	memset(&xb, 0, sizeof xb);
	xb.owned = xband_held = true;
	xb.p25 = r->mode[0] == XB_MODE_P25;
	xb_pcm = NULL;
	dmr_call_set_voice(&xb_sink, &xb_source);
	/* the first side heard; a P25 one waits in FM for the memory (the native receiver would take the worker) */
	uint8_t side = xband_listens(r) & 1u ? 0 : 1;
	xb_channel(sm, side, xb.p25 ? XB_MODE_FM : r->mode[side]);
	tune(sm, now_ms);
	if (ch->mode == RADIO_MODE_DMR)
		set_hear_all_latch(sm, true);
	changed(sm);
}

/* The session ended (the user's stop, the battery, no memory): off the air at once, the stack's voice and the P25
 * worker's memory given back, the user's channel or VFO as it was */
static void xb_restore(radio_sm_t *sm, uint32_t now_ms)
{
	xb_fm_rx_off();		/* FM -> DMR: the receive audio path and the encoder let go before the memory goes back */
	xb_let_go(sm);
	if (xb.fm_tail) {
		/* an FM over's DCS turn-off tail still on the air: cut short cleanly (the board's tail off, the carrier off),
		 * then the tone path's route; never the microphone on the air */
#if CONFIG_SUBTONE
		tail_cancel(sm);
#endif
		if (sm->view.state == RS_TX)
			fm_tx_end(sm, false, now_ms);
		board.afsk->tx_stop();
		xb.fm_tail = false;
	}
	xb.owned = xb.ending = xb.cw_wait = xband_held = false;
	if (sm->cw_phase != CW_OFF) {
		board.radio->tx_tone(0);
		board.radio->tx_off();
		sm->cw_phase = CW_OFF;
		sm->cw_key = false;
		xb_cw_on = false;
	}
	sm->ptt = false;
	if (sm->board_mode == RADIO_MODE_DMR)
		dmr_end(sm);	/* the transmission dropped, the stack stopped */
	sm->view.state = RS_IDLE;
	dmr_call_set_voice(ambeVoiceSink(), ambeVoiceSource());
	xband_release();
	xb_pcm = NULL;
	if (xb.lent)
		p25_lab_give_back();
	xb.lent = false;
	reload(sm, now_ms);
}

/* Before the request switch: true when the request was the session's or is dropped while one runs */
static bool xb_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	if (req->type == RREQ_XBAND) {
		if (req->arg != 0 && !xb.owned)
			xb_take(sm, now_ms);
		return true;	/* a stop is the engine's (xband_stop): the poll ends it */
	}
	if (!xb.owned)
		return false;
	switch (req->type) {
	case RREQ_PTT:
		if (req->arg != 0) {
			beep(sm, BEEP_TX_DENY);
			xband_ptt();
		}
		return true;
	case RREQ_BEEP:
	case RREQ_VIBRATE:
	case RREQ_VOLUME:
	case RREQ_VOLUME_OVERRIDE:
	case RREQ_SETTINGS:
	case RREQ_POWER_OFF:
		return false;
	default:
		return true;	/* nothing else moves the radio while it relays (owner: the screen is locked) */
	}
}

/* The memory lent (n pieces, 0: none): the engine's frames, and an FM side's decoded blocks from the first piece */
static void xb_memory(uint8_t *seg[], size_t len[], uint8_t n, uint32_t now_ms)
{
	const xband_req_t *r = xband_req();
	xb.asked = true;
	xb.lent = n != 0;
	if (n != 0 && (r->mode[0] == XB_MODE_FM || r->mode[1] == XB_MODE_FM) && len[0] > XB_PCM_BYTES) {
		xb_pcm = (int16_t (*)[VOCODER_FRAME_SAMPLES])(void *)seg[0];	/* (the worker's voice context: aligned) */
		seg[0] += XB_PCM_BYTES;
		len[0] -= XB_PCM_BYTES;
	}
	xband_memory(seg, len, n, now_ms);
}

/* Every poll: the session */
static void xb_rf_poll(radio_sm_t *sm, uint32_t now_ms)
{
	xb_sm = sm;
	if (!xb.owned) {
		if (!xband_on())
			return;
		xb_take(sm, now_ms);	/* the request came while the radio was busy: now */
		if (!xb.owned)
			return;
	}
	if (xb.ending) {
		/* the native receiver lets go of the P25 channel (the FM placeholder), then the user's channel */
		if (!xb_native_free(sm) && now_ms - xb.since_ms < XB_WAIT_MS)
			return;
		xb_restore(sm, now_ms);
		return;
	}
	if (!xb.asked) {
		/* The P25 worker lets go of its memory once it is idle (the session's channel stopped any P25 receive) */
		uint8_t *seg[XB_SEGMENTS];
		size_t len[XB_SEGMENTS];
		uint8_t n = p25_lab_lend(seg, len, xb.p25);
		if (n == 0 && (unsigned)xb.waits < XB_LEND_POLLS) {
			xb.waits++;
			p25_lab_channel_stop();
			return;
		}
		xb_memory(seg, len, n, now_ms);
	}
	if (xb.cw_wait) {
		/* a P25 session's CW ID: keyed once the native receiver let go; never while it holds the radio (given up
		 * after XB_WAIT_MS: never on the air, the engine beeps as for a refused ID) */
		bool free_now = xb_native_free(sm);
		if (free_now || now_ms - xb.since_ms >= XB_WAIT_MS) {
			xb.cw_wait = false;
			if (free_now)
				(void)xb_cw_now(sm, xb.cw_side);
		}
	}
	if (!xb.cw_wait)
		xb_send_poll(sm, now_ms);
	if (xb.p25 && !xb.keyed && !xb.cw_wait) {
		/* the frames the worker heard, with the call they came from */
		uint8_t fr[P25_LAB_XB_BYTES];
		while (p25_lab_xb_take(fr)) {
			uint16_t nac, tg;
			uint32_t src;
			p25_lab_xb_heard(&nac, &tg, &src);
			if (!xband_nac_ok(nac))
				continue;	/* another NAC than the side's: not recorded */
			xband_rx_begin(src, tg, true, false);	/* (only clear voice reaches the ring) */
			xband_rx_frame(fr);
		}
	}
	xb_fm_rx_poll(sm);	/* FM -> DMR: capture and encode an FM over on an FM receive side (radio_xband.inc) */
	uint32_t act = 0;
	bool rx = sm->view.state == RS_RX;
	bool heard = dmr_call_last_activity(&act) && now_ms - act < XB_HOLD_MS;
#if CONFIG_NXDN
	if (codeplug_mode_nxdn(sm->view.ch.mode))
		rx = heard = radio_nxdn_heard(now_ms);
#endif
	if (!xb.p25 && !xb.keyed && sm->view.ch.mode == RADIO_MODE_FM)
		rx = heard = sm->view.state == RS_RX && sm->view.tone_ok;	/* FM -> DMR: the squelch open with the RX tone */
	xband_in_t in = {
		.rx = rx,
		.activity = heard,
		.tx = sm->view.state == RS_TX || sm->cw_phase != CW_OFF || xb.keyed,
		.battery_critical = sm->batt.level == BATT_CRITICAL,
		.id_wait = xb.cw_wait,
	};
	static const xband_io_t io = { xb_tune, xb_key, xb_unkey, xb_cw, xb_beep };
	xband_poll(&io, &in, now_ms);
	if (xband_owns_radio())
		return;
	if (xb.p25 && xb.lent) {
		/* a P25 session: off the air, the native receiver let go of the channel first (xb.ending) */
		xb_let_go(sm);
		xb.cw_wait = false;
		xb_channel(sm, 0, XB_MODE_FM);
		changed(sm);
		xb.ending = true;
		xb.since_ms = now_ms;
		return;
	}
	xb_restore(sm, now_ms);
}

/* The radio task while the native receiver owns the radio (a P25 session's channel): the session goes on */
static void xb_native_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (xb.owned)
		xb_rf_poll(sm, now_ms);
}
