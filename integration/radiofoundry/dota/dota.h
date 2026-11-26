/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the session (docs/dota.md 3, 11). One transfer at a time, either role, over FM (AFSK 1200
 * in AX.25 UI frames, CTCSS) or DMR (unconfirmed UDP over the data service). The UI asks (any task) and reads a view;
 * the radio core runs it (dota_poll, the radio task) on the session's own channel and gives it the keying; the
 * receiver's codeplug work runs as storage jobs the UI posts (dota_job). All its working memory is one static buffer.
 * Installed as core/radio/dota.h. */
#ifndef DOTA_H
#define DOTA_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "radio/dota_import.h"
#if CONFIG_DATAPACK
#include "storage/datapack.h"
#endif

#define DOTA_PORT		4101u	/* UDP, both ends (zone sharing has 4100) */
#define DOTA_CTCSS_DEFAULT	885u	/* 88.5 Hz */
/* Both roles start here, so two radios pair without typing (docs/dota.md 12.6): 433.450 MHz, an FM simplex channel of
 * the IARU Region 1 70 cm plan (433.400-433.575), not its calling channel (433.500), not APRS (432.500 / 433.800 /
 * 144.800), no repeater input or output, below the satellite segment (435-438) */
#define DOTA_DEFAULT_HZ		433450000u
#define DOTA_LOW_BATTERY_PCT	10u

enum { DOTA_FM, DOTA_DMR };
enum { DOTA_SEND, DOTA_RECEIVE };

/* What the UI asks for */
typedef struct {
	uint8_t role;		/* DOTA_SEND / DOTA_RECEIVE */
	uint8_t method;		/* DOTA_FM / DOTA_DMR */
	uint32_t hz;
	uint16_t ctcss;		/* FM: Hz x 10 */
	uint8_t cc, ts;		/* DMR: colour code 0-15, slot 1-2 */
	uint32_t peer_id;	/* DMR, sending: the receiver's ID */
	uint8_t power;		/* 0 low, 1 mid, 2 high (dota_power_level - 1) */
	uint16_t code;		/* sending: the receiver's code */
	char call[CODEPLUG_NAME_LEN + 1];	/* our callsign (the radio name) */
	/* Sending: what (DOTA_KIND_*); kind CHANNEL: channel; kind ZONE: zone; SELECTED: dota_selection() */
	uint8_t kind;
	uint16_t channel, zone;
	uint8_t pack;		/* kind PACK: DPK_RADIOID / DPK_AIR / DPK_REF (the pack in force of that type) */
	/* Receiving: where it goes */
	dota_imp_cfg_t imp;
} dota_req_t;

/* Phases the screens show */
enum { DOTA_P_IDLE, DOTA_P_PREP, DOTA_P_WAIT, DOTA_P_RUN, DOTA_P_WRITING, DOTA_P_DONE, DOTA_P_FAILED };
/* Endings (dota_view_t.result) */
enum {
	DOTA_RES_NONE,
	DOTA_RES_SENT,		/* sender: the receiver wrote it all */
	DOTA_RES_DONE,		/* receiver: written */
	DOTA_RES_NO_ANSWER,	/* sender: nothing came back (a wrong code, out of range) */
	DOTA_RES_REFUSED,	/* sender: why in .why (DOTA_R_*) */
	DOTA_RES_STOPPED,	/* the user stopped it here */
	DOTA_RES_PEER_STOPPED,	/* the other side stopped it */
	DOTA_RES_UNSURE,	/* sender: every frame went, no final answer */
	DOTA_RES_INCOMPLETE,	/* receiver: nothing changed (.why: DOTA_RF_*) */
	DOTA_RES_FAILED,	/* a frame could not get through, or the codeplug changed underneath */
	DOTA_RES_FLASH,		/* receiver: a flash write failed */
	DOTA_RES_BUSY,		/* sender: the channel stayed busy */
	DOTA_RES_NOTHING,	/* sender: nothing to send */
};

typedef struct {
	uint8_t role, method, phase, result, why;
	uint8_t pct;
	uint16_t done, total;		/* sending: frames acknowledged / all; receiving: channels in / all */
	uint16_t channels;		/* in the transfer */
	uint16_t code;
	char peer[DOTA_CALL + 1];
	uint16_t ch_added;
	uint8_t zones_added;
	bool keyed;			/* our transmitter is on */
	bool asking;			/* sending: waiting for the answer; receiving: the user is asked about a data pack */
	bool ptt_prompt;		/* PTT was pressed during the transfer: ask "Stop the transfer?" */
	bool pack;		/* a data pack goes (sending) / came (receiving) */
	bool pack_other;		/* receiving: a map pack for another firmware version was offered (refused) */
	bool pack_bad;			/* receiving: the map pack arrived but its CRC or structure did not check */
	uint32_t serial;		/* bumped on every change worth a redraw */
} dota_view_t;

/* --- Any task --- */
bool dota_on(void);
/* The sending selection: a bit per codeplug channel (number - 1), set before dota_start with kind SELECTED */
uint8_t *dota_selection(void);
/* While no session runs: a scratch area (DOTA_SCRATCH bytes) the UI may use; NULL during a session */
#define DOTA_SCRATCH	512u
void *dota_scratch(void);
/* Start (false: one runs already). The radio core takes it at its next tick */
bool dota_start(const dota_req_t *req);
/* The user stops (Back on the progress screen, Yes on the PTT prompt) */
void dota_stop(void);
void dota_view(dota_view_t *v);
/* The PTT prompt answered No: the transfer goes on */
void dota_ptt_prompt_clear(void);
/* The UI's tick: true when a storage job (dota_job) is wanted now */
bool dota_job_wanted(void);
/* The storage job (the start-up task): stage queued frames, commit, undo */
void dota_job(void *ctx);
#if CONFIG_DATAPACK
/* Receiving: the data pack the user is asked about (view.asking; true: *h filled), and the answer (OK yes, Back
 * no): the sender's next HELLO is accepted or refused */
bool dota_pack_offer(dpk_hdr_t *h);
void dota_pack_answer(bool yes);
#endif

/* The radio task's request (radio_task.c): arg 1 a session to take, 0 one to end */
void radio_req_dota(bool on);

/* --- The radio core (radio task) --- */
typedef struct {
	bool (*key)(bool on);		/* FM: the session channel's transmitter on (with the tone) / back to receive */
	void (*beep)(uint8_t beep);	/* a beep_t at the user's beep level */
	bool clear;			/* FM: no carrier heard now */
	bool tone_ok;			/* FM: our CTCSS is heard now */
	bool dmr_ready;			/* DMR: the data service may send now */
	uint8_t battery;		/* radio_batt_t */
} dota_io_t;
/* RREQ_DOTA arg 1: the request taken; false when it can't start (*req: what the core tunes) */
bool dota_begin(dota_req_t *req, uint32_t now);
/* Every poll while dota_on(): the protocol, the transports, the sounds */
void dota_poll(const dota_io_t *io, uint32_t now);
/* The radio core is on the session channel still: false once the session is over (then it tunes back) */
bool dota_owns_radio(void);
/* PTT pressed during a session: refused here, the UI asks */
void dota_ptt(void);
/* FM: a frame the AFSK demodulator found (aprs.c, while dota_fm_listening) */
bool dota_fm_listening(uint32_t rx_hz);
void dota_fm_frame(const uint8_t *frame, size_t n);
/* DMR: a UDP datagram to DOTA_PORT, private to us (dmrDataService.c) */
void dota_dmr_rx(uint32_t src, const uint8_t *payload, int length);

#endif
