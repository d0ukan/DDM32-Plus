/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus: every mode in the VFO (CONFIG_VFO_MODES, docs/plus.md "Every mode in the VFO"), the radio core's part, on
 * the real radio state machine, its native P25 receiver and the fake board of radioCoreTest. Included by
 * tools/test_classic.py after tests/classic/test_radio.inc and tests/plus/test_mwatch_radio.inc (whose mains run first,
 * with this feature built in: upstream's radio suite with its VFO persistence tests, the Classic / Plus P25 suites, the
 * multi-watch ring). Then: the VFO in each mode (P25, YSF and the native receiver, NXDN / dPMR on the FM path, AM, DMR
 * Any), the TX gates, the modes kept across a restart (and an older record read as before), Copy to VFO of a P25
 * channel, a P25 VFO in dual watch and in a multi-watch ring, and C..F's own VFOs in the ring (P25, YSF, NXDN, dPMR
 * each on a letter; A / B's saved VFOs untouched). */
#include "radio/vfox.h"

/* The worker's YSF switch and caller (native_lab.c channel_service / channel_view) */
static int vx_ysf_on;
static uint8_t vx_ysf_dgid;
int p25_lab_set_ysf(int on, uint8_t dgid)
{
    if (!classic_busy) { vx_ysf_on = on; vx_ysf_dgid = dgid; }
    return !classic_busy;
}
uint32_t p25_lab_ysf_caller(char src[11], char dst[11])
{
    if (src) src[0] = 0;
    if (dst) dst[0] = 0;
    return 0;
}
/* NXDN / dPMR receive on the FM path: what the radio core asks for at its poll */
static struct { bool want; uint32_t hz; uint8_t arg; } vx_nxdn, vx_dpmr;
void radio_nxdn_poll(bool want, uint32_t hz, uint8_t ran, uint32_t now_ms)
{ (void)now_ms; vx_nxdn.want = want; vx_nxdn.hz = hz; vx_nxdn.arg = ran; }
bool radio_nxdn_heard(uint32_t now_ms) { (void)now_ms; return false; }
void radio_dpmr_poll(bool want, uint32_t hz, uint8_t cc, uint32_t now_ms)
{ (void)now_ms; vx_dpmr.want = want; vx_dpmr.hz = hz; vx_dpmr.arg = cc; }
bool radio_dpmr_heard(uint32_t now_ms) { (void)now_ms; return false; }
/* The VFO settings' copy, as radio_task.c hands it over */
static channel_t vx_apply_ch;
static void vx_request(uint8_t type, int32_t arg);
void radio_req_vfo_apply(const channel_t *ch) { vx_apply_ch = *ch; vx_request(RREQ_VFO_APPLY, 0); }
bool radio_vfo_apply_take(channel_t *out) { *out = vx_apply_ch; return true; }
/* AM: the board's AM demodulator */
static unsigned vx_am_calls;
static uint32_t vx_am_hz;
static bool vx_rx_am(uint32_t hz, radio_bw_t bw) { (void)bw; vx_am_calls++; vx_am_hz = hz; return true; }

/* A request as radio_task.c sends it: the native receiver first (it lets the channel go), then the radio core */
static void vx_request(uint8_t type, int32_t arg)
{
    radio_req_t r = { type, arg, 0 };
    for (int i = 0; i < 20; i++) {
        int h = native_lab_request(&sm, &r);
        if (h == NATIVE_REQUEST_WAIT) { classic_pump(); continue; }
        if (!h) radio_sm_request(&sm, &r, now);
        return;
    }
    CHECK(!"the native receiver never let go");
}

static void vx_pump(int n)
{
    for (int i = 0; i < n; i++) {
        now += RADIO_POLL_MS;
        if (native_lab_owns()) radio_sm_native_maintenance(&sm, now);
        else radio_sm_poll(&sm, now);
        classic_pump();
    }
}

/* The VFO in use with mode m and P25 / native parameters as the VFO settings screen sends them */
static void vx_apply(uint32_t hz, uint8_t m, uint16_t nac, uint16_t tg, uint32_t id)
{
    channel_t c = sm.view.ch;
    c.rx_hz = c.tx_hz = hz;
    vfox_set_mode(&c, m, 0);
    if (vfox_native(m)) { c.p25_nac = nac; c.p25_tg = tg; c.p25_id = id; }
    radio_req_vfo_apply(&c);
}

static void vx_start(void)
{
    setup_codeplug();
    start();
    native_lab_init();
    fake_power.switch_on = classic_on;
    fake_power.battery_mv = classic_battery;
    classic_busy = classic_stop = classic_receiving = false;
    classic_starts = classic_restores = 0;
    fake_radio.rx_am = vx_rx_am;
    vx_request(RREQ_VFO, 1);
    CHECK(sm.view.vfo);
}

/* 145.575 MHz, P25, TG 99, NAC $456: the native receiver takes the VFO with them; PTT is allowed (TX gates as a P25
 * channel's); talkgroup Any or no unit ID: listen only; NAC Any talks with $293 (Classic r13) */
static void test_vfox_p25(void)
{
    vx_start();
    unsigned sets = classic_sets;
    vx_apply(145575000, RADIO_MODE_P25, 0x456, 99, 3101234);
    CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_P25 && sm.vfo_ch.p25_nac == 0x456 && sm.vfo_ch.p25_tg == 99);
    CHECK(sm.view.ch.rx_hz == 145575000 && !sm.view.ch.rx_only && sm.view.ch.bw == RADIO_BW_12K5);
    vx_pump(4);
    CHECK(classic_sets > sets && classic_set_nac == 0x456 && classic_set_tg == 99 && native_lab_owns());
    CHECK(radio_sm_tx_check(&sm) == TXDENY_NONE);
    vx_apply(145575000, RADIO_MODE_P25, 0xF7E, 99, 3101234);
    vx_pump(6);
    /* NAC Any: Classic r13's TX NAC Same as RX is then $293, so it may talk (DDM32 Plus on the r13 base) */
    CHECK(classic_set_nac == 0xF7E && !sm.view.ch.rx_only && radio_sm_tx_check(&sm) == TXDENY_NONE);
    vx_apply(145575000, RADIO_MODE_P25, 0x456, 0, 3101234);
    vx_pump(6);
    CHECK(classic_set_tg == 0 && radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
    vx_apply(145575000, RADIO_MODE_P25, 0x456, 99, 0);
    CHECK(radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
    /* PTT through the native receiver on a valid P25 VFO: the transmitter is armed on the VFO's TX frequency */
    vx_apply(145575000, RADIO_MODE_P25, 0x456, 99, 3101234);
    vx_pump(6);
    radio_req_t p = { .type = RREQ_PTT, .arg = 1 };
    CHECK(native_lab_request(&sm, &p) == 1 && sm.ptt && sm.view.state != RS_TX_DENIED);
    classic_pump(); classic_pump();
    CHECK(classic_live && classic_live->config.mode == NT_TRANSMIT && classic_live->config.frequency_hz == 145575000);
    p.arg = 0; CHECK(native_lab_request(&sm, &p) == 1 && !sm.ptt);
    vx_pump(6);
    /* Out of band for P25 TX (the 70 cm / 2 m rule) */
    vx_apply(160000000, RADIO_MODE_P25, 0x456, 99, 3101234);
    CHECK(radio_sm_tx_check(&sm) == TXDENY_BAND);
    /* Back to FM with the quick mode cycle's request: the native receiver lets go, the FM path is tuned */
    vx_request(RREQ_MODE, RADIO_MODE_FM);
    vx_pump(6);
    CHECK(sm.view.ch.mode == RADIO_MODE_FM && !native_lab_owns() && sm.view.ch.listen_list == 0 && sm.view.ch.dmr_id == 0);
    printf("Plus VFO P25: NAC $456 TG 99 taken by the native receiver, TX gates, back to FM\n");
}

/* The other modes: YSF (native receiver, DG-ID), NXDN and dPMR (the FM path), AM anywhere, DMR Any */
static void test_vfox_modes(void)
{
    vx_start();
    vx_request(RREQ_MODE, RADIO_MODE_YSF);
    CHECK(sm.view.ch.mode == RADIO_MODE_YSF && sm.view.ch.rx_only && sm.view.ch.p25_tg == 0);
    vx_apply(430500000, RADIO_MODE_YSF, 0, 12, 0);
    vx_pump(6);
    CHECK(native_lab_owns() && vx_ysf_on && vx_ysf_dgid == 12 && radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
    vx_apply(446006250, RADIO_MODE_NXDN, 0, 5, 0);
    vx_pump(6);
    CHECK(!native_lab_owns() && sm.view.ch.mode == RADIO_MODE_NXDN && hw.rx_hz == 446006250);
    /* radio_nxdn_poll's gate is the view's mode and the FM board (this fixture builds without its APRS receive path,
     * so the poll itself isn't reached here): the VFO is an NXDN channel to it */
    CHECK(codeplug_mode_nxdn(sm.view.ch.mode) && sm.board_mode == RADIO_MODE_FM && sm.view.ch.p25_tg == 5 &&
          sm.view.ch.rx_only && radio_sm_tx_check(&sm) == TXDENY_MODE);
    vx_apply(446006250, RADIO_MODE_DPMR, 0, 8, 0);	/* CC 7 */
    vx_pump(2);
    CHECK(codeplug_mode_dpmr(sm.view.ch.mode) && sm.board_mode == RADIO_MODE_FM && sm.view.ch.p25_tg == 8 &&
          radio_sm_tx_check(&sm) == TXDENY_MODE);
    (void)vx_nxdn; (void)vx_dpmr;
    /* AM on 433.500 MHz (outside the air band): the AM demodulator, receive only */
    vx_request(RREQ_MODE, VFOX_AM);
    CHECK(sm.view.ch.mode == RADIO_MODE_FM && sm.view.ch.am && sm.view.am && vx_am_calls > 0);
    CHECK(radio_sm_tx_check(&sm) == TXDENY_AM);
    vx_request(RREQ_MODE, RADIO_MODE_FM);
    CHECK(!sm.view.ch.am && !sm.view.am && radio_sm_tx_check(&sm) == TXDENY_NONE);
    /* The air band: no other mode than FM (AM there by itself) */
    vx_request(RREQ_VFO_SET, 118000000);
    vx_request(RREQ_MODE, RADIO_MODE_P25);
    CHECK(sm.view.ch.mode == RADIO_MODE_FM);
    /* DMR Any: color code, timeslot and every group; listen only */
    vx_request(RREQ_VFO_SET, 438500000);
    vx_request(RREQ_MODE, RADIO_MODE_DMR);
    channel_t c = sm.view.ch;
    c.color_code = CODEPLUG_CC_ANY; c.timeslot = CODEPLUG_TS_ANY; c.listen_list = CODEPLUG_LIST_ANY;
    radio_req_vfo_apply(&c);
    CHECK(sm.view.ch.mode == RADIO_MODE_DMR && sm.view.ch.color_code == CODEPLUG_CC_ANY &&
          sm.view.ch.timeslot == CODEPLUG_TS_ANY && sm.view.ch.listen_list == CODEPLUG_LIST_ANY);
    CHECK(radio_sm_tx_check(&sm) == TXDENY_MODE);
    printf("Plus VFO modes: YSF DG-ID, NXDN RAN, dPMR CC, AM on UHF, DMR Any\n");
}

/* Kept across a restart: each mode on VFO A, and on VFO B (dual watch's second VFO); an older record reads as before */
static void test_vfox_persist(void)
{
    static const struct { uint8_t m; uint16_t nac, tg; uint32_t id; } cases[] = {
        { RADIO_MODE_P25, 0x456, 99, 3101234 }, { RADIO_MODE_P25, 0xFFF, 65535, 9999999 },
        { RADIO_MODE_P25, 0xF7E, 0, 0 }, { RADIO_MODE_YSF, 0, 127, 0 }, { RADIO_MODE_NXDN, 0, 63, 0 },
        { RADIO_MODE_DPMR, 0, 64, 0 }, { VFOX_AM, 0, 0, 0 },
    };
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        vx_start();
        vx_apply(433500000, cases[k].m, cases[k].nac, cases[k].tg, cases[k].id);
        channel_t before = sm.vfo_ch;
        boot();
        CHECK(sm.view.vfo && vfox_mode_of(&sm.vfo_ch) == cases[k].m && sm.vfo_ch.rx_hz == 433500000);
        CHECK(sm.vfo_ch.p25_nac == before.p25_nac && sm.vfo_ch.p25_tg == before.p25_tg &&
              sm.vfo_ch.p25_id == before.p25_id && sm.vfo_ch.rx_only == before.rx_only);
        CHECK(sm.vfo_ch.rx_tone.type == TONE_NONE && sm.vfo_ch.tx_tone.type == TONE_NONE);
    }
    /* DMR Any kept; the talk target beside it too */
    vx_start();
    vx_request(RREQ_VFO_SET, 438500000);
    vx_request(RREQ_MODE, RADIO_MODE_DMR);
    req(RREQ_DMR_TARGET, 3100);
    channel_t c = sm.view.ch;
    c.color_code = CODEPLUG_CC_ANY; c.listen_list = CODEPLUG_LIST_ANY;
    radio_req_vfo_apply(&c);
    boot();
    CHECK(sm.vfo_ch.mode == RADIO_MODE_DMR && sm.vfo_ch.color_code == CODEPLUG_CC_ANY &&
          sm.vfo_ch.listen_list == CODEPLUG_LIST_ANY && sm.vfo_ch.timeslot == 1 && sm.vfo_talk_id == 3100);
    /* An older record (upstream's VFO: FM with tones, an own DMR ID in the field it no longer reads): unchanged */
    settings_t *st = settings_begin();
    settings_defaults(st);
    st->vfo = 1; st->vfo_rx_hz = st->vfo_tx_hz = 145500000;
    st->vfo_rx_tone_type = st->vfo_tx_tone_type = TONE_CTCSS; st->vfo_rx_tone = st->vfo_tx_tone = 885;
    st->unused_dmr_id = 3101234;
    st->vfo2_rx_hz = st->vfo2_tx_hz = 439000000; st->vfo2_mode = RADIO_MODE_DMR; st->vfo2_cc = 7; st->vfo2_ts = 2;
    st->vfo2_talk = 91;
    settings_end(now);
    boot();
    CHECK(sm.vfo_ch.mode == RADIO_MODE_FM && !sm.vfo_ch.am && sm.vfo_ch.rx_tone.type == TONE_CTCSS &&
          sm.vfo_ch.rx_tone.value == 885 && sm.vfo_ch.color_code == 1 && sm.vfo_ch.listen_list == 0);
    CHECK(sm.vfo_other.ch.mode == RADIO_MODE_DMR && sm.vfo_other.ch.color_code == 7 && sm.vfo_other.ch.timeslot == 2 &&
          sm.vfo_other.talk_id == 91 && sm.vfo_other.ch.listen_list == 0);
    vx_request(RREQ_VFO_STEP, 1);	/* a save of the old record: what upstream wrote stays */
    settings_t now_st;
    settings_read(&now_st);
    CHECK(now_st.unused_dmr_id == 3101234 && now_st.vfo2_talk == 91 && now_st.vfo2_cc == 7 && now_st.vfo2_ts == 2);
    settings_defaults(settings_begin());
    settings_end(now);
    printf("Plus VFO modes kept across a restart (7 cases, DMR Any); an older record reads and saves as before\n");
}

/* Copy to VFO of a P25 channel: the VFO takes its NAC, talkgroup and ID */
static void test_vfox_copy(void)
{
    vx_start();
    vx_request(RREQ_VFO, 0);
    CHECK(!sm.view.vfo);
    sm.view.ch.mode = RADIO_MODE_P25; sm.view.ch.bw = RADIO_BW_12K5; sm.view.ch.rx_hz = sm.view.ch.tx_hz = 433450000;
    sm.view.ch.p25_nac = 0x293; sm.view.ch.p25_tg = 42; sm.view.ch.p25_id = 3101234;
    CHECK(radio_vfo_copy_check(&sm.view.ch, false) == VFO_COPY_OK);
    vx_request(RREQ_VFO_COPY, 0);
    CHECK(sm.view.vfo && sm.view.ch.mode == RADIO_MODE_P25 && sm.view.ch.p25_nac == 0x293 && sm.view.ch.p25_tg == 42 &&
          sm.view.ch.p25_id == 3101234 && !sm.view.ch.rx_only);
    printf("Plus VFO: Copy to VFO of a P25 channel\n");
}

/* Multi-watch: A and B channels, C a VFO in P25 (its own VFO 2); D a VFO in YSF, E NXDN, F dPMR. The ring tunes them
 * as channels; a signal on C holds the ring and the native receiver takes C with C's NAC; A / B's VFOs stay as saved */
static void test_vfox_ring(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    fake_radio.rx_am = vx_rx_am;
    /* VFO A (0) saved as FM 146.520: it must stay so while the letters use theirs */
    radio_dual_ref_t r[RADIO_DUAL_SLOTS] = { { false, 0, 0 }, { false, 0, 1 }, { true, 0, 2 }, { true, 0, 3 },
                                             { true, 0, 4 }, { true, 0, 5 } };
    static const struct { uint8_t m; uint32_t hz; uint16_t tg; } letter[4] = {
        { RADIO_MODE_P25, 145575000, 99 }, { RADIO_MODE_YSF, 430500000, 12 }, { RADIO_MODE_NXDN, 446006250, 5 },
        { RADIO_MODE_DPMR, 446018750, 8 } };
    /* Each letter selected in turn, set as the VFO settings would (the ring tunes the selected one first) */
    for (uint8_t k = 2; k < 6; k++) {
        radio_req_mwatch(r, 6, k);
        CHECK(sm.scan.kind == SCAN_DUAL && sm.dual_n == 6);
        vx_apply(letter[k - 2].hz, letter[k - 2].m, 0x456, letter[k - 2].tg, 3101234);
        vx_pump(2);
        CHECK(sm.vfox_at == k && sm.view.vfo_index == k && sm.vfo_ch.mode == letter[k - 2].m &&
              sm.vfo_ch.rx_hz == letter[k - 2].hz);
        vx_request(RREQ_DMR_FOCUS, 0);	/* as radio_task.c: the native receiver lets go before the next request */
        if (sm.scan.kind != SCAN_DUAL)
            radio_req_mwatch(r, 6, k);	/* the UI starts the ring again after a change, as dual watch's does */
    }
    vx_request(RREQ_DMR_FOCUS, 0);
    radio_req_mwatch(r, 6, 0);
    CHECK(sm.scan.kind == SCAN_DUAL);
    /* The ring visits each letter on its own frequency */
    uint32_t seen = 0;
    for (int i = 0; i < 400 && seen != 0x3Fu; i++) {
        mw_poll();
        if (sm.scan.kind == SCAN_DUAL && sm.dual_at >= 2 && !native_lab_owns()) {
            CHECK(sm.view.vfo && sm.vfox_at == sm.dual_at && hw.rx_hz == letter[sm.dual_at - 2].hz &&
                  sm.view.ch.mode == letter[sm.dual_at - 2].m);
        }
        seen |= 1u << sm.dual_at;
    }
    CHECK(seen == 0x3Fu);
    /* A P25 signal on C: the ring holds, the native receiver takes C with its NAC and talkgroup */
    mw_busy_hz = 145575000;
    bool took = false;
    for (int i = 0; i < 400 && !took; i++) {
        mw_poll();
        took = native_lab_owns() && sm.dual_at == 2;
    }
    CHECK(took && classic_set_nac == 0x456 && classic_set_tg == 99);
    mw_busy_hz = 0;
    for (int i = 0; i < 600 && native_lab_owns(); i++)
        mw_poll();
    CHECK(!native_lab_owns() && sm.scan.kind == SCAN_DUAL);
    /* The TX gates on the letters: C (P25 with NAC, talkgroup and ID) may transmit, D (YSF) and E (NXDN) only listen */
    radio_req_mwatch(r, 6, 2);
    for (int i = 0; i < 400 && !(sm.dual_at == 2 && sm.view.vfo && !native_lab_owns()); i++)
        mw_poll();
    CHECK(sm.dual_sel == 2 && sm.dual_at == 2 && radio_sm_tx_check(&sm) == TXDENY_NONE);
    for (int i = 0; i < 400 && sm.dual_at != 3; i++)
        mw_poll();
    CHECK(sm.dual_at == 3 && radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
    for (int i = 0; i < 400 && sm.dual_at != 4; i++)
        mw_poll();
    CHECK(sm.dual_at == 4 && radio_sm_tx_check(&sm) == TXDENY_MODE);
    /* A / B's VFOs: VFO A still FM as saved (the letters' VFOs are their own) */
    settings_t st;
    settings_read(&st);
    CHECK(settings_vfo_mode(&st) == RADIO_MODE_FM && (st.vfo2_mode & 0x0Fu) == RADIO_MODE_FM);
    CHECK(st.vfo_index <= 1);
    /* The stop on C: the radio stays on C's VFO; a restart comes back on A / B's (C..F are RAM only) */
    radio_req_dual(NULL, 0);
    CHECK(sm.scan.kind == SCAN_OFF && sm.view.vfo && sm.vfox_at == 2 && sm.view.ch.mode == RADIO_MODE_P25);
    vx_request(RREQ_VFO_STEP, 1);	/* a save */
    boot();
    CHECK(sm.vfox_at == 0 && sm.vfo_idx <= 1 && sm.vfo_ch.mode == RADIO_MODE_FM);
    printf("Plus VFO modes in multi-watch: C..F their own VFOs (P25, YSF, NXDN, dPMR), the ring tunes each, P25 hold "
           "and native receive on C, A / B's saved VFOs untouched, C..F RAM only\n");
}

/* M5: PTT on VFO letters of the ring (the radio task's way, native_lab_request first): C a P25 VFO (145.575, NAC $456,
 * TG 99) keys natively on its own frequency with its NAC from any ring position; D a YSF VFO is refused (no YSF
 * transmitter in this build): TX denied, nothing keyed, natively or by the board; both cleared on release */
static void test_vfox_ring_tx(void)
{
    int before = failures;
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    fake_radio.rx_am = vx_rx_am;
    codec_present = true;
    classic_retained = false;
    mw_busy_hz = mwx_voice_hz = 0;
    radio_dual_ref_t r[RADIO_DUAL_SLOTS] = { { false, 0, 0 }, { false, 0, 1 }, { true, 0, 2 }, { true, 0, 3 } };
    radio_req_mwatch(r, 4, 2);
    vx_apply(145575000, RADIO_MODE_P25, 0x456, 99, 3101234);
    vx_pump(2);
    vx_request(RREQ_DMR_FOCUS, 0);
    radio_req_mwatch(r, 4, 3);
    vx_apply(430500000, RADIO_MODE_YSF, 0, 12, 3101234);
    vx_pump(2);
    vx_request(RREQ_DMR_FOCUS, 0);
    for (uint8_t sel = 2; sel < 4; sel++) {
        for (uint8_t at = 0; at < 4; at++) {
            snprintf(mwx_tag, sizeof mwx_tag, "VFO letter %c (%s), ring at %c", 'A' + sel, sel == 2 ? "P25" : "YSF", 'A' + at);
            radio_req_mwatch(r, 4, sel);
            MWX_CHECK(sm.scan.kind == SCAN_DUAL && sm.dual_sel == sel);
            MWX_CHECK(mw_until_at(at, 3000));
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
            if (sel == 2) {
                MWX_CHECK(keyed == 'P' && hz == 145575000 && classic_set_nac == 0x456 && classic_set_tg == 99 &&
                          sm.dual_at == 2 && sm.view.vfo);
            } else {
                MWX_CHECK(keyed == 0 && sm.view.state == RS_TX_DENIED && hw.tx_calls == tx0 && !mwx_native_tx());
            }
            mwx_send(RREQ_PTT, 0);
            for (int i = 0; i < 320; i++)
                mwx_dispatch();
            MWX_CHECK(!sm.ptt && sm.view.state != RS_TX && sm.view.state != RS_TX_DENIED && !native_lab_owns());
        }
    }
    printf("Plus VFO modes in multi-watch: PTT on a P25 VFO letter keys it, on a YSF one refused: %d failures\n",
           failures - before);
}

/* Dual watch (2 channels): B the VFO in P25: taken while the watch holds there */
static void test_vfox_dual(void)
{
    mw_codeplug(RADIO_MODE_FM, -1);
    mw_start_radio();
    radio_dual_ref_t r[2] = { { false, 0, 0 }, { true, 0, 1 } };
    radio_req_dual(r, 1);
    CHECK(sm.scan.kind == SCAN_DUAL);
    vx_apply(145575000, RADIO_MODE_P25, 0x456, 99, 3101234);
    vx_pump(2);
    vx_request(RREQ_DMR_FOCUS, 0);	/* the UI's next request: the native receiver lets go, as radio_task.c has it */
    if (sm.scan.kind != SCAN_DUAL)
        radio_req_dual(r, 1);
    CHECK(sm.vfo_ch.mode == RADIO_MODE_P25 || sm.vfo_other.ch.mode == RADIO_MODE_P25);
    mw_busy_hz = 145575000;
    bool took = false;
    for (int i = 0; i < 400 && !took; i++) {
        mw_poll();
        took = native_lab_owns() && sm.view.vfo;
    }
    CHECK(took && classic_set_nac == 0x456);
    mw_busy_hz = 0;
    for (int i = 0; i < 600 && native_lab_owns(); i++)
        mw_poll();
    CHECK(!native_lab_owns() && sm.scan.kind == SCAN_DUAL);
    printf("Plus VFO P25 in dual watch: taken while the watch holds there, given back\n");
}

/* Hole hunt B4: OK on a search find outside the band plan (ui_dmrsearch.c tune_find: RREQ_VFO_SET, then the VFO's copy
 * with rx_only) makes the VFO receive only: PTT keys nothing. A new frequency (FM / DMR) transmits again; a find inside
 * the band plan (rx_only false) transmits as before */
static void test_vfox_find_rx_only(void)
{
    static const uint8_t modes[] = { RADIO_MODE_FM, RADIO_MODE_DMR };
    for (unsigned i = 0; i < sizeof modes; i++) {
        vx_start();
        vx_request(RREQ_VFO_SET, 159125000);
        channel_t c = sm.view.ch;
        c.rx_hz = c.tx_hz = 159125000;
        vfox_set_mode(&c, modes[i], 0);
        c.rx_only = true;
        radio_req_vfo_apply(&c);
        vx_pump(5);
        CHECK(sm.view.vfo && sm.view.ch.rx_hz == 159125000 && sm.view.ch.mode == modes[i]);
        CHECK(sm.vfo_ch.rx_only && radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
        int tx0 = hw.tx_calls;
        vx_request(RREQ_PTT, 1);
        vx_pump(5);
        CHECK(hw.tx_calls == tx0);
        vx_request(RREQ_PTT, 0);
        vx_pump(20);
        /* the same frequency again keeps it; a new one ends it */
        vx_request(RREQ_VFO_SET, 159125000);
        CHECK(radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
        vx_request(RREQ_VFO_SET, 145500000);
        vx_pump(5);
        CHECK(!sm.vfo_ch.rx_only && !sm.view.ch.rx_only && radio_sm_tx_check(&sm) == TXDENY_NONE);
        /* a find inside the band plan: the VFO transmits */
        vx_request(RREQ_VFO_SET, 433500000);
        c = sm.view.ch;
        c.rx_hz = c.tx_hz = 433500000;
        vfox_set_mode(&c, modes[i], 0);
        c.rx_only = false;
        radio_req_vfo_apply(&c);
        vx_pump(5);
        CHECK(sm.view.ch.rx_hz == 433500000 && radio_sm_tx_check(&sm) == TXDENY_NONE);
    }
    /* a P25 find stays receive only after a new frequency (NAC Any: vfox_fix's own rule) */
    vx_start();
    vx_request(RREQ_VFO_SET, 159125000);
    channel_t c = sm.view.ch;
    c.rx_hz = c.tx_hz = 159125000;
    vfox_set_mode(&c, RADIO_MODE_P25, 0);
    c.rx_only = true;
    radio_req_vfo_apply(&c);
    vx_pump(5);
    vx_request(RREQ_VFO_SET, 145500000);
    vx_pump(5);
    CHECK(sm.view.ch.mode == RADIO_MODE_P25 && radio_sm_tx_check(&sm) != TXDENY_NONE);
    vx_request(RREQ_VFO, 0);
    vx_pump(20);
    printf("Plus every mode in the VFO: a search find outside the band plan is receive only until the frequency changes\n");
}

/* Hole hunt, second round (CONFIG_VFO_PLAN): the VFO transmits only in the amateur bands, whatever made it (Heard,
 * a find, typed, the VFO settings) and after a restart: 159.125 refused in FM, DMR and a P25 that could otherwise
 * talk; PMR446 refused; 145.500 simplex keys; 438.650 simplex (a repeater output) refused, with TX 431.050 (the
 * repeater's input) allowed; a codeplug channel outside the bands is the user's (not affected). A find's receive only
 * ends when a split is set (RREQ_VFO_TX) */
static void vx_plan(uint32_t hz, uint8_t m, uint8_t want)
{
    vx_request(RREQ_VFO_SET, (int32_t)hz);
    vx_apply(hz, m, 0x293, 99, 3101234);
    vx_pump(5);
    CHECK(sm.view.vfo && sm.view.ch.rx_hz == hz && sm.view.ch.mode == m);
    if (radio_sm_tx_check(&sm) != want)
        printf("  VFO %u Hz mode %u: deny %u, want %u\n", (unsigned)hz, m, radio_sm_tx_check(&sm), want);
    CHECK(radio_sm_tx_check(&sm) == want);
}

#if CONFIG_VFO_PLAN
bool vfo_plan_test_on;	/* the plan only here (upstream's suites put the VFO anywhere): radio_sm.c VFO_PLAN_TEST */
#endif
static void test_vfox_plan(void)
{
#if CONFIG_VFO_PLAN
    vfo_plan_test_on = true;
    static const uint8_t modes[] = { RADIO_MODE_FM, RADIO_MODE_DMR, RADIO_MODE_P25 };
    vx_start();
    for (unsigned i = 0; i < sizeof modes; i++) {
        vx_plan(159125000, modes[i], TXDENY_VFO_PLAN);
        vx_plan(145500000, modes[i], TXDENY_NONE);
    }
    vx_plan(446006250, RADIO_MODE_FM, TXDENY_VFO_PLAN);
    vx_plan(438650000, RADIO_MODE_FM, TXDENY_VFO_PLAN);
    int tx0 = hw.tx_calls;
    vx_request(RREQ_PTT, 1);
    vx_pump(5);
    CHECK(hw.tx_calls == tx0 && sm.view.state != RS_TX);
    vx_request(RREQ_PTT, 0);
    vx_pump(20);
    vx_request(RREQ_VFO_TX, 431050000);
    CHECK(sm.view.ch.tx_hz == 431050000 && radio_sm_tx_check(&sm) == TXDENY_NONE);
    /* restart: the VFO comes back on 159.125 from the settings, still refused */
    vx_plan(159125000, RADIO_MODE_FM, TXDENY_VFO_PLAN);
    boot();
    CHECK(sm.view.vfo && sm.view.ch.rx_hz == 159125000 && radio_sm_tx_check(&sm) == TXDENY_VFO_PLAN);
    /* a search find's receive only on a repeater output ends with the split */
    vx_request(RREQ_VFO_SET, 438650000);
    channel_t c = sm.view.ch;
    c.rx_hz = c.tx_hz = 438650000;
    vfox_set_mode(&c, RADIO_MODE_FM, 0);
    c.rx_only = true;
    radio_req_vfo_apply(&c);
    vx_pump(5);
    CHECK(radio_sm_tx_check(&sm) == TXDENY_RX_ONLY);
    vx_request(RREQ_VFO_TX, 431050000);
    CHECK(!sm.vfo_ch.rx_only && radio_sm_tx_check(&sm) == TXDENY_NONE);
    /* a codeplug channel is not affected (channel 2 of the fixture: 162.550, receive only by its own flag; channel 1
     * 146.520, outside Region 1's 2 m band, still transmits) */
    vx_request(RREQ_VFO, 0);
    vx_pump(20);
    CHECK(!sm.view.vfo && sm.view.ch.rx_hz == 146520000 && radio_sm_tx_check(&sm) == TXDENY_NONE);
    vfo_plan_test_on = false;
    printf("Plus VFO band plan: 159.125 / PMR446 / simplex on 438.650 refused, 145.500 and the repeater split allowed, "
           "also after a restart; codeplug channels unchanged\n");
#endif
}

int main(void)
{
    int prior = mwatch_radio_main();
    int before = failures;
    test_vfox_p25();
    test_vfox_modes();
    test_vfox_persist();
    test_vfox_copy();
    test_vfox_dual();
    test_vfox_ring();
    test_vfox_ring_tx();
    test_vfox_find_rx_only();
    test_vfox_plan();
    printf("Plus every mode in the VFO, radio core: %d failures\n", failures - before);
    return prior || failures != before ? 1 : 0;
}
