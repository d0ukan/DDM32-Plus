/* SPDX-License-Identifier: MIT */
#ifndef P25_VOICE_H
#define P25_VOICE_H
#include "p25.h"
#include "dm32_imbe.h"

/* Conventional, clear, group voice; half duplex. All calls and the PCM sink
 * run in ONE worker task, never in an ISR. No dynamic allocation. The sink
 * must consume/copy 160 samples before returning. See docs/conventional.md.
 * A transport supplies dibits in on-air order; this does not assert that a
 * particular chip can expose that stream, or implement RF modulation.
 */
typedef void (*p25_pcm_sink)(void *ctx, const int16_t pcm[160]);
enum { P25_VOICE_QUEUE = 3 };
typedef struct {
    uint8_t data[P25_LDU_BYTES];
    uint16_t bytes;
} p25_voice_packet;
typedef struct {
    dm32_imbe_context codec; /* RX and TX share it; every direction change resets it */
    p25_rx framer;
    p25_call call;
    p25_pcm_sink sink;
    void *sink_ctx;
    uint16_t group_filter; /* 0 = all group voice; implementation policy */
    uint16_t active_nac;
    uint16_t active_group;
    uint32_t active_source;
#if defined(CONFIG_P25_EXTRAS) && CONFIG_P25_EXTRAS
    /* DM32 Plus P25 extras: the unit a unit-to-unit call is for (0: group voice), and our own unit ID on this
     * channel (0: none). With a talkgroup filter a unit-to-unit call plays only when it is for own_unit; with
     * group_filter 0 (any talkgroup) every one plays. Set own_unit after p25_voice_init. */
    uint32_t active_target, own_unit;
#endif
#if defined(CONFIG_P25_RXGL) && CONFIG_P25_RXGL
    /* DDM32 Plus P25 RX group list: with a talkgroup filter (group_filter != 0) a group call on one of these
     * rx_group_count talkgroups plays too. The caller's storage, unchanged while the receiver runs; set after
     * p25_voice_init (which clears it) */
    const uint16_t *rx_groups;
    uint8_t rx_group_count;
#endif
    uint16_t voice[9][8];
    p25_voice_packet queue[P25_VOICE_QUEUE];
    uint16_t packet_symbol;
    uint8_t head, tail, count, voice_count, next_duid, flush_remaining;
    uint8_t transmitting, started, stopping, terminator_pending, fault;
    uint8_t crypto_clear, have_group;
#if defined(CONFIG_P25_EXTRAS) && CONFIG_P25_EXTRAS
    uint8_t private_call, emergency; /* the call's LC: unit to unit (LCF 0x03); service options bit 7 */
#endif
    uint32_t rx_frames, rx_voice, rx_muted, rx_erasures, tx_frames, tx_underruns;
    uint32_t (*micros)(void); /* optional task-level profiling, no clock assumption */
    uint32_t fec_max_us, imbe_max_us;
#if defined(CONFIG_CALL_REPLAY) && CONFIG_CALL_REPLAY
    /* Optional: every admitted IMBE frame that decoded (DDM32 Plus call replay keeps the last call) */
    void (*tap)(void *ctx, const uint16_t u[8]);
    void *tap_ctx;
#endif
#if defined(CONFIG_SIGNAL_QUALITY) && CONFIG_SIGNAL_QUALITY
    /* DM32 Plus signal quality: IMBE frames of every voice superframe heard (admitted or not), the bits their FEC
     * corrected, and frames of superframes that failed to decode */
    uint32_t rx_fec_frames, rx_fec_bits, rx_fec_lost;
#endif
#if defined(CONFIG_P25_TRUNK) && CONFIG_P25_TRUNK
    /* DDM32 Plus trunk following: the received dibits also go through the CAI library's control channel receiver
     * tsdu_rx (the caller's storage); each TSDU it completes goes to tsdu (its TSBKs counted in rx_frames), and a TSDU
     * dropped on a failed block goes there as NULL. NULL: no TSDUs framed */
    p25cai_control_receiver *tsdu_rx;
    void (*tsdu)(void *ctx, const p25cai_control *unit);
    void *tsdu_ctx;
#endif
} p25_voice;

void p25_voice_init(p25_voice *v, uint16_t nac, uint16_t group,
                    p25_pcm_sink sink, void *ctx);
#if defined(CONFIG_P25_TRUNK) && CONFIG_P25_TRUNK
/* After p25_voice_init: frame TSDUs too with rx (initialized by the caller, its NAC filter set) and hand each to hook;
 * hook or rx NULL: no longer */
void p25_voice_set_tsdu(p25_voice *v, p25cai_control_receiver *rx,
                        void (*hook)(void *ctx, const p25cai_control *unit), void *ctx);
#endif
/* Clear RX admission/timing after tuning, a discontinuity, or carrier loss.
 * Caller owns the receive hang timer; no time unit is guessed here. */
void p25_voice_rx_reset(p25_voice *v);
void p25_voice_rx_dibit(p25_voice *v, uint8_t dibit);
int p25_voice_tx_begin(p25_voice *v, const p25_call *call);
/* Exactly one 8 kHz / 20 ms microphone block; mutable for the codec.
 * Returns 0 on invalid state, codec failure or queue overflow. Abort/unkey.
 * Producer must be paced by microphone time, not called in an unbounded loop. */
int p25_voice_tx_pcm(p25_voice *v, int16_t pcm[160]);
#if defined(CONFIG_XBAND) && CONFIG_XBAND
/* DDM32 Plus cross-band: one frame's eight IMBE parameters as heard (a P25 receive's, unchanged) in place of a
 * microphone block: the encoder is skipped. Paced and refused as p25_voice_tx_pcm */
int p25_voice_tx_imbe(p25_voice *v, const uint16_t u[8]);
#endif
/* Release: encode the codec tail, then pad the incomplete nine-frame LDU
 * and append a TDU. Queue backpressure defers finishing until tx_read drains;
 * new PCM is rejected after release. Empty PTT is canceled. Task context only. */
int p25_voice_tx_end(p25_voice *v);
/* Up to capacity dibits, including across CAI unit boundaries. Zero means
 * prefill waiting or completely drained; inspect tx_done(). No fabricated
 * idle/CACH/sync is inserted. An active underrun sets fault and ends TX.
 * Initial prefill holds HDU until the first complete LDU is queued. */
size_t p25_voice_tx_read(p25_voice *v, uint8_t *dibits, size_t capacity);
int p25_voice_tx_done(const p25_voice *v);
void p25_voice_tx_abort(p25_voice *v);
#endif
