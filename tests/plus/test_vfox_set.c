/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus: every mode in the VFO, the settings record (integration/radiofoundry/vfox/vfox.c vfox_store / vfox_load,
 * docs/plus.md "Every mode in the VFO": the byte map), on the actual settings store and the simulated DM32 flash:
 * - settings_t is upstream's 392 B plus the hold keys' 12 (404 B) and keeps every field's offset this feature writes;
 * - each mode with its parameters, on VFO A and on VFO B, kept across power cycles; edge values (NAC $FFF, talkgroup
 *   65535, unit ID 9999999 and 8388608 (bit 23), Any); AM; DMR's Any flags;
 * - an older record (upstream's FM / DMR VFOs, an own DMR ID in unused_dmr_id) loads unchanged, and a save of the same
 *   VFOs writes it back byte for byte;
 * - what upstream firmware reads from a record this feature wrote: FM (an unknown mode), no tone (the marked type bytes),
 *   a color code and slot in range;
 * - the ring letters' compact VFOs (vfox_slot_put / _get) round trip, a slot never set reads as a fresh VFO.
 * Built by tools/test_classic.py with -DCONFIG_VFO_MODES=1 -DCONFIG_YSF=1 -DCONFIG_NXDN=1 -DCONFIG_DPMR=1
 * -DCONFIG_DMR_ANY=1. */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "radio/vfox.h"
#include "radio/tones.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)
#define SIZE 0x1000000u
static const hal_flash_map_t map = {
    .settings_addr=0x711000, .settings_size=0x8000, .journal_addr=0x719000,
    .openfw_eeprom=0x700000, .openfw_flash=0x720000, .openfw_end=0xF00000,
    .native_addr=0xF00000, .native_size=0x100000
};
static const band_t bands[] = {{108000000,136000000,false},{136000000,174000000,true},{400000000,480000000,true}};
static bool am_rx(uint32_t hz, radio_bw_t bw) { (void)hz; (void)bw; return true; }
static const hal_radio_t radio = { .rx_am = am_rx };
const board_t board = {.name="DM32 test", .flash=&sim_flash, .bands=bands, .band_count=3, .power_count=9,
                       .caps=CAP_AM_RX, .radio=&radio};
void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
uint32_t hal_millis(void) { return 10000; }

/* What upstream's radio_sm.c restore_vfo makes of VFO idx (the VFO as upstream firmware reads it) */
static void upstream_restore(const settings_t *st, uint8_t idx, channel_t *c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->name, "VFO");
    uint8_t mode = idx ? st->vfo2_mode : settings_vfo_mode(st);
    uint8_t cc = idx ? st->vfo2_cc : settings_vfo_cc(st), ts = idx ? st->vfo2_ts : settings_vfo_ts(st);
    c->rx_hz = idx ? st->vfo2_rx_hz : st->vfo_rx_hz;
    c->tx_hz = idx ? st->vfo2_tx_hz : st->vfo_tx_hz;
    c->mode = mode == RADIO_MODE_DMR ? RADIO_MODE_DMR : RADIO_MODE_FM;
    c->bw = (idx ? st->vfo2_bw : settings_vfo_bw(st)) == RADIO_BW_25K ? RADIO_BW_25K : RADIO_BW_12K5;
    c->power = CODEPLUG_POWER_DEFAULT;
    c->color_code = cc <= 15 ? cc : 1;
    c->timeslot = ts == 2 ? 2 : 1;
    subtone_t rx = { idx ? st->vfo2_rx_tone_type : st->vfo_rx_tone_type, idx ? st->vfo2_rx_tone : st->vfo_rx_tone };
    subtone_t tx = { idx ? st->vfo2_tx_tone_type : st->vfo_tx_tone_type, idx ? st->vfo2_tx_tone : st->vfo_tx_tone };
    c->rx_tone = tones_valid(&rx) ? rx : (subtone_t){ TONE_NONE, 0 };
    c->tx_tone = tones_valid(&tx) ? tx : (subtone_t){ TONE_NONE, 0 };
}

/* ... and what upstream's persist_vfo writes for v (the fields this feature then adds to) */
static void upstream_persist(settings_t *st, uint8_t idx, const channel_t *v)
{
    if (idx) {
        st->vfo2_rx_hz = v->rx_hz; st->vfo2_tx_hz = v->tx_hz; st->vfo2_mode = v->mode; st->vfo2_bw = v->bw;
        st->vfo2_cc = v->color_code; st->vfo2_ts = v->timeslot;
        st->vfo2_rx_tone_type = v->rx_tone.type; st->vfo2_rx_tone = v->rx_tone.value;
        st->vfo2_tx_tone_type = v->tx_tone.type; st->vfo2_tx_tone = v->tx_tone.value;
        st->vfo2_talk = 91;	/* the talk target, as persist_vfo writes it */
        return;
    }
    st->vfo_rx_hz = v->rx_hz; st->vfo_tx_hz = v->tx_hz;
    settings_set_vfo_mode_bw(st, v->mode, v->bw);
    settings_set_vfo_cc_ts(st, v->color_code, v->timeslot);
    st->vfo_rx_tone_type = v->rx_tone.type; st->vfo_rx_tone = v->rx_tone.value;
    st->vfo_tx_tone_type = v->tx_tone.type; st->vfo_tx_tone = v->tx_tone.value;
    settings_set_vfo_talk(st, 3100, false);
}

static void power_cycle(void)
{
    CHECK(settings_flush());
    sim_flash_power_on();
    CHECK(storage_init());
    CHECK(settings_init() == SETTINGS_LOADED);
}

/* v into VFO idx as the radio core saves it, through a power cycle, back as the radio core loads it */
static void round_trip(uint8_t idx, const channel_t *v, channel_t *out)
{
    settings_t *st = settings_begin();
    upstream_persist(st, idx, v);
    vfox_store(st, idx, v);
    settings_end(hal_millis());
    power_cycle();
    settings_t rd;
    settings_read(&rd);
    upstream_restore(&rd, idx, out);
    vfox_load(&rd, idx, out);
}

static channel_t vfo(uint32_t hz, uint8_t m, uint16_t nac, uint16_t tg, uint32_t id)
{
    channel_t c;
    memset(&c, 0, sizeof c);
    strcpy(c.name, "VFO");
    c.rx_hz = c.tx_hz = hz;
    c.power = CODEPLUG_POWER_DEFAULT;
    c.color_code = 1;
    c.timeslot = 1;
    c.rx_tone = c.tx_tone = (subtone_t){ TONE_CTCSS, 885 };	/* the FM tones it had before */
    vfox_set_mode(&c, m, 0);
    if (vfox_native(m)) { c.p25_nac = nac; c.p25_tg = tg; c.p25_id = id; vfox_fix(&c); }
    return c;
}

int main(void)
{
    /* The layout this feature relies on: upstream's size and the fields' places (docs/plus.md byte map) */
    CHECK(sizeof(settings_t) == 404);  /* upstream 392 + hold_tool[12], appended: no field moves */
    CHECK(offsetof(settings_t, vfo_mode) == 20 && offsetof(settings_t, vfo_bw) == 21 && offsetof(settings_t, vfo_cc) == 23);
    CHECK(offsetof(settings_t, vfo_ts) == 24 && offsetof(settings_t, vfo_rx_tone_type) == 25 &&
          offsetof(settings_t, vfo_tx_tone_type) == 26 && offsetof(settings_t, vfo_rx_tone) == 28 &&
          offsetof(settings_t, vfo_tx_tone) == 30 && offsetof(settings_t, unused_dmr_id) == 40);
    CHECK(offsetof(settings_t, vfo2_talk) == 248 && offsetof(settings_t, vfo2_rx_tone) == 252 &&
          offsetof(settings_t, vfo2_tx_tone) == 254 && offsetof(settings_t, vfo2_mode) == 256 &&
          offsetof(settings_t, vfo2_cc) == 259 && offsetof(settings_t, vfo2_ts) == 260 &&
          offsetof(settings_t, vfo2_rx_tone_type) == 261 && offsetof(settings_t, vfo2_tx_tone_type) == 262);
    sim_flash_create(SIZE, &map);
    memset(sim_flash_data, 0xFF, SIZE);
    CHECK(storage_init());
    settings_init();

    /* An older record: upstream's VFOs (FM with tones on A, an own DMR ID in unused_dmr_id; DMR CC 7 TS 2 on B) */
    settings_t *st = settings_begin();
    st->vfo_rx_hz = st->vfo_tx_hz = 145500000;
    settings_set_vfo_mode_bw(st, RADIO_MODE_FM, RADIO_BW_25K);
    st->vfo_rx_tone_type = st->vfo_tx_tone_type = TONE_CTCSS;
    st->vfo_rx_tone = st->vfo_tx_tone = 885;
    settings_set_vfo_talk(st, 3100, false);
    st->unused_dmr_id = 3101234;
    st->vfo2_rx_hz = st->vfo2_tx_hz = 439000000;
    st->vfo2_mode = RADIO_MODE_DMR; st->vfo2_cc = 7; st->vfo2_ts = 2; st->vfo2_talk = 91;
    settings_end(hal_millis());
    power_cycle();
    settings_t old;
    settings_read(&old);
    channel_t a, b, a0, b0;
    upstream_restore(&old, 0, &a0);
    a = a0;
    vfox_load(&old, 0, &a);
    upstream_restore(&old, 1, &b0);
    b = b0;
    vfox_load(&old, 1, &b);
    CHECK(memcmp(&a, &a0, sizeof a) == 0 && a.mode == RADIO_MODE_FM && !a.am && a.rx_tone.value == 885);
    CHECK(memcmp(&b, &b0, sizeof b) == 0 && b.mode == RADIO_MODE_DMR && b.color_code == 7 && b.timeslot == 2 &&
          b.listen_list == 0);
    /* ... and saving the same VFOs changes no byte of it */
    settings_t again = old;
    upstream_persist(&again, 0, &a);
    vfox_store(&again, 0, &a);
    upstream_persist(&again, 1, &b);
    vfox_store(&again, 1, &b);
    CHECK(memcmp(&again, &old, sizeof old) == 0);

    /* Each mode with its parameters on A and on B, across power cycles */
    static const struct { uint32_t hz; uint8_t m; uint16_t nac, tg; uint32_t id; } cases[] = {
        { 145575000, RADIO_MODE_P25, 0x456, 99, 3101234 }, { 433500000, RADIO_MODE_P25, 0xFFF, 65535, 9999999 },
        { 433500000, RADIO_MODE_P25, 0x000, 1, 8388608 }, { 433500000, RADIO_MODE_P25, 0x800, 1, 8388607 },
        { 145575000, RADIO_MODE_P25, 0xF7E, 0, 0 }, { 430500000, RADIO_MODE_YSF, 0, 12, 0 },
        { 430500000, RADIO_MODE_YSF, 0, 0, 0 }, { 446006250, RADIO_MODE_NXDN, 0, 63, 0 },
        { 446006250, RADIO_MODE_DPMR, 0, 64, 0 }, { 433500000, VFOX_AM, 0, 0, 0 }, { 145500000, RADIO_MODE_FM, 0, 0, 0 },
    };
    for (uint8_t idx = 0; idx < 2; idx++) {
        for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            channel_t v = vfo(cases[k].hz, cases[k].m, cases[k].nac, cases[k].tg, cases[k].id), got;
            round_trip(idx, &v, &got);
            CHECK(vfox_mode_of(&got) == cases[k].m && got.rx_hz == cases[k].hz);
            CHECK(got.p25_nac == v.p25_nac && got.p25_tg == v.p25_tg && got.p25_id == v.p25_id && got.rx_only == v.rx_only);
            if (vfox_native(cases[k].m)) {
                CHECK(got.rx_tone.type == TONE_NONE && got.tx_tone.type == TONE_NONE && got.bw == RADIO_BW_12K5);
                /* What upstream firmware reads there: FM (an unknown mode), no tone, a color code and slot in range */
                settings_t rd;
                settings_read(&rd);
                channel_t up;
                upstream_restore(&rd, idx, &up);
                CHECK(up.mode == RADIO_MODE_FM && up.rx_tone.type == TONE_NONE && up.tx_tone.type == TONE_NONE &&
                      up.color_code <= 15 && (up.timeslot == 1 || up.timeslot == 2));
            } else if (cases[k].m == RADIO_MODE_FM) {
                CHECK(got.rx_tone.type == TONE_CTCSS && got.rx_tone.value == 885);
            }
        }
        /* DMR Any: color code, timeslot, every talkgroup */
        channel_t d = vfo(438500000, RADIO_MODE_DMR, 0, 0, 0), got;
        d.color_code = CODEPLUG_CC_ANY; d.timeslot = CODEPLUG_TS_ANY; d.listen_list = CODEPLUG_LIST_ANY;
        round_trip(idx, &d, &got);
        CHECK(got.mode == RADIO_MODE_DMR && got.color_code == CODEPLUG_CC_ANY && got.timeslot == CODEPLUG_TS_ANY &&
              got.listen_list == CODEPLUG_LIST_ANY);
        settings_t rd;
        settings_read(&rd);
        CHECK(idx ? (rd.vfo2_talk & 0xFFFFFFu) == 91 : settings_vfo_talk_id(&rd) == 3100);
        /* Back to a plain DMR VFO: the flags go, VFO A's field holds nothing of ours */
        d.color_code = 3; d.timeslot = 2; d.listen_list = 0;
        round_trip(idx, &d, &got);
        CHECK(got.color_code == 3 && got.timeslot == 2 && got.listen_list == 0);
        settings_read(&rd);
        CHECK(idx ? (rd.vfo2_talk >> 24) == 0 : rd.unused_dmr_id == 0);
    }

    /* VFO A's flags share settings_t.unused_dmr_id with the priority channel (scanx, bits 0-10 and 16-22, tag 0x50):
     * a priority word there survives the flags coming and going, and the flags read back with it */
    {
        const uint32_t prio = 0x50000000u | (2u << 16) | 8u;	/* channel 8, zone 2 */
        settings_t *st = settings_begin();
        st->unused_dmr_id = prio;
        settings_end(hal_millis());
        channel_t d = vfo(438500000, RADIO_MODE_DMR, 0, 0, 0), got;
        d.color_code = CODEPLUG_CC_ANY; d.timeslot = CODEPLUG_TS_ANY; d.listen_list = CODEPLUG_LIST_ANY;
        round_trip(0, &d, &got);
        settings_t rd;
        settings_read(&rd);
        CHECK(got.color_code == CODEPLUG_CC_ANY && got.timeslot == CODEPLUG_TS_ANY && got.listen_list == CODEPLUG_LIST_ANY);
        CHECK((rd.unused_dmr_id & ~(7u << 11)) == prio && ((rd.unused_dmr_id >> 11) & 7u) == 7u);
        d.color_code = 3; d.timeslot = 2; d.listen_list = 0;
        round_trip(0, &d, &got);
        settings_read(&rd);
        CHECK(got.color_code == 3 && rd.unused_dmr_id == prio);	/* flags gone, the priority channel kept */
        st = settings_begin();
        st->unused_dmr_id = 0;
        settings_end(hal_millis());
    }

    /* The ring letters' compact VFOs */
    vfox_slot_t s;
    memset(&s, 0, sizeof s);
    channel_t c, back;
    uint32_t talk;
    bool seeded;
    vfox_slot_get(&s, &back, &talk, &seeded);
    CHECK(!seeded && back.mode == RADIO_MODE_FM && back.color_code == 1 && back.timeslot == 1 &&
          back.power == CODEPLUG_POWER_DEFAULT && strcmp(back.name, "VFO") == 0);
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        c = vfo(cases[k].hz, cases[k].m, cases[k].nac, cases[k].tg, cases[k].id);
        c.power = 3;
        vfox_slot_put(&s, &c, 0x80000000u | 3101234u, true);
        vfox_slot_get(&s, &back, &talk, &seeded);
        CHECK(seeded && talk == (0x80000000u | 3101234u) && memcmp(&back, &c, sizeof c) == 0);
    }
    printf("Plus every mode in the VFO, settings record: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
