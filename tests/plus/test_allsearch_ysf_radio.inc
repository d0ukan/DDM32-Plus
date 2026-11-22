/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus all digital search with YSF built in, in the real radio state machine and native admission (native_lab.c
 * all_heard, B5 of the beta.7 hole hunt): a YSF probe's find carries only a callsign heard after the native receiver
 * took the frequency. The worker's last callsign (a call on another frequency, heard before) must not land in the
 * find, nor in the PC's search log; a callsign heard in the probe does. Built by tools/test_classic.py with
 * CONFIG_ALL_SEARCH=1 and CONFIG_YSF=1 after tests/classic/test_radio.inc (whose main is not run here: its all digital
 * suite has no YSF probe in its order); reuses that suite's as_* helpers (test_allsearch_sm.inc) and the zone scan's
 * sync fixture (zs_sync_ms, test_zscan.inc). */
#include <stdio.h>

/* The worker's YSF switch, DG-ID and caller (p25_lab.c): the caller's sequence counts changes */
static int ay_on;
static uint8_t ay_dgid;
static uint32_t ay_seq;
static char ay_src[11];
int p25_lab_set_ysf(int on, uint8_t dgid)
{
    (void)dgid;
    if (!classic_busy) ay_on = on != 0;
    return !classic_busy;
}
uint8_t p25_lab_ysf_dgid(void) { return ay_dgid; }
uint32_t p25_lab_ysf_caller(char src[11], char dst[11])
{
    if (src) snprintf(src, 11, "%s", ay_src);
    if (dst) dst[0] = 0;
    return ay_seq;
}

static const dmrs_found_t *ay_entry(const dmrs_view_t *v, uint32_t hz)
{
    for (unsigned i = 0; i < v->found_n; i++)
        if (v->found[i].hz == hz && v->found[i].mode == DMRS_MODE_YSF)
            return &v->found[i];
    return NULL;
}

static void test_plus_allsearch_ysf_sm(void)
{
    int before = failures;
    dmrs_view_t v;
    const dmrs_found_t *e;
    char call[11];
    classic_select(false);
    sm.view.ch.mode = RADIO_MODE_FM;
    ps_frames = 0; ps_nac = 0; ps_tg = 0; ps_src = 0; scan_voice_ms = 0; zs_sync_ms = 0;
    /* the worker still holds the last call's callsign, from another frequency */
    snprintf(ay_src, sizeof ay_src, "%s", "TA1OLD");
    ay_seq = 7;
    ay_dgid = 0;
    radio_req_t r = { .type = RREQ_ALL_SEARCH, .arg = 433000000, .arg2 = 434000000 };
    CHECK(native_lab_request(&sm, &r) == 0);
    radio_sm_request(&sm, &r, now);
    CHECK(dmrs_on() && dmrs_kind() == DMRS_KIND_ALL);

    /* the carrier at 433.500: P25 and DMR hear nothing, then the native receiver takes it for YSF */
    as_to_probe();
    CHECK(dmrs_phase() == DMRS_PROBE && dmrs_at_hz() == 433500000u);
    for (int i = 0; i < 400 && dmrs_on() && dmrs_mode() != DMRS_MODE_YSF; i++) as_step();
    CHECK(dmrs_mode() == DMRS_MODE_YSF && dmrs_at_hz() == 433500000u);
    for (int i = 0; i < 20 && !native_lab_owns(); i++) as_step();
    CHECK(native_lab_owns() && ay_on);

    /* YSF frames (DG-ID 12), no callsign of this call yet: found, without the old caller */
    ay_dgid = 12;
    for (int i = 0; i < 6; i++) { zs_sync_ms = now; as_step(); }
    CHECK(dmrs_snapshot_all(&v) && (e = ay_entry(&v, 433500000u)) && e->cc == 12);
    if (e) {
        dmrs_call_unpack(e->dst, e->src, call);
        CHECK(e->dst == 0 && e->src == 0 && call[0] == 0);
    }

    /* this call's callsign arrives (the sequence moves): it is the find's */
    snprintf(ay_src, sizeof ay_src, "%s", "TA2NEW");
    ay_seq++;
    for (int i = 0; i < 6; i++) { zs_sync_ms = now; as_step(); }
    CHECK(dmrs_snapshot_all(&v) && (e = ay_entry(&v, 433500000u)));
    if (e) {
        dmrs_call_unpack(e->dst, e->src, call);
        CHECK(strcmp(call, "TA2NEW") == 0);
    }

    r.arg = 0;
    radio_sm_request(&sm, &r, now);
    for (int i = 0; i < 50 && native_lab_owns(); i++) { now += 10; classic_pump(); }
    CHECK(!dmrs_on());
    printf("%s all digital search with YSF: a callsign heard before the take is not the find's, this call's is\n",
           failures == before ? "ok  " : "FAIL");
}

int main(void)
{
    test_plus_allsearch_ysf_sm();
    printf("All digital search YSF caller in the radio core: %d failures\n", failures);
    return failures ? 1 : 0;
}
