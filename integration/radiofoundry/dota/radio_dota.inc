/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the radio core's side (docs/dota.md 11.3). Included in core/radio/radio_sm.c before
 * radio_sm_request (tools/plus_dota.py). While a session runs the radio is on the session's own channel (the user's
 * channel or VFO stays where it is and comes back with reload() at the end), every request but beeps, settings and the
 * volume is dropped, PTT is refused (the UI asks "Stop the transfer?"), and the FM transmitter is keyed only by DOTA. */

#include "hal_time.h"	/* hal_millis (radio_sm.c has it only through the APRS headers) */

/* The band plan (radio/xband.h, built with DOTA): 0 (XB_R_OK) for an amateur frequency that is no repeater, satellite,
 * APRS or PMR446 frequency. The page checks the session frequency; the radio core checks it again before keying */
uint8_t xband_check_hz(uint32_t hz);

static radio_sm_t *dota_sm;
static bool dota_owned;		/* the radio is on the session's channel */

/* FM: key the session channel with its tone, or back to receive on it */
static bool dota_key(bool on)
{
	radio_sm_t *sm = dota_sm;
	uint32_t now = hal_millis();
	const channel_t *ch = &sm->view.ch;
	if (!on) {
		board.radio->tx_off();
		sm->tx_end_ms = now;
		sm->view.state = RS_IDLE;
		tune(sm, now);
		return true;
	}
	if (sm->set.tx_inhibit || !board_in_band(ch->tx_hz, true) || xband_check_hz(ch->tx_hz) != 0) {
		return false;
	}
	sm->view.state = RS_TX;		/* speaker off, red LED on, before keying */
	sm->view.tx_auto = true;
	sm->view.last_tx_ms = now != 0 ? now : 1;
	sm->view.tx_deny = TXDENY_NONE;
	outputs(sm);
	changed(sm);
	if (!KEY_TX(sm, ch->tx_hz, RADIO_BW_12K5, sm->view.power, &ch->tx_tone)) {	/* the radio core's keying gate (RF fixes) */
		sm->view.state = RS_IDLE;
		tune(sm, now);
		return false;
	}
	return true;
}

static void dota_beep(uint8_t b)
{
	beep(dota_sm, b);
}

/* A session to start: the user's activities stop, the session's channel is tuned */
static void dota_take(radio_sm_t *sm, uint32_t now_ms)
{
	dota_req_t r;
	/* (xband_on: a cross-band session keeps the radio, radio_xband.inc) */
	if (sm->hotspot || sm->ptt || sm->view.state == RS_TX || xband_on() || !dota_begin(&r, now_ms)) {
		return;
	}
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
	if (scan_on(&sm->scan)) {
		scan_stop(sm, dual_running(sm), now_ms);
	}
#endif
#if CONFIG_CW_BEACON
	if (beacon.on || beacon.keying) {
		beacon_now_ms = now_ms;
		beacon_stop(sm);
	}
#endif
	channel_t *ch = &sm->view.ch;
	static const subtone_t none = { TONE_NONE, 0 };
	ch->rx_hz = ch->tx_hz = r.hz;
	ch->bw = RADIO_BW_12K5;
	ch->rx_only = false;
	ch->dmo = true;
	if (r.method == DOTA_FM) {
		ch->mode = RADIO_MODE_FM;
		ch->rx_tone = ch->tx_tone = (subtone_t){ TONE_CTCSS, r.ctcss };
	} else {
		ch->mode = RADIO_MODE_DMR;
		ch->rx_tone = ch->tx_tone = none;
		ch->color_code = r.cc;
		ch->timeslot = r.ts;
		ch->contact = 0;
		ch->listen_list = 0;
	}
	/* RAM only: reload() puts the channel's back */
	sm->view.power = dota_power_level((uint8_t)(r.power + 1u), board.power_count);
	dota_owned = true;
	tune(sm, now_ms);
}

/* Before the request switch: true when the request was DOTA's or is dropped during a session */
static bool dota_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	if (req->type == RREQ_DOTA) {
		if (req->arg != 0 && !dota_owned) {
			dota_take(sm, now_ms);
		}
		return true;	/* a stop is the session's (dota_stop): the poll ends it */
	}
	if (!dota_owned) {
		return false;
	}
	switch (req->type) {
	case RREQ_PTT:
		if (req->arg != 0) {
			beep(sm, BEEP_TX_DENY);
			dota_ptt();
		}
		return true;
	case RREQ_BEEP:
	case RREQ_VIBRATE:
	case RREQ_VOLUME:
	case RREQ_VOLUME_OVERRIDE:
	case RREQ_SETTINGS:
	case RREQ_RELOAD:
	case RREQ_POWER_OFF:
		return false;
	default:
		return true;	/* nothing else moves the radio while a transfer runs */
	}
}

/* Every poll, after the readings: the session; true while DOTA keys the FM transmitter (the rest of the poll waits) */
static bool dota_rf_poll(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	dota_sm = sm;
	if (!dota_owned) {
		if (!dota_on()) {
			return false;
		}
		dota_take(sm, now_ms);	/* the request came while the radio was busy: now */
		if (!dota_owned) {
			return false;
		}
	}
	uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
	dota_io_t io = {
		.key = dota_key,
		.beep = dota_beep,
		.clear = st->noise >= radio_sm_squelch_open_below(sq),
		.tone_ok = st->tone_ok,
		.battery = sm->view.battery,
	};
#if CONFIG_DMR_DATA_CORE
	io.dmr_ready = data_can_send(sm) && !dmrDataServiceIsBusy() && !dmrDataServiceIpBusy() && !sm->dmr_keyed &&
		      xband_check_hz(sm->view.ch.tx_hz) == 0;
#endif
	dota_poll(&io, now_ms);
	if (!dota_owns_radio()) {
		dota_owned = false;
		if (sm->view.state == RS_TX) {
			board.radio->tx_off();
			sm->view.state = RS_IDLE;
		}
		reload(sm, now_ms);	/* the user's channel or VFO, as it was */
		return false;
	}
	return sm->view.state == RS_TX && sm->view.ch.mode == RADIO_MODE_FM;
}
