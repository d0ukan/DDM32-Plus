/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus air watch (CONFIG_AIR_WATCH, item 44b, docs/plus.md "Round 14: air band and spectrum"), the radio core's
 * part (radio_airwatch.inc), on the real radio state machine, its native P25 receiver and the fake board of
 * radioCoreTest (an AM receiver, the 108-136 MHz receive only band). Included by tools/test_air.py after
 * tests/classic/test_radio.inc, tests/plus/test_mwatch_radio.inc and tests/plus/test_vfox_radio.inc, whose mains run
 * first with it built in. Then: an airfield's four frequencies on the letters' own VFOs and the ring over them (each
 * tuned in AM, its own frequency, TX refused), the AM squelch per letter (a signal holds the ring on its letter, the
 * ring goes on after it), nothing noted while it runs, the end (VFOs A / B and the position back from the settings,
 * C..F's VFOs empty), a power off during it (back as before it), the user's own ring kept on its selected channel,
 * an ATIS letter (held 5 s at most, then passed over 60 s) and the refusals. Owner's beta.7 report (the letters heard
 * in FM): the fake board's demodulator is checked on every stop (AM there, FM back after the end), around a refused
 * PTT, and a priority channel (CONFIG_SCAN_PRIO) is never a stop of the air watch's ring. */

#if CONFIG_SCAN_PRIO
#include "radio/scanprio.h"
#endif

static void air_req(uint8_t type, int32_t arg, uint32_t arg2)
{
    radio_req_t r = { type, arg, arg2 };
    for (int i = 0; i < 20; i++) {
        int h = native_lab_request(&sm, &r);
        if (h == NATIVE_REQUEST_WAIT) { classic_pump(); continue; }
        if (!h) radio_sm_request(&sm, &r, now);
        return;
    }
    CHECK(!"the native receiver never let go");
}
void radio_req_air_vfo(uint8_t idx, uint32_t hz) { air_req(RREQ_AIR_VFO, idx, hz); }
void radio_req_air_end(void) { air_req(RREQ_AIR_END, 0, 0); }

/* The fake board's demodulator as the DM-32's (radio_dm32.c rx_tune): rx() plays FM, rx_am() AM, and AM is refused
 * unless the chip is in FM mode. air_board_am: what the last tune left playing (owner's beta.7 report: the air watch's
 * letters heard in FM) */
static bool air_board_am;
static bool air_r_rx(uint32_t hz, radio_bw_t bw, const subtone_t *t)
{
    air_board_am = false;
    return r_rx(hz, bw, t);
}
static bool air_rx_am(uint32_t hz, radio_bw_t bw)
{
    if (hw_mode != RADIO_MODE_FM)
        return false;
    air_board_am = true;
    return vx_rx_am(hz, bw);
}

/* The receiver on air frequency hz: the view in AM there (an FM VFO in the air band) and the board's AM demodulator
 * playing that frequency */
static bool air_am_on(uint32_t hz)
{
    return sm.view.vfo && sm.view.am && sm.view.ch.mode == RADIO_MODE_FM && sm.view.ch.rx_hz == hz && air_board_am &&
           vx_am_hz == hz;
}

/* mw_poll for AM letters: a signal on air_busy_hz while the board's AM receiver is tuned there (the fake board's AM
 * receiver leaves hw.rx_hz alone), at -60 dBm: the AM squelch reads the strength, not the noise */
static uint32_t air_busy_hz;
static void air_poll(void)
{
    now += RADIO_POLL_MS;
    bool on = air_busy_hz != 0 && sm.view.am && air_board_am && vx_am_hz == air_busy_hz;
    signal(on ? 0 : 120, true);
    hw.st.rssi_dbm = on ? -60 : -125;	/* the default AM squelch (level 5) opens above -70 dBm */
    if (native_lab_owns())
        radio_sm_native_maintenance(&sm, now);
    else
        radio_sm_poll(&sm, now);
    classic_pump();
}

static const uint32_t air_hz[4] = { 118100000, 121900000, 123600000, 119100000 };	/* TWR, GND, ATIS, APP */

/* The user's state: VFO A at 145.500 MHz, then back on the zone's third channel; noted (a few polls) */
static void air_user(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    fake_radio.rx = air_r_rx;
    fake_radio.rx_am = air_rx_am;
    vx_request(RREQ_VFO, 1);
    vx_request(RREQ_VFO_SET, 145500000);
    vx_request(RREQ_VFO, 0);
    req(RREQ_ZONE_POS, 2);
    for (int i = 0; i < 40; i++)
        mw_poll();
    settings_t st;
    settings_read(&st);
    CHECK(!st.vfo && st.zone_pos == 2 && st.vfo_rx_hz == 145500000 && sm.view.ch.rx_hz == mw_hz[2]);
}

/* The air watch as the UI asks for it: the VFOs, then the ring of their letters */
static void air_start(uint8_t n)
{
    radio_dual_ref_t r[RADIO_DUAL_SLOTS];
    for (uint8_t k = 0; k < n; k++) {
        radio_req_air_vfo(k, air_hz[k]);
        r[k] = (radio_dual_ref_t){ true, 0, k };
    }
    radio_req_mwatch(r, n, 0);
}

/* What the user had: noted, untouched */
static bool air_noted_as_before(void)
{
    settings_t st;
    settings_read(&st);
    return !st.vfo && st.zone_pos == 2 && st.vfo_rx_hz == 145500000 && st.vfo_index == 0;
}

static void test_air_ring(void)
{
    air_user();
#if CONFIG_SCAN_PRIO
    /* A priority channel set by the user (the zone's third channel, number 4): never a stop of the air watch's ring
     * (owner's beta.7 report: the letters heard in FM) */
    radio_scan_prio_set(scanprio_pack(4, 0));
#endif
    air_start(4);
    CHECK(sm.air && sm.scan.kind == SCAN_DUAL && sm.dual_n == 4 && sm.dual_sel == 0);
    /* The ring visits each letter on its own frequency, the board's AM demodulator playing it (every stop, not just
     * once), and none transmits; 10 s of it, so a priority stop would have come (every 2 s) */
    unsigned seen = 0, stops = 0;
    for (int i = 0; i < 400; i++) {
        mw_poll();
        CHECK(sm.scan.kind == SCAN_DUAL && sm.view.vfo && sm.dual_at < 4);
        uint8_t k = sm.dual_at;
        CHECK(air_am_on(air_hz[k]));
        CHECK(radio_sm_tx_check(&sm) == TXDENY_AM);
        stops += !((seen >> k) & 1u) || i % 7 == 0;
        seen |= 1u << k;
    }
    CHECK(seen == 0xFu && stops > 4);
    /* PTT on the air watch: refused (TX denied AM), nothing keyed; the ring goes on in AM after it */
    unsigned tx0 = (unsigned)hw.tx_calls;
    air_req(RREQ_PTT, 1, 0);
    for (int i = 0; i < 8; i++)
        mw_poll();
    CHECK(sm.view.state != RS_TX && (unsigned)hw.tx_calls == tx0 && radio_sm_tx_check(&sm) == TXDENY_AM);
    CHECK(sm.view.state != RS_TX_DENIED || sm.view.tx_deny == TXDENY_AM);
    CHECK(air_am_on(air_hz[sm.dual_at]));
    air_req(RREQ_PTT, 0, 0);
    seen = 0;
    for (int i = 0; i < 200 && seen != 0xFu; i++) {
        mw_poll();
        CHECK(sm.scan.kind == SCAN_DUAL && air_am_on(air_hz[sm.dual_at]));
        seen |= 1u << sm.dual_at;
    }
    CHECK(seen == 0xFu && (unsigned)hw.tx_calls == tx0);
    /* A signal on C (123.600): the AM squelch opens on its strength and holds the ring there, in AM */
    air_busy_hz = air_hz[2];
    bool held = false;
    for (int i = 0; i < 400 && !held; i++) {
        air_poll();
        held = sm.dual_at == 2 && sm.view.state == RS_RX;
    }
    CHECK(held && air_am_on(air_hz[2]));
    unsigned stayed = 0;
    for (int i = 0; i < 80; i++) {	/* 2 s: it stays, receiving in AM */
        air_poll();
        stayed += sm.dual_at == 2 && sm.view.state == RS_RX && air_am_on(air_hz[2]);
    }
    CHECK(stayed == 80);
    air_busy_hz = 0;
    bool moved = false;
    for (int i = 0; i < 800 && !moved; i++) {
        air_poll();
        CHECK(air_am_on(air_hz[sm.dual_at]));
        moved = sm.dual_at != 2 && sm.scan.kind == SCAN_DUAL;
    }
    CHECK(moved);
    /* Nothing noted while it runs (the polls above, and a request that would note) */
    vx_request(RREQ_DMR_FOCUS, 0);
    CHECK(air_noted_as_before());
    /* The end: the ring stops, the radio back on the third channel, VFO A as it was, C..F's VFOs empty */
    radio_req_air_end();
    CHECK(!sm.air && sm.scan.kind == SCAN_OFF && !sm.view.vfo && sm.view.zone_pos == 2 && !sm.view.am);
    CHECK(sm.view.ch.rx_hz == mw_hz[2] && hw.rx_hz == mw_hz[2] && !air_board_am);	/* the user's channel, FM again */
#if CONFIG_SCAN_PRIO
    radio_scan_prio_set(0);
#endif
    CHECK(sm.vfo_idx == 0 && sm.vfox_at == 0 && sm.vfo_ch.rx_hz == 145500000 && sm.vfo_ch.mode == RADIO_MODE_FM);
    static const vfox_slot_t empty;
    for (int k = 0; k < 4; k++)
        CHECK(memcmp(&sm.vfox_x[k], &empty, sizeof empty) == 0);
    for (int i = 0; i < 40; i++)
        mw_poll();
    CHECK(air_noted_as_before());
    /* Again, ended by a power off: back as before it too */
    air_start(3);
    CHECK(sm.air && sm.dual_n == 3);
    for (int i = 0; i < 40; i++)
        mw_poll();
    boot();
    CHECK(!sm.air && !sm.view.vfo && sm.view.zone_pos == 2 && sm.vfo_ch.rx_hz == 145500000);
    printf("Plus air watch: four air frequencies on the letters' VFOs, the ring in AM, a signal holds its letter, "
           "nothing noted, the end and a power off back as before\n");
}

/* The user's own ring (B selected, the receiver elsewhere): stopped on B, noted there, back there at the end */
static void test_air_user_ring(void)
{
    air_user();
    CHECK(mw_watch(4, 1));
    for (int i = 0; i < 100 && sm.dual_at != 3; i++)
        mw_poll();
    CHECK(sm.dual_at == 3);
    air_start(2);	/* two frequencies: the pair */
    settings_t st;
    settings_read(&st);
    CHECK(sm.air && !st.vfo && st.zone_pos == 1 && sm.scan.kind == SCAN_DUAL && sm.dual_n == 2);
    radio_req_air_end();
    CHECK(!sm.air && sm.scan.kind == SCAN_OFF && !sm.view.vfo && sm.view.zone_pos == 1);
    CHECK(mw_watch(4, 1) && sm.dual_at == 1);	/* the UI asks for the user's ring again: on B */
    printf("Plus air watch: the user's ring kept on its selected channel\n");
}

/* An ATIS letter (RADIO_AIR_ATIS; the UI puts it last): Esenboga's four, TWR, GND, APP and the ATIS on D. Its
 * recording never stops: the ring holds on D AIR_ATIS_HOLD_MS (5 s), then goes on and passes D over (a quiet stop)
 * for AIR_ATIS_COOL_MS (60 s) while the others are held as always (a 8 s call on A is heard to its end); then D holds
 * again. The end forgets the mark */
static void test_air_atis(void)
{
    static const uint32_t hz[4] = { 118100000, 121900000, 119100000, 123600000 };	/* TWR, GND, APP, ATIS */
    air_user();
    radio_dual_ref_t r[RADIO_DUAL_SLOTS];
    for (uint8_t k = 0; k < 4; k++) {
        radio_req_air_vfo((uint8_t)(k | (k == 3 ? RADIO_AIR_ATIS : 0u)), hz[k]);
        r[k] = (radio_dual_ref_t){ true, 0, k };
    }
    radio_req_mwatch(r, 4, 0);
    CHECK(sm.air && sm.air_atis == 1u << 3 && sm.dual_n == 4 && sm.vfo_ch.rx_hz == hz[0]);
    /* The ATIS on the air for good: D holds the ring, then lets go after 5 s */
    air_busy_hz = hz[3];
    bool held = false;
    for (int i = 0; i < 400 && !held; i++) {
        air_poll();
        held = sm.dual_at == 3 && sm.scan.phase == SCAN_PH_HOLD;
    }
    CHECK(held);
    uint32_t t0 = now;
    while (now - t0 < 10000u && sm.dual_at == 3)
        air_poll();
    uint32_t held_ms = now - t0;
    CHECK(sm.dual_at != 3 && held_ms >= 4900u && held_ms <= 5300u);
    /* 20 s of it: D passed over (never held, never a stop longer than the quiet one), A..C visited again and again */
    unsigned visits[4] = { 0 }, on_d = 0, longest = 0;
    uint8_t at = sm.dual_at;
    t0 = now;
    while (now - t0 < 20000u) {
        air_poll();
        CHECK(!(sm.dual_at == 3 && sm.scan.phase == SCAN_PH_HOLD));
        if (sm.dual_at != at) {
            at = sm.dual_at;
            if (at < 4)
                visits[at]++;
            on_d = 0;
        } else if (at == 3 && ++on_d > longest) {
            longest = on_d;
        }
    }
    CHECK(visits[0] > 20 && visits[1] > 20 && visits[2] > 20 && visits[3] > 20);
    CHECK(longest * RADIO_POLL_MS <= 200u);
    /* A call on A (the tower) meanwhile: held its whole 8 s, as any signal */
    air_busy_hz = hz[0];
    held = false;
    for (int i = 0; i < 400 && !held; i++) {
        air_poll();
        held = sm.dual_at == 0 && sm.scan.phase == SCAN_PH_HOLD;
    }
    CHECK(held);
    unsigned stayed = 0;
    for (int i = 0; i < (int)(8000u / RADIO_POLL_MS); i++) {
        air_poll();
        stayed += sm.dual_at == 0;
    }
    CHECK(stayed == 8000u / RADIO_POLL_MS);
    /* The ATIS again after the cooldown (60 s from its let go): D holds once more */
    air_busy_hz = hz[3];
    held = false;
    for (int i = 0; i < (int)(40000u / RADIO_POLL_MS) && !held; i++) {
        air_poll();
        held = sm.dual_at == 3 && sm.scan.phase == SCAN_PH_HOLD;
    }
    CHECK(held);
    air_busy_hz = 0;
    radio_req_air_end();
    CHECK(!sm.air && sm.air_atis == 0 && sm.scan.kind == SCAN_OFF && air_noted_as_before());
    printf("Plus air watch: an ATIS letter holds the ring %u ms, then is passed over (A..C visited %u/%u/%u times, "
           "D %u, its stops <= %u ms); a tower call held 8 s; the ATIS held again after the cooldown\n",
           (unsigned)held_ms, visits[0], visits[1], visits[2], visits[3], longest * RADIO_POLL_MS);
}

/* Refusals: while transmitting, off the air band, a seventh VFO; an end without an air watch changes nothing */
static void test_air_refused(void)
{
    air_user();
    radio_req_air_vfo(0, 433500000);
    radio_req_air_vfo(6, 118100000);
    CHECK(!sm.air && sm.vfo_ch.rx_hz == 145500000);
    radio_req_air_end();
    CHECK(!sm.view.vfo && sm.view.zone_pos == 2);
    sm.view.state = RS_TX;
    radio_req_air_vfo(0, 118100000);
    CHECK(!sm.air);
    sm.view.state = RS_IDLE;
    printf("Plus air watch: refused while transmitting, off the air band, past F\n");
}

/* Hole hunt B3: a search asked for during the air watch is refused (before: the ring stopped with sm->air still set,
 * the UI's RREQ_AIR_END then ended the search, or nothing was noted until a power cycle); the air watch goes on and
 * ends as usual */
static void test_air_search_refused(void)
{
#if CONFIG_P25_SEARCH
    air_user();
    air_start(4);
    CHECK(sm.air && sm.scan.kind == SCAN_DUAL);
    radio_req_t r = { .type = RREQ_P25_SEARCH, .arg = 433000000, .arg2 = 434000000 };
    radio_sm_request(&sm, &r, now);
    for (int i = 0; i < 20; i++)
        mw_poll();
    CHECK(!p25s_on() && sm.air && sm.scan.kind == SCAN_DUAL && sm.dual_n == 4);
    radio_req_air_end();
    CHECK(!sm.air && sm.scan.kind == SCAN_OFF && air_noted_as_before());
    printf("Plus air watch: a search asked for during it is refused, the air watch goes on and ends as usual\n");
#endif
}

int main(void)
{
    int prior = vfox_radio_main();
    int before = failures;
    test_air_ring();
    test_air_user_ring();
    test_air_atis();
    test_air_refused();
    test_air_search_refused();
    printf("Plus air watch, radio core: %d failures\n", failures - before);
    return prior || failures != before ? 1 : 0;
}
