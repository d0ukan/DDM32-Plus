/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus cross-band store and forward: the radio core's side (radio/xband.h). Included in core/radio/radio_sm.c
 * before radio_sm_request (tools/plus_xband.py), after DOTA's. While a session runs the radio is on the session's own
 * DMR channel (A or B, the colour code and slot asked for, every talkgroup heard), the user's channel or VFO stays
 * where it is and comes back with reload() at the end; every request but beeps, settings, the volume and power off is
 * dropped; PTT is refused with the deny tone; the transmitter is keyed only for an over the engine sends (through the
 * same path as PTT: ptt_down, so every gate of a user's transmission holds: TX inhibit, the battery, the band, the
 * radio's time-out) and for the CW ID (the CW beacon's keyer). The call stack's voice sink and source are wrapped for
 * the session: what it plays is also recorded, what it sends comes from the recording. The memory is the P25 worker's,
 * lent for the session (p25_lab_lend) and given back before the user's channel returns. */

#include "hal_time.h"
#include "p25/p25_lab.h"

static radio_sm_t *xb_sm;
static struct {
	bool owned : 1;		/* the radio is on the session's channel */
	bool lent : 1;		/* the P25 worker's memory is ours */
	bool asked : 1;		/* the memory was asked for */
	uint8_t waits : 7;	/* polls the P25 worker took to let go of its memory so far */
} xb;
#define XB_LEND_POLLS	(2000u / RADIO_POLL_MS)	/* it has 2 s */

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

/* Receive on A or B: the session channel retuned, every talkgroup on its slot and colour code heard */
static void xb_tune(uint8_t side)
{
	radio_sm_t *sm = xb_sm;
	sm->view.ch.rx_hz = sm->view.ch.tx_hz = xband_req()->hz[side & 1u];
	tune(sm, xb_now);
	set_hear_all_latch(sm, true);
}

/* Transmit the over on A or B to the talkgroup (or unit) it was heard for, as a PTT would */
static bool xb_key(uint8_t side, uint32_t dst, bool group)
{
	radio_sm_t *sm = xb_sm;
	(void)side;
	if (!sm->dmr_running || dst == 0)
		return false;
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

static void xb_unkey(void)
{
	ptt_release(xb_sm, false, xb_now);	/* the call stack ends its superframe with the terminator */
}

/* The callsign in Morse on A or B: the CW beacon's keyer (FM carrier, the settings' speed and tone) */
static bool xb_cw(uint8_t side)
{
	radio_sm_t *sm = xb_sm;
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
	xb_cw_on = true;	/* cw_restore brings the session's DMR channel back */
	if (cw_begin(sm, hz, sm->view.power, text, n, xb_now) == 0) {
		xb_cw_on = false;
		return false;
	}
	return true;
}

static void xb_beep(uint8_t what)
{
	beep(xb_sm, what == XB_BEEP_CUT ? BEEP_TOT_WARN : BEEP_ERROR);
}

/* A session to start: the user's activities stop, the session's channel is tuned (A first) */
static void xb_take(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->hotspot || sm->ptt || sm->view.state == RS_TX || dota_on() || sm->cw_phase != CW_OFF ||
	    !xband_begin(now_ms))
		return;
	const xband_req_t *r = xband_req();
	xb_sm = sm;
	aprs_rf_abort(sm, now_ms);
	roam_cancel(sm, now_ms);
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
	/* RAM only: reload() puts the user's channel back. A plain DMR channel: no talker alias, privacy or SFR role */
	channel_t *ch = &sm->view.ch;
	memset(ch, 0, sizeof *ch);
	strcpy(ch->name, "CROSS");
	ch->rx_hz = ch->tx_hz = r->hz[0];
	ch->mode = RADIO_MODE_DMR;
	ch->bw = RADIO_BW_12K5;
	ch->power = CODEPLUG_POWER_DEFAULT;
	ch->dmo = true;
	ch->color_code = (uint8_t)(r->cc & 15u);
	ch->timeslot = r->ts == 2 ? 2 : 1;
#if CONFIG_DMR_PRIVACY
	sm->privacy_type = DMR_PRIVACY_OFF;
#endif
	sm->view.power = xband_power_level(r->power, board.power_count);
	xb.owned = true;
	xb.lent = xb.asked = false;
	xb.waits = 0;
	dmr_call_set_voice(&xb_sink, &xb_source);
	xb_tune(0);
	changed(sm);
}

/* The session ended (the user's stop, the battery, no memory): off the air at once, the stack's voice and the P25
 * worker's memory given back, the user's channel or VFO as it was */
static void xb_restore(radio_sm_t *sm, uint32_t now_ms)
{
	xb.owned = false;
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
	if (!xb.asked) {
		/* The P25 worker lets go of its memory once it is idle (the session's DMR channel stopped any P25 receive) */
		uint8_t *seg[XB_SEGMENTS];
		size_t len[XB_SEGMENTS];
		uint8_t n = p25_lab_lend(seg, len);
		if (n == 0 && (unsigned)xb.waits < XB_LEND_POLLS) {
			xb.waits++;
			p25_lab_channel_stop();
			return;
		}
		xb.asked = true;
		xb.lent = n != 0;
		xband_memory(seg, len, n, now_ms);
	}
	uint32_t act = 0;
	bool heard = dmr_call_last_activity(&act) && now_ms - act < XB_HOLD_MS;
	xband_in_t in = {
		.rx = sm->view.state == RS_RX,
		.activity = heard,
		.tx = sm->view.state == RS_TX || sm->cw_phase != CW_OFF,
		.battery_critical = sm->batt.level == BATT_CRITICAL,
	};
	static const xband_io_t io = { xb_tune, xb_key, xb_unkey, xb_cw, xb_beep };
	xband_poll(&io, &in, now_ms);
	if (!xband_owns_radio())
		xb_restore(sm, now_ms);
}
