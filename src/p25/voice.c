/* SPDX-License-Identifier: MIT */
#include "voice.h"
#include "p25_runtime.h"
#if defined(CONFIG_P25_EXTRAS) && CONFIG_P25_EXTRAS
#include "p25_lcx.h"
#endif

static void admission_reset(p25_voice *v)
{
    v->crypto_clear = v->have_group = 0;
    v->active_nac = P25_NAC_ANY;
    v->active_group = 0;
    v->active_source = 0;
#if defined(CONFIG_P25_EXTRAS) && CONFIG_P25_EXTRAS
    v->active_target = 0;
    v->private_call = v->emergency = 0;
#endif
    dm32_imbe_init(&v->codec);
}

/* The group call's talkgroup is the filter's, All Call, or (DDM32 Plus) one of the RX group list's */
static int group_wanted(const p25_voice *v)
{
    if (v->active_group == v->group_filter || v->active_group == 0xffff) return 1;
#if defined(CONFIG_P25_RXGL) && CONFIG_P25_RXGL
    for (unsigned i = 0; i < v->rx_group_count; i++)
        if (v->rx_groups[i] == v->active_group) return 1;
#endif
    return 0;
}

void p25_voice_rx_reset(p25_voice *v)
{
    /* Stale receive records must not reset the shared active TX codec. */
    if (v->transmitting) return;
    p25_rx_init(&v->framer, v->framer.expected_nac, 2);
#if defined(CONFIG_P25_TRUNK) && CONFIG_P25_TRUNK
    if (v->tsdu_rx) p25cai_control_receiver_gap(v->tsdu_rx);
#endif
    admission_reset(v);
}

#if defined(CONFIG_P25_TRUNK) && CONFIG_P25_TRUNK
void p25_voice_set_tsdu(p25_voice *v, p25cai_control_receiver *rx,
                        void (*hook)(void *ctx, const p25cai_control *unit), void *ctx)
{
    v->tsdu_rx = hook ? rx : NULL;
    v->tsdu = rx ? hook : NULL;
    v->tsdu_ctx = ctx;
}

/* DDM32 Plus trunk following: the same dibits through the library's control channel receiver. A TSDU (one to three
 * TSBKs, each CRC-checked) goes to the hook and counts its blocks in rx_frames; a TSDU dropped after its NID on a
 * block that failed goes to the hook as NULL */
static void tsdu_dibit(p25_voice *v, uint8_t dibit)
{
    p25cai_control unit;
    uint16_t before = v->tsdu_rx->symbols;
    int got = p25cai_control_receiver_push(v->tsdu_rx, dibit, &unit);
    if (got == 1 && unit.duid == P25CAI_TSDU) {
        v->rx_frames += unit.count;
        v->tsdu(v->tsdu_ctx, &unit);
    } else if (got == 0 && before >= 57u && !v->tsdu_rx->symbols && v->tsdu_rx->duid == P25CAI_TSDU)
        v->tsdu(v->tsdu_ctx, NULL);
}
#endif

void p25_voice_init(p25_voice *v, uint16_t nac, uint16_t group,
                    p25_pcm_sink sink, void *ctx)
{
    memset(v, 0, sizeof *v);
    v->sink = sink;
    v->sink_ctx = ctx;
    v->group_filter = group;
    p25_rx_init(&v->framer, nac == 0xf7e ? P25_NAC_ANY : nac, 2);
    admission_reset(v);
}

void p25_voice_rx_dibit(p25_voice *v, uint8_t dibit)
{
#if defined(CONFIG_P25_TRUNK) && CONFIG_P25_TRUNK
    if (v->tsdu && !v->transmitting) tsdu_dibit(v, dibit);
#endif
    if (v->transmitting || !p25_rx_push_dibit(&v->framer, dibit)) return;
    p25_decoded d;
    uint32_t at=v->micros?v->micros():0;
#if defined(CONFIG_P25_EXTRAS) && CONFIG_P25_EXTRAS
    p25x_lc x;
    int decoded=p25x_decode_frame(v->framer.frame, p25_frame_bytes(v->framer.nid.duid), &d, &x);
#else
    int decoded=p25_decode_frame(v->framer.frame, p25_frame_bytes(v->framer.nid.duid), &d);
#endif
    uint32_t elapsed=v->micros?v->micros()-at:0;
    if (elapsed>v->fec_max_us)v->fec_max_us=elapsed;
    if (decoded) {
#if defined(CONFIG_SIGNAL_QUALITY) && CONFIG_SIGNAL_QUALITY
        if (v->framer.nid.duid == P25_DUID_LDU1 || v->framer.nid.duid == P25_DUID_LDU2) v->rx_fec_lost += 9;
#endif
        admission_reset(v);
        return;
    }
    v->rx_frames++;
    if (v->active_nac != d.nid.nac) {
        admission_reset(v);
        v->active_nac = d.nid.nac;
    }
    if (d.nid.duid == P25_DUID_TDU || d.nid.duid == P25_DUID_TDULC) {
        admission_reset(v);
        return;
    }
#if defined(CONFIG_P25_EXTRAS) && CONFIG_P25_EXTRAS
    if (d.nid.duid == P25_DUID_LDU1 && !d.metadata_valid && x.lc_other) {
        /* DM32 Plus: an LDU1 carrying another link control format (a vendor's talker alias, ...) is not damage: it
         * says nothing of the call, so the identity and permission from the LCs around it hold (they used to be
         * dropped, muting that superframe). A new call starts with an HDU or a new NAC, which reset them. */
    } else
#endif
    if (!d.metadata_valid || (d.nid.duid == P25_DUID_HDU && d.manufacturer_id > 1)) {
        /* Do not reuse permission from a previous good header over damage. */
        v->crypto_clear = v->have_group = 0;
    } else if (d.nid.duid == P25_DUID_HDU) {
        admission_reset(v);
        v->active_nac = d.nid.nac;
        v->crypto_clear = d.clear_voice;
        v->active_group = d.talkgroup;
        v->have_group = d.talkgroup != 0;
    } else if (d.nid.duid == P25_DUID_LDU1) {
#if defined(CONFIG_P25_EXTRAS) && CONFIG_P25_EXTRAS
        /* DM32 Plus: unit-to-unit voice too. Another kind of call, target or talkgroup than the header's (or the
         * last LDU1's) waits for the next encryption sync, as a talkgroup change always did */
        uint8_t unit = d.link_control_format == 3;
        if ((v->have_group && (v->private_call != unit || v->active_group != d.talkgroup ||
                               v->active_target != x.target_id)) ||
            (v->active_source && v->active_source != d.source_id))
            v->crypto_clear = 0;
        v->private_call = unit;
        v->emergency = (d.service_options & 0x80) != 0;
        v->active_target = x.target_id;
        v->active_group = d.talkgroup;
        v->active_source = d.source_id;
        v->have_group = (unit ? x.target_id != 0 : d.talkgroup != 0) && d.source_id && d.source_id <= 0xFFFFFEu;
#else
        if ((v->have_group && v->active_group != d.talkgroup) ||
            (v->active_source && v->active_source != d.source_id))
            v->crypto_clear = 0;
        v->active_group = d.talkgroup;
        v->active_source = d.source_id;
        v->have_group = d.talkgroup && d.source_id && d.source_id <= 0xFFFFFEu;
#endif
        if (!d.clear_voice) v->crypto_clear = 0;
        /* LC's unprotected bit alone cannot override an encrypted ALGID.
         * Late entry waits for a valid LDU2 encryption sync. */
    } else if (d.nid.duid == P25_DUID_LDU2) {
        v->crypto_clear = d.clear_voice;
    }
    if (d.nid.duid != P25_DUID_LDU1 && d.nid.duid != P25_DUID_LDU2) return;
#if defined(CONFIG_SIGNAL_QUALITY) && CONFIG_SIGNAL_QUALITY
    v->rx_fec_frames += 9;
    for (unsigned i = 0; i < 9; i++) v->rx_fec_bits += d.voice_corrected_bits[i];
#endif
#if defined(CONFIG_P25_EXTRAS) && CONFIG_P25_EXTRAS
    /* A unit-to-unit call plays with no talkgroup filter (an Any channel, P25 search), or when it is for us */
    int admitted = v->crypto_clear && v->have_group &&
        (!v->group_filter ||
         (v->private_call ? v->own_unit && v->active_target == v->own_unit
                          : group_wanted(v)));
#else
    int admitted = v->crypto_clear && v->have_group &&
        (!v->group_filter || group_wanted(v));
#endif
    for (unsigned i = 0; i < 9; i++) {
        int16_t pcm[160] = {0};
        if (!admitted) v->rx_muted++;
        /* Conservative implementation policy, not a TIA concealment rule.
         * Golay/Hamming can miscorrect beyond t; no audio-quality claim. */
        else {
            at=v->micros?v->micros():0;
            int ok=d.voice_u0_corrected_bits[i] <= 3 && d.voice_corrected_bits[i] <= 10 &&
                   dm32_imbe_decode(&v->codec, d.imbe[i], pcm);
            elapsed=v->micros?v->micros()-at:0;
            if(elapsed>v->imbe_max_us)v->imbe_max_us=elapsed;
            if(!ok) {
                memset(pcm, 0, sizeof pcm);
                v->rx_erasures++;
                dm32_imbe_init(&v->codec);
            } else {
                v->rx_voice++;
#if defined(CONFIG_CALL_REPLAY) && CONFIG_CALL_REPLAY
                if (v->tap) v->tap(v->tap_ctx, d.imbe[i]);
#endif
            }
        }
#if defined(CONFIG_DMR_AUDIO_DIAG) && CONFIG_DMR_AUDIO_DIAG
        /* DDM32 Plus: the decoded voice x2, saturating, the same level as DMR (owner 2026-11-21; docs/plus.md
         * "DMR receive audio diagnostics", Result). Plus builds only; the lab and host builds keep the samples. */
        for (unsigned k = 0; k < 160u; k++) {
            int32_t g = (int32_t)pcm[k] * 2;
            pcm[k] = (int16_t)(g > 32767 ? 32767 : g < -32768 ? -32768 : g);
        }
#endif
        if (v->sink) v->sink(v->sink_ctx, pcm);
    }
}

static p25_voice_packet *next_packet(p25_voice *v)
{
    return v->count < P25_VOICE_QUEUE ? &v->queue[v->head] : NULL;
}

static void commit_packet(p25_voice *v, uint16_t bytes)
{
    v->queue[v->head].bytes = bytes;
    v->head = (uint8_t)((v->head + 1) % P25_VOICE_QUEUE);
    v->count++;
    v->tx_frames++;
}

void p25_voice_tx_abort(p25_voice *v)
{
    v->transmitting = v->started = v->stopping = v->terminator_pending = 0;
    v->count = v->head = v->tail = v->voice_count = v->flush_remaining = 0;
    v->packet_symbol = 0;
    p25_voice_rx_reset(v);
}

static int fail(p25_voice *v)
{
    p25_voice_tx_abort(v);
    v->fault = 1;
    return 0;
}

int p25_voice_tx_begin(p25_voice *v, const p25_call *call)
{
    if (!call || v->transmitting || !call->talkgroup ||
        !call->source_id || call->source_id > 0xFFFFFEu) return 0;
    p25_voice_tx_abort(v);
    v->fault = 0;
    if (p25_encode_hdu(call, v->queue[0].data)) return 0;
    v->call = *call;
    v->transmitting = 1;
    v->next_duid = P25_DUID_LDU1;
    commit_packet(v, P25_HDU_BYTES);
    return 1;
}

static int encode_frame(p25_voice *v, int16_t pcm[160])
{
    if (!dm32_imbe_encode(&v->codec, pcm, v->voice[v->voice_count])) return fail(v);
    if (++v->voice_count < 9) return 1;
    p25_voice_packet *p = next_packet(v);
    const uint8_t lsd[2] = {0, 0};
    if (!p || p25_encode_ldu(&v->call, v->next_duid, v->voice, lsd, p->data)) return fail(v);
    commit_packet(v, P25_LDU_BYTES);
    v->voice_count = 0;
    v->next_duid ^= P25_DUID_LDU1 ^ P25_DUID_LDU2;
    v->started = 1;
    return 1;
}

int p25_voice_tx_pcm(p25_voice *v, int16_t pcm[160])
{
    if (!v->transmitting || v->stopping || !pcm) return 0;
    return encode_frame(v, pcm);
}

static void append_terminator(p25_voice *v)
{
    p25_voice_packet *p = next_packet(v);
    if (v->terminator_pending && !v->flush_remaining && !v->voice_count && p) {
        if (p25_encode_tdu(&v->call, p->data)) { fail(v); return; }
        commit_packet(v, P25_TDU_BYTES);
        v->terminator_pending = 0;
    }
}

/* Finishing owns the remaining codec tail and LDU padding. A full queue is
 * backpressure here: wait before the ninth encode, so neither codec history
 * nor the remaining-tail count advances until its LDU can be committed. */
static void finish_call(p25_voice *v)
{
    if (!v->stopping) return;
    while (v->flush_remaining || v->voice_count) {
        if (v->voice_count == 8 && !next_packet(v)) return;
        int16_t silence[160] = {0};
        if (!encode_frame(v, silence)) return;
        if (v->flush_remaining) --v->flush_remaining;
    }
    append_terminator(v);
}

int p25_voice_tx_end(p25_voice *v)
{
    if (!v->transmitting || v->stopping) return 0;
    if (!v->started && !v->voice_count) { p25_voice_tx_abort(v); return 1; }
    v->stopping = 1;
    v->flush_remaining = DM32_IMBE_ENCODER_FLUSH_FRAMES;
    v->terminator_pending = 1;
    finish_call(v);
    return !v->fault;
}

size_t p25_voice_tx_read(p25_voice *v, uint8_t *out, size_t capacity)
{
    if (!v->transmitting || !v->started || !out || !capacity) return 0;
    size_t n = 0;
    while (n < capacity) {
        finish_call(v);
        if (!v->transmitting) break;
        if (!v->count) {
            if (v->stopping) { p25_voice_tx_abort(v); break; }
            v->tx_underruns++;
            fail(v);
            break;
        }
        p25_voice_packet *p = &v->queue[v->tail];
        unsigned bit = v->packet_symbol * 2u;
        out[n++] = (uint8_t)((p->data[bit / 8] >> (6 - bit % 8)) & 3u);
        if (++v->packet_symbol == p->bytes * 4u) {
            v->packet_symbol = 0;
            v->tail = (uint8_t)((v->tail + 1) % P25_VOICE_QUEUE);
            v->count--;
        }
    }
    /* A read ending exactly at the TDU boundary is done immediately. */
    if (v->transmitting && v->stopping && !v->count && !v->terminator_pending &&
        !v->flush_remaining && !v->voice_count)
        p25_voice_tx_abort(v);
    return n;
}

int p25_voice_tx_done(const p25_voice *v) { return !v->transmitting; }
