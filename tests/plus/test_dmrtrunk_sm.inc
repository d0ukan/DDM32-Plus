/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR Tier III trunk following in the real radio state machine and call stack (radio_sm.c, dmr_call.c):
 * Tools > DMR trunk's request starts it from the selected DMR channel (the TSCC: its frequency and color code, both
 * slots, every group heard); CSBKs go to the follower and never to the data service; a grant for the channel's
 * talkgroup moves the receiver to the payload channel with its slot locked, the call plays, PTT is denied (RX only)
 * and keys nothing, the terminator and the hang bring it back, P_CLEAR at once; another request ends it and the
 * channel is received as before; leaving the screen stops it; an FM channel can't start it. Included by
 * tests/classic/test_radio.inc with CONFIG_DMR_TRUNK=1; the simulated modem (sim_rx) brings the bursts. */
#include "radio/dmrtrunk.h"
#include "dmr_csbk_build.h"

static dmrtk_view_t dt_view;

/* One radio poll with the screen looking */
static void dt_poll(void)
{
    dmrtk_snapshot(&dt_view);
    run(RADIO_POLL_MS);
    dmrtk_snapshot(&dt_view);
}
static void dt_wait(uint32_t ms) { for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) dt_poll(); }

/* A burst of the site's outbound (BS sync, color code 2) on slot ts, as the HR_C7000 glue reports it */
static void dt_burst(uint8_t kind, int8_t ts, uint8_t index, const uint8_t *payload, const uint8_t *lc)
{
    dmr_burst_t b;
    memset(&b, 0, sizeof b);
    b.kind = kind;
    b.voice_index = index;
    b.sync = kind == DMR_BURST_VOICE && index != 0 ? DMR_SYNC_NONE : DMR_SYNC_BS;
    b.cc = 2;
    b.ts = ts;
    b.crc_ok = true;
    b.in_step = true;
    b.rssi_dbm = -80;
    if (kind == DMR_BURST_VOICE) {
        b.len = 27;
        for (int f = 0; f < 3; f++) memcpy(&b.payload[f * 9], dmr_ambe_silence, 9);
        if (lc != NULL) { b.lc_valid = true; memcpy(b.lc, lc, 9); }
    } else if (lc != NULL) {
        b.len = 12;
        dmr_lc_encode_full(lc, kind == DMR_BURST_TERMINATOR_LC ? DMR_RS_MASK_TERMINATOR : DMR_RS_MASK_VOICE_HEADER,
                           b.payload);
    } else {
        b.len = 12;
        memcpy(b.payload, payload, 12);
    }
    sim_rx(&b);
}
static void dt_csbk(int8_t ts, const uint8_t b[12])
{
    dt_burst((b[0] & 0x80u) ? DMR_BURST_CSBK : DMR_BURST_MBC_HEADER, ts, 0, b, NULL);
    dt_wait(30);
}

/* A group call TG 201 from 2620123 on TS2: headers, `sf` superframes */
static void dt_call(int sf)
{
    uint8_t lc[9];
    mk_lc(lc, true, 201, 2620123);
    for (int i = 0; i < 3; i++) { dt_burst(DMR_BURST_VOICE_LC_HEADER, 2, 0, NULL, lc); dt_wait(60); }
    for (int s = 0; s < sf; s++)
        for (int i = 0; i < 6; i++) { dt_burst(DMR_BURST_VOICE, 2, (uint8_t)i, NULL, i == 5 ? lc : NULL); dt_wait(60); }
}

static void test_plus_dmrtrunk_sm(void)
{
    uint8_t b[12], c[12];
    setup_codeplug();
    start();
    req(RREQ_ZONE_POS, 3);  /* channel 4: DMR, 438.500, CC 2, TS 1, talk target TG 201, listen list 1 */
    run(50);
    CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sim.started && sim.cc_filter == 2 && hw.rx_hz == 438500000u);
    int tx0 = hw.tx_calls;
    unsigned begins0 = sim.tx_begins;
    radio_req_t r = { .type = RREQ_DMR_TRUNK, .arg = 1 };
    radio_sm_request(&sm, &r, now);
    dt_poll();
    /* the TSCC: the channel's frequency and color code, both slots, no repeater framing */
    CHECK(dmrtk_on() && hw_mode == RADIO_MODE_DMR && sim.started && sim.cc_filter == 2 && sim.locked_ts == 0 &&
          !sim.cfg.repeater && hw.rx_hz == 438500000u && dt_view.follow_tg == 201 && !dt_view.all &&
          dt_view.state == DMRTK_HUNT);
    /* C_ALOHA on TS1: locked */
    cb_aloha(b, cb_syscode(1, 12, 3, 0));
    dt_csbk(1, b);
    CHECK(dt_view.state == DMRTK_CONTROL && dt_view.have_sys && dt_view.net == 12 && dt_view.csbks == 1);
    CHECK(sm.view.state == RS_IDLE && sm.view.dmr.state != RDMR_RX);   /* the TSCC's CSBKs are not shown as data RX */
    /* the plan (MBC: header, then the continuation): LPCN 5 is 438.5125 */
    cb_bcast(b, DMR_ANN_CHAN_FREQ, 0, cb_syscode(1, 12, 3, 0), 0, false);
    cb_chan(c, 5, 443512500u, 438512500u);
    dt_burst(DMR_BURST_MBC_HEADER, 1, 0, b, NULL);
    dt_burst(DMR_BURST_MBC_CONT, 1, 0, c, NULL);
    dt_wait(30);
    CHECK(dt_view.plan_n == 1 && dmrtk_channel_hz(5) == 438512500u);
    /* another group's grant (on TS2 of the TSCC carrier, CSBKs of both slots are heard): listed, the receiver stays */
    cb_grant(b, CSBKO_TV_GRANT, 5, 2, false, 3100, 2620999, true);   /* not in the RX group list */
    dt_csbk(2, b);
    CHECK(hw.rx_hz == 438500000u && dt_view.n == 1 && dt_view.recent[0].flags == DMRTK_G_OTHER);
    /* the channel's talkgroup: the receiver to 438.5125, slot 2 locked as on a repeater */
    unsigned restarts = sim.rx_restarts;
    cb_grant(b, CSBKO_TV_GRANT, 5, 2, false, 201, 2620123, true);
    /* the retune's delay: the grant reaches the follower at the stack's next tick (RADIO_TICK_MS), the receiver moves
     * at the next radio poll (RADIO_POLL_MS) */
    dt_burst(DMR_BURST_CSBK, 1, 0, b, NULL);
    uint32_t heard = now, moved = 0;
    for (int i = 0; i < 40 && !moved; i++) {
        dmrtk_snapshot(&dt_view);
        run(5);
        if (hw.rx_hz == 438512500u) moved = now;
    }
    CHECK(moved != 0 && moved - heard <= RADIO_TICK_MS + RADIO_POLL_MS);
    printf("Plus DMR trunk: grant heard to the receiver on the payload channel in %u ms of radio task time\n",
           (unsigned)(moved - heard));
    dt_wait(30);
    CHECK(dmrtk_ts() == 2 && hw.rx_hz == 438512500u && sim.locked_ts == 2 && sim.cfg.repeater && sim.cc_filter == 2 &&
          sim.rx_restarts > restarts && dt_view.state == DMRTK_VOICE && dt_view.n == 2);
    /* the call plays (Home's caller line has it) */
    dt_call(2);
    CHECK(sm.view.state == RS_RX && sm.view.dmr.state == RDMR_RX && sm.view.dmr.call.dst == 201 &&
          sm.view.dmr.call.src == 2620123 && aud.started && dt_view.state == DMRTK_VOICE);
    /* PTT: denied as on an RX-only channel, nothing keyed, following goes on */
    radio_req_t ptt = { .type = RREQ_PTT, .arg = 1 };
    radio_sm_request(&sm, &ptt, now);
    dt_wait(200);
    CHECK(dmrtk_on() && hw.tx_calls == tx0 && sim.tx_begins == begins0 && sm.view.tx_deny == TXDENY_RX_ONLY &&
          hw.rx_hz == 438512500u);
    ptt.arg = 0;
    radio_sm_request(&sm, &ptt, now);
    dt_wait(50);
    CHECK(dmrtk_on() && hw.tx_calls == tx0);
    /* the terminator, then the hang: back to the TSCC on both slots */
    uint8_t lc[9];
    mk_lc(lc, true, 201, 2620123);
    dt_burst(DMR_BURST_TERMINATOR_LC, 2, 0, NULL, lc);
    dt_wait(DMRTK_HANG_MS - 300u);
    CHECK(hw.rx_hz == 438512500u && dt_view.state == DMRTK_VOICE);
    dt_wait(800);
    CHECK(hw.rx_hz == 438500000u && sim.locked_ts == 0 && !sim.cfg.repeater && dt_view.state == DMRTK_CONTROL &&
          dt_view.calls == 1);
    /* the next call, ended by P_CLEAR on the payload channel: back at once */
    dt_wait(DMRTK_HOLDOFF_MS);
    cb_grant(b, CSBKO_TV_GRANT, 5, 2, false, 201, 2620124, true);
    dt_csbk(1, b);
    CHECK(hw.rx_hz == 438512500u && dt_view.calls == 2);
    dt_call(1);
    cb_grant(c, CSBKO_P_CLEAR, 5, 2, false, 201, 0, true);
    dt_burst(DMR_BURST_CSBK, 2, 0, c, NULL);
    dt_wait(60);
    CHECK(hw.rx_hz == 438500000u && dt_view.state == DMRTK_CONTROL);
    /* nothing went on air all along (no CSBK answered, no data, no registration) */
    CHECK(hw.tx_calls == tx0 && sim.tx_begins == begins0);

    /* another request ends it: the channel again (its slot, its color code) */
    req(RREQ_MONITOR, 1);
    req(RREQ_MONITOR, 0);
    run(50);
    CHECK(!dmrtk_on() && hw.rx_hz == 438500000u && sim.started && sim.cc_filter == 2);
    /* the screen's Left / Right while following: every group, the system kept */
    radio_sm_request(&sm, &r, now);
    dt_poll();
    cb_aloha(b, cb_syscode(1, 12, 3, 0));
    dt_csbk(1, b);
    radio_req_t all = { .type = RREQ_DMR_TRUNK, .arg = 1, .arg2 = 1 };
    radio_sm_request(&sm, &all, now);
    dt_poll();
    CHECK(dmrtk_on() && dt_view.all && dt_view.state == DMRTK_CONTROL);
    /* nobody looks (the screen was left): it stops by itself, the channel again */
    run(DMRTK_IDLE_MS + 200u);
    CHECK(!dmrtk_on() && hw.rx_hz == 438500000u && sim.cc_filter == 2);
    /* the screen's stop */
    radio_sm_request(&sm, &r, now);
    CHECK(dmrtk_on());
    r.arg = 0;
    radio_sm_request(&sm, &r, now);
    CHECK(!dmrtk_on() && hw.rx_hz == 438500000u);
    /* an FM channel can't start it */
    req(RREQ_ZONE_POS, 0);
    run(50);
    r.arg = 1;
    radio_sm_request(&sm, &r, now);
    run(50);
    CHECK(!dmrtk_on() && hw_mode == RADIO_MODE_FM && hw.rx_hz == 146520000u && hw.tx_calls == tx0);
#if CONFIG_DMR_EMERGENCY && CONFIG_P25_CHANNELS
    /* DDM32 Plus (satfix): an emergency while following (receive only: TX refused) ends the following through its own
     * stop (native_lab.c channel_service; the start waits for it, emergency_tick, here with the radio tick first):
     * the channel tuned again and the hot mic keyed on it */
    req(RREQ_ZONE_POS, 3);
    run(50);
    {
        dmr_emergency_settings_t es;
        dmr_emergency_settings_default(&es);
        es.mode = DMR_EMERGENCY_MODE_CALL;
        es.hot_mic_s = 1;
        dmr_emergency_set_settings(&es);
    }
    run(20);
    radio_sm_request(&sm, &r, now);
    dt_poll();
    CHECK(dmrtk_on());
    int etx = hw.tx_calls;
    CHECK(dmr_emergency_start());
    run(10);
    for (int i = 0; i < 30; i++) { classic_pump(); run(10); }
    CHECK(!dmrtk_on() && dmr_emergency_state() == DMR_EMERGENCY_HOT_MIC && sm.view.state == RS_TX &&
          hw.tx_calls == etx + 1 && hw.tx_hz == 438500000u && sim.cc_filter == 2);
    run(1500);
    dmr_emergency_cancel();
    run(500);
    CHECK(sm.view.state != RS_TX && !dmrtk_on());
    printf("Plus DMR trunk: an emergency while following ends it, keyed on the channel\n");
#endif
    printf("Plus DMR trunk in the state machine: TSCC both slots, CSBKs to the follower, grant > payload slot locked > "
           "call played, PTT denied, terminator / P_CLEAR > back, nothing keyed, stops\n");
}
