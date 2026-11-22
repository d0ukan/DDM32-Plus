# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus: every mode in the VFO (CONFIG_VFO_MODES, docs/plus.md "Every mode in the VFO"), called last from
plus_features.prepare(). Its own module so the shared prepare list gains one line only.

The VFO (radio_sm.c vfo_ch, a channel_t) may be FM, DMR, P25, YSF, NXDN, dPMR or AM, with each mode's parameters (NAC,
talkgroup and unit ID with Any; DG-ID; RAN; dPMR CC; DMR color code / timeslot / every group with Any). Quick settings'
Mode tile cycles the modes; # there (or on the VFO row of Zones) opens "VFO settings", the channel editor on the VFO
(integration/radiofoundry/vfox/ui_vfox.inc). The radio core (radio_vfox.inc) applies it and tunes; the native receiver
takes a P25 / YSF VFO like a channel (native_lab.c). Saved in spare bits of settings_t (vfox.c, byte map in the doc); the
record keeps upstream's size and layout. With multi-watch every ring letter may be a VFO: A / B upstream's two, C..F
four more in RAM (radio_vfox.inc). The Settings > Keys function "Dual watch" reads "Dual/multi-watch" there (item 22)."""

F = 'CONFIG_VFO_MODES'
MW = 'CONFIG_VFO_MODES && CONFIG_MULTI_WATCH'


def _if(cond, new, old=None):
    """new under #if cond (else old when given)"""
    return f'#if {cond}\n{new}' + (f'#else\n{old}' if old is not None else '') + '#endif\n'


def prepare_vfo_modes(transform, install):
    install('core/radio/vfox.h', 'integration/radiofoundry/vfox/vfox.h')
    install('core/radio/vfox.c', 'integration/radiofoundry/vfox/vfox.c')
    install('core/radio/radio_vfox.inc', 'integration/radiofoundry/vfox/radio_vfox.inc')
    install('core/ui/ui_vfox.inc', 'integration/radiofoundry/vfox/ui_vfox.inc')
    transform('config/features.mk', [
        ('CONFIG_NAC_DEC ?= n\n',
         'CONFIG_VFO_MODES ?= n\nFEATURES += VFO_MODES\n'
         'ifeq ($(CONFIG_VFO_MODES),y)\nifneq ($(CONFIG_P25_CHANNELS)$(CONFIG_CHANNEL_EDIT),yy)\n'
         '$(error VFO_MODES needs P25_CHANNELS=y and CHANNEL_EDIT=y)\nendif\nendif\n'
         'CONFIG_NAC_DEC ?= n\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_NAC_DEC\n', '#ifndef CONFIG_VFO_MODES\n#define CONFIG_VFO_MODES 0\n#endif\n#ifndef CONFIG_NAC_DEC\n'),
    ])

    # ---- AM in the VFO on any frequency: FM received through the AM demodulator (in the channel's tail padding) ----
    transform('core/codeplug/codeplug.h', [
        ('\tuint8_t kind,system; /* upstream channel mode tag */\n} channel_t;',
         '\tuint8_t kind,system; /* upstream channel mode tag */\n' + _if(F,'\tbool am;\t\t// DM32 Plus: received in AM wherever it is (the VFO only; radio/vfox.h)\n')
         + '} channel_t;'),
    ])
    transform('core/radio/radio_am.c', [
        ('\treturn radio_am_capable() && ch->mode == RADIO_MODE_FM && radio_am_band(ch->rx_hz);\n',
         _if(F, '\treturn radio_am_capable() && ch->mode == RADIO_MODE_FM && (radio_am_band(ch->rx_hz) || ch->am);\n',
             '\treturn radio_am_capable() && ch->mode == RADIO_MODE_FM && radio_am_band(ch->rx_hz);\n')),
    ])

    # ---- The radio core: requests, the VFO's modes, persistence, the ring letters' VFOs ----
    transform('core/radio/radio.h', [
        ('void radio_req_vfo_tx(uint32_t hz);',
         'void radio_req_vfo_tx(uint32_t hz);\n'
         '#if CONFIG_VFO_MODES\n'
         '// DM32 Plus (radio/vfox.h): the VFO in use takes ch\'s frequencies, mode and that mode\'s parameters, width and tones\n'
         '// (the VFO settings screen); the radio task takes it with radio_vfo_apply_take\n'
         'void radio_req_vfo_apply(const channel_t *ch);\n'
         'bool radio_vfo_apply_take(channel_t *out);\n'
         '#endif'),
    ])
    transform('core/radio/radio_sm.h', [
        ('#include "settings/settings.h"\t// SETTINGS_ROAM_ZONE_BYTES\n',
         '#include "settings/settings.h"\t// SETTINGS_ROAM_ZONE_BYTES\n#if CONFIG_VFO_MODES\n#include "radio/vfox.h"\n#endif\n'),
        ('\n} radio_req_type_t;',
         '\n#if CONFIG_VFO_MODES\n\tRREQ_VFO_APPLY,\t\t// DM32 Plus: the VFO settings radio_req_vfo_apply left (radio_vfo_apply_take)\n'
         '#endif\n} radio_req_type_t;'),
        ('the looks take the others in turn)\n#endif\n',
         'the looks take the others in turn)\n#endif\n'
         + _if(MW, '\tvfox_slot_t vfox_x[RADIO_DUAL_SLOTS - 2];\t// DM32 Plus: ring letters C..F\'s own VFOs (RAM only)\n'
                   '\tvfox_slot_t vfox_park;\t\t// ... VFO vfo_idx (A / B) while one of those is in use\n'
                   '\tuint8_t vfox_at;\t\t// ... the one in use, 2..5; 0: vfo_ch is VFO vfo_idx\n')),
    ])
    transform('core/radio/radio_task.c', [
        ('static bool shared_vfo_ok[2];\n',
         'static bool shared_vfo_ok[2];\n'
         + _if(MW, 'static vfox_slot_t shared_vfox[RADIO_DUAL_SLOTS - 2];\t// DM32 Plus: C..F\'s VFOs, under view_lock\n')),
        ('\tshared_vfo_ok[!sm.vfo_idx] = sm.vfo_other.seeded;\n',
         '\tshared_vfo_ok[!sm.vfo_idx] = sm.vfo_other.seeded;\n'
         + _if(MW, '\tmemcpy(shared_vfox, sm.vfox_x, sizeof shared_vfox);\n'
                   '\tif (sm.vfox_at >= 2) {\t// C..F\'s in use; A / B\'s parked\n'
                   '\t\tuint32_t talk;\n'
                   '\t\tvfox_slot_get(&sm.vfox_park, &shared_vfo[sm.vfo_idx & 1u], &talk, &shared_vfo_ok[sm.vfo_idx & 1u]);\n'
                   '\t\tvfox_slot_put(&shared_vfox[sm.vfox_at - 2u], &sm.vfo_ch, 0, sm.vfo_seeded);\n'
                   '\t}\n')),
        ('\t*out = shared_vfo[index & 1u];\n\tbool ok = shared_vfo_ok[index & 1u];\n',
         _if(MW, '\tbool ok;\n'
                 '\tif (index >= 2 && index < RADIO_DUAL_SLOTS) {\n'
                 '\t\tuint32_t talk;\n'
                 '\t\tvfox_slot_get(&shared_vfox[index - 2u], out, &talk, &ok);\n'
                 '\t\tok = true;\t// a letter\'s VFO never set reads as a fresh one\n'
                 '\t} else {\n'
                 '\t\t*out = shared_vfo[index & 1u];\n\t\tok = shared_vfo_ok[index & 1u];\n'
                 '\t}\n',
             '\t*out = shared_vfo[index & 1u];\n\tbool ok = shared_vfo_ok[index & 1u];\n')),
        ('bool radio_get_vfo(uint8_t index, channel_t *out)\n',
         '#if CONFIG_VFO_MODES\n'
         '// DM32 Plus: the VFO settings\' copy for the radio task, under view_lock (a request carries two numbers)\n'
         'static channel_t shared_vfo_apply;\n'
         'void radio_req_vfo_apply(const channel_t *ch)\n{\n'
         '\tif (view_lock == NULL) {\n\t\treturn;\n\t}\n'
         '\txSemaphoreTake(view_lock, portMAX_DELAY);\n\tshared_vfo_apply = *ch;\n\txSemaphoreGive(view_lock);\n'
         '\tpost_pair(RREQ_VFO_APPLY, 0, 0, 0);\n}\n\n'
         'bool radio_vfo_apply_take(channel_t *out)\n{\n'
         '\txSemaphoreTake(view_lock, portMAX_DELAY);\n\t*out = shared_vfo_apply;\n\txSemaphoreGive(view_lock);\n'
         '\treturn true;\n}\n#endif\n\n'
         'bool radio_get_vfo(uint8_t index, channel_t *out)\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_DUAL_WATCH\n// VFO idx (0 or 1) becomes the one in use',
         '#if CONFIG_VFO_MODES\n#include "radio/radio_vfox.inc"\t// DM32 Plus: the VFO\'s modes, the ring letters\' VFOs\n#endif\n\n'
         '#if CONFIG_DUAL_WATCH\n// VFO idx (0 or 1) becomes the one in use'),
        # Upstream's swap of its two VFOs: a ring letter's C..F VFO in use goes back to its slot first
        ('{\n\tidx &= 1u;', '{\n' + _if(MW, '\tvfox_unpark(sm);\n') + '\tidx &= 1u;'),
        ('\t\tvfo_use(sm, (uint8_t)r->pos);\t// each channel its own VFO\n',
         _if(MW, '\t\tvfox_use(sm, (uint8_t)r->pos);\t// each channel its own VFO (C..F: VFOs 2..5)\n',
             '\t\tvfo_use(sm, (uint8_t)r->pos);\t// each channel its own VFO\n')),
        ('\treturn r->vfo ? sm->view.vfo && sm->vfo_idx == r->pos :\n',
         _if(MW, '\treturn r->vfo ? sm->view.vfo && vfox_in_use(sm) == r->pos :\n',
             '\treturn r->vfo ? sm->view.vfo && sm->vfo_idx == r->pos :\n')),
        # A ring letter C..F that is a channel goes into its own VFO (Copy to VFO, Tune VFO)
        ('\t\tvfo_pick = dual_vfo_taken(sm, sm->vfo_idx) ? !sm->vfo_idx : sm->vfo_idx;',
         '\t\tvfo_pick = dual_vfo_taken(sm, sm->vfo_idx) ? !sm->vfo_idx : sm->vfo_idx;\n'
         + _if(MW, '\t\tif (sm->dual_sel >= 2)\n\t\t\tvfo_pick = sm->dual_sel;\n')),
        ('\t\tvfo_use(sm, (uint8_t)vfo_pick);\n',
         _if(MW, '\t\tvfox_use(sm, (uint8_t)vfo_pick);\n', '\t\tvfo_use(sm, (uint8_t)vfo_pick);\n')),
        ('case RREQ_DMR_CC: case RREQ_VFO_COPY:\n\t\treturn true;',
         'case RREQ_DMR_CC: case RREQ_VFO_COPY:\n#if CONFIG_VFO_MODES\n\tcase RREQ_VFO_APPLY:\n#endif\n\t\treturn true;'),
        ('\tcase RREQ_VFO_COPY:\n\t\tif (!busy) {',
         '#if CONFIG_VFO_MODES\n\tcase RREQ_VFO_APPLY:\n\t\tvfox_apply_request(sm, busy, now_ms);\n\t\tbreak;\n#endif\n'
         '\tcase RREQ_VFO_COPY:\n\t\tif (!busy) {'),
        ('\tcase RREQ_MODE:\n\t\tif (codeplug_mode_p25(sm->view.ch.mode)) break;',
         '\tcase RREQ_MODE:\n'
         + _if(F, '\t\tif (sm->view.vfo && vfox_mode_request(sm, req->arg, busy, now_ms))\n\t\t\tbreak;\t// the VFO\'s other modes\n')
         + '\t\tif (codeplug_mode_p25(sm->view.ch.mode)) break;'),
        # Copy to VFO: a P25 / YSF channel too, with its parameters; a DMR channel's "any" come along
        ('\tif (codeplug_mode_p25(ch->mode) || (ch->mode == RADIO_MODE_DMR ? !CONFIG_VFO_DMR : !CONFIG_VFO_FM)) {',
         '\tif ((codeplug_mode_p25(ch->mode) && !CONFIG_VFO_MODES) ||\n'
         '\t    (ch->mode == RADIO_MODE_DMR ? !CONFIG_VFO_DMR : !CONFIG_VFO_FM)) {'),
        ('\t}\n\tsm->vfo_seeded = true;\n\tenter_vfo(sm, now_ms);',
         '\t}\n' + _if(F, '\tvfox_copy(v, c);\n') + '\tsm->vfo_seeded = true;\n\tenter_vfo(sm, now_ms);'),
        # The settings record: the VFO's mode and parameters in spare bits (vfox.c), A / B only
        ('restore_vfo(sm, &st, 1);\n',
         'restore_vfo(sm, &st, 1);\n' + _if(F, '\tif (sm->vfo_seeded)\n\t\tvfox_load(&st, 1, &sm->vfo_ch);\n')),
        ('restore_vfo(sm, &st, 0);\n',
         'restore_vfo(sm, &st, 0);\n' + _if(F, '\tif (sm->vfo_seeded)\n\t\tvfox_load(&st, 0, &sm->vfo_ch);\n')),
        ('\t\t\tvfo_index = (uint8_t)r->pos;\t// power up in the selected channel\'s VFO\n',
         _if(MW, '\t\t\tvfo_index = r->pos < 2 ? (uint8_t)r->pos : sm->vfo_idx;\t// C..F\'s VFOs are RAM only\n',
             '\t\t\tvfo_index = (uint8_t)r->pos;\t// power up in the selected channel\'s VFO\n')),
        ('\tpersist_vfo(sm, st, sm->vfo_idx);\n#if CONFIG_DUAL_WATCH\n\tuint8_t in_use',
         _if(MW, '\tuint8_t vfox_was = sm->vfox_at;\t// A / B into their fields, not a C..F VFO in use\n\tvfox_unpark(sm);\n')
         + '\tpersist_vfo(sm, st, sm->vfo_idx);\n'
         + _if(F, '\tif (sm->vfo_seeded)\n\t\tvfox_store(st, sm->vfo_idx, &sm->vfo_ch);\n')
         + '#if CONFIG_DUAL_WATCH\n\tuint8_t in_use'),
        ('\tvfo_use(sm, in_use);\n\tst->vfo_index = vfo_index;\n',
         '\tvfo_use(sm, in_use);\n\tst->vfo_index = vfo_index;\n'
         + _if(MW, '\tif (vfox_was >= 2)\n\t\tvfox_use(sm, vfox_was);\n')),
        ('\tvfo_use(sm, (uint8_t)!in_use);\t\t// the other VFO into its own fields: swap, write, swap back\n'
         '\tpersist_vfo(sm, st, sm->vfo_idx);\n',
         '\tvfo_use(sm, (uint8_t)!in_use);\t\t// the other VFO into its own fields: swap, write, swap back\n'
         '\tpersist_vfo(sm, st, sm->vfo_idx);\n'
         + _if(F, '\tif (sm->vfo_seeded)\n\t\tvfox_store(st, sm->vfo_idx, &sm->vfo_ch);\n')),
    ])
    # The CPS's view of VFO A: its mode and parameters as a channel record of that mode (read side only)
    transform('core/cps/cps_openfw.c', [
        ('\tch->timeslot = settings_vfo_ts(&st);\n\treturn true;',
         '\tch->timeslot = settings_vfo_ts(&st);\n' + _if(F, '\tvfox_load(&st, 0, ch);\n') + '\treturn true;'),
        ('#include "settings/settings.h"\n', '#include "settings/settings.h"\n#if CONFIG_VFO_MODES\n#include "radio/vfox.h"\n#endif\n'),
    ])

    # ---- The UI ----
    transform('core/ui/ui_svc.h', [
        ('bool ui_svc_vfo_get(',
         '#if CONFIG_VFO_MODES\n// DM32 Plus: the VFO in use takes ch (radio_req_vfo_apply). False: not in this build\n'
         'bool ui_svc_vfo_apply(const channel_t *ch);\n#endif\nbool ui_svc_vfo_get('),
    ])
    transform('core/ui/ui_svc_stub.c', [
        ('WEAK bool ui_svc_vfo_get(',
         '#if CONFIG_VFO_MODES\nWEAK bool ui_svc_vfo_apply(const channel_t *ch) { (void)ch; return false; }\n#endif\n'
         'WEAK bool ui_svc_vfo_get('),
    ])
    transform('core/ui/ui_svc_radio.c', [
        ('\tif (mode != RADIO_MODE_FM && mode != RADIO_MODE_DMR) {',
         '\tif (mode != RADIO_MODE_FM && mode != RADIO_MODE_DMR && !(CONFIG_VFO_MODES && mode < 7u)) {'
         '\t// DM32 Plus: the VFO\'s other modes (radio/vfox.h)'),
        ('bool ui_svc_vfo_get(uint8_t index, channel_t *out)\n',
         '#if CONFIG_VFO_MODES\nbool ui_svc_vfo_apply(const channel_t *ch)\n{\n\tradio_req_vfo_apply(ch);\n\treturn true;\n}\n#endif\n\n'
         'bool ui_svc_vfo_get(uint8_t index, channel_t *out)\n'),
        ('(uint16_t)(ref[i].pos & 1u) : ref[i].pos };\n\t\tif (r[i].vfo ? !(CONFIG_VFO_FM',
         '(uint16_t)(CONFIG_VFO_MODES && ref[i].pos < RADIO_DUAL_SLOTS ? ref[i].pos : ref[i].pos & 1u) :\n'
         '\t\t\t\t\t\t\t ref[i].pos };\t// DM32 Plus: C..F have VFOs of their own\n'
         '\t\tif (r[i].vfo ? !(CONFIG_VFO_FM'),
    ])
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;', '\tSCR_VFOSET,\t\t\t// DM32 Plus: VFO settings, every mode (CONFIG_VFO_MODES)\n\tSCR_COUNT\n} ui_scr_t;'),
        ('void ui_menus_save_failed(void);',
         'void ui_menus_save_failed(void);\n#if CONFIG_VFO_MODES\n'
         '// DM32 Plus: Quick settings\' Mode tile in the VFO, the next mode of the VFO\'s cycle (false: upstream\'s FM / DMR)\n'
         'bool ui_vfox_quick_mode(const radio_view_t *v);\n#endif'),
    ])
    transform('core/ui/ui_menus.c', [
        ('#include "radio/radio_am.h"\n', '#include "radio/radio_am.h"\n#if CONFIG_VFO_MODES\n#include "radio/vfox.h"\n#endif\n'),
        ('static uint8_t ced_n;\n',
         'static uint8_t ced_n;\n'
         + _if(F, 'static bool ced_vfo;\t\t\t// DM32 Plus: the rows are VFO settings\' (ui_vfox.inc), not a channel\'s\n'
                  'static bool vfox_from_vfo;\t\t// ... the next New from current copies ced_ch (the VFO) as it is\n'
                  'static bool vfox_row(uint8_t r);\nstatic bool vfox_select(uint8_t r, bool hold);\nstatic void vfox_apply(void);\n'
                  '// Its strings here: tools/i18n.py reads .c files only\n'
                  '#define VFOX_TITLE\tTR("VFO settings")\n#define VFOX_APPLIED\tTR("VFO: changes apply at once")\n')),
        ('\tfor (uint8_t r = 0; r < CE_COUNT; r++) {\n',
         '\tfor (uint8_t r = 0; r < CE_COUNT; r++) {\n' + _if(F, '\t\tif (ced_vfo && !vfox_row(r))\n\t\t\tcontinue;\n')),
        ('\tced_num = ui_frame()->arg;\n', _if(F, '\tced_vfo = false;\n') + '\tced_num = ui_frame()->arg;\n'),
        ('\tced_ch = v->ch;\n',
         _if(F, '\tif (!vfox_from_vfo)\n\t\tced_ch = v->ch;\t// else VFO settings\' copy, as it is now\n'
                '\tvfox_from_vfo = false;\n'
                '\tif (ced_ch.am && !radio_am_band(ced_ch.rx_hz))\n'
                '\t\tui_toast(TR("AM is the VFO\'s only: the channel is FM"));\n'
                '\tced_ch.am = false;\n', '\tced_ch = v->ch;\n')),
        ('\tcase CE_MODE:\n\t\tsb_str(&b,',
         '\tcase CE_MODE:\n' + _if(F, '\t\tif (ch->am) {\n\t\t\tsb_str(&b, "AM");\n\t\t\tbreak;\n\t\t}\n') + '\t\tsb_str(&b,'),
        ('\tif (hold) {\n\t\tced_save(false);\t// long OK',
         _if(F, '\tif (ced_vfo && vfox_select(ced_rows[i], hold))\n\t\treturn;\n') +
         '\tif (hold) {\n\t\tced_save(false);\t// long OK'),
        ('\tcase CE_SAVE_NEW: ced_save(true); break;\n\tdefault: break;\n\t}\n',
         '\tcase CE_SAVE_NEW: ced_save(true); break;\n\tdefault: break;\n\t}\n'
         + _if(F, '\tif (ced_vfo && ced_edit == CE_NONE)\n\t\tvfox_apply();\t// a row changed at once (width, timeslot)\n')),
        ('\tced_edit = CE_NONE;\n}\n',
         '\tced_edit = CE_NONE;\n' + _if(F, '\tif (ced_vfo)\n\t\tvfox_apply();\n') + '}\n'),
        ('ced_ch.p25_tg = 0; ced_edit = CE_NONE; }',
         'ced_ch.p25_tg = 0; ced_edit = CE_NONE;\n' + _if(F, '\t\t\tif (ced_vfo)\n\t\t\t\tvfox_apply();\n') + '\t\t}'),
        ('\t.enter = chinfo_enter,\n};\n',
         '\t.enter = chinfo_enter,\n};\n#if CONFIG_VFO_MODES\n#include "ui_vfox.inc"\t// DM32 Plus: VFO settings\n#endif\n'),
        ('[SCR_CHANNELS] = &chlist_screen, [SCR_CHANNEL] = &chinfo_screen,\n',
         '[SCR_CHANNELS] = &chlist_screen, [SCR_CHANNEL] = &chinfo_screen,\n#if CONFIG_VFO_MODES\n\t[SCR_VFOSET] = &vfoset_screen,\n#endif\n'),
        ('\t\tui_toast(TR("The VFO has no channel list"));\n',
         _if(F, '\t\tif (ui_view()->vfo) {\n\t\t\tui_push(SCR_VFOSET, 0);\t// DM32 Plus: its settings\n\t\t\treturn;\n\t\t}\n')
         + '\t\tui_toast(TR("The VFO has no channel list"));\n'),
        # Last call: the P25 VFO plays it again too
        ('bool p25 = !rv->vfo && codeplug_mode_p25(rv->ch.mode);',
         'bool p25 = (CONFIG_VFO_MODES || !rv->vfo) && codeplug_mode_p25(rv->ch.mode);'),
        # Item 22: Settings > Keys' "Dual watch" function is the multi-watch's on / off with 3..6 watch channels
        ('"A/B", TRN("Dual watch"),',
         '"A/B",\n#if CONFIG_MULTI_WATCH\n\t\t\t\t\t     TRN("Dual/multi-watch"),\n#else\n\t\t\t\t\t     TRN("Dual watch"),\n#endif\n', 2),
    ])
    transform('core/ui/ui_app.c', [
        ('\tcase QT_MODE: {\n',
         '\tcase QT_MODE: {\n' + _if(F, '\t\tif (ui_vfox_quick_mode(view))\n\t\t\tbreak;\t// DM32 Plus: the VFO\'s mode cycle\n')),
        ('\t\t} else if (press) {\n\t\t\tui_toast(TR("No actions here"));\n\t\t}\n\t\treturn true;\n\tcase ACT_SELECT:',
         '\t\t} else if (press && CONFIG_VFO_MODES && view->vfo) {\n'
         '\t\t\tui_st.quick_edit = false;\n\t\t\tui_push(SCR_VFOSET, 0);\t// DM32 Plus: the VFO\'s settings, every mode\n'
         '\t\t} else if (press) {\n\t\t\tui_toast(TR("No actions here"));\n\t\t}\n\t\treturn true;\n\tcase ACT_SELECT:'),
    ])
    transform('core/ui/ui_screens.c', [
        ('// The mode word: AM',
         '#if CONFIG_VFO_MODES\n#include "radio/vfox.h"\n#include "radio/radio_am.h"\n'
         '// DM32 Plus: Home\'s VFO row with a digital mode\'s parameters, shorter forms where they don\'t fit: "VFO P25 NAC\n'
         '// $456 TG 99", "P25 NAC $456 TG 99", "NAC456 TG99". Static: Home\'s drawing is the UI task\'s deepest path\n'
         'static const char *__attribute__((noinline)) vfox_home(const radio_view_t *v)\n{\n'
         '\tstatic char buf[72];\n'
         '\tuint8_t m = vfox_mode_of(&v->ch);\n'
         '\tif (!vfox_native(m) && m != VFOX_AM)\n\t\treturn "VFO";\n'
         '\tconst channel_t *c = &v->ch;\n'
         '\tsbuf_t b;\n\tsb_init(&b, buf, sizeof buf);\n\tsb_str(&b, TEXT_ALT_OPEN);\n'
         '\tfor (int form = 0; form < 3; form++) {\n'
         '\t\tconst char *gap = form < 2 ? " " : "";\n'
         '\t\tif (form)\n\t\t\tsb_str(&b, TEXT_ALT_NEXT);\n'
         '\t\tif (form == 0)\n\t\t\tsb_str(&b, "VFO ");\n'
         '\t\tif (form < 2 || m == VFOX_AM)\n\t\t\tsb_str(&b, vfox_mode_name(m));\n'
         '\t\tif (m == VFOX_AM)\n\t\t\tcontinue;\n'
         '\t\tif (form < 2)\n\t\t\tsb_chr(&b, \' \');\n'
         '\t\tif (m == RADIO_MODE_P25) {\n'
         '\t\t\tsb_str(&b, "NAC");\n\t\t\tsb_str(&b, gap);\n'
         '\t\t\tif (c->p25_nac == 0xF7Eu) {\n\t\t\t\tsb_str(&b, TR("Any"));\n\t\t\t} else {\n'
         '\t\t\t\tif (form < 2)\n\t\t\t\t\tsb_chr(&b, \'$\');\n'
         '\t\t\t\tfor (unsigned d = 0; d < 3; d++)\n'
         '\t\t\t\t\tsb_chr(&b, "0123456789ABCDEF"[(c->p25_nac >> (4 * (2 - d))) & 15u]);\n\t\t\t}\n'
         '\t\t\tsb_chr(&b, \' \');\n'
         '\t\t}\n'
         '\t\tsb_str(&b, m == RADIO_MODE_P25 ? "TG" : m == RADIO_MODE_YSF ? "DG-ID" : m == RADIO_MODE_NXDN ? "RAN" : "CC");\n'
         '\t\tsb_str(&b, gap);\n'
         '\t\tif (c->p25_tg == 0)\n\t\t\tsb_str(&b, TR("Any"));\n'
         '\t\telse\n\t\t\tsb_u32(&b, m == RADIO_MODE_DPMR ? c->p25_tg - 1u : c->p25_tg, 1);\n'
         '\t}\n'
         '\tsb_str(&b, TEXT_ALT_CLOSE);\n\treturn buf;\n}\n#endif\n\n'
         '// The mode word: AM'),
        ('\t\tconst char *left = v->vfo ? "VFO" : v->zone_name;',
         _if(F, '\t\tconst char *left = v->vfo ? vfox_home(v) : v->zone_name;\n',
             '\t\tconst char *left = v->vfo ? "VFO" : v->zone_name;\n')),
        ('\t\tx = gfx_text(f, x, y, v->am ? TR("Air Band AM") : TR("Broadcast FM"), ui_col.fg) + f->w;\n',
         _if(F, '\t\tx = gfx_text(f, x, y, v->am ? (radio_am_band(v->ch.rx_hz) ? TR("Air Band AM") : "AM") : TR("Broadcast FM"),\n'
                '\t\t\t     ui_col.fg) + f->w;\t// DM32 Plus: the VFO\'s AM off the air band\n',
             '\t\tx = gfx_text(f, x, y, v->am ? TR("Air Band AM") : TR("Broadcast FM"), ui_col.fg) + f->w;\n')),
    ])
    # Multi-watch: the letter's own VFO (C..F: 2..5); A / B's codes for the saved state (C..F are RAM only)
    transform('core/ui/ui_dual.c', [
        ('static const char *mw_title(uint16_t arg);\n',
         'static const char *mw_title(uint16_t arg);\n'
         + _if(F, 'static uint8_t mw_vfo_letter(uint16_t arg);\nstatic int32_t mw_code_b(void);\n')),
        ('static ui_dual_ref_t vfo_for(uint8_t i)\n{\n\tuint16_t u = ui_view()->vfo_index;\n',
         'static ui_dual_ref_t vfo_for(uint8_t i)\n{\n'
         + _if(MW, '\tuint8_t k = mw_vfo_letter(i);\t// DM32 Plus: C..F, the letter\'s own\n'
                   '\tif (k >= 2)\n\t\treturn (ui_dual_ref_t){ true, 0, k };\n'
                   '\tuint16_t u = ui_view()->vfo_index < 2 ? ui_view()->vfo_index : 0;\n',
               '\tuint16_t u = ui_view()->vfo_index;\n')),
        ('\treturn ref[i].vfo ? UI_DUAL_B_VFO + ref[i].pos',
         _if(MW, '\tif (ref[i].vfo && ref[i].pos >= 2)\n\t\treturn mw_code_b();\t// C..F\'s VFOs are RAM only: B is saved in its place\n')
         + '\treturn ref[i].vfo ? UI_DUAL_B_VFO + ref[i].pos'),
    ])

    # ---- The upstream UI simulator (tests/host/sim), for tests/plus/test_ui_vfox.c ----
    sim_use = (
        'static void sim_vfo_use_ab(uint8_t idx);\n'
        'static sim_vfo_t sim_store[6];\t// DM32 Plus: VFOs 2..5 (letters C..F), and A / B parked while one of those is in use\n'
        'static uint8_t sim_lastab;\t\t// ... the A / B VFO in use last\n'
        'static channel_t sim_vx[6];\t\t// ... each VFO\'s mode and parameters as the radio core keeps them\n'
        'static bool sim_vx_ok[6];\n'
        'static void sim_vfo_use(uint8_t idx)\n{\n'
        '\tuint8_t cur = view.vfo_index;\n'
        '\tif (idx >= 6 || idx == cur)\n\t\treturn;\n'
        '\tif (view.vfo) {\n\t\tsim_vx[cur] = view.ch;\n\t\tsim_vx_ok[cur] = true;\n\t}\n'
        '\tif (cur < 2 && idx < 2) {\n\t\tsim_vfo_use_ab(idx);\n\t\tsim_lastab = idx;\n\t\treturn;\n\t}\n'
        '\tif (view.vfo) {\n'
        '\t\tvfo_rx_hz = view.ch.rx_hz;\n\t\tvfo_tx_hz = view.ch.tx_hz;\n\t\tvfo_rx_tone = view.ch.rx_tone;\n'
        '\t\tvfo_tx_tone = view.ch.tx_tone;\n\t\tvfo_bw = view.ch.bw;\n\t\tvfo_mode = view.ch.mode;\n'
        '\t\tvfo_cc = view.ch.color_code;\n\t\tvfo_ts = view.ch.timeslot;\n\t}\n'
        '\tif (cur < 2)\n\t\tsim_lastab = cur;\n'
        '\tsim_vfo_get(&sim_store[cur]);\n'
        '\tsim_vfo_t t;\n'
        '\tif (idx >= 2 || idx == sim_lastab) {\n\t\tt = sim_store[idx];\n'
        '\t} else {\t// the other A / B: out of vfo_other, which takes the parked one\n'
        '\t\tt = vfo_other;\n\t\tvfo_other = sim_store[sim_lastab];\n\t}\n'
        '\tif (idx < 2)\n\t\tsim_lastab = idx;\n'
        '\tvfo_mode = t.mode;\n\tvfo_cc = t.cc;\n\tvfo_ts = t.ts;\n\tvfo_bw = t.bw;\n\tvfo_rx_hz = t.rx_hz;\n'
        '\tvfo_tx_hz = t.tx_hz;\n\tvfo_talk = t.talk;\n\tvfo_rx_tone = t.rx_tone;\n\tvfo_tx_tone = t.tx_tone;\n'
        '\tvfo_talk_private = t.talk_private;\n\tview.vfo_index = idx;\n}\n'
        'static void sim_vfo_use_ab(uint8_t idx)\n')
    def sim_overlay(index, dst, tab):
        return (f'{tab}if (sim_vx_ok[{index}]) {{\t// DM32 Plus: the native modes\' parameters, AM\n'
                f'{tab}\tconst channel_t *x = &sim_vx[{index}];\n'
                f'{tab}\t{dst}am = x->am;\n{tab}\t{dst}contact = x->contact;\n{tab}\t{dst}listen_list = x->listen_list;\n'
                f'{tab}\t{dst}dmr_id = x->dmr_id;\n{tab}\t{dst}rx_only = x->rx_only;\n'
                # Classic r13's channel_t keeps the P25 fields apart from DMR's (a union before)
                f'{tab}\t{dst}p25_tg = x->p25_tg;\n{tab}\t{dst}p25_nac = x->p25_nac;\n{tab}\t{dst}p25_id = x->p25_id;\n'
                f'{tab}\t{dst}p25_squelch = x->p25_squelch;\n{tab}\t{dst}p25_tx_nac_override = x->p25_tx_nac_override;\n'
                f'{tab}}}\n')
    transform('tests/host/sim/ui_sim.c', [
        ('#include "ui/ui_svc_aprs.h"\n', '#include "ui/ui_svc_aprs.h"\n#if CONFIG_VFO_MODES\n#include "radio/vfox.h"\n#endif\n'),
        ('static void sim_vfo_use(uint8_t idx)\n{\n\tif (idx == view.vfo_index)\n\t\treturn;\n',
         _if(F, sim_use, 'static void sim_vfo_use(uint8_t idx)\n') + '{\n\tif (idx == view.vfo_index)\n\t\treturn;\n'),
        ('\t\tview.dmr.ts = vfo_ts;\n\t\tview.channel_number = 0;\n',
         '\t\tview.dmr.ts = vfo_ts;\n'
         + _if(F, sim_overlay('view.vfo_index', 'view.ch.', '\t\t') + '\t\tview.am = view.ch.am;\n')
         + '\t\tview.channel_number = 0;\n'),
        ('static void load_channel(void)\n{\n\tzone_t z;\n',
         'static void load_channel(void)\n{\n\tzone_t z;\n' + _if(F, '\tview.am = false;\n')),
        ('\tout->timeslot = o.ts;\n\treturn true;\n}\n',
         '\tout->timeslot = o.ts;\n' + _if(F, sim_overlay('index', 'out->', '\t')) + '\treturn true;\n}\n'),
        ('\tif (index != view.vfo_index) {\n\t\to = vfo_other;\n',
         '\tif (index != view.vfo_index) {\n\t\to = vfo_other;\n'
         + _if(F, '\t\tif (index >= 2 || (view.vfo_index >= 2 && index == sim_lastab))\n\t\t\to = sim_store[index];\n')),
        ('bool ui_svc_mode(uint8_t mode)\n{\n',
         _if(F, 'bool ui_svc_vfo_apply(const channel_t *ch)\n{\n'
                '\tif (!view.vfo)\n\t\treturn false;\n'
                '\tretune();\n'
                '\tchannel_t c = *ch;\n\tstrcpy(c.name, view.ch.name);\n\tvfox_fix(&c);\n'
                '\tview.ch = c;\n\tview.am = c.am;\n'
                '\tvfo_mode = c.mode;\n\tvfo_rx_hz = c.rx_hz;\n\tvfo_tx_hz = c.tx_hz;\n\tvfo_bw = c.bw;\n'
                '\tvfo_cc = c.color_code;\n\tvfo_ts = c.timeslot;\n\tvfo_rx_tone = c.rx_tone;\n\tvfo_tx_tone = c.tx_tone;\n'
                '\tsim_vx[view.vfo_index] = c;\n\tsim_vx_ok[view.vfo_index] = true;\n'
                '\tview.dmr.active = c.mode == RADIO_MODE_DMR;\n\tview.dmr.cc = c.color_code;\n\tview.dmr.ts = c.timeslot;\n'
                '\tdefault_target();\n\tchanged();\n\treturn true;\n}\n')
         + 'bool ui_svc_mode(uint8_t mode)\n{\n'
         + _if(F, '\tif (view.vfo && (mode > RADIO_MODE_DMR || vfox_mode_of(&view.ch) > RADIO_MODE_DMR)) {\n'
                  '\t\tchannel_t c = view.ch;\n\t\tvfox_set_mode(&c, mode, view.dmr.own_id);\n'
                  '\t\treturn ui_svc_vfo_apply(&c);\n\t}\n')),
        ('\telse if (strcmp(key, "mode") == 0)',
         '#if CONFIG_VFO_MODES\n'
         '\t// DM32 Plus: the VFO\'s mode (fm dmr p25 ysf nxdn dpmr am), its parameters, which VFO\n'
         '\telse if (strcmp(key, "vmode") == 0) snprintf(got, sizeof got, "%s", vfox_mode_name(vfox_mode_of(&view.ch)));\n'
         '\telse if (strcmp(key, "nac") == 0) snprintf(got, sizeof got, "%X", view.ch.p25_nac);\n'
         '\telse if (strcmp(key, "tg") == 0) snprintf(got, sizeof got, "%u", view.ch.p25_tg);\n'
         '\telse if (strcmp(key, "p25id") == 0) snprintf(got, sizeof got, "%u", (unsigned)view.ch.p25_id);\n'
         '\telse if (strcmp(key, "rxonly") == 0) snprintf(got, sizeof got, "%u", view.ch.rx_only);\n'
         '\telse if (strcmp(key, "vcc") == 0) snprintf(got, sizeof got, "%u", view.ch.color_code);\n'
         '\telse if (strcmp(key, "vts") == 0) snprintf(got, sizeof got, "%u", view.ch.timeslot);\n'
         '\telse if (strcmp(key, "listany") == 0) snprintf(got, sizeof got, "%u", view.ch.listen_list == CODEPLUG_LIST_ANY);\n'
         '\telse if (strcmp(key, "vfoidx") == 0) snprintf(got, sizeof got, "%u", view.vfo_index);\n'
         '#endif\n'
         '\telse if (strcmp(key, "mode") == 0)'),
        ('\tif (strcmp(t, "receivers") == 0 && arg != NULL) {\n',
         '#if CONFIG_VFO_MODES\n'
         '\tif (strcmp(t, "amrx") == 0) {\t// DM32 Plus: a radio that receives AM (radio_am_capable)\n'
         '\t\tstatic hal_radio_t am_radio;\n\t\tam_radio.rx_am = sim_rx_am;\n'
         '\t\tboard.caps |= CAP_AM_RX;\n\t\tboard.radio = &am_radio;\n\t\treturn true;\n\t}\n'
         '#endif\n'
         '\tif (strcmp(t, "receivers") == 0 && arg != NULL) {\n'),
        ('static bool run_token(char *t, char *err, size_t len)\n{\n',
         '#if CONFIG_VFO_MODES\nstatic bool sim_rx_am(uint32_t hz, radio_bw_t bw) { (void)hz; (void)bw; return true; }\n#endif\n'
         'static bool run_token(char *t, char *err, size_t len)\n{\n'),
        ('\tvfo_other = (sim_vfo_t){ RADIO_MODE_FM, 1, 1, RADIO_BW_25K, 146520000, 146520000, 0, { TONE_NONE, 0 },\n'
         '\t\t\t\t { TONE_NONE, 0 }, false };\n',
         '\tvfo_other = (sim_vfo_t){ RADIO_MODE_FM, 1, 1, RADIO_BW_25K, 146520000, 146520000, 0, { TONE_NONE, 0 },\n'
         '\t\t\t\t { TONE_NONE, 0 }, false };\n'
         + _if(F, '\tfor (int i = 0; i < 6; i++)\n\t\tsim_store[i] = vfo_other;\n'
                  '\tmemset(sim_vx_ok, 0, sizeof sim_vx_ok);\n\tsim_lastab = 0;\n'
                  '\tboard.radio = NULL;\n')),
    ])
