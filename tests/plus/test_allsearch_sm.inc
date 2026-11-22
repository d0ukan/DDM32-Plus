/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus all digital search in the real radio state machine and native admission (docs/plus.md): the sweep on the
 * FM path, a carrier probed P25 first by the native receiver (any NAC / talkgroup), then DMR with the board in DMR and
 * the call stack hearing everything; P25 frames noted and played, the hang, the give back and the sweep on; a DMR call
 * found after P25 ran out, the board back on FM after it; PTT never keys (during the native probe, the DMR probe, the
 * sweep); the other search's request ends it; the idle stop. Included by test_radio.inc with CONFIG_ALL_SEARCH=1 (and
 * the P25 scan / search fixtures of test_scan.inc, the DMR search's burst helpers of test_dmrsearch_sm.inc). This
 * build has no YSF / NXDN (their probes are the engine's, tests/plus/test_allsearch.c). */

static int16_t as_level(uint32_t hz) { return (hz == 433500000u || hz == 433600000u) ? -80 : -124; }

/* One radio loop as radio_task.c runs it: the native service, the radio poll only while the native side doesn't own the
 * modem; the screen looks (all digital snapshot); the fake receiver's level follows its frequency */
static void as_step(void)
{
    dmrs_view_t v;
    dmrs_snapshot_all(&v);
    hw.st.rssi_dbm = as_level(hw.rx_hz);
    if (native_lab_owns()) {
        now += RADIO_POLL_MS;
    } else {
        run(RADIO_POLL_MS);
    }
    classic_pump();
}
static void as_wait(uint32_t ms) { for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) as_step(); }
static void as_to_probe(void)
{
    for (int i = 0; i < 4000 && dmrs_on() && dmrs_phase() == DMRS_SWEEP; i++) as_step();
}

static void test_plus_allsearch_sm(void)
{
    dmrs_view_t v;
    classic_select(false);
    sm.view.ch.mode = RADIO_MODE_FM;
    ps_frames = 0; ps_nac = 0; ps_tg = 0; ps_src = 0; scan_voice_ms = 0;
    int tx0 = hw.tx_calls;
    unsigned begins0 = sim.tx_begins;
    radio_req_t r = { .type = RREQ_ALL_SEARCH, .arg = 433000000, .arg2 = 434000000 };
    CHECK(native_lab_request(&sm, &r) == 0);
    radio_sm_request(&sm, &r, now);
    CHECK(dmrs_on() && dmrs_kind() == DMRS_KIND_ALL && hw_mode == RADIO_MODE_FM && hw.rx_hz == 433000000u &&
          !hw.audio && !sim.started);

    /* The carrier at 433.500: P25 first, the native receiver takes it with any NAC and talkgroup */
    as_to_probe();
    CHECK(dmrs_phase() == DMRS_PROBE && dmrs_mode() == DMRS_MODE_P25 && dmrs_at_hz() == 433500000u);
    classic_pump(); classic_pump();
    CHECK(native_lab_owns() && classic_live && classic_live->config.frequency_hz == 433500000u &&
          classic_live->state == NT_RX && classic_set_nac == 0xF7E && classic_set_tg == 0);
    /* P25 frames and voice: noted (NAC, talkgroup, source) and played */
    for (int i = 0; i < 40; i++) {
        if (i % 9 == 0) { ps_frames++; ps_frame_ms = now; ps_nac = 0x1C8; ps_tg = 655; ps_src = 1234567; }
        scan_voice_ms = now;
        as_step();
    }
    CHECK(dmrs_snapshot_all(&v) && v.playing && v.mode == DMRS_MODE_P25 && v.found_n == 1 &&
          v.found[0].hz == 433500000u && v.found[0].mode == DMRS_MODE_P25 && v.found[0].cc == 0x1C8 &&
          v.found[0].dst == 655 && v.found[0].src == 1234567 && v.found[0].calls == 1 && native_lab_owns());
    /* the call ends: the hang, then the receiver is handed back and the sweep goes on from the next step */
    as_wait(DMRS_HANG_MS - 300u);
    CHECK(native_lab_owns() && dmrs_phase() == DMRS_CALL);
    for (int i = 0; i < 80 && native_lab_owns(); i++) as_step();
    CHECK(!native_lab_owns() && dmrs_phase() == DMRS_SWEEP);
    as_step();
    CHECK(hw.rx_hz == 433512500u && hw_mode == RADIO_MODE_FM && dmrs_on());

    /* 433.600: P25 runs out (given back), then DMR with the board in DMR, every color code and slot */
    as_to_probe();
    CHECK(dmrs_at_hz() == 433600000u && dmrs_mode() == DMRS_MODE_P25);
    uint32_t t0 = now;
    for (int i = 0; i < 200 && dmrs_mode() == DMRS_MODE_P25; i++) as_step();
    CHECK(dmrs_mode() == DMRS_MODE_DMR && now - t0 >= DMRS_P25_PROBE_MS && now - t0 < DMRS_P25_PROBE_MS + 300u);
    for (int i = 0; i < 20 && hw_mode != RADIO_MODE_DMR; i++) as_step();
    CHECK(!native_lab_owns() && hw_mode == RADIO_MODE_DMR && sim.started && sim.cc_filter == -1 &&
          sim.locked_ts == 0 && !sim.cfg.repeater && hw.rx_hz == 433600000u && !hw.audio);
    ds_call(2, false);   /* a repeater's group call TG 2620 from 2620123 on TS2, color code 5 */
    CHECK(dmrs_snapshot_all(&v) && v.playing && v.mode == DMRS_MODE_DMR && v.found_n == 2 &&
          v.found[0].hz == 433600000u && v.found[0].mode == DMRS_MODE_DMR && v.found[0].cc == 5 &&
          v.found[0].ts_mask == 2u && v.found[0].dst == 2620 && v.found[0].src == 2620123);
    CHECK(sm.view.dmr.state == RDMR_RX && aud.started && !hw.audio);
    uint8_t lc[9];
    mk_lc(lc, true, 2620, 2620123);
    ds_burst(DMR_BURST_TERMINATOR_LC, 0, lc);
    for (int i = 0; i < 200 && dmrs_phase() != DMRS_SWEEP; i++) as_step();
    as_step();
    CHECK(dmrs_phase() == DMRS_SWEEP && hw_mode == RADIO_MODE_FM && !sim.started && hw.rx_hz > 433600000u);
    CHECK(hw.tx_calls == tx0 && sim.tx_begins == begins0);

    /* PTT during the native P25 probe: consumed, the search stops, the receiver handed back, nothing keyed */
    as_to_probe();
    classic_pump(); classic_pump();
    CHECK(native_lab_owns() && dmrs_mode() == DMRS_MODE_P25);
    radio_req_t ptt = { .type = RREQ_PTT, .arg = 1 };
    CHECK(native_lab_request(&sm, &ptt) == 1);
    for (int i = 0; i < 50; i++) { now += 10; classic_pump(); }
    radio_sm_poll(&sm, now += 300);
    CHECK(!dmrs_on() && !native_lab_owns() && hw.tx_calls == tx0 && !sm.ptt && sm.view.state != RS_TX);
    ptt.arg = 0;
    if (native_lab_request(&sm, &ptt) == 0)
        radio_sm_request(&sm, &ptt, now);

    /* PTT during the DMR probe: the radio core's, the search ends, nothing keyed, the FM channel again */
    radio_sm_request(&sm, &r, now);
    for (int k = 0; k < 4 && !(dmrs_mode() == DMRS_MODE_DMR && hw_mode == RADIO_MODE_DMR); k++) {
        as_to_probe();
        for (int i = 0; i < 200 && dmrs_on() && dmrs_phase() != DMRS_SWEEP &&
                        !(dmrs_mode() == DMRS_MODE_DMR && hw_mode == RADIO_MODE_DMR); i++) as_step();
    }
    CHECK(dmrs_mode() == DMRS_MODE_DMR && hw_mode == RADIO_MODE_DMR && !native_lab_owns());
    ptt.arg = 1;
    CHECK(native_lab_request(&sm, &ptt) == 0);
    radio_sm_request(&sm, &ptt, now);
    run(200);
    CHECK(!dmrs_on() && hw.tx_calls == tx0 && sim.tx_begins == begins0 && sm.view.state != RS_TX);
    CHECK(hw_mode == RADIO_MODE_FM && !sim.started);
    ptt.arg = 0;
    if (native_lab_request(&sm, &ptt) == 0)
        radio_sm_request(&sm, &ptt, now);

    /* The DMR search's request while it runs: this one ends, the DMR search starts (its own list) */
    radio_sm_request(&sm, &r, now);
    as_step();
    CHECK(dmrs_on() && dmrs_kind() == DMRS_KIND_ALL && hw_mode == RADIO_MODE_FM);
    radio_req_t d = { .type = RREQ_DMR_SEARCH, .arg = 433000000, .arg2 = 434000000 };
    if (native_lab_request(&sm, &d) == 0)
        radio_sm_request(&sm, &d, now);
    CHECK(dmrs_on() && dmrs_kind() == DMRS_KIND_DMR && hw_mode == RADIO_MODE_DMR && dmrs_snapshot(&v) &&
          v.found_n == 0);
    d.arg = 0;
    radio_sm_request(&sm, &d, now);
    CHECK(!dmrs_on() && hw_mode == RADIO_MODE_FM);

    /* Nobody looks (the screen was left): it stops by itself, the channel again; also while the PC reads it */
    radio_sm_request(&sm, &r, now);
    CHECK(dmrs_on());
    hw.st.rssi_dbm = -124;
    dmrs_peek_t pk;
    for (uint32_t t = 0; t < DMRS_IDLE_MS + 300u && dmrs_on(); t += RADIO_POLL_MS) {
        (void)dmrs_peek_all(0, &pk);
        run(RADIO_POLL_MS);
    }
    CHECK(!dmrs_on() && hw_mode == RADIO_MODE_FM && hw.tx_calls == tx0);
    hw.st.rssi_dbm = 0;
    printf("Plus all digital search in the state machine: FM sweep, native P25 probe found/played/hang/give back, "
           "DMR after P25, PTT never keys, the other search, idle stop\n");
}

/* Hole hunt B3: no search over a dual watch (the ring would be stopped with scan_stop(false), the air watch's flag left
 * set). The all digital, DMR and P25 searches are refused while it runs; the ring goes on. A plain zone scan is still
 * stopped by the search, as before */
static void test_plus_allsearch_watch(void)
{
#if CONFIG_DUAL_WATCH
    int failed = failures;
    classic_select(false);
    sm.view.ch.mode = RADIO_MODE_FM;
    radio_dual_ref_t ref[2] = { { false, 0, 0 }, { false, 0, 1 } };
    radio_req_dual(ref, 0);
    run(300);
    CHECK(scan_on(&sm.scan) && sm.scan.kind == SCAN_DUAL);
    static const uint8_t types[] = { RREQ_ALL_SEARCH, RREQ_DMR_SEARCH,
#if CONFIG_P25_SEARCH
                                     RREQ_P25_SEARCH,
#endif
    };
    for (unsigned i = 0; i < sizeof types; i++) {
        radio_req_t r = { .type = types[i], .arg = 433000000, .arg2 = 434000000 };
        if (native_lab_request(&sm, &r) == 0)
            radio_sm_request(&sm, &r, now);
        as_wait(200);
        CHECK(!dmrs_on() && sm.scan.kind == SCAN_DUAL);
#if CONFIG_P25_SEARCH
        CHECK(!p25s_on());
#endif
    }
    radio_req_dual(NULL, 0);
    run(300);
    CHECK(!scan_on(&sm.scan));
    /* a zone scan: the search stops it and runs, as before */
    radio_req_t z = { .type = RREQ_SCAN, .arg = RSCAN_ZONE };
    radio_sm_request(&sm, &z, now);
    run(300);
    CHECK(scan_on(&sm.scan) && sm.scan.kind == SCAN_ZONE);
    radio_req_t r = { .type = RREQ_ALL_SEARCH, .arg = 433000000, .arg2 = 434000000 };
    if (native_lab_request(&sm, &r) == 0)
        radio_sm_request(&sm, &r, now);
    CHECK(dmrs_on() && !scan_on(&sm.scan));
    r.arg = 0;
    radio_sm_request(&sm, &r, now);
    as_wait(200);
    CHECK(!dmrs_on());
    printf("Plus all digital search: refused over a dual watch (all digital, DMR%s), a zone scan stopped as before: "
           "%d failures\n", CONFIG_P25_SEARCH ? ", P25" : "", failures - failed);
#endif
}
