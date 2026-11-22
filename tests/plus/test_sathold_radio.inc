/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Satellites' auto track, the radio core's part (radio_sathold.inc: RREQ_SAT_HOLD), on the real radio
 * state machine and radioCoreTest's fake board, after upstream's and Classic's radio suites (tests/classic/
 * test_radio.inc) have run again with CONFIG_SATELLITES built in. Included after them by tools/test_classic.py
 * (test_radio_sathold). The satellites' service itself is tests/plus/test_sat_svc.c; here only the hold:
 *  - from a channel: held, the user's state noted (as a power off notes it) and nothing more noted while the VFO is
 *    moved as the tracking moves it (another frequency, FM wide, no tones, simplex), TX refused (RX only) and PTT
 *    denied, a reboot meanwhile coming back on the channel; the end: the channel again, TX allowed, the VFO's own
 *    settings as before;
 *  - from the VFO (a split, a CTCSS, narrow): the end puts all of it back;
 *  - refused while PTT is held, the monitor is open, a call is heard or a scan runs: nothing noted, nothing held;
 *  - the radio core's own save of its settings keeps the satellites' auto track bits in rs_flags. */

void radio_req_sat_hold(bool on) { req(RREQ_SAT_HOLD, on); }
/* The service (radio_task.c's loop, classic_dispatch.inc) is not under test here */
void sat_service(bool vfo, uint32_t rx_hz, uint32_t tx_hz, uint8_t transmitting, bool light, uint32_t now_ms)
{
	(void)vfo; (void)rx_hz; (void)tx_hz; (void)transmitting; (void)light; (void)now_ms;
}

/* The VFO moved as the satellites' tracking moves it (sat_svc.c track_service's set-up), then a few retunes */
static void sh_track_moves(uint32_t hz)
{
	req(RREQ_VFO_SET, (int32_t)hz);
	req(RREQ_MODE, RADIO_MODE_FM);
	req(RREQ_BANDWIDTH, RADIO_BW_25K);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_NONE, 0 }));
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_RX, (subtone_t){ TONE_NONE, 0 }));
	req(RREQ_VFO_TX, (int32_t)hz);
	poll_for(200);
	for (int k = 1; k <= 3; k++) {
		req(RREQ_VFO_SET, (int32_t)(hz - 1000u * (uint32_t)k));
		req(RREQ_VFO_TX, (int32_t)(hz - 1000u * (uint32_t)k));
		poll_for(1000);
	}
}

static void test_sathold_channel(void)
{
	start();
	poll_for(200);
	CHECK(!sm.view.vfo && sm.view.channel_number == 1 && sm.view.ch.rx_hz == 146520000);
	/* the VFO has its own settings (the user tuned it once) */
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 145500000);
	poll_for(200);
	req(RREQ_VFO, 0);
	poll_for(200);
	CHECK(!sm.view.vfo && sm.view.channel_number == 1);
	uint32_t vfo_was = sm.vfo_ch.rx_hz;
	CHECK(vfo_was == 145500000);
	radio_req_sat_hold(true);
	CHECK(radio_sat_held());
	settings_t st;
	settings_read(&st);
	CHECK(st.vfo == 0 && st.channel == 1 && st.vfo_rx_hz == 145500000);
	sh_track_moves(145802000);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == 145799000 && sm.view.ch.tx_hz == 145799000 && sm.view.ch.bw == RADIO_BW_25K);
	/* nothing noted meanwhile: a power off now comes back on the channel */
	settings_read(&st);
	CHECK(st.vfo == 0 && st.channel == 1 && st.vfo_rx_hz == 145500000);
	/* receive only */
	CHECK(radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
	req(RREQ_PTT, 1);
	poll_for(100);
	CHECK(sm.view.state == RS_TX_DENIED);
	req(RREQ_PTT, 0);
	poll_for(200);
	CHECK(sm.view.state != RS_TX && sm.view.state != RS_TX_DENIED);
	/* the end: the channel again, TX allowed, the VFO's own settings as before */
	radio_req_sat_hold(false);
	poll_for(200);
	CHECK(!radio_sat_held());
	CHECK(!sm.view.vfo && sm.view.channel_number == 1 && sm.view.ch.rx_hz == 146520000);
	CHECK(sm.vfo_ch.rx_hz == vfo_was && sm.vfo_ch.tx_hz == vfo_was);
	CHECK(radio_sm_tx_check(&sm) == TXDENY_NONE);
	/* a second end: nothing */
	radio_req_sat_hold(false);
	CHECK(!sm.view.vfo && sm.view.channel_number == 1);
	/* held again, then a reboot (a power cycle: the flag is RAM, so the end is sent here): the channel */
	radio_req_sat_hold(true);
	sh_track_moves(437800000);
	CHECK(sm.view.vfo);
	boot();
	CHECK(!sm.view.vfo && sm.view.channel_number == 1 && sm.vfo_ch.rx_hz == vfo_was);
	radio_req_sat_hold(false);
	CHECK(!radio_sat_held() && !sm.view.vfo && sm.view.channel_number == 1);
	printf("Plus satellites' auto track hold: from a channel, nothing noted, RX only, back on the channel\n");
}

static void test_sathold_vfo(void)
{
	start();
	req(RREQ_VFO, 1);
	req(RREQ_VFO_SET, 145500000);
	req(RREQ_VFO_TX, 145000000);
	req(RREQ_BANDWIDTH, RADIO_BW_12K5);
	req(RREQ_TONE, radio_tone_arg(RADIO_TONE_TX, (subtone_t){ TONE_CTCSS, 885 }));
	poll_for(200);
	channel_t was = sm.view.ch;
	CHECK(sm.view.vfo && was.rx_hz == 145500000 && was.tx_hz == 145000000 && was.tx_tone.value == 885);
	radio_req_sat_hold(true);
	CHECK(radio_sat_held());
	sh_track_moves(145825000);
	CHECK(sm.view.ch.rx_hz == 145822000 && sm.view.ch.tx_tone.type == TONE_NONE && sm.view.ch.bw == RADIO_BW_25K);
	radio_req_sat_hold(false);
	poll_for(200);
	CHECK(sm.view.vfo && sm.view.ch.rx_hz == was.rx_hz && sm.view.ch.tx_hz == was.tx_hz);
	CHECK(sm.view.ch.bw == was.bw && sm.view.ch.mode == was.mode);
	CHECK(sm.view.ch.tx_tone.type == was.tx_tone.type && sm.view.ch.tx_tone.value == was.tx_tone.value);
	CHECK(sm.view.ch.rx_tone.type == was.rx_tone.type);
	printf("Plus satellites' auto track hold: from the VFO, its split, tone and width back\n");
}

/* A refused hold: not held, and nothing noted by it (the settings as just before) */
static void sh_refused(void)
{
	settings_t before, after;
	settings_read(&before);
	radio_req_sat_hold(true);
	settings_read(&after);
	CHECK(!radio_sat_held());
	CHECK(memcmp(&before, &after, sizeof before) == 0);
}

static void test_sathold_refused(void)
{
	start();
	poll_for(200);
	/* PTT held */
	req(RREQ_PTT, 1);
	poll_for(100);
	sh_refused();
	req(RREQ_PTT, 0);
	poll_for(200);
	/* the monitor open */
	req(RREQ_MONITOR, 1);
	poll_for(100);
	sh_refused();
	req(RREQ_MONITOR, 0);
	poll_for(200);
	/* a call heard */
	signal(20, true);
	poll_for(300);
	CHECK(sm.view.state == RS_RX);
	sh_refused();
	signal(120, true);
	poll_for(3000);
#if CONFIG_SCAN
	/* a scan */
	req(RREQ_SCAN, RSCAN_ZONE);
	poll_for(200);
	if (scan_on(&sm.scan)) {
		sh_refused();
		req(RREQ_SCAN, RSCAN_STOP);
		poll_for(200);
	}
#endif
	/* free again: taken */
	CHECK(sm.view.state == RS_IDLE);
	radio_req_sat_hold(true);
	CHECK(radio_sat_held());
	radio_req_sat_hold(false);
	CHECK(!radio_sat_held());
	printf("Plus satellites' auto track hold: refused with PTT, monitor, a call, a scan\n");
}

static void test_sathold_rs_bits(void)
{
	start();
	settings_t *st = settings_begin();
	st->rs_flags = (uint8_t)(st->rs_flags | (2u << SETTINGS_RS_SAT_SHIFT));
	settings_end(now);
	radio_settings_t rs;
	radio_settings_default(&rs);
	rs.roger_beep = true;
	radio_sm_set_settings(&sm, &rs, now);
	settings_t cur;
	settings_read(&cur);
	CHECK((cur.rs_flags & SETTINGS_RS_SAT_AUTO) == (2u << SETTINGS_RS_SAT_SHIFT));
	CHECK((cur.rs_flags & SETTINGS_RS_ROGER_BEEP) != 0 && cur.rs_stored == 1);
	rs.roger_beep = false;
	radio_sm_set_settings(&sm, &rs, now);
	settings_read(&cur);
	CHECK((cur.rs_flags & SETTINGS_RS_SAT_AUTO) == (2u << SETTINGS_RS_SAT_SHIFT) && (cur.rs_flags & SETTINGS_RS_ROGER_BEEP) == 0);
	boot();
	CHECK(sm.set.roger_beep == false);
	printf("Plus satellites' auto track setting: kept by the radio core's own settings save\n");
}

int main(void)
{
	int prior = classic_radio_main();
	int before = failures;
	test_sathold_channel();
	test_sathold_vfo();
	test_sathold_refused();
	test_sathold_rs_bits();
	printf("Plus satellites' auto track hold, radio core: %d failures (upstream's and Classic's suites with it built "
	       "in: %s)\n", failures - before, prior ? "FAILED" : "passed");
	return prior != 0 || failures != before;
}
