/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Satellites' stored settings (ui_sat.c's "Every pass" and "Auto track": UI_SET_SAT_NOTE / UI_SET_SAT_AUTO)
 * through the actual UI storage service and settings store on the DM32 flash map (sim_flash): off in a new radio,
 * each value kept across power cycles, stored in spare bits (settings_t.cp_flags bits 1-6, rs_flags bits 6-7) next to
 * "Use anyway" and FM caller ID (cp_flags bits 0 and 7) and the radio core's rs_flags bits, none disturbing another;
 * a factory reset's defaults read off. The hold keys' ten tool codes (UI_SET_HOLD_FIRST + digit, settings_t.hold_tool,
 * the one field appended to the record): hold 4 = the ISS in a new radio and after a factory reset, the others none, each
 * kept across power cycles, none disturbing the satellites' bits or another digit. Built by tools/test_classic.py like test_fmid_set.c, with
 * -DCONFIG_SATELLITES=1 -DCONFIG_FM_ID=1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ui_svc.h"
#include "storage/storage.h"
#include "settings/settings.h"
#include "radio/radio.h"
#include "sim/sim_flash.h"
#include "dmr/dmr_data_api.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)
#define SIZE 0x1000000u
static const hal_flash_map_t map = {
    .settings_addr=0x711000, .settings_size=0x8000, .journal_addr=0x719000,
    .openfw_eeprom=0x700000, .openfw_flash=0x720000, .openfw_end=0xF00000,
    .native_addr=0xF00000, .native_size=0x100000
};
static const band_t bands[] = {{136000000,174000000,true},{400000000,480000000,true}};
const board_t board = {.name="DM32 test", .flash=&sim_flash, .bands=bands, .band_count=2, .power_count=9};
void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
uint32_t hal_millis(void) { return 10000; }
void radio_get_view(radio_view_t *out) { memset(out,0,sizeof(*out)); }
void radio_req_clear_temporary(void) {}
void radio_req_dmr_target_clear(void) {}
bool ui_notify(const ui_event_t *ev) { (void)ev; return true; }
/* The rest of ui_svc_setting_get / _set's world (other settings), never reached here */
void radio_get_settings(radio_settings_t *s) { memset(s,0,sizeof(*s)); }
uint8_t radio_volume_step(uint8_t stored) { return stored; }
uint8_t radio_volume_stored(uint8_t step) { return step; }
void radio_req_volume(uint8_t step) { (void)step; }
void radio_req_reload(void) {}
void radio_req_dmr_privacy(uint8_t type, uint16_t key) { (void)type; (void)key; }
int32_t ui_svc_ram_get(uint8_t id) { (void)id; return 0; }
bool ui_svc_ram_set(uint8_t id, int32_t value) { (void)id; (void)value; return true; }
int32_t ui_svc_reply_shown(const radio_settings_t *s) { (void)s; return 0; }
#if CONFIG_DMR_DATA_CORE
void dmr_data_get_settings(dmrDataSettings_t *out) { memset(out,0,sizeof(*out)); }
void dmr_data_set_settings(const dmrDataSettings_t *in) { (void)in; }
#endif
void zone_share_set_offers(uint8_t mode) { (void)mode; }
void hostlink_settings_changed(void) {}
void hostlink_set_hotspot_allowed(bool allowed) { (void)allowed; }
void tftp_codeplug_allow_write(bool allow) { (void)allow; }

static settings_t now_st(void)
{
    settings_t st;
    settings_read(&st);
    return st;
}

static void power_cycle(void)
{
    CHECK(settings_flush());
    sim_flash_power_on();
    CHECK(storage_init());
    CHECK(settings_init()==SETTINGS_LOADED);
}

int main(void)
{
    sim_flash_create(SIZE,&map);
    memset(sim_flash_data,0xFF,SIZE);
    CHECK(storage_init());
    settings_init();
    /* A new radio: both off */
    CHECK(ui_svc_setting_get(UI_SET_SAT_NOTE)==0 && ui_svc_setting_get(UI_SET_SAT_AUTO)==0);
    /* Hold keys: a new radio holds 4 = the ISS (code 40), every other digit none; the record is upstream's 392 B and the
     * 12 appended, still two pages */
    CHECK(sizeof(settings_t)==404 && SETTINGS_PAGES==2 && SETTINGS_HOLD_ISS==UI_HOLD_ISS);
    for (int d=0; d<10; d++)
        CHECK(ui_svc_setting_get((uint8_t)(UI_SET_HOLD_FIRST+d))==(d==4?UI_HOLD_ISS:0));
    /* "Use anyway", FM caller ID off and the radio core's roger beep and key beep set first */
    settings_t *st=settings_begin();
    st->cp_flags|=SETTINGS_CP_ACCEPTED|SETTINGS_CP_NO_FM_ID;
    st->rs_flags|=SETTINGS_RS_ROGER_BEEP|SETTINGS_RS_KEY_BEEP|SETTINGS_RS_NO_TALK_PERMIT;
    settings_end(hal_millis());
    /* Every pass for any satellite, 10 min before, 30 deg (ui_sat.c's code: 2 | (1 * 5 + 3) << 2), auto track APRS */
    int32_t note=2|((1*5+3)<<2);
    CHECK(ui_svc_setting_set(UI_SET_SAT_NOTE,note));
    CHECK(ui_svc_setting_set(UI_SET_SAT_AUTO,2));
    CHECK(ui_svc_setting_get(UI_SET_SAT_NOTE)==note && ui_svc_setting_get(UI_SET_SAT_AUTO)==2);
    power_cycle();
    CHECK(ui_svc_setting_get(UI_SET_SAT_NOTE)==note && ui_svc_setting_get(UI_SET_SAT_AUTO)==2);
    settings_t s=now_st();
    CHECK(s.cp_flags==(SETTINGS_CP_ACCEPTED|SETTINGS_CP_NO_FM_ID|(uint8_t)(note<<1)));
    CHECK((s.rs_flags&(SETTINGS_RS_ROGER_BEEP|SETTINGS_RS_KEY_BEEP|SETTINGS_RS_NO_TALK_PERMIT))==
          (SETTINGS_RS_ROGER_BEEP|SETTINGS_RS_KEY_BEEP|SETTINGS_RS_NO_TALK_PERMIT));
    CHECK((s.rs_flags&SETTINGS_RS_SAT_AUTO)==(2u<<SETTINGS_RS_SAT_SHIFT));
    /* FM caller ID on and "Use anyway" cleared (app.c's mask): the satellites' bits stay */
    CHECK(ui_svc_setting_set(UI_SET_FM_ID,1));
    st=settings_begin(); st->cp_flags&=(uint8_t)~SETTINGS_CP_ACCEPTED; settings_end(hal_millis());
    power_cycle();
    CHECK(ui_svc_setting_get(UI_SET_SAT_NOTE)==note && ui_svc_setting_get(UI_SET_FM_ID)==1);
    CHECK(now_st().cp_flags==(uint8_t)(note<<1));
    /* Every pass for the ISS at 5 min, 0 deg (code 1), auto track the downlink; then both off: kept */
    CHECK(ui_svc_setting_set(UI_SET_SAT_NOTE,1) && ui_svc_setting_set(UI_SET_SAT_AUTO,1));
    power_cycle();
    CHECK(ui_svc_setting_get(UI_SET_SAT_NOTE)==1 && ui_svc_setting_get(UI_SET_SAT_AUTO)==1);
    CHECK(ui_svc_setting_set(UI_SET_SAT_NOTE,0) && ui_svc_setting_set(UI_SET_SAT_AUTO,0));
    power_cycle();
    CHECK(ui_svc_setting_get(UI_SET_SAT_NOTE)==0 && ui_svc_setting_get(UI_SET_SAT_AUTO)==0);
    CHECK((now_st().rs_flags&SETTINGS_RS_ROGER_BEEP)!=0 && now_st().cp_flags==0);
    /* Out-of-range values only touch their own bits */
    CHECK(ui_svc_setting_set(UI_SET_SAT_NOTE,0x1FF) && ui_svc_setting_set(UI_SET_SAT_AUTO,7));
    s=now_st();
    CHECK(s.cp_flags==SETTINGS_CP_SAT_NOTE && (s.rs_flags&~SETTINGS_RS_SAT_AUTO)==
          (SETTINGS_RS_ROGER_BEEP|SETTINGS_RS_KEY_BEEP|SETTINGS_RS_NO_TALK_PERMIT));
    /* Hold 7 = a tool (code 21), hold 4 none, hold 0 the last tool (41): kept across power cycles, every other digit and
     * the satellites' bits untouched; a value outside a byte is refused */
    CHECK(ui_svc_setting_set(UI_SET_HOLD_FIRST+7,21) && ui_svc_setting_set(UI_SET_HOLD_FIRST+4,0) &&
          ui_svc_setting_set(UI_SET_HOLD_FIRST+0,41));
    CHECK(!ui_svc_setting_set(UI_SET_HOLD_FIRST+1,256) && !ui_svc_setting_set(UI_SET_HOLD_FIRST+1,-1));
    CHECK(ui_svc_setting_set(UI_SET_HOLD_FIRST+9,UI_HOLD_ISS));
    power_cycle();
    for (int d=0; d<10; d++)
        CHECK(ui_svc_setting_get((uint8_t)(UI_SET_HOLD_FIRST+d))==(d==7?21:d==0?41:d==9?UI_HOLD_ISS:0));
    s=now_st();
    CHECK(s.cp_flags==SETTINGS_CP_SAT_NOTE && (s.rs_flags&SETTINGS_RS_SAT_AUTO)==SETTINGS_RS_SAT_AUTO);
    CHECK(s.hold_tool[7]==21 && s.hold_tool[10]==0 && s.hold_tool[11]==0);
    /* A factory reset's defaults: off */
    settings_t def;
    settings_defaults(&def);
    CHECK((def.cp_flags&SETTINGS_CP_SAT_NOTE)==0 && (def.rs_flags&SETTINGS_RS_SAT_AUTO)==0);
    CHECK(def.hold_tool[4]==SETTINGS_HOLD_ISS && def.hold_tool[7]==0 && def.hold_tool[0]==0);
    CHECK(sim_flash_violations()==0);
    sim_flash_free();
    printf("# test_sat_set: satellites' Every pass and Auto track stored and kept across power cycles: %d failures\n",
           failures);
    return failures!=0;
}
