/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR search in the real radio state machine (docs/plus.md): the board in DMR with the call stack hearing
 * every color code and slot, the sweep to a carrier, the bursts there into the found list, a call played (any
 * talkgroup), the hang and the resume, PTT and other requests ending it without keying, the idle stop, and the
 * P25 channel receiver kept off meanwhile. Included by test_radio.inc with CONFIG_DMR_SEARCH=1; the simulated modem
 * (sim_rx) brings the bursts. */
#include "radio/dmrsearch.h"

static int16_t ds_level(uint32_t hz) { return hz == 433500000u ? -80 : -124; }
#if CONFIG_SEARCH_SURE
/* The owner's beta.7 case (docs/plus.md "Search reliability"): the other radio in the room on 436.275 in DMR direct
 * mode, LOW power: the reading at its ceiling there, its neighbours raised 15-55 dB over the floor, on air 30 ms in 60
 * while it is keyed */
static bool ds_owner_keyed;
static int16_t ds_owner(uint32_t hz)
{
    if (!ds_owner_keyed || (now / 30u) % 2u != 0)
        return -124;
    uint32_t o = hz > 436275000u ? hz - 436275000u : 436275000u - hz;
    return o == 0 ? -45 : o <= 12500u ? -60 : o <= 25000u ? -75 : o <= 37500u ? -90 : o <= 50000u ? -100 : -124;
}
static int16_t (*ds_band)(uint32_t) = ds_level;
#define ds_level(hz) ds_band(hz)
#endif

/* One radio poll with the screen looking: the fake receiver's level follows the frequency it is on */
static void ds_poll(void)
{
    dmrs_view_t v;
    dmrs_snapshot(&v);
    hw.st.rssi_dbm = ds_level(hw.rx_hz);
    run(RADIO_POLL_MS);
}
static void ds_wait(uint32_t ms) { for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) ds_poll(); }

/* A burst from a repeater's outbound (BS sync, TS2, color code 5), as the HR_C7000 glue reports it */
static void ds_burst(uint8_t kind, uint8_t index, const uint8_t *lc)
{
    dmr_burst_t b;
    memset(&b, 0, sizeof b);
    b.kind = kind;
    b.voice_index = index;
    b.sync = kind == DMR_BURST_VOICE && index != 0 ? DMR_SYNC_NONE : DMR_SYNC_BS;
    b.cc = 5;
    b.ts = 2;
    b.crc_ok = true;
    b.in_step = true;
    b.rssi_dbm = -80;
    if (kind == DMR_BURST_VOICE) {
        b.len = 27;
        for (int f = 0; f < 3; f++) memcpy(&b.payload[f * 9], dmr_ambe_silence, 9);
        if (lc != NULL) { b.lc_valid = true; memcpy(b.lc, lc, 9); }
    } else if (kind == DMR_BURST_IDLE) {
        b.len = 12;
    } else {
        b.len = 12;
        dmr_lc_encode_full(lc, kind == DMR_BURST_TERMINATOR_LC ? DMR_RS_MASK_TERMINATOR : DMR_RS_MASK_VOICE_HEADER,
                           b.payload);
    }
    sim_rx(&b);
}

/* A group call TG 2620 from 2620123: headers, `sf` superframes, a terminator when `end` */
static void ds_call(int sf, bool end)
{
    uint8_t lc[9];
    mk_lc(lc, true, 2620, 2620123);
    for (int i = 0; i < 3; i++) { ds_burst(DMR_BURST_VOICE_LC_HEADER, 0, lc); ds_wait(60); }
    for (int s = 0; s < sf; s++)
        for (int i = 0; i < 6; i++) { ds_burst(DMR_BURST_VOICE, (uint8_t)i, i == 5 ? lc : NULL); ds_wait(60); }
    if (end) { ds_burst(DMR_BURST_TERMINATOR_LC, 0, lc); ds_wait(60); }
}

static void ds_sweep_to_hit(void)
{
    for (int i = 0; i < 4000 && dmrs_on() && dmrs_phase() == DMRS_SWEEP; i++) ds_poll();
}

static void test_plus_dmrsearch_sm(void)
{
    dmrs_view_t v;
    setup_codeplug();
    start();
    req(RREQ_ZONE_POS, 0);  /* Simplex, FM 146.520 */
    run(50);
    CHECK(sm.view.ch.mode == RADIO_MODE_FM && hw_mode == RADIO_MODE_FM && !sim.started);
    int tx0 = hw.tx_calls;
    unsigned begins0 = sim.tx_begins;
    radio_req_t r = { .type = RREQ_DMR_SEARCH, .arg = 433000000, .arg2 = 434000000 };
    radio_sm_request(&sm, &r, now);
    /* the board in DMR, the stack hearing every color code and both slots, direct mode framing */
    CHECK(dmrs_on() && hw_mode == RADIO_MODE_DMR && sim.started && sim.cc_filter == -1 && sim.locked_ts == 0 &&
          !sim.cfg.repeater && hw.rx_hz == 433000000u && !hw.audio);

    /* the sweep reaches the carrier at 433.500 and holds it */
    ds_sweep_to_hit();
    CHECK(dmrs_phase() == DMRS_PROBE && dmrs_at_hz() == 433500000u && hw.rx_hz == 433500000u);
    /* a repeater's call on TS2, color code 5, a talkgroup nobody listens to: noted and played */
    ds_call(2, false);
    CHECK(dmrs_snapshot(&v) && v.playing && dmrs_phase() == DMRS_CALL && v.found_n == 1 &&
          v.found[0].hz == 433500000u && v.found[0].cc == 5 && v.found[0].ts_mask == 2u &&
          v.found[0].sync_mask == DMRS_SYNC_BS && v.found[0].dst == 2620 && v.found[0].group &&
          v.found[0].src == 2620123 && v.found[0].calls == 1);
    CHECK(sm.view.state == RS_RX && sm.view.dmr.state == RDMR_RX && sm.view.dmr.call.dst == 2620 && aud.started);
    CHECK(hw.rx_hz == 433500000u && !hw.audio);   /* held there, the FM path muted (DMR voice is PCM) */
    /* the call ends: the hang, then the sweep goes on from the next step */
    uint8_t lc[9];
    mk_lc(lc, true, 2620, 2620123);
    ds_burst(DMR_BURST_TERMINATOR_LC, 0, lc);
    ds_wait(DMRS_HANG_MS - 300u);
    CHECK(dmrs_phase() == DMRS_CALL && hw.rx_hz == 433500000u && sm.view.dmr.state != RDMR_RX);
    ds_wait(600u);
    CHECK(dmrs_phase() == DMRS_SWEEP && dmrs_at_hz() >= 433512500u && hw.rx_hz >= 433512500u);
    CHECK(sm.view.dmr.state == RDMR_IDLE && sm.view.state == RS_IDLE);

    /* a carrier there again with only idle bursts (the repeater's hang): noted, given up after DMRS_LATE_MS */
    ds_sweep_to_hit();
    CHECK(dmrs_at_hz() == 433500000u);
    for (int i = 0; i < 10; i++) { ds_burst(DMR_BURST_IDLE, 0, NULL); ds_wait(60); }
    for (int i = 0; i < 40 && dmrs_phase() != DMRS_SWEEP; i++) ds_poll();
    CHECK(dmrs_phase() == DMRS_SWEEP && dmrs_snapshot(&v) && v.found_n == 1 && v.found[0].calls == 1 && !v.playing);

#if CONFIG_SEARCH_SURE
    {
        /* The owner's case in the real state machine: a typed range 436.000-436.500, the radio in the room on 436.275
         * (direct mode, CC1, TS1, TG 1). The probe goes to 436.275 itself (not a raised neighbour), the call is found
         * and played, the neighbours are passed over after it, and an over that ends during a probe is not skipped */
        radio_req_t own = { .type = RREQ_DMR_SEARCH, .arg = 436000000, .arg2 = 436500000 };
        radio_sm_request(&sm, &own, now);
        ds_band = ds_owner;
        ds_owner_keyed = true;
        ds_sweep_to_hit();
        CHECK(dmrs_phase() == DMRS_PROBE && dmrs_at_hz() == 436275000u && hw.rx_hz == 436275000u &&
              dmrs_snapshot(&v) && v.hits == 1);
        uint8_t olc[9];
        mk_lc(olc, true, 1, 2869001);
        for (int i = 0; i < 6; i++) {
            dmr_burst_t b;
            memset(&b, 0, sizeof b);
            b.kind = DMR_BURST_VOICE;
            b.voice_index = (uint8_t)i;
            b.sync = i == 0 ? DMR_SYNC_MS : DMR_SYNC_NONE;
            b.cc = 1;
            b.ts = 1;
            b.crc_ok = true;
            b.in_step = true;
            b.rssi_dbm = -45;
            b.len = 27;
            for (int f = 0; f < 3; f++) memcpy(&b.payload[f * 9], dmr_ambe_silence, 9);
            if (i == 5) { b.lc_valid = true; memcpy(b.lc, olc, 9); }
            sim_rx(&b);
            ds_wait(60);
        }
        dmrs_view_t ov;
        CHECK(dmrs_snapshot(&ov) && ov.found[0].hz == 436275000u && ov.found[0].cc == 1 &&
              (ov.found[0].sync_mask & DMRS_SYNC_MS));
        /* the over ends: after the hang, on from 436.2875; the raised neighbours above it are its skirt */
        ds_owner_keyed = false;
        for (int i = 0; i < 200 && dmrs_phase() != DMRS_SWEEP; i++) ds_poll();
        CHECK(dmrs_phase() == DMRS_SWEEP && hw.rx_hz == 436287500u);
        ds_owner_keyed = true;
        ds_wait(400);
        CHECK(dmrs_phase() == DMRS_SWEEP && dmrs_snapshot(&ov) && ov.hits == 1);
        /* the next pass meets it again; this over ends 200 ms into the probe, nothing heard: given back at once (the
         * level gone), not skipped, so the pass after finds it again */
        ds_sweep_to_hit();
        CHECK(dmrs_at_hz() == 436275000u);
        ds_wait(200);
        ds_owner_keyed = false;
        uint32_t g0 = now;
        for (int i = 0; i < 100 && dmrs_phase() != DMRS_SWEEP; i++) ds_poll();
        CHECK(dmrs_phase() == DMRS_SWEEP && now - g0 < DMRS_PROBE_MS);
        ds_owner_keyed = true;
        ds_sweep_to_hit();
        CHECK(dmrs_phase() == DMRS_PROBE && dmrs_at_hz() == 436275000u);
        ds_owner_keyed = false;
        ds_band = ds_level;
        own.arg = 0;
        radio_sm_request(&sm, &own, now);   /* stopped: the channel again */
        radio_sm_request(&sm, &r, now);     /* the first search's range again */
        ds_poll();
    }
#endif
    /* PTT while searching: consumed, the search ends, nothing keyed, the channel tuned again (FM) */
    ds_poll();
    radio_req_t ptt = { .type = RREQ_PTT, .arg = 1 };
    CHECK(native_lab_request(&sm, &ptt) == 0);      /* the native owner leaves it to the radio core */
    radio_sm_request(&sm, &ptt, now);
    run(200);
    CHECK(!dmrs_on() && hw.tx_calls == tx0 && sim.tx_begins == begins0 && !sm.ptt && sm.view.state != RS_TX);
    CHECK(hw.rx_hz == 146520000u && hw_mode == RADIO_MODE_FM && !sim.started);
    ptt.arg = 0;
    if (native_lab_request(&sm, &ptt) == 0)         /* as radio_task.c dispatches it */
        radio_sm_request(&sm, &ptt, now);
    run(50);
    CHECK(hw.tx_calls == tx0);

    /* while sweeping, another request ends the search and tunes the channel */
    radio_sm_request(&sm, &r, now);
    ds_poll();
    CHECK(dmrs_on());
    req(RREQ_MONITOR, 1);
    req(RREQ_MONITOR, 0);
    CHECK(!dmrs_on() && hw.rx_hz == 146520000u && hw_mode == RADIO_MODE_FM);
    /* nobody looks (the screen was left): it stops by itself, the channel again */
    radio_sm_request(&sm, &r, now);
    CHECK(dmrs_on());
    hw.st.rssi_dbm = -124;
    run(DMRS_IDLE_MS + 200u);
    CHECK(!dmrs_on() && hw.rx_hz == 146520000u && hw_mode == RADIO_MODE_FM);

    /* on a DMR channel: the stop gives the stack back its channel (color code 2) */
    req(RREQ_ZONE_POS, 3);
    run(50);
    CHECK(sim.started && sim.cc_filter == 2);
    radio_sm_request(&sm, &r, now);
    ds_poll();
    CHECK(dmrs_on() && sim.started && sim.cc_filter == -1 && hw.rx_hz >= 433000000u && hw.rx_hz < 434000000u);
    /* PTT on a DMR channel during the search: still nothing keyed */
    ptt.arg = 1;
    radio_sm_request(&sm, &ptt, now);
    run(300);
    CHECK(!dmrs_on() && hw.tx_calls == tx0 && sim.tx_begins == begins0 && sm.view.state != RS_TX);
    CHECK(sim.started && sim.cc_filter == 2 && hw.rx_hz == 438500000u);
    ptt.arg = 0;
    radio_sm_request(&sm, &ptt, now);
    run(50);
    radio_sm_request(&sm, &r, now);
    r.arg = 0;
    radio_sm_request(&sm, &r, now);   /* the screen's stop */
    CHECK(!dmrs_on() && sim.cc_filter == 2 && hw.rx_hz == 438500000u);
    r.arg = 433000000;

    /* on a P25 channel: the native receiver stays off while searching, PTT goes to the radio core and keys nothing */
    classic_select(false);
    classic_pump();
    classic_pump();
    CHECK(native_lab_owns());                       /* the channel's receiver */
    CHECK(native_lab_request(&sm, &r) == NATIVE_REQUEST_WAIT);  /* it lets go first */
    classic_pump();
    CHECK(native_lab_request(&sm, &r) == 0);
    radio_sm_request(&sm, &r, now);
    CHECK(dmrs_on() && hw_mode == RADIO_MODE_DMR);
    unsigned starts = classic_starts;
    for (int i = 0; i < 10; i++) { ds_poll(); classic_pump(); }
    CHECK(dmrs_on() && !native_lab_owns() && classic_starts == starts);
    ptt.arg = 1;
    CHECK(native_lab_request(&sm, &ptt) == 0);
    radio_sm_request(&sm, &ptt, now);
    CHECK(!dmrs_on() && hw.tx_calls == tx0 && !sm.ptt);
    ptt.arg = 0;
    native_lab_request(&sm, &ptt);
    classic_pump();
    classic_pump();
    CHECK(native_lab_owns());                       /* the P25 channel's receiver again */
#if CONFIG_DMR_EMERGENCY
    /* An emergency during the search (the radio core's, not the native receiver's): native_lab.c's channel_service
     * stops it and tunes the channel again before the radio tick takes the start (as radio_task.c runs them): on a
     * DMR channel the stack back on color code 2 and the hot mic keyed there, on an FM channel the channel again and
     * the emergency failed (SOS / the lone worker fall back). Both orders: the native service first, and the radio
     * tick first (the usual case on the radio: the UI task sets the start while the radio task waits in its queue;
     * the start then waits for the search's stop, emergency_tick) */
    for (int kk = 0; kk < 4; kk++) {
        int k = kk & 1;
        bool tick_first = kk >= 2;
        setup_codeplug();
        start();
        native_lab_init();
        req(RREQ_ZONE_POS, k == 0 ? 3 : 0);
        run(50);
        dmr_emergency_settings_t es;
        dmr_emergency_settings_default(&es);
        es.mode = DMR_EMERGENCY_MODE_CALL;
        es.hot_mic_s = 1;
        dmr_emergency_set_settings(&es);
        run(20);
        r.arg = 433000000;
        radio_sm_request(&sm, &r, now);
        ds_poll();
        CHECK(dmrs_on() && sim.started && sim.cc_filter == -1 && hw.rx_hz >= 433000000u && hw.rx_hz < 434000000u);
        int etx = hw.tx_calls;
        CHECK(dmr_emergency_start());
        if (tick_first)
            run(10);
        for (int i = 0; i < 30; i++) { classic_pump(); run(10); }
        CHECK(!dmrs_on());
        if (k == 0) {
            CHECK(dmr_emergency_state() == DMR_EMERGENCY_HOT_MIC && sm.view.state == RS_TX && hw.tx_calls == etx + 1 &&
                  hw.tx_hz == 438500000u && sim.started && sim.cc_filter == 2);
        } else {
            CHECK(dmr_emergency_state() == DMR_EMERGENCY_FAILED && hw.tx_calls == etx && hw.rx_hz == 146520000u &&
                  hw_mode == RADIO_MODE_FM && !sim.started);
        }
        run(1500);
        dmr_emergency_cancel();
        run(500);
        CHECK(sm.view.state != RS_TX && (k == 1 || (sim.cc_filter == 2 && hw.rx_hz == 438500000u)));
    }
    printf("Plus DMR search: an emergency ends it and tunes the channel again, native or tick first (DMR keyed on CC 2, FM failed)\n");
#endif
    hw.st.rssi_dbm = 0;
    printf("Plus DMR search in the state machine: hear-all stack, sweep, found list, call played, hang, resume, "
           "PTT never keys, stops, P25 channel kept off\n");
}
