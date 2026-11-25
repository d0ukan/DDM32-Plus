/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus cross-band store and forward (CONFIG_XBAND, docs/plus.md "Cross-band store and forward", ROADMAP item
 * 46). The DM-32UV has one transceiver with one synthesizer (docs/r14.md 46): it cannot receive on one band while it
 * transmits on the other, so a real-time cross-band repeater is impossible. This one records a whole over heard on one
 * frequency, then transmits it on the other: A to B and B to A, the receiver going between the two while nothing is
 * heard. Attended operation only (the screen says so before it starts); the radio's screen is locked while it runs,
 * only the stop key works (ui_xband.c).
 *
 * A mode per side (owner, 2026-11-24; docs/xband-multiproto-study.md steps 2 and 3): A and B each have their own
 * frequency and mode, and an over goes from one side to the other only where the pair is built (xband_pair_ok):
 *   DMR -> DMR, NXDN -> DMR    the 72-bit AMBE+2 air frames as heard (an NXDN voice field is a DMR air frame), 9 bytes
 *                              a 20 ms frame, sent again through the call stack's voice source;
 *   DMR -> FM, NXDN -> FM      the same frames kept, decoded by the stock AMBE+2 codec while it sends and played into
 *                              the FM modulator (the SSTV / APRS tone path, hal_afsk.h tx_pcm), with the TX side's
 *                              CTCSS / DCS sub-tone on the carrier where one is set (owner, 2026-11-25);
 *   FM -> DMR                  the FM receive audio encoded by the stock AMBE+2 codec as the over is heard (radio
 *                              task feeds it the board.afsk receive audio; the audio task runs the encoder), the
 *                              9-byte air frames sent through the call stack's voice source as a DMR over; an FM
 *                              receive side's CTCSS / DCS gates what is relayed (owner, 2026-11-25);
 *   P25 -> P25                 the IMBE voice parameters as heard (88 bits in 11 bytes a frame), sent again through the
 *                              P25 transmitter with its encoder skipped (p25_lab.c).
 * Nothing else: no transcoding (P25 <-> DMR), no transmitter for NXDN, no YSF, no FM -> P25 / NXDN (XB_R_MODE). A pair that
 * works one way only (NXDN -> DMR, DMR -> FM, ...) listens on its source side only. The memory is the P25 worker's
 * lent while the session runs and given back after (p25_lab.c, plus_xband.py): its voice context, audio FIFO and the
 * call replay's frames (~17 KB, about 38 s of AMBE+2 frames; less the FM side's 160 ms of decoded audio), or for a P25
 * session, which needs the worker's voice context, the FIFO and the call replay's frames only (~10 KB, about 18 s).
 *
 * Rules (owner, 2026-11-20): both frequencies in the amateur bands (144-146, 430-440 MHz), never PMR446
 * (446.000-446.200 MHz: no repeaters there, the radio is not type-approved for it), never a repeater input or output
 * (the IARU Region 1 band plan's repeater segments, and a channel whose RX and TX differ), never the satellite
 * segments or the APRS frequency; a callsign (the radio name) for the CW ID, sent on each frequency transmitted on at
 * the first over and then at least every 10 minutes while it relays; at most 3 minutes an over (a longer one is cut,
 * with a beep; the memory cuts it sooner). The portable part (no RTOS): the rules, the state machine, the frame store.
 * Installed as core/radio/xband.{c,h}; the radio core's side is radio_xband.inc. */
#ifndef XBAND_H
#define XBAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XB_FRAME_BYTES	9u		/* one 72-bit AMBE+2 air frame */
#define XB_P25_FRAME_BYTES 11u		/* one IMBE frame's eight parameters, 88 bits (p25_lab.c's call replay packing) */
#define XB_FRAME_MAX	11u
#define XB_FRAME_MS	20u
#define XB_DWELL_MS	1000u		/* listening: each frequency this long, longer while bursts are heard */
#define XB_HOLD_MS	400u		/* bursts heard this recently hold the frequency */
#define XB_OVER_MAX_MS	180000u		/* owner: an over is cut after 3 minutes (the memory cuts it sooner) */
#define XB_MIN_FRAMES	25u		/* shorter than 0.5 s: a key-up, not relayed */
#define XB_REC_QUIET_MS	1500u		/* recording: no frame and no call heard this long ends the over */
#define XB_ID_EVERY_MS	600000u		/* owner: the callsign in CW at least every 10 minutes */
#define XB_SEND_SLACK_MS 4000u		/* a transmission still on this long after its frames should be out: cut */
#define XB_KEY_WAIT_MS	2000u		/* the transmitter off this long after the retune: keyed now */
#define XB_SEGMENTS	3u
#define XB_CALL_MAX	7u		/* a callsign: radio_cwid_callsign's 4 to 7 characters */

enum { XB_MODE_DMR, XB_MODE_FM, XB_MODE_P25, XB_MODE_NXDN, XB_MODES };

/* Why a frequency or a request is refused (0: allowed) */
enum {
	XB_R_OK,
	XB_R_SAME,		/* A and B are the same frequency */
	XB_R_PMR446,		/* 446.000-446.200 MHz */
	XB_R_BAND,		/* outside 144-146 and 430-440 MHz */
	XB_R_REPEATER,		/* a repeater input or output segment */
	XB_R_SATELLITE,		/* a satellite segment */
	XB_R_APRS,		/* the APRS frequency */
	XB_R_MODE,		/* the pair of modes is not built either way (xband_pair_ok) */
	XB_R_CALL,		/* no callsign for the CW ID */
	XB_R_ID,		/* no DMR ID */
	XB_R_OFFSET,		/* a channel whose RX and TX frequencies differ (a repeater channel) */
	XB_R_COUNT
};

/* Phases (xband_view_t.phase) */
enum { XB_P_OFF, XB_P_LISTEN, XB_P_RECORD, XB_P_SEND, XB_P_ID };
/* Why the last session ended (xband_view_t.ended) */
enum { XB_E_NONE, XB_E_STOPPED, XB_E_BATTERY, XB_E_MEMORY, XB_E_REFUSED };
/* Sounds the engine asks for (xband_io_t.beep) */
enum { XB_BEEP_CUT, XB_BEEP_ERROR };

#define XB_CC_ANY	16u		/* rx_cc: every colour code (CODEPLUG_CC_ANY) */
#define XB_TS_ANY	3u		/* rx_ts: either slot (CODEPLUG_TS_ANY) */
#define XB_NAC_ANY	0xF7Eu		/* rx_nac: every NAC (P25's "receive any") */
#define XB_NAC_DEFAULT	0x293u		/* tx_nac before one is set (P25's default NAC) */
#define XB_TG_MAX	16777215u	/* a DMR talkgroup (24 bits) */
#define XB_P25_TG_MAX	65535u

/* A side's filters (what is recorded there) and what is sent there (owner, 2026-11-25). RAM only, as the rest of the
 * request */
typedef struct {
	uint32_t rx_tg;			/* DMR: only overs to this talkgroup (or unit) are recorded; 0 any */
	uint32_t tx_tg;			/* DMR / P25: overs sent there go to this talkgroup; 0 the one heard */
	uint16_t rx_nac;		/* P25: only this NAC is recorded; XB_NAC_ANY any */
	uint16_t tx_nac;		/* P25: sent with this NAC (0-0xF7D) */
	uint8_t rx_cc, rx_ts;		/* DMR: heard on 0-15 / XB_CC_ANY, slot 1, 2 / XB_TS_ANY */
	uint8_t tx_cc, tx_ts;		/* DMR: sent on 0-15, slot 1, 2 */
	/* FM sub-tone (owner, 2026-11-25; tone_type_t mirrored: 0 none, 1 CTCSS, 2 DCS, 3 DCS inverted). RX gates an FM
	 * receive side: only a carrier carrying this tone is relayed (none: carrier squelch). TX adds it to an FM transmit
	 * side's carrier (DMR -> FM), so tone-gated radios and repeaters open. Value: CTCSS Hz x 10, DCS the octal code */
	uint8_t rx_tone_type, tx_tone_type;
	uint16_t rx_tone_value, tx_tone_value;
} xband_side_t;

typedef struct {
	uint32_t hz[2];			/* A, B */
	uint8_t mode[2];		/* XB_MODE_* of A, B */
	xband_side_t side[2];
	uint8_t power;			/* 1 low, 2 mid, 3 high (a power class) */
	char call[XB_CALL_MAX + 1];	/* the CW ID's callsign (radio_cwid_callsign of the radio name) */
} xband_req_t;

typedef struct {
	uint8_t phase;
	uint8_t side;			/* LISTEN / RECORD: the frequency heard; SEND: sent to; ID: identified on */
	uint8_t ended;			/* XB_E_*: why the last session ended */
	bool cut;			/* the over being recorded was cut */
	uint16_t frames;		/* recorded, or left to send */
	uint16_t capacity;		/* frames the memory holds */
	uint8_t relayed, ids;		/* overs sent, CW IDs sent (past 255: from 0 again) */
	uint16_t serial;		/* bumped on every change worth a redraw */
} xband_view_t;

/* --- The rules (any task) --- */
/* The band plan (DOTA's too): the amateur bands, PMR446, the repeater, satellite and APRS segments */
uint8_t xband_check_hz(uint32_t hz);
/* free: the segment refusals lifted (XB_R_REPEATER, XB_R_SATELLITE, XB_R_APRS); outside the bands and PMR446 stay */
uint8_t xband_check_hz_free(uint32_t hz, bool free);
/* "Segments free" (Tools > Cross-band's # key, owner 2026-11-25): xband_check lifts the band plan's segment refusals
 * while it is set. RAM only: off at power-on, never saved. The setter refuses (false) while a session runs */
bool xband_segs_free(void);
bool xband_set_segs_free(bool on);
/* An over heard in mode from can be sent in mode to (the pairs built) */
bool xband_pair_ok(uint8_t from, uint8_t to);
/* The sides an over is heard on: bit 0 A (relayed to B), bit 1 B (relayed to A); 0: the pair is refused */
uint8_t xband_listens(const xband_req_t *r);
/* The bytes a frame of r's session takes: XB_P25_FRAME_BYTES for P25, else XB_FRAME_BYTES */
uint8_t xband_frame_bytes(const xband_req_t *r);
/* Both frequencies (the segments as xband_segs_free says), the pair, the modes, the callsign */
uint8_t xband_check(const xband_req_t *r);
/* A power class (1 low, 2 mid, 3 high) as the board's level */
uint8_t xband_power_level(uint8_t cls, uint8_t power_count);
/* The frames that n bytes hold (of the session's size, XB_FRAME_BYTES before the first) */
uint16_t xband_frames_for(size_t bytes);

/* The listening side's NAC filter passes nac (P25 frames heard; the radio core drops the others) */
bool xband_nac_ok(uint16_t nac);

/* --- The UI (any task) --- */
bool xband_on(void);
/* The request: the UI's form edits it while no session runs (the one copy; the radio core reads it during one) */
xband_req_t *xband_req(void);
/* Start what xband_req() holds (false: refused by xband_check, or one runs already). The radio core takes it at its
 * next request */
bool xband_start(void);
void xband_stop(void);
void xband_view(xband_view_t *v);
/* The radio task's request (radio_task.c): arg 1 a session to take, 0 one to end */
void radio_req_xband(bool on);

/* --- The radio core (radio task) --- */
typedef struct {
	void (*tune)(uint8_t side);			/* receive on hz[side] in its mode (the session's channel) */
	bool (*key)(uint8_t side, uint32_t dst, bool group);	/* transmit the over on hz[side] to dst (false: refused) */
	/* the over's frames are all out of the store: let go once they are on the air (DMR: the call stack ends its
	 * superframe; FM: the decoded audio still queued plays out; P25: the transmitter sends what it holds, then its
	 * terminator). The radio core cuts what is still on the air at the latest XB_SEND_SLACK_MS later */
	void (*unkey)(void);
	bool (*cw)(uint8_t side);			/* the CW ID on hz[side] (false: refused) */
	void (*beep)(uint8_t what);			/* XB_BEEP_* */
} xband_io_t;
typedef struct {
	bool rx;			/* the receiver hears a call now */
	bool activity;			/* a burst heard on our slot and colour code within XB_HOLD_MS */
	bool tx;			/* our transmitter is on (an over or the CW ID) */
	bool battery_critical;
	bool id_wait;			/* the CW ID io->cw accepted is not keyed yet (a P25 channel lets go first) */
} xband_in_t;
/* RREQ_XBAND arg 1: the session to take; false when none waits (then xband_req() is what the core tunes) */
bool xband_begin(uint32_t now);
/* The memory lent for the session (n segments); n 0: none could be had (the session ends, XB_E_MEMORY) */
void xband_memory(uint8_t *const seg[], const size_t len[], uint8_t n, uint32_t now);
/* Every poll while the session runs */
void xband_poll(const xband_io_t *io, const xband_in_t *in, uint32_t now);
/* The radio core is the session's still: false once it ended (then the core gives the memory back and tunes back) */
bool xband_owns_radio(void);
/* The memory is no longer the engine's (the core gives it back) */
void xband_release(void);
/* PTT pressed during a session: refused (the core plays the deny tone) */
void xband_ptt(void);
/* True only while the radio core keys or lets go of a P25 over itself (native_lab.c passes that PTT request on to the
 * P25 transmitter; every other PTT request during a session is the radio core's, refused) */
extern bool xband_native_key;
/* True while the radio core holds a session's channel, its end included (a P25 session's native receiver lets go
 * after the engine is off): native_lab.c refuses PTT on that channel until the user's channel is back */
extern bool xband_held;
/* The voice heard and sent (the radio task: the call stack's sink and source, the NXDN receiver, the P25 worker's frames
 * handed over by the radio core): a call heard and its frames, and the frames to send. A frame is the session's size
 * (xband_frame_bytes) */
void xband_rx_begin(uint32_t src, uint32_t dst, bool group, bool encrypted);
void xband_rx_frame(const uint8_t *frame);
void xband_rx_end(void);
bool xband_tx_next(uint8_t *frame);

#endif
