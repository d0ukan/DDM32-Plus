// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus: every mode in the VFO, the radio core's part (CONFIG_VFO_MODES, radio/vfox.h, docs/plus.md "Every mode in
// the VFO"). Included by radio_sm.c before vfo_use: the VFO's mode and its parameters change here, and the receiver is
// tuned again. A P25 or YSF VFO is then taken by the native receiver like a channel of that mode (native_lab.c
// channel_service), NXDN and dPMR listen on the FM path (radio_nxdn.c), AM through the AM demodulator.
//
// Multi-watch: every ring letter may be a VFO. A and B are upstream's two VFOs (vfo_ch / vfo_other, vfo_idx, saved in
// the settings); C..F are VFOs 2..5, RAM only, kept compact in vfox_x[]. While one of those is in use (vfox_at) it is
// vfo_ch, and VFO vfo_idx waits in vfox_park; vfo_idx itself stays 0 or 1 for everything upstream does with it.

static uint32_t vfox_own_id(const radio_sm_t *sm)
{
#if CONFIG_DMR
	return sm->own_id;
#else
	(void)sm;
	return 0;
#endif
}

#if CONFIG_MULTI_WATCH
static void vfo_use(radio_sm_t *sm, uint8_t idx);

// vfo_ch and its talk target from a slot
static void vfox_slot_take(radio_sm_t *sm, const vfox_slot_t *s)
{
	uint32_t talk;
	bool seeded;
	vfox_slot_get(s, &sm->vfo_ch, &talk, &seeded);
	sm->vfo_seeded = seeded;
	sm->vfo_talk_id = talk & 0xFFFFFFu;
	sm->vfo_talk_private = sm->vfo_talk_id != 0 && (talk & 0x80000000u) != 0;
}

static void vfox_slot_keep(const radio_sm_t *sm, vfox_slot_t *s)
{
	vfox_slot_put(s, &sm->vfo_ch, sm->vfo_talk_id | (sm->vfo_talk_private ? 0x80000000u : 0), sm->vfo_seeded);
}

// The VFO in use: 0..5
static uint8_t vfox_in_use(const radio_sm_t *sm)
{
	return sm->vfox_at >= 2 ? sm->vfox_at : sm->vfo_idx;
}

// A C..F VFO in use goes back to its slot, VFO vfo_idx out of the park: upstream's two VFOs as they were
static void vfox_unpark(radio_sm_t *sm)
{
	if (sm->vfox_at < 2) {
		return;
	}
	vfox_slot_keep(sm, &sm->vfox_x[sm->vfox_at - 2u]);
	vfox_slot_take(sm, &sm->vfox_park);
	sm->vfox_at = 0;
	sm->view.vfo_index = sm->vfo_idx;
}

// VFO idx (0..5) becomes the one in use (dual_tune: each ring letter its own VFO). The caller retunes
static void vfox_use(radio_sm_t *sm, uint8_t idx)
{
	if (idx < 2 || idx >= RADIO_DUAL_SLOTS) {
		vfo_use(sm, idx < 2 ? idx : 0);	// it unparks first
		return;
	}
	if (idx == sm->vfox_at) {
		return;
	}
	vfox_unpark(sm);
	vfox_slot_keep(sm, &sm->vfox_park);
	vfox_slot_take(sm, &sm->vfox_x[idx - 2u]);
	sm->vfox_at = idx;
	sm->view.vfo_index = idx;
}
#endif

// The VFO in use changed: on view (its name kept), tuned again. Off the VFO only kept (saved by persist)
static void vfox_retune(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->view.vfo) {
		changed(sm);
		return;
	}
	sm->view.ch = sm->vfo_ch;
	set_temporary(sm);
	tune(sm, now_ms);	// the call stack starts or stops with the mode; the native receiver follows the view
}

// RREQ_MODE in the VFO to or from a mode upstream's own handler doesn't know (P25, YSF, NXDN, dPMR, AM): that mode's
// defaults (vfox_set_mode). False: FM <-> DMR, which upstream's handler does as before
static bool __attribute__((noinline)) vfox_mode_request(radio_sm_t *sm, int32_t arg, bool busy, uint32_t now_ms)
{
	uint8_t from = vfox_mode_of(&sm->vfo_ch);
	if (arg < 0 || arg >= (int32_t)VFOX_MODES)
		return false;
	uint8_t m = (uint8_t)arg;
	if ((m == RADIO_MODE_FM || m == RADIO_MODE_DMR) && (from == RADIO_MODE_FM || from == RADIO_MODE_DMR))
		return false;
	// Receive-only bands (the air band, broadcast FM) stay FM (AM there by itself)
	if (busy || m == from || !vfox_mode_built(m) || (m != RADIO_MODE_FM && listen_band(sm->vfo_ch.rx_hz)))
		return true;
	vfox_set_mode(&sm->vfo_ch, m, vfox_own_id(sm));
	vfox_retune(sm, now_ms);
	return true;
}

// RREQ_VFO_APPLY: the VFO settings screen's copy of the VFO (radio_req_vfo_apply), checked field by field: what this
// radio can't do stays as it was
static void __attribute__((noinline)) vfox_apply_request(radio_sm_t *sm, bool busy, uint32_t now_ms)
{
	channel_t in;
	if (!radio_vfo_apply_take(&in) || busy)
		return;
	channel_t *v = &sm->vfo_ch;
	if (in.rx_hz != 0 && board_in_band(in.rx_hz, false)) {
		uint32_t tx = in.tx_hz != 0 ? in.tx_hz : in.rx_hz;
		if (tx != in.rx_hz && !board_in_band(tx, true))
			tx = in.rx_hz;
		v->rx_hz = in.rx_hz;
		v->tx_hz = tx;
	}
	uint8_t m = vfox_mode_of(&in);
	if (vfox_mode_built(m) && (m == RADIO_MODE_FM || !listen_band(v->rx_hz))) {
		v->mode = in.mode;
		v->am = in.am;
		v->color_code = in.color_code;
		v->timeslot = in.timeslot;
		v->contact = in.contact;	// the mode's parameters: DMR's ...
		v->listen_list = in.listen_list;
		v->dmr_id = in.dmr_id;
		v->p25_tg = in.p25_tg;		// ... and the native modes' (P25 talkgroup, NAC, unit ID; DG-ID, RAN, CC)
		v->p25_nac = in.p25_nac;
		v->p25_id = in.p25_id;
		v->p25_squelch = in.p25_squelch;
		v->p25_tx_nac_override = in.p25_tx_nac_override;	// the P25 TX NAC (vfox_fix keeps it for P25 only)
	}
	v->bw = in.bw == RADIO_BW_25K ? RADIO_BW_25K : RADIO_BW_12K5;
	if (tones_valid(&in.rx_tone))
		v->rx_tone = in.rx_tone;
	if (tones_valid(&in.tx_tone))
		v->tx_tone = in.tx_tone;
	vfox_fix(v);
	sm->vfo_seeded = true;
	vfox_retune(sm, now_ms);
}
