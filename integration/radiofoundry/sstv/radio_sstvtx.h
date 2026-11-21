/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus SSTV send (CONFIG_SSTV_TX, docs/plus.md "SSTV send"), the radio task's side. Installed as
 * core/radio/radio_sstvtx.{c,h}; radio_sm.c keys the transmitter through radio_sstvtx.inc.
 *
 * Tools > SSTV > OK asks for it (radio_req_sstv_tx). The radio core checks the channel (the TX checks every PTT
 * press gets: TX inhibit, receive only, the band, the battery, busy channel lockout; then FM, simplex, a callsign as
 * the radio name, the time-out timer longer than the picture, nothing else on the air), keys the channel's TX
 * frequency with its tone and power, and hands board.afsk the generator (hal_afsk.h tx_pcm, the APRS / DOTA path:
 * 8 kHz PCM through the HR_C7000's prompt FIFO into the modulator, the microphone muted). The generator's state
 * (sstv_tx_t) comes from the RTOS heap while it sends. PTT, any other request, the screen's Back or leaving the
 * screen stops it at once and unkeys; so does the time-out timer.
 *
 * RAM: about 16 bytes static, sstv_tx_t (48 bytes) from the heap while sending. */
#ifndef RADIO_SSTVTX_H
#define RADIO_SSTVTX_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/sstv_tx.h"

enum { SSTVTX_IDLE, SSTVTX_KEYUP, SSTVTX_SENDING, SSTVTX_SENT, SSTVTX_STOPPED, SSTVTX_REFUSED };

/* Why a start was refused or a transmission ended early */
enum {
	SSTVTX_R_NONE,
	SSTVTX_R_DENY,		/* the channel's TX check (radio_sstvtx_info_t.deny: a TXDENY_* code) */
	SSTVTX_R_BUSY,		/* something else transmits or holds the radio (PTT, APRS, DOTA, cross-band, hotspot) */
	SSTVTX_R_MODE,		/* not an FM channel */
	SSTVTX_R_SPLIT,		/* RX and TX differ (a repeater): simplex only */
	SSTVTX_R_CALL,		/* the radio name is not a callsign */
	SSTVTX_R_TOT,		/* the time-out timer is shorter than the picture (refused), or cut it (ended) */
	SSTVTX_R_NOMEM,		/* no heap for the generator */
	SSTVTX_R_AUDIO,		/* the board has no tone path or refused it */
	SSTVTX_R_KEY,		/* the board would not key */
};

typedef struct {
	uint8_t state, mode, why, deny;
	uint16_t line, lines;	/* transmitted lines sent, of */
	uint16_t seq;		/* changes with every change of the above */
} radio_sstvtx_info_t;

/* UI task: start mode (SSTV_TX_*) on the channel in use, or stop (mode SSTV_TX_MODES) */
void radio_req_sstv_tx(uint8_t mode);

/* Any task */
void radio_sstvtx_info(radio_sstvtx_info_t *out);
uint16_t radio_sstvtx_seq(void);

/* Radio task (radio_sstvtx.inc) */
bool radio_sstvtx_begin(uint8_t mode, const char *call);	/* the generator from the heap; false: none */
uint16_t radio_sstvtx_gen(int16_t *out, uint16_t n, int16_t amp);	/* board.afsk's tx_pcm source (its interrupt) */
void radio_sstvtx_state(uint8_t state);		/* the state, the line reached */
void radio_sstvtx_end(uint8_t state, uint8_t why, uint8_t deny);	/* the generator freed: sent, stopped, refused */

#endif
