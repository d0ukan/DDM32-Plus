/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus multi-watch (docs/plus.md "Multi-watch"): the radio core's ring of 3..6 channels (radio_mwatch.inc) on the
 * real radio state machine, its native P25 receiver and the fake board of radioCoreTest. Included after
 * tests/classic/test_radio.inc (whose main runs first: upstream's radio suite, dual watch included, and the Classic /
 * Plus P25 suites, all with CONFIG_MULTI_WATCH=1, so 2 channels stay dual watch) by tools/test_classic.py.
 * Checks: the order A, B, C...; each quiet stop's dwell; a signal holds its channel, the wait after it looks at the
 * others in turn and comes back, then the ring goes on from the next one; PTT on the selected channel (tuned first)
 * and the ring going on after; a new selection alone doesn't retune; a P25 channel on the FM path and handed to the
 * native receiver; refusals; the stop. Then the ring's period for 2..6 channels (FM, DMR, FM with one P25) against
 * radio_mwatch.h's estimate, printed as a table for docs/plus.md. */
#include "radio/radio_mwatch.h"

static radio_dual_ref_t mw_req_ref[RADIO_DUAL_SLOTS];
static uint8_t mw_req_n, mw_req_sel;
void radio_req_mwatch(const radio_dual_ref_t ref[], uint8_t n, uint8_t sel)
{
    memcpy(mw_req_ref, ref, n * sizeof ref[0]);
    mw_req_n = n;
    mw_req_sel = sel;
    req(RREQ_MWATCH, 0);
}
bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel)
{
    memcpy(ref, mw_req_ref, sizeof mw_req_ref);
    *n = mw_req_n;
    *sel = mw_req_sel;
    return mw_req_n >= 2;
}

/* Zone 0 with six channels (1, 2, 4, 5, 6, 7 of the fixture), all FM or all DMR; p25 > 0: that position is P25 */
static uint32_t mw_hz[6];
static void mw_codeplug(uint8_t mode, int p25)
{
    static const uint16_t numbers[6] = { 1, 2, 4, 5, 6, 7 };
    setup_codeplug();
    zones[0].count = 6;
    for (int i = 0; i < 6; i++) {
        channel_t *c = &chans[numbers[i]];
        zones[0].channels[i] = numbers[i];
        c->mode = mode;
        c->rx_hz = c->tx_hz = 446006250u + 12500u * (uint32_t)i;   /* in band, simplex, no tones */
        c->rx_tone = c->tx_tone = (subtone_t){ TONE_NONE, 0 };
        c->rx_only = false;
        c->color_code = 1;
        c->timeslot = 1;
        mw_hz[i] = c->rx_hz;
        if (i == p25) {
            c->mode = RADIO_MODE_P25;
            c->bw = RADIO_BW_12K5;
            c->p25_nac = 0x293;
            c->p25_tg = 42;
            c->p25_id = 3101234;
        }
    }
}

static void mw_start_radio(void)
{
    start();
    native_lab_init();
    fake_power.switch_on = classic_on;
    fake_power.battery_mv = classic_battery;
    classic_busy = classic_stop = classic_receiving = false;
    classic_starts = classic_restores = 0;
    scan_voice_ms = zs_sync_ms = 0;   /* no frame or voice left from the suites before (the time starts over) */
    now += 1000;
    radio_sm_poll(&sm, now);
}

static bool mw_watch(uint8_t n, uint8_t sel)
{
    radio_dual_ref_t r[RADIO_DUAL_SLOTS];
    for (uint8_t i = 0; i < n; i++)
        r[i] = (radio_dual_ref_t){ false, 0, i };
    radio_req_mwatch(r, n, sel);
    return sm.scan.kind == SCAN_DUAL && sm.dual_n == n && sm.dual_sel == sel;
}

/* A signal on one frequency only (0: none): the receiver hears it while it is tuned there. As radio_task.c: while the
 * native P25 receiver has the channel, the radio core's poll waits (its maintenance only) */
static uint32_t mw_busy_hz;
static void mw_poll(void)
{
    now += RADIO_POLL_MS;
    signal(mw_busy_hz != 0 && hw.rx_hz == mw_busy_hz ? 0 : 120, true);
    if (native_lab_owns())
        radio_sm_native_maintenance(&sm, now);
    else
        radio_sm_poll(&sm, now);
    classic_pump();
}

/* Polls until the receiver is on channel i (false: not within ms) */
static bool mw_until_at(uint8_t i, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += RADIO_POLL_MS) {
        if (sm.dual_at == i && sm.scan.kind == SCAN_DUAL)
            return true;
        mw_poll();
    }
    return sm.dual_at == i;
}

/* The ring's period: the mean time between arrivals on channel 0 over `rounds` rounds */
static uint32_t mw_period(int rounds)
{
    uint8_t prev = sm.dual_at;
    uint32_t first = 0, last = 0;
    int seen = 0;
    for (int i = 0; i < 40000 && seen <= rounds; i++) {
        mw_poll();
        if (sm.dual_at != prev) {
            prev = sm.dual_at;
            if (prev == 0) {
                if (seen == 0)
                    first = now;
                last = now;
                seen++;
            }
        }
    }
    return seen > 1 ? (last - first) / (uint32_t)(seen - 1) : 0;
}

static void test_mwatch_order_dwell(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    mw_busy_hz = 0;
    CHECK(mw_watch(4, 0) && sm.dual_at == 0 && hw.rx_hz == mw_hz[0]);
    /* A, B, C, D, A, B: each stop the dwell, the step at the poll after it */
    uint8_t seq[6];
    uint32_t at[6];
    int k = 0;
    uint8_t prev = sm.dual_at;
    uint32_t t0 = now;
    for (int i = 0; i < 400 && k < 6; i++) {
        mw_poll();
        if (sm.dual_at != prev) {
            prev = sm.dual_at;
            seq[k] = prev;
            at[k++] = now;
            CHECK(hw.rx_hz == mw_hz[prev]);
        }
    }
    CHECK(k == 6 && seq[0] == 1 && seq[1] == 2 && seq[2] == 3 && seq[3] == 0 && seq[4] == 1 && seq[5] == 2);
    for (int i = 1; i < k; i++)
        CHECK(at[i] - at[i - 1] == radio_mw_quiet_ms(4, false));
    CHECK(at[0] - t0 <= radio_mw_quiet_ms(4, false));
    CHECK(sm.view.scan == SCAN_DUAL && sm.view.dual_sel == 0);
    /* The same ring with C selected: the selection only, no retune now */
    int calls = hw.rx_calls;
    uint8_t here = sm.dual_at;
    CHECK(mw_watch(4, 2) && sm.dual_at == here && hw.rx_calls == calls && sm.view.dual_sel == 2);
    printf("Plus multi-watch: order A B C D, %u ms a stop\n", (unsigned)radio_mw_quiet_ms(4, false));
}

static void test_mwatch_hold_wait(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    mw_busy_hz = 0;
    CHECK(mw_watch(5, 0));
    /* A signal on C: the ring stops there while it lasts */
    mw_busy_hz = mw_hz[2];
    CHECK(mw_until_at(2, 2000));
    for (int i = 0; i < 80; i++) {   /* 2 s */
        mw_poll();
        CHECK(sm.dual_at == 2 && hw.rx_hz == mw_hz[2]);
    }
    CHECK(sm.scan.phase == SCAN_PH_HOLD);
    /* It ends: the wait for the reply, with looks at the others in turn (each back to C), then the ring goes on at D */
    mw_busy_hz = 0;
    uint8_t looked = 0;
    uint32_t end = now;
    while (now - end < 1500u && !(sm.dual_at == 3 && !sm.dual_peek)) {
        mw_poll();
        if (sm.dual_peek) {
            CHECK(sm.dual_at != 2 && sm.dual_home == 2);
            looked |= (uint8_t)(1u << sm.dual_at);
        }
    }
    CHECK(sm.dual_at == 3 && !sm.dual_peek);
    CHECK(looked != 0 && (looked & (1u << 2)) == 0);
    CHECK(now - end >= sm.set.dual_delay_ms);
    /* A look that finds a signal marks it Busy and doesn't stay: a signal on E during a wait on A */
    mw_busy_hz = mw_hz[0];
    CHECK(mw_until_at(0, 2000));
    for (int i = 0; i < 20; i++)
        mw_poll();
    mw_busy_hz = mw_hz[4];
    uint32_t w0 = now;
    while (now - w0 < 3000u && (sm.view.dual_busy & (1u << 4)) == 0)
        mw_poll();
    CHECK((sm.view.dual_busy & (1u << 4)) != 0);
    /* ... and the ring holds there once the wait ends */
    CHECK(mw_until_at(4, 3000));
    for (int i = 0; i < 20; i++)
        mw_poll();
    CHECK(sm.dual_at == 4 && sm.scan.phase == SCAN_PH_HOLD && (sm.view.dual_busy & (1u << 4)) == 0);
    mw_busy_hz = 0;
    printf("Plus multi-watch: a signal holds, the wait looks at the others in turn, Busy, the ring goes on\n");
}

static void test_mwatch_ptt(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    mw_busy_hz = 0;
    CHECK(mw_watch(4, 3));   /* D selected */
    CHECK(mw_until_at(1, 2000));
    int tx = hw.tx_calls;
    req(RREQ_PTT, 1);
    CHECK(sm.view.state == RS_TX && hw.tx_calls == tx + 1 && hw.tx_hz == mw_hz[3] && sm.dual_at == 3);
    for (int i = 0; i < 40; i++) {
        mw_poll();
        CHECK(sm.dual_at == 3);
    }
    req(RREQ_PTT, 0);
    CHECK(sm.view.state != RS_TX && sm.scan.kind == SCAN_DUAL && sm.dual_n == 4);
    /* The wait after the transmission on D, then the ring goes on from A */
    CHECK(mw_until_at(0, 3000) && sm.scan.kind == SCAN_DUAL);
    CHECK(mw_until_at(2, 1000));
    printf("Plus multi-watch: PTT on the selected channel, the ring goes on after\n");
}

static void test_mwatch_refuse_stop(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    radio_dual_ref_t r[RADIO_DUAL_SLOTS] = { { false, 0, 0 }, { false, 0, 1 }, { false, 0, 1 } };
    radio_req_mwatch(r, 3, 0);
    CHECK(sm.scan.kind != SCAN_DUAL);   /* a channel twice */
    r[2] = (radio_dual_ref_t){ false, 0, 9 };
    radio_req_mwatch(r, 3, 0);
    CHECK(sm.scan.kind != SCAN_DUAL);   /* past the zone */
    r[2] = (radio_dual_ref_t){ false, 1, 0 };   /* another zone's channel is fine */
    radio_req_mwatch(r, 3, 2);   /* C selected: the receiver stays on A, where it is (one of the ring) */
    CHECK(sm.scan.kind == SCAN_DUAL && sm.dual_n == 3 && sm.dual_at == 0 && sm.view.zone_index == 0);
    CHECK(mw_until_at(2, 1000) && sm.view.zone_index == 1 && sm.view.zone_pos == 0);
    /* Back to the pair (dual watch): the third channel leaves the ring */
    radio_req_dual(r, 1);
    CHECK(sm.scan.kind == SCAN_DUAL && sm.dual_n == 2);
    CHECK(mw_until_at(0, 1000) && mw_until_at(1, 1000) && mw_until_at(0, 1000));
    /* The stop: on the selected channel, every channel's changes for now dropped */
    CHECK(mw_watch(5, 4));
    sm.dual_keep[3].valid = true;
    radio_req_dual(NULL, 0);
    CHECK(sm.scan.kind == SCAN_OFF && sm.view.zone_pos == 4 && hw.rx_hz == mw_hz[4] && !sm.dual_keep[3].valid);
    printf("Plus multi-watch: refusals, back to the pair, the stop\n");
}

/* A P25 channel in the ring: listened on the FM path like the others while quiet; a signal there holds the ring and
 * the native receiver takes it; no voice for us: it gives the channel back and the ring goes on after the wait */
static uint32_t test_mwatch_p25(void)
{
    mw_codeplug(RADIO_MODE_FM, 2);
    mw_start_radio();
    mw_busy_hz = 0;
    CHECK(mw_watch(4, 0));
    CHECK(mw_until_at(2, 1000) && !native_lab_owns());
    CHECK(mw_until_at(3, 1000) && classic_starts == 0);
    /* A signal on C (a P25 transmission without voice for us), 2 s long */
    CHECK(mw_until_at(0, 1000));
    mw_busy_hz = mw_hz[2];
    CHECK(mw_until_at(2, 1000));
    uint32_t arrived = now;
    bool took = false;
    while (now - arrived < 10000u && !(sm.dual_at == 3 && !sm.dual_peek)) {	/* the looks in the wait come back to C */
        mw_poll();
        took = took || native_lab_owns();
        if (now - arrived >= 2000u)
            mw_busy_hz = 0;
    }
    uint32_t stayed = now - arrived;
    CHECK(took && sm.dual_at == 3 && !native_lab_owns() && sm.scan.kind == SCAN_DUAL);
#if CONFIG_MWATCH_FAST
    /* The faster ring: no frame of ours in the zone scan's probe time, given back then, and passed over meanwhile */
    CHECK(stayed <= radio_mw_p25_extra_ms(4, sm.set.dual_delay_ms) + 1500u);
#else
    CHECK(stayed >= RADIO_MW_P25_HOLD_MS && stayed <= radio_mw_p25_extra_ms(4, sm.set.dual_delay_ms) + 1500u);
#endif
    printf("Plus multi-watch: P25 channel on the FM path, handed to the native receiver, %u ms with a voiceless signal\n",
           (unsigned)stayed);
    return stayed;
}

static void test_mwatch_period(uint32_t p25_stayed)
{
    static const char *const kinds[3] = { "FM", "DMR", "FM + 1 P25" };
    printf("Plus multi-watch ring period (host model, quiet channels; estimate radio_mw_ring_ms):\n");
    printf("  n   FM      DMR     FM+P25  estimate\n");
    for (uint8_t n = 2; n <= 6; n++) {
        uint32_t ms[3];
        for (int kind = 0; kind < 3; kind++) {
            mw_codeplug(kind == 1 ? RADIO_MODE_DMR : RADIO_MODE_FM, kind == 2 ? n - 1 : -1);
            mw_start_radio();
            mw_busy_hz = 0;
            CHECK(mw_watch(n, 0));
            ms[kind] = mw_period(4);
            uint32_t est = radio_mw_ring_ms(n, kind == 1 ? n : 0);
            if (ms[kind] != est)
                printf("FAIL %s n=%u: %u ms, estimate %u\n", kinds[kind], n, (unsigned)ms[kind], (unsigned)est);
            CHECK(ms[kind] == est);
        }
        printf("  %u   %-6u  %-6u  %-6u  %u / %u ms\n", n, (unsigned)ms[0], (unsigned)ms[1], (unsigned)ms[2],
               (unsigned)radio_mw_ring_ms(n, 0), (unsigned)radio_mw_ring_ms(n, n));
    }
    printf("  P25 channel with a voiceless signal: +%u ms measured (estimate +%u ms)\n", (unsigned)p25_stayed,
           (unsigned)radio_mw_p25_extra_ms(4, RADIO_DUAL_DELAY_MS));
}

/* DDM32 Plus round 15 item 3 (docs/plus.md "The multi-watch ring"): how fast the ring catches a call, in this build
 * (the faster ring with CONFIG_MWATCH_FAST, else as before). A signal starts on the last channel at a random moment of
 * the round (40 times; a fresh ring each time, all channels FM): the time until the ring holds there. Then a P25
 * channel in a ring of 6 with a carrier that brings no P25 frame for us (an analog signal on its frequency) for 20 s:
 * how many rounds the other channels still get. Printed for docs/plus.md; checked against the estimate */
static void test_mwatch_catch(void)
{
    static const uint8_t ns[3] = { 3, 4, 6 };
    unsigned seed = 7;
    printf("Plus multi-watch catch time (host model, FM; a call starting at a random moment):\n");
    for (int i = 0; i < 3; i++) {
        uint8_t n = ns[i];
        uint32_t sum = 0, worst = 0;
        for (int k = 0; k < 40; k++) {
            mw_codeplug(RADIO_MODE_FM, -1);
            mw_start_radio();
            mw_busy_hz = 0;
            CHECK(mw_watch(n, 0));
            seed = seed * 1103515245u + 12345u;
            uint32_t polls = 8u + (seed >> 16) % (radio_mw_ring_ms(n, 0) / RADIO_POLL_MS + 1u);
            for (uint32_t q = 0; q < polls; q++)
                mw_poll();
            mw_busy_hz = mw_hz[n - 1];
            uint32_t t0 = now;
            while (now - t0 < 5000u && !(sm.dual_at == n - 1u && sm.scan.phase == SCAN_PH_HOLD))
                mw_poll();
            uint32_t took = now - t0;
            sum += took;
            worst = took > worst ? took : worst;
        }
        mw_busy_hz = 0;
        printf("  n=%u: mean %u ms, worst %u ms (ring %u ms)\n", n, (unsigned)(sum / 40u), (unsigned)worst,
               (unsigned)radio_mw_ring_ms(n, 0));
        CHECK(worst <= radio_mw_ring_ms(n, 0) + 2u * RADIO_POLL_MS);
    }
    /* A P25 channel (C) with a foreign carrier for 20 s: the rounds A still gets */
    mw_codeplug(RADIO_MODE_FM, 2);
    mw_start_radio();
    mw_busy_hz = 0;
    CHECK(mw_watch(6, 0));
    CHECK(mw_until_at(1, 1000));
    mw_busy_hz = mw_hz[2];
    uint8_t prev = sm.dual_at;
    int rounds = 0;
    uint32_t t0 = now;
    while (now - t0 < 20000u) {
        mw_poll();
        if (sm.dual_at != prev) {
            prev = sm.dual_at;
            rounds += prev == 0;
        }
    }
    mw_busy_hz = 0;
    for (int i = 0; i < 400 && native_lab_owns(); i++)
        mw_poll();
    printf("  6 channels, a foreign carrier on the P25 one for 20 s: A visited %d times\n", rounds);
#if CONFIG_MWATCH_FAST
    CHECK(rounds >= 5);	/* given back after the probe time, passed over in its cooldown */
#endif
}

/* ---- DDM32 Plus TX target fix (docs/plus.md "Multi-watch", "The TX target"; tools/plus_mwfix.py, native_lab.c) ----
 * The owner's report on v1.0.0-beta.7 (A DMR, B P25 NAC Any / TX $293, C FM): PTT on C keyed P25 on B; PTT on the P25
 * letter did nothing; TALKING stayed on after release; the P25 letter went deaf. Here every PTT goes the way the radio
 * task sends it (tests/classic/test_radio.inc classic_dispatch: native_lab_request first, its NATIVE_REQUEST_WAIT kept),
 * for 2..6 letters of mixed modes, each letter selected, the receiver parked on each letter or held on a P25 letter by a
 * call the native receiver has (the reply right after the other station's over). Checked: the letter keyed is the
 * selected one, with its frequency, mode, power and tone / NAC; the ring holds while PTT is down; on release the
 * indicator clears, the native receiver lets go, the ring goes on; then a new P25 call on a P25 letter is heard again
 * (no cooldown after our over). */
#define MWX_CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #c, mwx_tag); } } while (0)
static char mwx_tag[64];
static uint32_t mwx_voice_hz;	/* a P25 call with voice for us on this frequency (0: none) */
static bool mwx_frames_only;	/* ... its frames only so far (a late entry before its link control, another TG) */

/* Zone 0's six channels in the modes given ('F' FM with a TX tone of its own, 'D' DMR CC 1 TS 1, 'P' P25 RX NAC Any,
 * TX NAC $293 (inherited), TG 42), each its own power; owner: A, B, C on the owner's frequencies */
static void mwx_codeplug(const char *modes, bool owner)
{
    static const uint32_t own[3] = { 436275000u, 145500000u, 433500000u };
    mw_codeplug(RADIO_MODE_FM, -1);
    for (int i = 0; i < 6; i++) {
        channel_t *c = &chans[zones[0].channels[i]];
        if (owner && i < 3)
            c->rx_hz = c->tx_hz = own[i];
        mw_hz[i] = c->rx_hz;
        c->power = (uint8_t)(i % 3);
        c->contact = 1;
        c->listen_list = 0;
        if (modes[i] == 'F') {
            c->tx_tone = (subtone_t){ TONE_CTCSS, (uint16_t)(885 + 10 * i) };
        } else if (modes[i] == 'D' || modes[i] == 'R') {
            c->mode = RADIO_MODE_DMR;
            c->rx_only = modes[i] == 'R';   /* R: a DMR channel that may not transmit (receive only) */
        } else {
            c->mode = RADIO_MODE_P25;
            c->bw = RADIO_BW_12K5;
            c->p25_nac = 0xF7E;
            c->p25_tg = 42;
            c->p25_id = 3101234;
        }
    }
}

/* One pass of the radio task, a signal on mw_busy_hz, the P25 call's frames and voice while the native receiver is on it */
static void mwx_dispatch(void)
{
    signal(mw_busy_hz != 0 && hw.rx_hz == mw_busy_hz ? 0 : 120, true);
    bool rx = mwx_voice_hz != 0 && classic_busy && classic_live != NULL && classic_live->config.mode != NT_TRANSMIT &&
              classic_live->config.frequency_hz == mwx_voice_hz;
    classic_receiving = rx;
    if (rx) {
        zs_sync_ms = now;
        if (!mwx_frames_only)
            scan_voice_ms = now;
    }
    classic_dispatch();
}

/* A request as the radio task takes it, kept while the native owner asks it to wait */
static void mwx_send(uint8_t type, int32_t arg)
{
    classic_request = (radio_req_t){ type, arg, 0 };
    classic_retained = true;
    for (int i = 0; i < 80 && classic_retained; i++)
        mwx_dispatch();
    MWX_CHECK(!classic_retained);
}

/* What is on the air: 'F' / 'D' through the board (the radio core), 'P' the native P25 transmitter, 0 nothing */
static char mwx_keyed(int tx0, uint32_t *hz, uint8_t *power)
{
    if (sm.view.state != RS_TX)
        return 0;
    if (hw.tx_calls > tx0) {
        *hz = hw.tx_hz;
        *power = hw.tx_power;
        return sm.view.ch.mode == RADIO_MODE_DMR ? 'D' : codeplug_mode_p25(sm.view.ch.mode) ? 'X' : 'F';
    }
    if (classic_live != NULL && classic_live->config.mode == NT_TRANSMIT && native_lab_owns()) {
        *hz = classic_live->config.frequency_hz;
        *power = classic_live->config.power;
        return 'P';
    }
    return 0;
}

/* The native receiver hears a P25 call on letter i within ms */
static bool mwx_heard(uint8_t i, uint32_t ms)
{
    mw_busy_hz = mwx_voice_hz = mw_hz[i];
    bool heard = false;
    for (uint32_t t = 0; t < ms && !heard; t += RADIO_POLL_MS) {
        mwx_dispatch();
        heard = native_lab_owns() && sm.dual_at == i && classic_receiving;
    }
    return heard;
}

/* One PTT (see above). Returns the mode keyed */
static char mwx_ptt_case(const char *modes, uint8_t n, uint8_t sel, uint8_t at, bool call, bool owner)
{
    snprintf(mwx_tag, sizeof mwx_tag, "%.*s%s n=%u sel %c at %c%s", (int)n, modes, owner ? " owner" : "", n, 'A' + sel,
             'A' + at, call ? " held by a call" : "");
    mwx_codeplug(modes, owner);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    MWX_CHECK(mw_watch(n, sel));
    if (call) {
        /* The other station's over on letter `at`, heard; it ends, the native receiver still holds the letter */
        MWX_CHECK(mwx_heard(at, 4000));
        mw_busy_hz = mwx_voice_hz = 0;
        for (int i = 0; i < 10; i++)
            mwx_dispatch();
        MWX_CHECK(native_lab_owns() && sm.dual_at == at);
    } else {
        MWX_CHECK(mw_until_at(at, 3000));
    }
    int tx0 = hw.tx_calls;
    unsigned dmr0 = sim.tx_begins;
    uint32_t hz = 0;
    uint8_t power = 0;
    char keyed = 0;
    mwx_send(RREQ_PTT, 1);
    for (int i = 0; i < 8 && !keyed; i++) {
        keyed = mwx_keyed(tx0, &hz, &power);
        if (!keyed)
            mwx_dispatch();
    }
    char want = modes[sel];
    MWX_CHECK(keyed == want && hz == mw_hz[sel] && power == sel % 3 && sm.dual_at == sel);
    if (keyed == 'F')
        MWX_CHECK(hw.tx_tone.type == TONE_CTCSS && hw.tx_tone.value == 885 + 10 * sel);
    if (keyed == 'D')
        MWX_CHECK(sim.tx_begins == dmr0 + 1);
    if (keyed == 'P')
        MWX_CHECK(classic_set_nac == 0xF7E && classic_set_tx_nac == 0x293 && classic_set_tg == 42 && hw.tx_calls == tx0);
    /* Held 2 s (longer than the 1.5 s a call's take is kept after its voice: the give back is due at the release, the
     * case that left TALKING on): the ring stays on the letter, still on the air */
    for (int i = 0; i < 80; i++) {
        mwx_dispatch();
        MWX_CHECK(sm.dual_at == sel && sm.view.state == RS_TX);
    }
    /* Released: the indicator clears, the native receiver lets go, the ring goes on, the board on its letter */
    mwx_send(RREQ_PTT, 0);
    if (keyed == 'D')
        run(1000);
    bool moved = false;
    for (int i = 0; i < 320; i++) {
        mwx_dispatch();
        moved = moved || sm.dual_at != sel;
    }
    MWX_CHECK(!sm.ptt && sm.view.state != RS_TX && sm.view.state != RS_TX_DENIED && !native_lab_owns());
    MWX_CHECK(moved && sm.scan.kind == SCAN_DUAL && sm.dual_n == n && sm.dual_sel == sel);
    MWX_CHECK(hw.rx_hz == mw_hz[sm.dual_at]);
    /* A new P25 call on a P25 letter (the reply to our over among them): heard within a round and the probe */
    for (uint8_t p = 0; p < n; p++) {
        if (modes[p] == 'P') {
            MWX_CHECK(mwx_heard(p, 4000));
            mw_busy_hz = mwx_voice_hz = 0;
            for (int i = 0; i < 160 && native_lab_owns(); i++)
                mwx_dispatch();
            break;
        }
    }
    mw_busy_hz = mwx_voice_hz = 0;
    return keyed;
}

static void test_mwatch_tx_target(void)
{
    static const char *const pattern[2] = { "DPFPDF", "FDPDFP" };
    int before = failures;
    /* The owner's exact setup: A DMR 436.275, B P25 145.500 (NAC Any, TX $293), C FM 433.500 */
    for (uint8_t sel = 0; sel < 3; sel++) {
        for (uint8_t at = 0; at < 3; at++)
            mwx_ptt_case("DPFFFF", 3, sel, at, false, true);
        mwx_ptt_case("DPFFFF", 3, sel, 1, true, true);
    }
    printf("Plus multi-watch TX target: the owner's A DMR / B P25 / C FM, PTT on each letter from each: %d failures\n",
           failures - before);
    for (int p = 0; p < 2; p++) {
        for (uint8_t n = 2; n <= 6; n++) {
            int cases = 0;
            before = failures;
            for (uint8_t sel = 0; sel < n; sel++) {
                for (uint8_t at = 0; at < n; at++) {
                    mwx_ptt_case(pattern[p], n, sel, at, false, false);
                    cases++;
                    if (p == 0 && pattern[p][at] == 'P') {
                        mwx_ptt_case(pattern[p], n, sel, at, true, false);
                        cases++;
                    }
                }
            }
            printf("Plus multi-watch TX target: %.*s, %d PTT cases: %d failures\n", (int)n, pattern[p], cases,
                   failures - before);
        }
    }
}

/* A letter that can't transmit (RX only: FM and P25), selected while the ring is on another: refused (TX denied, nothing
 * keyed on any letter), cleared on release, the ring goes on */
static void test_mwatch_tx_refused(void)
{
    int before = failures;
    for (int k = 0; k < 2; k++) {
        uint8_t sel = k == 0 ? 2 : 1;	/* C FM, B P25 */
        snprintf(mwx_tag, sizeof mwx_tag, "refused, sel %c", 'A' + sel);
        mwx_codeplug("DPFFFF", true);
        chans[zones[0].channels[sel]].rx_only = true;
        mw_start_radio();
        codec_present = true;
        classic_retained = false;
        mw_busy_hz = mwx_voice_hz = 0;
        MWX_CHECK(mw_watch(3, sel));
        MWX_CHECK(mw_until_at(0, 3000));
        int tx0 = hw.tx_calls;
        mwx_send(RREQ_PTT, 1);
        for (int i = 0; i < 8; i++)
            mwx_dispatch();
        MWX_CHECK(sm.view.state == RS_TX_DENIED && sm.view.tx_deny == TXDENY_RX_ONLY && hw.tx_calls == tx0);
        MWX_CHECK(!(classic_live != NULL && classic_live->config.mode == NT_TRANSMIT && classic_busy));
        mwx_send(RREQ_PTT, 0);
        bool moved = false;
        for (int i = 0; i < 320; i++) {
            mwx_dispatch();
            moved = moved || sm.dual_at != sel;
        }
        MWX_CHECK(sm.view.state != RS_TX_DENIED && sm.view.state != RS_TX && moved && !native_lab_owns());
    }
    printf("Plus multi-watch TX target: an RX only letter refused and cleared: %d failures\n", failures - before);
}

/* Report 3 (TALKING after release) and 1 (the ring deaf until a power cycle): the native receiver gives a letter back
 * in the poll right after its transmission ended. On the radio the worker ends a transmission as DRAIN (shown as TX by
 * channel_view), the radio task's next service moves it to STOPPED and returns early (nt_cleanup, live_enabled), the
 * worker lets go, and the service after that gives the letter back (its call's voice ended 1.5 s before): no
 * channel_view ran in between, so the TX stayed in the view. The fixture's worker lets go inside its own pump, so the
 * order is played by hand here: the letter's take let go, the view as the drain left it, then the give back. Checked:
 * the TX is cleared, the ring goes on, the board is retuned, a new call is heard (before: TALKING until the time-out,
 * the ring stood still on scan_talked, the retune waited for the end of that TX) */
static void test_mwatch_tx_stale(void)
{
    int before = failures;
    static const radio_state_t shown[3] = { RS_TX, RS_TX_DENIED, RS_RX };
    for (int k = 0; k < 3; k++) {
        snprintf(mwx_tag, sizeof mwx_tag, "native %s left at the give back", k == 2 ? "RX" : k ? "TX denied" : "TX");
        mwx_codeplug("DPFFFF", true);
        mw_start_radio();
        codec_present = true;
        classic_retained = false;
        MWX_CHECK(mw_watch(3, 1));
        MWX_CHECK(mwx_heard(1, 4000));	/* the other station's over on B, heard; then our reply (not modelled) */
        mw_busy_hz = mwx_voice_hz = 0;
        classic_receiving = false;
        now += 2000;			/* the give back is due: the over's voice ended over 1.5 s ago */
        native_lab_voice_stop(classic_sid);	/* the worker ends its session */
        native_lab_service(&sm, now);	/* the give back starts, the session's cleanup returns early */
        MWX_CHECK(native_lab_voice_release(classic_sid));
        classic_busy = classic_stop = false;
        sm.view.state = shown[k];	/* as the last channel_view showed the drain */
        bool moved = false;
        for (int i = 0; i < 200; i++) {
            mwx_dispatch();
            moved = moved || sm.dual_at != 1;
        }
        MWX_CHECK(sm.view.state != RS_TX && sm.view.state != RS_TX_DENIED && !native_lab_owns() && moved);
        MWX_CHECK(sm.view.state == RS_IDLE);
        MWX_CHECK(hw.rx_hz == mw_hz[sm.dual_at]);
        MWX_CHECK(mwx_heard(1, 4000));
        mw_busy_hz = mwx_voice_hz = 0;
    }
    printf("Plus multi-watch TX target: a TX left in the view by the native receiver's give back cleared: %d failures\n",
           failures - before);
}


/* The verifier's B1 / S2: the radio core keys by itself (the 1750 Hz burst, the emergency hot mic; VOX and cross-band
 * go the same way, ptt_down) with each letter of the owner's A DMR / B P25 / C FM selected and the receiver on each
 * letter, the time-out off. Never a native P25 transmission (before: a burst with B selected keyed P25 on B and held it
 * until the time-out, never with it off); the burst only on an FM selected letter (before: 1750 Hz on a DMR carrier);
 * when it ends nothing stays on the air, no TX / TX denied left. Also with a P25 call holding B */
static bool mwx_native_tx(void)
{
    return classic_live != NULL && classic_live->config.mode == NT_TRANSMIT && classic_busy;
}

static void mwx_core_step(bool ticks)
{
    mwx_dispatch();
    if (ticks && !native_lab_owns()) {	/* as radio_task.c: no tick while the native receiver owns */
        sim_slot();
        radio_sm_tick(&sm, now);
    }
}

static void mwx_core_key_case(uint8_t sel, uint8_t at, bool hot_mic, bool held)
{
    snprintf(mwx_tag, sizeof mwx_tag, "%s, sel %c, ring at %c%s", hot_mic ? "hot mic" : "1750 burst", 'A' + sel, 'A' + at,
             held ? " held by a P25 call" : "");
    static const char modes[] = "DPFFFF";
    mwx_codeplug(modes, true);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    mwx_frames_only = false;
    MWX_CHECK(mw_watch(3, sel));
    sm.view.tot_s = 0;
    if (held) {
        MWX_CHECK(mwx_heard(at, 4000));
        mw_busy_hz = mwx_voice_hz = 0;
        for (int i = 0; i < 10; i++)
            mwx_dispatch();
    } else {
        MWX_CHECK(mw_until_at(at, 3000));
    }
    int tx0 = hw.tx_calls;
    tone.hz = 0;
    if (hot_mic) {
        dmr_emergency_settings_t es;
        dmr_emergency_settings_default(&es);
        es.mode = DMR_EMERGENCY_MODE_CALL;
        es.hot_mic_s = 1;
        dmr_emergency_set_settings(&es);
        dmr_emergency_start();
    } else {
        mwx_send(RREQ_TONE_BURST, 1);
    }
    int p25 = 0;
    bool wrong = false;
    for (int i = 0; i < 40; i++) {
        mwx_core_step(hot_mic);
        p25 += mwx_native_tx();
        if (sm.view.state == RS_TX && hw.tx_calls > tx0 && hw.tx_hz != mw_hz[hot_mic ? 0 : sel])
            wrong = true;	/* keyed another letter (the hot mic: A, the only DMR letter, since mwfix3) */
        if (tone.hz != 0 && (modes[sel] != 'F' || sm.view.ch.mode != RADIO_MODE_FM))
            wrong = true;	/* the burst on a DMR / P25 carrier */
    }
    MWX_CHECK(p25 == 0 && !wrong);
    if (!hot_mic && modes[sel] == 'F')
        MWX_CHECK(tone.hz == RADIO_BURST_HZ && sm.view.state == RS_TX && hw.tx_hz == mw_hz[sel]);
    if (!hot_mic && modes[sel] != 'F')
        MWX_CHECK(sm.view.state != RS_TX && hw.tx_calls == tx0);	/* not an FM letter: no burst, nothing keyed */
    if (hot_mic) {
        for (int i = 0; i < 80; i++)	/* the 1 s hot mic runs out */
            mwx_core_step(true);
        dmr_emergency_cancel();
    } else {
        mwx_send(RREQ_TONE_BURST, 0);
    }
    for (int i = 0; i < 200; i++) {
        mwx_core_step(true);
        p25 += mwx_native_tx();
    }
    MWX_CHECK(p25 == 0 && !mwx_native_tx() && sm.view.state != RS_TX && sm.view.state != RS_TX_DENIED && !sm.ptt);
}

static void test_mwatch_core_keying(void)
{
    int before = failures;
    for (uint8_t sel = 0; sel < 3; sel++) {
        for (uint8_t at = 0; at < 3; at++) {
            mwx_core_key_case(sel, at, false, false);
            mwx_core_key_case(sel, at, true, false);
        }
        mwx_core_key_case(sel, 1, false, true);
        mwx_core_key_case(sel, 1, true, true);
    }
    printf("Plus multi-watch TX target: the 1750 burst and the emergency hot mic from each letter, TOT off: %d failures\n",
           failures - before);
}

/* S1: a selection asked while transmitting is dropped by the radio (RREQ_MWATCH while TX); the UI refuses it then
 * (tests/plus/test_ui_mwdetail.c). Here: the radio keeps transmitting on B and keeps B selected */
static void test_mwatch_sel_during_tx(void)
{
    int before = failures;
    snprintf(mwx_tag, sizeof mwx_tag, "selection asked during TX");
    mwx_codeplug("DPFFFF", true);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    MWX_CHECK(mw_watch(3, 1));
    MWX_CHECK(mw_until_at(0, 3000));
    mwx_send(RREQ_PTT, 1);
    for (int i = 0; i < 20; i++)
        mwx_dispatch();
    radio_dual_ref_t r[RADIO_DUAL_SLOTS];
    for (uint8_t i = 0; i < 3; i++)
        r[i] = (radio_dual_ref_t){ false, 0, i };
    memcpy(mw_req_ref, r, 3 * sizeof r[0]);
    mw_req_n = 3;
    mw_req_sel = 2;
    mwx_send(RREQ_MWATCH, 0);
    for (int i = 0; i < 20; i++)
        mwx_dispatch();
    MWX_CHECK(sm.dual_sel == 1 && sm.view.state == RS_TX);
    mwx_send(RREQ_PTT, 0);
    for (int i = 0; i < 200; i++)
        mwx_dispatch();
    MWX_CHECK(sm.dual_sel == 1 && !mwx_native_tx() && sm.view.state != RS_TX);
    printf("Plus multi-watch TX target: a selection during TX is not taken (the UI refuses it): %d failures\n",
           failures - before);
}

/* S3, listening only (the owner's B: RX NAC Any, TG 1): a call already running when the ring comes to B (late entry);
 * frames for 1.5 s before voice (its link control late); frames without voice for 3 s, then voice (before: the 8 s
 * cooldown skipped most of the call); in a ring of 3 and 6 */
static void mwx_listen_case(uint8_t n, uint32_t frames_ms, const char *what)
{
    snprintf(mwx_tag, sizeof mwx_tag, "listen n=%u: %s", n, what);
    mwx_codeplug("DPFDFF", true);
    mw_start_radio();
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    MWX_CHECK(mw_watch(n, 0));
    MWX_CHECK(mw_until_at(2, 3000));
    mw_busy_hz = mwx_voice_hz = mw_hz[1];	/* the call starts while the receiver is elsewhere */
    mwx_frames_only = frames_ms != 0;
    uint32_t t0 = now, heard_at = 0;
    bool heard = false;
    for (int i = 0; i < 600 && !heard; i++) {	/* 15 s */
        if (mwx_frames_only && now - t0 >= frames_ms)
            mwx_frames_only = false;	/* the voice for us starts */
        mwx_dispatch();
        heard = native_lab_owns() && sm.dual_at == 1 && classic_receiving && now - scan_voice_ms <= 2u * RADIO_POLL_MS;
        heard_at = now - t0;
    }
    mwx_frames_only = false;
    /* At most the frames-only time, the native take's no-voice limit, a short pause and a ring */
    uint32_t limit = frames_ms + 2500u + 2000u + 1000u;
    MWX_CHECK(heard && heard_at <= limit);
    printf("  %s, %u letters: voice heard %u ms after the call started (limit %u)\n", what, n, (unsigned)heard_at,
           (unsigned)limit);
    mw_busy_hz = mwx_voice_hz = 0;
    for (int i = 0; i < 200 && native_lab_owns(); i++)
        mwx_dispatch();
}

static void test_mwatch_listen(void)
{
    int before = failures;
    for (uint8_t n = 3; n <= 6; n += 3) {
        mwx_listen_case(n, 0, "late entry");
        mwx_listen_case(n, 1500, "frames 1.5 s before voice");
        mwx_listen_case(n, 3000, "frames 3 s without voice, then voice");
    }
    printf("Plus multi-watch: P25 calls on the letter heard while only listening: %d failures\n", failures - before);
}

/* M1, the safety net: in the ring a TX / TX denied with PTT up and nothing keyed (whatever left it) is cleared at the
 * poll, and the ring goes on; one with the 1750 burst keying is left alone */
static void test_mwatch_stale_net(void)
{
    int before = failures;
    static const radio_state_t shown[2] = { RS_TX, RS_TX_DENIED };
    for (int k = 0; k < 2; k++) {
        snprintf(mwx_tag, sizeof mwx_tag, "safety net, %s", k ? "TX denied" : "TX");
        mwx_codeplug("DPFFFF", true);
        mw_start_radio();
        classic_retained = false;
        mw_busy_hz = mwx_voice_hz = 0;
        MWX_CHECK(mw_watch(3, 2));
        MWX_CHECK(mw_until_at(0, 3000));
        sm.view.state = shown[k];
        bool moved = false;
        for (int i = 0; i < 40; i++) {
            mwx_dispatch();
            moved = moved || sm.dual_at != 0;
        }
        MWX_CHECK(sm.view.state == RS_IDLE && moved);
    }
    snprintf(mwx_tag, sizeof mwx_tag, "safety net, the burst keying");
    MWX_CHECK(mw_until_at(2, 3000));
    mwx_send(RREQ_TONE_BURST, 1);
    for (int i = 0; i < 20; i++)
        mwx_dispatch();
    MWX_CHECK(sm.view.state == RS_TX && tone.hz == RADIO_BURST_HZ);
    mwx_send(RREQ_TONE_BURST, 0);
    for (int i = 0; i < 40; i++)
        mwx_dispatch();
    MWX_CHECK(sm.view.state != RS_TX);
    printf("Plus multi-watch: a stale TX in the ring cleared, a keyed one kept: %d failures\n", failures - before);
}

/* S1 follow-up: a new selection while a P25 call is held on B (the A/B key, or the UI asking again for the pill's
 * letter) doesn't cut the call: the ring keeps B, only the selection changes (radio_sm_dual_keeps); PTT then goes to
 * the new letter. A ring without B (a letter changed to another channel) still lets B go */
static void test_mwatch_sel_during_call(void)
{
    int before = failures;
    snprintf(mwx_tag, sizeof mwx_tag, "selection during a P25 call on B");
    mwx_codeplug("DPFFFF", true);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    MWX_CHECK(mw_watch(3, 0));
    MWX_CHECK(mwx_heard(1, 4000));
    unsigned starts = classic_starts;
    radio_dual_ref_t r[RADIO_DUAL_SLOTS];
    for (uint8_t i = 0; i < 3; i++)
        r[i] = (radio_dual_ref_t){ false, 0, i };
    memcpy(mw_req_ref, r, 3 * sizeof r[0]);
    mw_req_n = 3;
    mw_req_sel = 2;
    mwx_send(RREQ_MWATCH, 0);
    for (int i = 0; i < 20; i++)
        mwx_dispatch();
    MWX_CHECK(sm.dual_sel == 2 && native_lab_owns() && sm.dual_at == 1 && classic_receiving && classic_starts == starts);
    /* The call ends: PTT goes to C (FM) */
    mw_busy_hz = mwx_voice_hz = 0;
    for (int i = 0; i < 10; i++)
        mwx_dispatch();
    int tx0 = hw.tx_calls;
    uint32_t hz = 0;
    uint8_t pw = 0;
    char keyed = 0;
    mwx_send(RREQ_PTT, 1);
    for (int i = 0; i < 8 && !keyed; i++) {
        keyed = mwx_keyed(tx0, &hz, &pw);
        if (!keyed)
            mwx_dispatch();
    }
    MWX_CHECK(keyed == 'F' && hz == mw_hz[2]);
    mwx_send(RREQ_PTT, 0);
    for (int i = 0; i < 200; i++)
        mwx_dispatch();
    /* A ring without B while B is held: B is let go */
    MWX_CHECK(mwx_heard(1, 4000));
    r[1] = (radio_dual_ref_t){ false, 0, 3 };
    memcpy(mw_req_ref, r, 3 * sizeof r[0]);
    mw_req_sel = 0;
    mwx_send(RREQ_MWATCH, 0);
    for (int i = 0; i < 20; i++)
        mwx_dispatch();
    MWX_CHECK(!(native_lab_owns() && hw.rx_hz == mw_hz[1]) && sm.dual_n == 3);
    mw_busy_hz = mwx_voice_hz = 0;
    printf("Plus multi-watch TX target: a selection during a P25 call keeps the call: %d failures\n", failures - before);
}

/* Third review, S1 / S2: the DMR emergency started while the watch runs (as the user's key, the lone worker and SOS
 * start it: dmr_emergency_start, a mailbox the radio tick takes). The radio core ends the watch on the selected letter
 * when it is DMR, else on the first DMR letter, in the same tick, before the start is taken: the hot mic keys that
 * letter's frequency. No DMR letter in the ring: the watch goes on, nothing keyed, the emergency fails */
static void mwx_emergency_case(const char *modes, uint8_t sel, uint8_t at, int want)
{
    snprintf(mwx_tag, sizeof mwx_tag, "emergency %.3s sel %c at %c", modes, 'A' + sel, 'A' + at);
    mwx_codeplug(modes, true);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    MWX_CHECK(mw_watch(3, sel));
    MWX_CHECK(mw_until_at(at, 3000));
    dmr_emergency_settings_t es;
    dmr_emergency_settings_default(&es);
    es.mode = DMR_EMERGENCY_MODE_CALL;
    es.hot_mic_s = 1;
    dmr_emergency_set_settings(&es);
    uint16_t stops = sm.view.dual_stops;
    int tx0 = hw.tx_calls;
    MWX_CHECK(dmr_emergency_start());
    bool keyed = false, wrong = false;
    for (int i = 0; i < 40; i++) {
        mwx_core_step(true);
        if (sm.view.state == RS_TX && hw.tx_calls > tx0) {
            keyed = true;
            wrong = wrong || want < 0 || hw.tx_hz != mw_hz[want];
        }
        wrong = wrong || mwx_native_tx();
    }
    if (want >= 0) {
        MWX_CHECK(keyed && !wrong && sm.scan.kind == SCAN_OFF && sm.view.dual_stops == stops + 1u &&
                  sm.view.ch.mode == RADIO_MODE_DMR && sm.dual_sel == want);
    } else {
        MWX_CHECK(!keyed && !wrong && sm.scan.kind == SCAN_DUAL && dmr_emergency_state() == DMR_EMERGENCY_FAILED);
    }
    for (int i = 0; i < 80; i++)
        mwx_core_step(true);
    dmr_emergency_cancel();
    for (int i = 0; i < 80; i++)
        mwx_core_step(true);
    MWX_CHECK(sm.view.state != RS_TX && !sm.ptt);
}

static void test_mwatch_emergency(void)
{
    int before = failures;
    for (uint8_t sel = 0; sel < 3; sel++)
        for (uint8_t at = 0; at < 3; at++)
            mwx_emergency_case("DPFFFF", sel, at, 0);	/* A the only DMR letter: always A */
    mwx_emergency_case("FDDFFF", 2, 0, 2);	/* C DMR selected: C */
    mwx_emergency_case("FDDFFF", 0, 2, 1);	/* A FM selected: the first DMR letter, B */
    mwx_emergency_case("FPFFFF", 1, 0, -1);	/* no DMR letter: the watch goes on, the emergency fails */
    /* Hole hunt B2: a DMR letter that may not transmit (receive only) is passed over */
    mwx_emergency_case("RDFFFF", 0, 2, 1);	/* A selected but RX only: B */
    mwx_emergency_case("RFDFFF", 1, 0, 2);	/* A RX only, B FM selected: C, not A */
    mwx_emergency_case("RFFFFF", 0, 1, -1);	/* the only DMR letter RX only: the watch goes on, the emergency fails */
    mwx_emergency_case("FRRFFF", 2, 0, -1);	/* both DMR letters RX only: the same */
    printf("Plus multi-watch: the emergency on the selected or the first DMR letter, the watch ended there: %d failures\n",
           failures - before);
}

/* M1 (the verifier's probe): PTT while B's P25 voice is still on the air, after selecting C (FM) or A (DMR) during the
 * call: the native receiver lets B go before the board keys (never both), the selected letter keyed on its frequency,
 * no native transmission; cleared on release */
static void mwx_ptt_mid_call(uint8_t sel)
{
    snprintf(mwx_tag, sizeof mwx_tag, "PTT during B's voice, sel %c", 'A' + sel);
    mwx_codeplug("DPFFFF", true);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    MWX_CHECK(mw_watch(3, 1));
    MWX_CHECK(mwx_heard(1, 4000));
    radio_dual_ref_t r[RADIO_DUAL_SLOTS];
    for (uint8_t i = 0; i < 3; i++)
        r[i] = (radio_dual_ref_t){ false, 0, i };
    memcpy(mw_req_ref, r, 3 * sizeof r[0]);
    mw_req_n = 3;
    mw_req_sel = sel;
    mwx_send(RREQ_MWATCH, 0);
    for (int i = 0; i < 8; i++)
        mwx_dispatch();
    MWX_CHECK(sm.dual_sel == sel && native_lab_owns() && classic_receiving);	/* the call goes on */
    int tx0 = hw.tx_calls, overlap = 0, native_tx = 0;
    uint32_t hz = 0;
    uint8_t pw = 0;
    char k = 0;
    classic_request = (radio_req_t){ RREQ_PTT, 1, 0 };
    classic_retained = true;
    for (int i = 0; i < 80; i++) {	/* B's voice still running */
        mwx_dispatch();
        if (hw.tx_calls > tx0 && classic_busy && classic_live != NULL && classic_live->config.mode != NT_TRANSMIT)
            overlap++;
        native_tx += mwx_native_tx();
        if (!k)
            k = mwx_keyed(tx0, &hz, &pw);
    }
    MWX_CHECK(k == (sel == 0 ? 'D' : 'F') && hz == mw_hz[sel] && overlap == 0 && native_tx == 0 && sm.dual_at == sel);
    mwx_send(RREQ_PTT, 0);
    mw_busy_hz = mwx_voice_hz = 0;
    if (sel == 0)
        run(1000);
    for (int i = 0; i < 200; i++)
        mwx_dispatch();
    MWX_CHECK(sm.view.state != RS_TX && sm.view.state != RS_TX_DENIED && !sm.ptt);
}

static void test_mwatch_ptt_mid_call(void)
{
    int before = failures;
    mwx_ptt_mid_call(2);
    mwx_ptt_mid_call(0);
    printf("Plus multi-watch TX target: PTT during a P25 call held on another letter: %d failures\n", failures - before);
}

/* Fourth review, B1 / S1 (the verifier's probe): an emergency never waits for a P25 call. The native receiver holds a
 * call on B (its voice goes on for 30 s): the emergency is keyed on A, the DMR letter, within about a second (before:
 * after the call, 31.5 s; a busy net held it for good). On a single P25 channel (no watch) with a call running it
 * fails within about a second (before: it never resolved), so SOS and the lone worker fall back. M1: pressed while
 * PTT is held on C (FM): nothing until PTT ends, then on A, never on C */
static void mwx_emergency_setup(void)
{
    dmr_emergency_settings_t es;
    dmr_emergency_settings_default(&es);
    es.mode = DMR_EMERGENCY_MODE_CALL;
    es.hot_mic_s = 1;
    dmr_emergency_set_settings(&es);
}

static void mwx_emergency_during_call(uint8_t sel)
{
    snprintf(mwx_tag, sizeof mwx_tag, "emergency during B's call, sel %c", 'A' + sel);
    mwx_codeplug("DPFFFF", true);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    MWX_CHECK(mw_watch(3, sel));
    MWX_CHECK(mwx_heard(1, 4000));
    mwx_emergency_setup();
    int tx0 = hw.tx_calls;
    uint32_t t0 = now, keyed_at = 0;
    MWX_CHECK(dmr_emergency_start());
    for (uint32_t t = 0; t < 30000u && !keyed_at; t += 25u) {	/* B's voice goes on all the time */
        mwx_core_step(true);
        if (sm.view.state == RS_TX && hw.tx_calls > tx0) {
            keyed_at = now - t0;
            MWX_CHECK(hw.tx_hz == mw_hz[0] && sm.view.ch.mode == RADIO_MODE_DMR && sm.scan.kind == SCAN_OFF);
        }
    }
    MWX_CHECK(keyed_at != 0 && keyed_at <= 1000u);
    printf("  emergency during a P25 call on B, %c selected: keyed on A after %u ms\n", 'A' + sel, (unsigned)keyed_at);
    mw_busy_hz = mwx_voice_hz = 0;
    dmr_emergency_cancel();
    for (int i = 0; i < 120; i++)
        mwx_core_step(true);
    MWX_CHECK(sm.view.state != RS_TX && !sm.ptt);
}

static void mwx_emergency_single_p25(void)
{
    snprintf(mwx_tag, sizeof mwx_tag, "emergency on a single P25 channel");
    mwx_codeplug("DPFFFF", true);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    req(RREQ_ZONE_POS, 1);	/* B, no watch */
    mw_busy_hz = mwx_voice_hz = mw_hz[1];
    bool heard = false;
    for (int i = 0; i < 200 && !heard; i++) {
        mwx_dispatch();
        heard = native_lab_owns() && classic_receiving;
    }
    MWX_CHECK(heard);
    mwx_emergency_setup();
    int tx0 = hw.tx_calls;
    uint32_t t0 = now, failed_at = 0;
    MWX_CHECK(dmr_emergency_start());
    for (uint32_t t = 0; t < 30000u && !failed_at; t += 25u) {
        mwx_core_step(true);
        if (dmr_emergency_state() == DMR_EMERGENCY_FAILED)
            failed_at = now - t0;
    }
    MWX_CHECK(failed_at != 0 && failed_at <= 1000u && hw.tx_calls == tx0 && !mwx_native_tx());
    printf("  emergency on a single P25 channel with a call: failed after %u ms (SOS / the lone worker fall back)\n",
           (unsigned)failed_at);
    /* ... and the P25 channel is taken again after it */
    bool again = false;
    for (int i = 0; i < 200 && !again; i++) {
        mwx_dispatch();
        again = native_lab_owns() && classic_receiving;
    }
    MWX_CHECK(again);
    mw_busy_hz = mwx_voice_hz = 0;
    dmr_emergency_cancel();
    for (int i = 0; i < 80; i++)
        mwx_core_step(true);
}

static void mwx_emergency_during_ptt(void)
{
    snprintf(mwx_tag, sizeof mwx_tag, "emergency while PTT is held on C");
    mwx_codeplug("DPFFFF", true);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    MWX_CHECK(mw_watch(3, 2));
    MWX_CHECK(mw_until_at(1, 3000));
    mwx_send(RREQ_PTT, 1);
    for (int i = 0; i < 8; i++)
        mwx_core_step(true);
    MWX_CHECK(sm.view.state == RS_TX && hw.tx_hz == mw_hz[2]);
    mwx_emergency_setup();
    MWX_CHECK(dmr_emergency_start());
    for (int i = 0; i < 40; i++) {
        mwx_core_step(true);
        MWX_CHECK(hw.tx_hz == mw_hz[2] && dmr_emergency_start_pending() && sm.scan.kind == SCAN_DUAL);
    }
    int tx0 = hw.tx_calls;
    mwx_send(RREQ_PTT, 0);
    bool keyed = false;
    for (int i = 0; i < 80 && !keyed; i++) {
        mwx_core_step(true);
        keyed = sm.view.state == RS_TX && hw.tx_calls > tx0;
    }
    MWX_CHECK(keyed && hw.tx_hz == mw_hz[0] && sm.view.ch.mode == RADIO_MODE_DMR && sm.scan.kind == SCAN_OFF);
    dmr_emergency_cancel();
    for (int i = 0; i < 120; i++)
        mwx_core_step(true);
}

/* mwfix4 M1: PTT held on C for long: the pending start waits 10 s at most, then is taken on the channel in use (C, FM:
 * it fails, so SOS / the lone worker fall back); C stays on the air until release */
static void mwx_emergency_ptt_cap(void)
{
    snprintf(mwx_tag, sizeof mwx_tag, "emergency, PTT held on C for 12 s");
    mwx_codeplug("DPFFFF", true);
    mw_start_radio();
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    MWX_CHECK(mw_watch(3, 2));
    MWX_CHECK(mw_until_at(1, 3000));
    mwx_send(RREQ_PTT, 1);
    for (int i = 0; i < 8; i++)
        mwx_core_step(true);
    MWX_CHECK(sm.view.state == RS_TX && hw.tx_hz == mw_hz[2]);
    mwx_emergency_setup();
    MWX_CHECK(dmr_emergency_start());
    uint32_t t0 = now, failed = 0;
    for (int i = 0; i < 480 && !failed; i++) {	/* 12 s */
        mwx_core_step(true);
        if (dmr_emergency_state() == DMR_EMERGENCY_FAILED)
            failed = now - t0;
    }
    MWX_CHECK(failed >= 9900u && failed <= 10500u && sm.view.state == RS_TX && hw.tx_hz == mw_hz[2]);
    printf("  emergency with PTT held on C: waited %u ms, then taken on C (FM: failed)\n", (unsigned)failed);
    mwx_send(RREQ_PTT, 0);
    dmr_emergency_cancel();
    for (int i = 0; i < 120; i++)
        mwx_core_step(true);
    MWX_CHECK(sm.view.state != RS_TX && !sm.ptt);
}

static void test_mwatch_emergency_now(void)
{
    int before = failures;
    mwx_emergency_during_call(2);
    mwx_emergency_during_call(0);
    mwx_emergency_single_p25();
    mwx_emergency_during_ptt();
    mwx_emergency_ptt_cap();
    printf("Plus multi-watch: the emergency never waits for a P25 call or a transmission's letter: %d failures\n",
           failures - before);
}

int main(void)
{
    int prior = classic_radio_main();
    int before = failures;
    test_mwatch_order_dwell();
    test_mwatch_hold_wait();
    test_mwatch_ptt();
    test_mwatch_refuse_stop();
    uint32_t stayed = test_mwatch_p25();
    test_mwatch_period(stayed);
    test_mwatch_catch();
    test_mwatch_tx_target();
    test_mwatch_tx_refused();
    test_mwatch_tx_stale();
    test_mwatch_core_keying();
    test_mwatch_sel_during_tx();
    test_mwatch_listen();
    test_mwatch_stale_net();
    test_mwatch_sel_during_call();
    test_mwatch_emergency();
    test_mwatch_ptt_mid_call();
    test_mwatch_emergency_now();
    printf("Plus multi-watch radio core: %d failures\n", failures - before);
    return prior || failures != before ? 1 : 0;
}
