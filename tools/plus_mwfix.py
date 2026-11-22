#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus dual watch / multi-watch TX target fix (docs/plus.md "Multi-watch", "The TX target"): the owner's report on
v1.0.0-beta.7 (A DMR, B P25, C FM; PTT on C keyed P25 on B, PTT on the P25 letter did nothing, TALKING stayed on
after release, the P25 letter went deaf until a power cycle). Called from plus_features.prepare(); a module of its own
so the shared prepare list gains one line. The native side is integration/radiofoundry/native/native_lab.c (our file);
the radio core only gains the hunks below:

- radio_sm_dual_away(): the native P25 owner, which sees PTT before the radio core (radio_task.c), asks whether the
  receiver is on a letter other than the selected one and has the radio core tune the selected one first (dual_talk,
  as the radio core's own PTT does). Before, native_lab.c looked at the channel the receiver happened to be on: a PTT
  with C selected while the ring was on (or held by a call on) B keyed P25 on B, and with B selected while the ring
  was on A the radio core keyed an FM carrier on B's frequency.
- ptt_down(): never an FM carrier on a P25 / YSF letter, whatever reaches it (refused, TXDENY_MODE).
No switch: a fix wherever dual watch and P25 channels are built.

Home's details (part 2 of the same job, with VFO_MODES and NAC_DEC, i.e. Plus): each channel row says its mode's
own settings (integration/radiofoundry/mwatch/ui_detail.inc, installed as core/ui/ui_detail.inc): the single channel's
chips row for P25 / YSF / NXDN / dPMR ("NAC $293/659 TG 42" instead of FM's "No tone 12.5k"), dual watch's panes
(what PTT talks to on row 2 as on DMR, "TG 42", the NAC on row 3; a P25 VFO pane said "VFO FM") and the multi-watch
rows (ui_mwatch.inc / ui_mwatch_draw.inc: "P25 145.5000 NAC Any>$293/659 TG 1", "DMR 436.2750 CC1 TS1 TG 1",
"FM 433.5000 T88.5"), the wider form that fits."""


def prepare_mwfix(transform, install):
    transform('core/radio/radio_sm.h', [
        ('#if CONFIG_P25_CHANNELS\nvoid radio_sm_native_maintenance(radio_sm_t *sm, uint32_t now_ms);\n#endif\n',
         '#if CONFIG_P25_CHANNELS\nvoid radio_sm_native_maintenance(radio_sm_t *sm, uint32_t now_ms);\n'
         '#if CONFIG_DUAL_WATCH\n'
         '// DDM32 Plus: PTT in dual watch / the ring talks on the selected letter. 0: the receiver is on it (or no watch);\n'
         '// else (tune) tuned there now: 1, or 2 when it can\'t be read (the watch ended, the PTT refused: TXDENY_MODE)\n'
         'uint8_t radio_sm_dual_away(radio_sm_t *sm, bool tune, uint32_t now_ms);\n'
         '#endif\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('\tscan_tuned(&sm->scan, now_ms);\n\tscan_publish(sm);\n}\n\n'
         '// Requests that act on the channel the receiver is on: in dual watch that must be the selected one\n',
         '\tscan_tuned(&sm->scan, now_ms);\n\tscan_publish(sm);\n}\n\n'
         '#if CONFIG_P25_CHANNELS\n'
         '// DDM32 Plus (docs/plus.md "Multi-watch", the TX target): the native P25 owner sees PTT first (radio_task.c) and\n'
         '// asks here, so it never keys the letter the receiver happens to be on. tune: the selected one is tuned, as\n'
         '// dual_talk does for the radio core\'s own PTT; one that can\'t be read ends the watch and refuses the PTT\n'
         'uint8_t radio_sm_dual_away(radio_sm_t *sm, bool tune, uint32_t now_ms)\n{\n'
         '\tif (!dual_running(sm) || dual_here(sm, sm->dual_sel)) {\n\t\treturn 0;\n\t}\n'
         '\tif (tune) {\n\t\tdual_talk(sm, now_ms);\n'
         '\t\tif (!dual_running(sm)) {\n\t\t\tsm->ptt = true;\n\t\t\tdeny(sm, TXDENY_MODE);\n\t\t\treturn 2;\n\t\t}\n\t}\n'
         '\treturn 1;\n}\n#endif\n\n'
         '// Requests that act on the channel the receiver is on: in dual watch that must be the selected one\n'),
        ('\tif (dual_running(sm)) {\n\t\tdual_talk(sm, now_ms);\n\t} else\n',
         '\tif (dual_running(sm)) {\n\t\tdual_talk(sm, now_ms);\n'
         '#if CONFIG_P25_CHANNELS\n'
         '\t\tif (codeplug_mode_p25(sm->view.ch.mode)) {\n'
         '\t\t\tsm->ptt = true;\t// DDM32 Plus: never an FM carrier on a P25 letter (native_lab.c keys P25)\n'
         '\t\t\tdeny(sm, TXDENY_MODE);\n\t\t\treturn;\n\t\t}\n'
         '#endif\n'
         '\t} else\n'),
        # The 1750 Hz burst: the mode checked on the letter PTT keys (before: the ring's letter, then dual_talk keyed
        # the selected one, a DMR or P25 letter too, and the tone went out on it)
        ('\tif (on) {\n\t\tif (sm->view.tone_burst || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot ||\n',
         '\tif (on) {\n'
         '#if CONFIG_DUAL_WATCH\n'
         '\t\tif (dual_running(sm) && sm->view.state != RS_TX && !sm->hotspot) {\n'
         '\t\t\tdual_to_selected(sm, now_ms);\t// DDM32 Plus: on the selected letter first, as PTT\n'
         '\t\t}\n'
         '#endif\n'
         '\t\tif (sm->view.tone_burst || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot ||\n'),
    ])
    transform('core/ui/ui_dual.c', [
        # No new selection while transmitting: the radio drops it then (RREQ_DUAL / RREQ_MWATCH while TX), so the PTT
        # pill would sit on a letter PTT doesn't go to (radio_mwatch.inc / ui_mwatch.inc mw_tick re-ask one that raced)
        ('static bool apply(uint8_t sel)\n{\n',
         'static bool apply(uint8_t sel)\n{\n'
         '#if CONFIG_MULTI_WATCH\n'
         '\tif (ui_view()->state == RS_TX || ui_view()->state == RS_TX_DENIED) {\n'
         '\t\tui_toast(TR("Not while transmitting"));\t// DDM32 Plus: the pill stays where PTT goes\n'
         '\t\treturn false;\n\t}\n'
         '#endif\n'),
        # Talk back moves the PTT pill: a beep with the toast, so the next PTT's letter is never a surprise
        ('\td->back_ms = now + TALK_BACK_MS;\n\tui_toast(pane ? TR("Talk back on B") : TR("Talk back on A"));\n',
         '\td->back_ms = now + TALK_BACK_MS;\n\tui_toast(pane ? TR("Talk back on B") : TR("Talk back on A"));\n'
         '#if CONFIG_MULTI_WATCH\n\tui_svc_beep(UI_BEEP_MESSAGE);\t// DDM32 Plus: PTT goes there now\n#endif\n'),
        ('\tif (v->state == RS_TX || d->pane[d->sel].rx) {\n\t\td->back_ms = now + TALK_BACK_MS;',
         '\tif (v->state == RS_TX || v->state == RS_TX_DENIED || d->pane[d->sel].rx) {\n\t\td->back_ms = now + TALK_BACK_MS;'),
    ])

    # ---- Home's details (part 2) ----
    install('core/ui/ui_detail.inc', 'integration/radiofoundry/mwatch/ui_detail.inc')
    transform('core/ui/ui_screens.h', [
        ('#if CONFIG_MULTI_WATCH\n// DM32 Plus multi-watch (ui_dual.c): Home\'s row of a watched channel, A..F\n',
         '#if CONFIG_VFO_MODES && CONFIG_NAC_DEC && CONFIG_P25_CHANNELS\n'
         '// DDM32 Plus Home details (ui_detail.inc): a channel\'s own settings in its mode\'s terms, packed for a row\n'
         '#include "fmt.h"\n'
         '#define UI_DETAIL\t\t1\n'
         '#define UI_DETAIL_TALK\t\t0x100u\t// DMR, in c: a talk target, its ID in b and c\'s low byte\n'
         '#define UI_DETAIL_PRIVATE\t0x200u\t// ... a private call (an ID, not a TG)\n'
         'enum { UI_DETAIL_WIDE, UI_DETAIL_TIGHT, UI_DETAIL_TALK_ONLY, UI_DETAIL_REST };\n'
         'typedef struct {\n'
         '\tuint16_t a, b, c;\t// P25: RX NAC, TG, TX NAC; YSF / NXDN / dPMR: b; DMR: a = CC | TS << 8; FM: the tones\n'
         '\tuint8_t mode;\n'
         '} ui_detail_t;\n'
         'void ui_detail_of(const channel_t *ch, ui_detail_t *d);\n'
         'void ui_sb_detail(sbuf_t *b, const ui_detail_t *d, uint8_t form);\n'
         '#else\n#define UI_DETAIL\t\t0\n#endif\n'
         '// A dual watch pane that names what PTT talks to on its second row: DMR, with UI_DETAIL P25 / YSF / NXDN / dPMR too\n'
         '#define UI_DUAL_DIGITAL(p)\t((p)->mode == RADIO_MODE_DMR || (UI_DETAIL && (p)->target[0] != \'\\0\'))\n'
         '// The status strip\'s word while the watch runs (DDM32 Plus multi-watch: by its letters, ui_watch_name)\n'
         '#if CONFIG_MULTI_WATCH\n#define UI_WATCH_WORD\tui_watch_name(1)\n#else\n#define UI_WATCH_WORD\tTR("DUAL")\n#endif\n'
         '#if CONFIG_MULTI_WATCH\n// DM32 Plus multi-watch (ui_dual.c): Home\'s row of a watched channel, A..F\n'),
        ('\tconst char *caller;\t\t// while a call comes in on it\n} ui_mw_row_t;',
         '\tconst char *caller;\t\t// while a call comes in on it\n'
         '#if UI_DETAIL\n\tui_detail_t detail;\t\t// DDM32 Plus: its mode\'s settings (ui_detail.inc)\n#endif\n'
         '} ui_mw_row_t;'),
    ])
    transform('core/ui/ui_screens.c', [
        ('static const char *__attribute__((noinline)) vfox_home(const radio_view_t *v)\n',
         '#if UI_DETAIL\n#include "ui_detail.inc"\t// DDM32 Plus Home details (tools/plus_mwfix.py)\n#endif\n'
         'static const char *__attribute__((noinline)) vfox_home(const radio_view_t *v)\n'),
        ('\tsb_init(&b, buf, sizeof buf);\n\tbool tone_temp = (v->temp & (RADIO_TEMP_TX_TONE | RADIO_TEMP_RX_TONE)) != 0;\n',
         '#if UI_DETAIL\n'
         '\t// DDM32 Plus: a P25 / YSF / NXDN / dPMR channel shows its own settings here, not FM\'s tones and width (the VFO\n'
         '\t// has them on its row already)\n'
         '\tif (!v->vfo && (codeplug_mode_p25(v->ch.mode) || codeplug_mode_nxdn(v->ch.mode) ||\n'
         '\t\t\tcodeplug_mode_dpmr(v->ch.mode))) {\n'
         '\t\tx = detail_draw(f, x, y, w - 5 * f->w, &v->ch, buf, sizeof buf, ui_col.fg) + f->w;\n'
         '\t} else\n#endif\n'
         '\t{\n'
         '\tsb_init(&b, buf, sizeof buf);\n\tbool tone_temp = (v->temp & (RADIO_TEMP_TX_TONE | RADIO_TEMP_RX_TONE)) != 0;\n'),
        ('\tx = gfx_text(f, x, y, v->ch.bw == RADIO_BW_25K ? "25k" : "12.5k", bw_temp ? ui_col.warn : ui_col.fg) + f->w;\n',
         '\tx = gfx_text(f, x, y, v->ch.bw == RADIO_BW_25K ? "25k" : "12.5k", bw_temp ? ui_col.warn : ui_col.fg) + f->w;\n'
         '\t}\n'),
        ('\t\tif (p->mode == RADIO_MODE_DMR && !p->vfo) {\n', '\t\tif (UI_DUAL_DIGITAL(p) && !p->vfo) {\n', 2),
        ('\t} else if (p->mode == RADIO_MODE_DMR) {\n\t\tsb_str(&b, p->target[0] ? p->target : "DMR");\n',
         '\t} else if (UI_DUAL_DIGITAL(p)) {\n\t\tsb_str(&b, p->target[0] ? p->target : "DMR");\n'),
    ])
    transform('core/ui/ui_dual.c', [
        ('\tif (ch.rx_tone.type == TONE_NONE && ch.tx_tone.type == TONE_NONE) {\n',
         '#if UI_DETAIL\n'
         '\t// DDM32 Plus: P25 / YSF / NXDN / dPMR: what PTT talks to on row 2 ("TG 42"), the NAC on row 3, as DMR has them\n'
         '\tif (codeplug_mode_p25(ch.mode) || codeplug_mode_nxdn(ch.mode) || codeplug_mode_dpmr(ch.mode)) {\n'
         '\t\tui_detail_t det;\n\t\tsbuf_t tb;\n'
         '\t\tui_detail_of(&ch, &det);\n'
         '\t\tsb_init(&tb, p->target, sizeof p->target);\n'
         '\t\tui_sb_detail(&tb, &det, UI_DETAIL_TALK_ONLY);\n'
         '\t\tui_sb_detail(&b, &det, UI_DETAIL_REST);\n'
         '\t\treturn;\n\t}\n'
         '#endif\n'
         '\tif (ch.rx_tone.type == TONE_NONE && ch.tx_tone.type == TONE_NONE) {\n'),
    ])

    # ---- The watch named by its letters (owner 2026-11-22): Dual / Triple / Quad / 5x / 6x watch, in Turkish Çift /
    # Üçlü / Dörtlü / Beşli / Altılı izleme; the strip's word, the Quick tile, Tools' row, the # screen, the toasts ----
    transform('core/ui/ui_screens.h', [
        ('void ui_dual_ring_row(uint8_t k, const radio_view_t *v, ui_mw_row_t *out);\n',
         'void ui_dual_ring_row(uint8_t k, const radio_view_t *v, ui_mw_row_t *out);\n'
         '// DDM32 Plus: the watch named by its letters (in the ring, else Settings > Watch channels): form 0 "Triple watch",\n'
         '// 1 the strip\'s "TRIPLE", 2 the Quick tile\'s "Triple watch" | "Triple"; translated\n'
         'const char *ui_watch_name(uint8_t form);\n'
         'const char *ui_watch_on_text(void);\t// "Triple watch on" (2: "Dual watch on")\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('SCX_WORD(TR("DUAL"))', 'SCX_WORD(UI_WATCH_WORD)', 2),
        ('\tsb_str(&b, tr(names[kind]));\n',
         '#if CONFIG_MULTI_WATCH\n'
         '\tsb_str(&b, kind == QT_SCAN ? ui_watch_name(2) : tr(names[kind]));\t// DDM32 Plus: Triple watch, Quad watch ...\n'
         '#else\n'
         '\tsb_str(&b, tr(names[kind]));\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('\t\tif (ui_st.dual.on) {\n\t\t\tsb_str(&b, TR("Dual watch"));\n',
         '\t\tif (ui_st.dual.on) {\n'
         '#if CONFIG_MULTI_WATCH\n\t\t\tsb_str(&b, ui_watch_name(0));\t// DDM32 Plus: by its letters\n'
         '#else\n\t\t\tsb_str(&b, TR("Dual watch"));\n#endif\n'),
    ])
    transform('core/ui/ui_dual.c', [
        ('\tui_toast(TR("Dual watch on"));\n',
         '#if CONFIG_MULTI_WATCH\n\tui_toast(ui_watch_on_text());\t// DDM32 Plus: "Triple watch on"\n'
         '#else\n\tui_toast(TR("Dual watch on"));\n#endif\n', 3),
        ('\tsb_str(&b, TR("Dual watch off: on "));\n',
         '#if CONFIG_MULTI_WATCH\n\tsb_str(&b, mw_in_ring() ? TR("Watch off: on ") : TR("Dual watch off: on "));\n'
         '#else\n\tsb_str(&b, TR("Dual watch off: on "));\n#endif\n'),
        ('static uint16_t actions_count(void) { return mw_actions_count(); }\n',
         '// DDM32 Plus: the # screen\'s title names the watch by its letters (translated already: tr() gives it back)\n'
         'static char mw_scr_title[16] = "Dual watch";\n'
         'static uint16_t actions_count(void)\n{\n'
         '\tstr_copy(mw_scr_title, sizeof mw_scr_title, ui_watch_name(0));\n'
         '\treturn mw_actions_count();\n}\n'),
        ('\t.name = "dual", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("Dual Watch"),\n',
         '#if CONFIG_MULTI_WATCH\n'
         '\t.name = "dual", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = mw_scr_title,\n'
         '#else\n'
         '\t.name = "dual", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("Dual Watch"),\n'
         '#endif\n'),
    ])

