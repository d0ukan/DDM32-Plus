/* SPDX-License-Identifier: MIT */
#ifndef P25_LAB_H
#define P25_LAB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void p25_lab_start(void);
int p25_lab_busy(void);
size_t p25_lab_command(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
int p25_lab_live_begin(uint32_t session);
int p25_lab_channel_rx(uint32_t frequency_hz);
void p25_lab_live_up(void);
/* Native RF-switch edge, ISR-safe; only marks the corresponding mic block. */
void p25_lab_live_key(void);
#if CONFIG_P25_CHANNELS
/* Worker input snapshot; changed only after the previous worker has stopped. */
int p25_lab_set_channel(uint16_t rx_nac, uint16_t tx_nac, uint16_t talkgroup, uint32_t radio_id, int selective);
#if CONFIG_P25_RXGL
/* DDM32 Plus P25 RX group list: after p25_lab_set_channel (which clears it), the talkgroups that play besides the
 * channel's own when it is Selective; at most P25_LAB_GROUPS are kept (copied). Refused while the worker runs */
#define P25_LAB_GROUPS 32u
int p25_lab_set_groups(const uint16_t *talkgroups, uint8_t count);
#endif
void p25_lab_channel_stop(void);
int p25_lab_channel_receiving(void);
#if CONFIG_P25_TRUNK
/* DM32 Plus P25 trunk following (radio/p25trunk.h): the next channel receives also frame TSDUs and decode their
 * TSBKs with the CAI library's control channel receiver (p25_lab_set_channel turns it off again). Changed only while
 * the worker is idle */
int p25_lab_set_trunk(int on);
/* The oldest valid TSBK not taken yet (octets 0-9); 0 = none */
int p25_lab_tsbk_pop(uint8_t out[10]);
/* Running counts of the channel receive while it decodes TSDUs: P25 frames (TSBK blocks that held included),
 * terminators, when the last frame came and its NAC, TSDUs dropped on a block that failed (trellis or CRC), TSBKs
 * dropped (none taken in time) */
typedef struct { uint32_t frames, terms, last_ms, bad, dropped; uint16_t nac; } p25_lab_trunk_t;
void p25_lab_trunk_counts(p25_lab_trunk_t *out);
#endif
#if CONFIG_P25_SCAN
/* When the channel receiver last heard P25 voice (hal_millis), 0 = none since it started */
uint32_t p25_lab_channel_last_voice_ms(void);
/* When it last decoded a frame of the channel's own kind, voice for us or not (P25: a valid frame with the
 * channel's NAC; YSF: a frame whose FICH held), 0 = none yet. The receiver starts over 0.5 s after the last valid
 * frame and the voice time above then reads 0 again, soon after every call: the scan keeps its own copy of both
 * (native_lab.c) */
uint32_t p25_lab_channel_last_sync_ms(void);
#endif
#if CONFIG_P25_SEARCH
/* Valid P25 frames heard by the channel receive so far (any NAC, encrypted too); the last one's time and NAC */
uint32_t p25_lab_channel_frames(uint32_t *last_ms, uint16_t *heard_nac);
#endif
#if CONFIG_P25_CALLER
/* The P25 call heard last: talkgroup and source ID (0 = none yet); returns a sequence that changes with them */
uint32_t p25_lab_p25_caller(uint16_t *tg, uint32_t *src);
/* That call as one line of text alternates, longest first (core/ui/font.h): Home's ("P25 TG 101 ID 77285 NAC 659")
 * or, pane true, a dual watch pane's ("TG 101 ID 77285 NAC 659"); the length, 0 (empty) before a call is known */
size_t p25_lab_p25_caller_line(char *out, size_t cap, bool pane);
#if CONFIG_P25_EXTRAS
/* DM32 Plus P25 extras: what the link control said of that call: P25_LAB_EMERGENCY (service options bit 7) and / or
 * P25_LAB_PRIVATE (unit to unit, LCF 0x03; *target the called unit, its talkgroup 0). The caller line then says it:
 * "EMERGENCY TG 101 ID 77285 ..." and "P25 Private 77285 > 1234 NAC ..." (TR, Turkish too). target may be NULL */
enum { P25_LAB_EMERGENCY = 1, P25_LAB_PRIVATE = 2 };
unsigned p25_lab_p25_kind(uint32_t *target);
#endif
#if CONFIG_SIGNAL_QUALITY
/* DM32 Plus signal quality: the bit error rate of the P25 voice heard in about the last second, in 0.1 % steps
 * (radio/sigq.h); -1 when none was heard. Home's caller line ends with it ("BER 1.2%") */
int p25_lab_ber_permille(void);
#endif
#if CONFIG_CALL_LOG
/* DM32 Plus call history: the P25 call heard last. n changes with each new call (a 1.5 s gap, another talkgroup
 * or NAC; 0 = none yet); src fills in when the link control names it; encrypted: no clear-voice superframe in the
 * call's first 0.5 s (its audio is muted) */
typedef struct {
    uint32_t n, src, start_ms, last_ms;
    uint16_t tg, nac;
    bool clear, encrypted;
#if CONFIG_P25_EXTRAS
    /* DM32 Plus P25 extras: unit to unit (target the called unit, tg 0; a new target is a new call); an emergency
     * (any LDU1 of the call had the emergency bit) */
    bool priv, emergency;
    uint32_t target;
#endif
} p25_lab_call_t;
uint32_t p25_lab_call(p25_lab_call_t *out);
#endif
#endif
#if CONFIG_XBAND
/* DDM32 Plus cross-band (radio/xband.h): the worker's voice context, audio FIFO and the call replay's frames as plain
 * memory (up to 3 segments: their addresses and sizes); 0 while the worker runs (or they are lent already). Until they
 * come back no receive, transmission or bench request starts; given back cleared, as at power on (the call replay's
 * last call is gone) */
uint8_t p25_lab_lend(uint8_t *seg[], size_t len[]);
void p25_lab_give_back(void);
#endif
#if CONFIG_CALL_REPLAY
/* The last P25 call kept for replay: its frames (20 ms each, 0 = none) and who it was; start (1) plays it on the
 * P25 channel in use, 0 stops; replaying: playing or about to */
uint16_t p25_lab_replay_info(uint16_t *tg, uint32_t *src, uint16_t *heard_nac);
int p25_lab_replay(int start);
int p25_lab_replaying(void);
#endif
#if CONFIG_YSF
/* The next channel receive is YSF (DM32 Plus): DG-ID 0 = any. Changed only while the worker is idle */
int p25_lab_set_ysf(int on, uint8_t dgid);
/* The YSF call being heard: source and destination callsigns ("" until heard, or no call); returns a sequence
 * that changes whenever they do. Either pointer may be NULL */
uint32_t p25_lab_ysf_caller(char src[11], char dst[11]);
#if CONFIG_ALL_SEARCH
/* DDM32 Plus all digital search: the DG-ID of the last YSF frame whose FICH held */
uint8_t p25_lab_ysf_dgid(void);
#endif
#if CONFIG_YSF_TX
/* DM32 Plus YSF transmit (docs/ysf-nxdn-tx.md): the callsign the next PTT on a YSF channel sends (up to 10
 * characters; "" = none, and then the worker refuses to transmit). The radio task sets it on PTT down, before it
 * asks the worker to transmit; returns 1 */
int p25_lab_set_ysf_tx(const char *call);
#endif
#endif
#endif
#endif
#endif
