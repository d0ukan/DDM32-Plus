/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus SSTV send: the radio core's side (radio_sstvtx.h, docs/plus.md "SSTV send"). Included in
 * core/radio/radio_sm.c before radio_sm_request (tools/plus_sstvtx.py). Keys the channel in use (FM, simplex: its TX
 * frequency, bandwidth, tone and power, as PTT would) after the checks every PTT press gets and SSTV's own, lets
 * board.afsk play the generator (tx_pcm) and unkeys when it is over. While it runs the poll does nothing else
 * (sstvtx_poll, like APRS's and DOTA's transmissions); the time-out timer still counts and cuts it. Any request but
 * beeps, the volume, settings and retunes stops it at once, PTT included (that press does not key the microphone). */

#define SSTVTX_KEYUP_MS		50u	/* carrier up before the tones (the generator's own 250 ms of silence follow) */
#define SSTVTX_LEVEL		50u	/* the tone path's nominal deviation (hal_afsk.h: about 3 kHz peak at 25 kHz) */
#define SSTVTX_SLACK_MS		3000u	/* past the picture's length: the tone path is stuck, give up */

static uint8_t sstvtx_phase;		/* SSTVTX_IDLE, _KEYUP, _SENDING */
static uint32_t sstvtx_ms, sstvtx_until;	/* the phase's start; give up after */

/* Off the air and back to receive on the channel; how it ended for the screen */
static void sstvtx_end(radio_sm_t *sm, uint8_t state, uint8_t why, uint8_t deny, uint32_t now_ms)
{
	if (sstvtx_phase == SSTVTX_IDLE)
		return;
	if (sstvtx_phase == SSTVTX_SENDING)
		board.afsk->tx_stop();	/* the tones first (the interrupt stops reading the generator) */
	board.radio->tx_off();
	sm->tx_end_ms = now_ms;
	sstvtx_phase = SSTVTX_IDLE;
	sm->view.state = RS_IDLE;
	sm->view.tot_warning = false;
	radio_sstvtx_end(state, why, deny);
	tune(sm, now_ms);
	outputs(sm);
	changed(sm);
}

static void sstvtx_refuse(radio_sm_t *sm, uint8_t why, uint8_t deny)
{
	radio_sstvtx_end(SSTVTX_REFUSED, why, deny);
	beep(sm, BEEP_TX_DENY);
}

static void sstvtx_start(radio_sm_t *sm, uint8_t mode, uint32_t now_ms)
{
	const channel_t *ch = &sm->view.ch;
	char call[RADIO_CWID_CALL_MAX + 1];
	if (sstvtx_phase != SSTVTX_IDLE)
		return;
#if CONFIG_DUAL_WATCH
	/* Dual / multi-watch: the selected letter (the one with PTT), never the letter the ring is on (docs/plus.md
	 * "The TX target"); the watch ends there when it keys, as before */
	if (dual_running(sm) && !sm->ptt && sm->view.state != RS_TX && !sm->hotspot)
		dual_to_selected(sm, now_ms);
#endif
	bool busy = sm->hotspot || sm->ptt || sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED || dota_on() ||
		    xband_on();
#if CONFIG_APRS
	busy = busy || sm->aprs_phase != APRS_RF_OFF;
#endif
#if RADIO_SM_CW
	busy = busy || sm->cw_phase != CW_OFF;
#endif
#if CONFIG_SFR
	busy = busy || sm->sfr_rpt;
#endif
	uint8_t deny = radio_sm_tx_check(sm);
	uint32_t ms = sstv_tx_ms(mode) + SSTVTX_KEYUP_MS;
	if (busy) {
		sstvtx_refuse(sm, SSTVTX_R_BUSY, 0);
	} else if (ch->mode != RADIO_MODE_FM) {
		sstvtx_refuse(sm, SSTVTX_R_MODE, 0);
	} else if (deny != TXDENY_NONE) {
		sstvtx_refuse(sm, SSTVTX_R_DENY, deny);
	} else if (ch->rx_hz != ch->tx_hz) {
		sstvtx_refuse(sm, SSTVTX_R_SPLIT, 0);
	} else if (radio_cwid_callsign(sm->radio_name, call, sizeof call) == 0) {
		sstvtx_refuse(sm, SSTVTX_R_CALL, 0);
	} else if (sm->view.tot_s != 0 && (uint32_t)sm->view.tot_s * 1000u < ms) {
		sstvtx_refuse(sm, SSTVTX_R_TOT, 0);
	} else if (board.afsk == NULL || board.afsk->tx_pcm == NULL || board.afsk->tx_busy == NULL ||
		   board.afsk->tx_stop == NULL) {
		sstvtx_refuse(sm, SSTVTX_R_AUDIO, 0);
	} else if (!radio_sstvtx_begin(mode, call)) {
		sstvtx_refuse(sm, SSTVTX_R_NOMEM, 0);
	} else {
#if CONFIG_SCAN
		if (scan_on(&sm->scan))
			scan_stop(sm, false, now_ms);
#endif
		sm->tot_warned = false;
		sm->view.tot_warning = false;
		sm->view.state = RS_TX;		/* speaker off, red LED on, before keying */
		sm->view.tx_auto = true;
		sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
		sm->view.tx_deny = TXDENY_NONE;
		sm->view.tx_elapsed_ms = 0;
		sm->tx_start_ms = now_ms;
		outputs(sm);
		changed(sm);
		sstvtx_phase = SSTVTX_KEYUP;	/* before tx(): sstvtx_end unkeys from here on */
		sstvtx_ms = now_ms;
		sstvtx_until = now_ms + ms + SSTVTX_SLACK_MS;
		if (!board.radio->tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &ch->tx_tone))
			sstvtx_end(sm, SSTVTX_REFUSED, SSTVTX_R_KEY, 0, now_ms);
		return;
	}
	changed(sm);
}

/* Before the request switch: true when the request was SSTV's own or was the PTT press that stopped it */
static bool sstvtx_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	if (req->type == RREQ_SSTV_TX) {
		if (req->arg >= 0 && req->arg < SSTV_TX_MODES)
			sstvtx_start(sm, (uint8_t)req->arg, now_ms);
		else
			sstvtx_end(sm, SSTVTX_STOPPED, SSTVTX_R_NONE, 0, now_ms);
		return true;
	}
	if (sstvtx_phase == SSTVTX_IDLE || req->type == RREQ_BEEP || req->type == RREQ_VIBRATE ||
	    req->type == RREQ_VOLUME || req->type == RREQ_VOLUME_OVERRIDE || req->type == RREQ_SETTINGS ||
	    req->type == RREQ_RETUNE || req->type == RREQ_DMR_FOCUS)
		return false;
	sstvtx_end(sm, SSTVTX_STOPPED, SSTVTX_R_NONE, 0, now_ms);	/* the user does something: off the air first */
	return req->type == RREQ_PTT && req->arg != 0;	/* the press that stopped it keys nothing */
}

/* Every poll, after the readings: true while it transmits (the rest of the poll waits) */
static bool sstvtx_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (sstvtx_phase == SSTVTX_IDLE)
		return false;
	tx_timer(sm, now_ms);	/* the TX time shown, the time-out's warning */
	uint32_t on = now_ms - sm->tx_start_ms;
	if (sm->view.tot_s != 0 && on >= (uint32_t)sm->view.tot_s * 1000u) {
		sstvtx_end(sm, SSTVTX_STOPPED, SSTVTX_R_TOT, 0, now_ms);
		beep(sm, BEEP_TX_DENY);
		return false;
	}
	if (sm->batt.level == BATT_CRITICAL) {
		sstvtx_end(sm, SSTVTX_STOPPED, SSTVTX_R_DENY, TXDENY_BATTERY, now_ms);
		return false;
	}
	if (sstvtx_phase == SSTVTX_KEYUP) {
		if (now_ms - sstvtx_ms < SSTVTX_KEYUP_MS)
			return true;
		if (!board.afsk->tx_pcm(radio_sstvtx_gen, SSTVTX_LEVEL)) {
			sstvtx_end(sm, SSTVTX_REFUSED, SSTVTX_R_AUDIO, 0, now_ms);
			return false;
		}
		sstvtx_phase = SSTVTX_SENDING;
		sstvtx_ms = now_ms;
		radio_sstvtx_state(SSTVTX_SENDING);
		return true;
	}
	if (!board.afsk->tx_busy()) {
		sstvtx_end(sm, SSTVTX_SENT, SSTVTX_R_NONE, 0, now_ms);
		return false;
	}
	if ((int32_t)(now_ms - sstvtx_until) > 0) {
		sstvtx_end(sm, SSTVTX_STOPPED, SSTVTX_R_AUDIO, 0, now_ms);
		return false;
	}
	radio_sstvtx_state(SSTVTX_SENDING);	/* the line reached, for the screen */
	return true;
}
