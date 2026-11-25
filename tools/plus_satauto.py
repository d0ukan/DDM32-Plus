#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus Satellites: ISS notifications and auto track (part of CONFIG_SATELLITES; docs/plus.md "Satellites: ISS
notifications and auto track"). Called last from plus_features.prepare(); a module of its own so the shared prepare
list gains one line. Our code is in integration/radiofoundry/sat/ (sat_svc.c, ui_sat.c, radio_sathold.inc);
upstream files change only through the hunks below.

  - The pass alarm page's "Every pass" (the alarm armed again after each rise, for the ISS or any satellite) and its
    lead and lowest elevation are kept in settings_t.cp_flags bits 1-6 (SETTINGS_CP_SAT_*), "Auto track" in
    settings_t.rs_flags bits 6-7 (SETTINGS_RS_SAT_AUTO): spare bits of existing bytes, 0 = off in an older record and
    after a factory reset, the record's size and layout upstream's. app.c changes cp_flags bit 0 through a mask; the
    radio core's rs_flags write (radio_sm.c save_settings) keeps bits 6-7 (hunk below). The UI reads and writes them
    through the UI storage service (UI_SET_SAT_NOTE / UI_SET_SAT_AUTO).
  - The radio core holds the user's state for an auto track (RREQ_SAT_HOLD, radio_sathold.inc): noted as a power off
    notes it, nothing noted meanwhile (persist), TX refused (receive only), put back at the end.
  - ui_app_key: a key while an auto track runs ends it (ui_sat_auto_key) before anything else; the emergency key goes
    on (the radio core ends the hold for the emergency itself, emergency_tick). A hold left without its track ends by
    itself after 2 s (sat_hold_watch).
"""

SAT = 'integration/radiofoundry/sat/'


def prepare_sat_auto(transform, install):
    install('core/radio/radio_sathold.inc', SAT + 'radio_sathold.inc')
    install('core/ui/ui_hold.inc', SAT + 'ui_hold.inc')

    # ---- The stored settings: spare bits of cp_flags and rs_flags ----
    transform('core/settings/settings.h', [
        ('#define SETTINGS_CP_NO_FM_ID\t\t0x80u\t// DM32 Plus: FM caller ID off (Settings > Talk & listen; 0 = on)\n',
         '#define SETTINGS_CP_NO_FM_ID\t\t0x80u\t// DM32 Plus: FM caller ID off (Settings > Talk & listen; 0 = on)\n'
         '// DDM32 Plus Satellites (ui_sat.c): the pass alarm\'s "Every pass", bits 1-2 for whom (0 off, 1 the ISS, 2 any\n'
         '// satellite), bits 3-6 its lead and lowest top elevation (an index into the alarm page\'s choices, 0 = 5 min, 0 deg)\n'
         '#define SETTINGS_CP_SAT_NOTE\t\t0x7Eu\n'
         '#define SETTINGS_CP_SAT_SHIFT\t\t1\n'),
        ('#define SETTINGS_RS_NO_TALK_PERMIT',
         '#define SETTINGS_RS_SAT_AUTO\t\t0xC0u\t// DDM32 Plus Satellites\' auto track: 0 off, 1 the downlink, 2 the ISS\'s APRS\n'
         '#define SETTINGS_RS_SAT_SHIFT\t\t6\t// (ui_sat.c; the radio core\'s rs_flags write keeps these bits)\n'
         '#define SETTINGS_RS_NO_TALK_PERMIT'),
        # DDM32 Plus hold keys: the one appended field (page 1 grows from 392 to 404 B, still two pages; an older record
        # or a firmware without it reads the defaults below; a firmware from before clamps the page it reads)
        ('\tsettings_zs_import_t zs_import[SETTINGS_ZS_IMPORTS];\n} settings_t;',
         '\tsettings_zs_import_t zs_import[SETTINGS_ZS_IMPORTS];\n'
         '\t// DDM32 Plus hold keys (ui_menus.c, Settings > Keys): the tool a held digit 0-9 opens on Home, 0 none, else its\n'
         '\t// number in the Tools list (the default: 4 the ISS, SETTINGS_HOLD_ISS). 12 B keeps the size a multiple of 4\n'
         '\tuint8_t hold_tool[12];\n'
         '} settings_t;\n'
         '#define SETTINGS_HOLD_ISS\t40u\t// Tools > ISS is number 39 (code 40): ui_menus.c checks it'),
    ])
    transform('core/settings/settings.c', [
        ('_Static_assert(sizeof(settings_t) == 392, "settings_t changed size: update docs/spec/settings-store.md 3 and this");',
         '_Static_assert(sizeof(settings_t) == 404, "settings_t changed size: update docs/spec/settings-store.md 3 and this");'),
        ('\ts->vfo2_cc = 1;\n\ts->vfo2_ts = 1;\n}',
         '\ts->vfo2_cc = 1;\n\ts->vfo2_ts = 1;\n\ts->hold_tool[4] = SETTINGS_HOLD_ISS;\t// DDM32 Plus: hold 4 = ISS\n}'),
    ])
    transform('core/radio/radio_sm.c', [
        # The radio core's own rs_flags write keeps the satellites' bits (they are the UI's)
        ('\tst->rs_flags = (uint8_t)((s->roger_beep ? SETTINGS_RS_ROGER_BEEP : 0) |',
         '\tst->rs_flags = (uint8_t)((st->rs_flags & SETTINGS_RS_SAT_AUTO) |\t// DDM32 Plus: the satellites\' (ui_sat.c)\n'
         '\t\t\t\t (s->roger_beep ? SETTINGS_RS_ROGER_BEEP : 0) |'),
        # The hold's flag, read by persist and the TX check before radio_sathold.inc is included
        ('#if CONFIG_DOTA\n#include "radio/dota.h"\t\t// DDM32 Plus DOTA Transfer (docs/dota.md 11.3)\n#else\n'
         '#define dota_on() false\n#endif\n',
         '#if CONFIG_DOTA\n#include "radio/dota.h"\t\t// DDM32 Plus DOTA Transfer (docs/dota.md 11.3)\n#else\n'
         '#define dota_on() false\n#endif\n'
         '#if CONFIG_SATELLITES\n'
         'static volatile bool sat_held;\t\t// DDM32 Plus Satellites\' auto track holds the user\'s state (radio_sathold.inc)\n'
         '#endif\n'),
        ('\t\treturn;\t\t// the hotspot\'s frequencies are the host\'s, not the radio\'s\n\t}\n',
         '\t\treturn;\t\t// the hotspot\'s frequencies are the host\'s, not the radio\'s\n\t}\n'
         '#if CONFIG_SATELLITES\n'
         '\tif (sat_held) {\n'
         '\t\treturn;\t\t// DDM32 Plus auto track: the user\'s state stays as noted before it (radio_sathold.inc)\n'
         '\t}\n'
         '#endif\n'),
        ('\tif (sm->set.tx_inhibit) {\n\t\treturn TXDENY_INHIBIT;\n\t}\n',
         '\tif (sm->set.tx_inhibit) {\n\t\treturn TXDENY_INHIBIT;\n\t}\n'
         '#if CONFIG_SATELLITES\n'
         '\tif (sat_held)\n'
         '\t\treturn TXDENY_RX_ONLY;\t// DDM32 Plus: a satellite\'s auto track is receive only\n'
         '#endif\n'),
        ('void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n',
         '#if CONFIG_SATELLITES\n#include "radio/radio_sathold.inc"\t// DDM32 Plus Satellites\' auto track\n#endif\n\n'
         'void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n'),
        ('\t// A beep or a vibration is an alert, not the user doing something; the volume changes nothing on air or on the\n',
         '#if CONFIG_SATELLITES\n'
         '\tif (req->type == RREQ_SAT_HOLD) {\n'
         '\t\tsat_hold_request(sm, req->arg != 0, now_ms);\t// DDM32 Plus: not the user doing something (it stops nothing)\n'
         '\t\treturn;\n'
         '\t}\n'
         '#endif\n'
         '\t// A beep or a vibration is an alert, not the user doing something; the volume changes nothing on air or on the\n'),
    ])
    transform('core/radio/radio_sm.h', [
        ('uint8_t radio_sm_tx_check(const radio_sm_t *sm);\n',
         'uint8_t radio_sm_tx_check(const radio_sm_t *sm);\n'
         '#if CONFIG_SATELLITES\n'
         'uint8_t radio_sat_txk(const radio_sm_t *sm);\t// DDM32 Plus: sat_service\'s SAT_TX_* (radio_sathold.inc)\n'
         '#endif\n'),
        ('\tRREQ_XBAND,\t\t// DDM32 Plus: arg 1 = a cross-band session starts (xband.h), 0 = it stops\n',
         '\tRREQ_XBAND,\t\t// DDM32 Plus: arg 1 = a cross-band session starts (xband.h), 0 = it stops\n'
         '#if CONFIG_SATELLITES\n'
         '\tRREQ_SAT_HOLD,\t\t// DDM32 Plus Satellites\' auto track: arg 1 hold the user\'s state, 0 put it back\n'
         '#endif\n'),
    ])
    transform('core/radio/radio.h', [
        ('void radio_req_vfo_tx(uint32_t hz);\n',
         'void radio_req_vfo_tx(uint32_t hz);\n'
         '#if CONFIG_SATELLITES\n'
         '// DDM32 Plus Satellites\' auto track (radio_sathold.inc): hold the user\'s position and VFOs (noted as a power off\n'
         '// notes them; refused unless the radio is free), TX refused meanwhile; false puts them back. Held: taken\n'
         'void radio_req_sat_hold(bool on);\n'
         'bool radio_sat_held(void);\n'
         '#endif\n'),
    ])
    transform('core/radio/radio_task.c', [
        ('void radio_req_mode(uint8_t mode) { post(RREQ_MODE, mode, 0); }\n',
         'void radio_req_mode(uint8_t mode) { post(RREQ_MODE, mode, 0); }\n'
         '#if CONFIG_SATELLITES\n'
         '// DDM32 Plus: from the radio task itself (the satellites\' service): waits a little for room rather than lose it\n'
         'void radio_req_sat_hold(bool on) { post(RREQ_SAT_HOLD, on, pdMS_TO_TICKS(20)); }\n'
         '#endif\n'),
    ])

    # ---- The UI storage service: the two settings ----
    transform('core/ui/ui_svc.h', [
        ('\tUI_SET_COUNT\n',
         '#if CONFIG_SATELLITES\n'
         '\tUI_SET_SAT_NOTE,\t// DDM32 Plus Satellites\' "Every pass" alarm, 0..63 (settings_t.cp_flags SETTINGS_CP_SAT_NOTE)\n'
         '\tUI_SET_SAT_AUTO,\t// ... auto track 0..2 (settings_t.rs_flags SETTINGS_RS_SAT_AUTO)\n'
         '\tUI_SET_HOLD_FIRST,\t// DDM32 Plus hold keys: the tool a held digit 0-9 opens on Home, 0 none, else its Tools\n'
         '\tUI_SET_HOLD_LAST = UI_SET_HOLD_FIRST + 9,\t// number (settings_t.hold_tool; ui_menus.c)\n'
         '#define UI_HOLD_ISS\t40\t// the ISS (Tools number 39) as a hold code: hold 4\'s default (settings.h)\n'
         '#endif\n'
         '\tUI_SET_COUNT\n'),
    ])
    transform('core/ui/ui_svc_storage.c', [
        ('\tcase UI_SET_NOISE_REDUCTION:\n',
         '#if CONFIG_SATELLITES\n'
         '\tcase UI_SET_SAT_NOTE:\t// DDM32 Plus (ui_sat.c): spare bits, 0 in an older record\n'
         '\t\tsettings_read(&st);\n'
         '\t\treturn (st.cp_flags & SETTINGS_CP_SAT_NOTE) >> SETTINGS_CP_SAT_SHIFT;\n'
         '\tcase UI_SET_SAT_AUTO:\n'
         '\t\tsettings_read(&st);\n'
         '\t\treturn (st.rs_flags & SETTINGS_RS_SAT_AUTO) >> SETTINGS_RS_SAT_SHIFT;\n'
         '\tcase UI_SET_HOLD_FIRST ... UI_SET_HOLD_LAST:\t// DDM32 Plus hold keys (ui_menus.c)\n'
         '\t\tsettings_read(&st);\n'
         '\t\treturn st.hold_tool[id - UI_SET_HOLD_FIRST];\n'
         '#endif\n'
         '\tcase UI_SET_NOISE_REDUCTION:\n'),
        ('\tcase UI_SET_NOISE_REDUCTION:\t// ui_svc_apply hands them to the vocoder\n',
         '#if CONFIG_SATELLITES\n'
         '\tcase UI_SET_SAT_NOTE:\t// DDM32 Plus (ui_sat.c arms the alarm itself)\n'
         '\t\tst = settings_begin();\n'
         '\t\tst->cp_flags = (uint8_t)((st->cp_flags & ~SETTINGS_CP_SAT_NOTE) |\n'
         '\t\t\t\t\t  (((uint32_t)value << SETTINGS_CP_SAT_SHIFT) & SETTINGS_CP_SAT_NOTE));\n'
         '\t\tsettings_end(hal_millis());\n'
         '\t\treturn true;\n'
         '\tcase UI_SET_SAT_AUTO:\n'
         '\t\tst = settings_begin();\n'
         '\t\tst->rs_flags = (uint8_t)((st->rs_flags & ~SETTINGS_RS_SAT_AUTO) |\n'
         '\t\t\t\t\t  (((uint32_t)value << SETTINGS_RS_SAT_SHIFT) & SETTINGS_RS_SAT_AUTO));\n'
         '\t\tsettings_end(hal_millis());\n'
         '\t\treturn true;\n'
         '\tcase UI_SET_HOLD_FIRST ... UI_SET_HOLD_LAST:\t// DDM32 Plus hold keys (ui_menus.c)\n'
         '\t\tif (value < 0 || value > 255)\n'
         '\t\t\treturn false;\n'
         '\t\tst = settings_begin();\n'
         '\t\tst->hold_tool[id - UI_SET_HOLD_FIRST] = (uint8_t)value;\n'
         '\t\tsettings_end(hal_millis());\n'
         '\t\treturn true;\n'
         '#endif\n'
         '\tcase UI_SET_NOISE_REDUCTION:\t// ui_svc_apply hands them to the vocoder\n'),
    ])

    # ---- A key while an auto track runs ends it (the radio back where it was), before anything else ----
    transform('core/ui/ui_app.c', [
        ('void ui_app_key(const input_event_t *ev)\n{\n\tdirty = true;\n\tlast_key_ms = now;\n\tlast_use_ms = now;\n'
         '\toff_warned = false;\n',
         '#if CONFIG_SATELLITES\n'
         'bool ui_sat_auto_key(const input_event_t *ev);\t// ui_sat.c (DDM32 Plus)\n'
         'bool radio_sat_held(void);\t// radio_sathold.inc (DDM32 Plus)\n'
         '#endif\n'
         'void ui_app_key(const input_event_t *ev)\n{\n\tdirty = true;\n\tlast_key_ms = now;\n\tlast_use_ms = now;\n'
         '\toff_warned = false;\n'
         '#if CONFIG_SATELLITES\n'
         '\tif (ui_sat_auto_key(ev))\n'
         '\t\treturn;\t// DDM32 Plus: the key ended a satellite\'s auto track (PTT and monitor go on below)\n'
         '#endif\n'),
        # A side key's hold function runs on DMR only (on FM holding the key is the squelch break); during a satellite
        # hold the radio sits on the FM satellite VFO, so an emergency / SOS hold function goes on (holehunt verifier)
        ('\tif (ev->gesture == GESTURE_LONG && ev->value != 0 && view->ch.mode != RADIO_MODE_DMR)\n\t\treturn;\n',
         '\tif (ev->gesture == GESTURE_LONG && ev->value != 0 && view->ch.mode != RADIO_MODE_DMR\n'
         '#if CONFIG_SATELLITES\n'
         '\t    && !((ev->action == ACT_EMERGENCY || ev->action == ACT_SOS) && radio_sat_held())\n'
         '#endif\n'
         '\t    )\n\t\treturn;\n'),
        # The AOS pop-up (auto track Off): "OK: track" under the rise's title (ui_sat_pop_text; ui_sat_auto_key acts)
        ('\t\t\tbanner(UI_BANNER_INFO, false, 0, sat_alarm, NULL);\n',
         '\t\t\tbanner(UI_BANNER_INFO, false, 0, sat_alarm, ui_sat_pop_text());\t// DDM32 Plus: the AOS pop-up\'s OK\n'),
    ])
    # ... and Home's hint row names what its keys do while it is up ("OK track  Back"; Home only)
    transform('core/ui/ui_screens.c', [
        ('void ui_draw_bottom(const ui_state_t *s, const char *hint)\n{\n',
         '#if CONFIG_SATELLITES\nconst char *ui_sat_pop_hint(void);\t// ui_sat.c (DDM32 Plus)\n#endif\n'
         'void ui_draw_bottom(const ui_state_t *s, const char *hint)\n{\n'
         '#if CONFIG_SATELLITES\n'
         '\tconst char *pop = ui_sat_pop_hint();\n'
         '\tif (pop != NULL)\n'
         '\t\thint = pop;\t// DDM32 Plus: the AOS pop-up on Home (OK tracks, Back hides it)\n'
         '#endif\n'),
    ])
    # The service learns PTT apart from the radio's own transmissions: an APRS frame (the user's # APRS via ISS) never
    # ends a held track, PTT does (sat.h SAT_TX_*); the frame counts from its wait for a clear channel on, so no Doppler
    # step aborts it (radio_sathold.inc radio_sat_txk, holehunt B8)
    transform('core/radio/radio_task.c', [
        ('\t\t\tsat_service(sm.view.vfo, sm.view.ch.rx_hz, sm.view.ch.tx_hz, sm.view.state == RS_TX || sm.ptt, false,\n',
         '\t\t\tsat_service(sm.view.vfo, sm.view.ch.rx_hz, sm.view.ch.tx_hz, radio_sat_txk(&sm), false,\n'),
        ('                sat_service(sm.view.vfo, sm.view.ch.rx_hz, sm.view.ch.tx_hz, sm.view.state == RS_TX || sm.ptt, true,\n',
         '                sat_service(sm.view.vfo, sm.view.ch.rx_hz, sm.view.ch.tx_hz, radio_sat_txk(&sm), true,\n'),
    ])
    # While a held (auto) track holds the radio, no APRS frame keys (beacons, SOS, digipeats wait for its end) but the
    # one the user asked for with # APRS via ISS, at the head of the queue
    transform('core/aprs/aprs.h', [
        ('bool aprs_rf_pending(void);\n',
         'bool aprs_rf_pending(void);\n'
         '// DDM32 Plus: an ISS frame (aprs_iss_send) waits, moved to the head of the queue: the only one sent while a\n'
         '// satellite track holds the radio (radio_sm.c aprs_can_start)\n'
         'bool aprs_rf_next_iss(void);\n'),
    ])
    transform('core/aprs/aprs.c', [
        ('bool aprs_rf_pending(void)\n{\n',
         '#if CONFIG_SATELLITES\n'
         '// DDM32 Plus: frames lo .. hi of the queue in reverse order, swapped a byte at a time (no frame-sized copy on\n'
         '// the radio task\'s stack; lock held)\n'
         'static void queue_reverse(uint8_t lo, uint8_t hi)\n{\n'
         '\tfor (; lo < hi; lo++, hi--) {\n'
         '\t\tuint8_t *x = (uint8_t *)&a.q[lo], *y = (uint8_t *)&a.q[hi];\n'
         '\t\tfor (size_t k = 0; k < sizeof(qframe_t); k++) {\n'
         '\t\t\tuint8_t t = x[k];\n'
         '\t\t\tx[k] = y[k];\n'
         '\t\t\ty[k] = t;\n'
         '\t\t}\n'
         '\t}\n'
         '}\n\n'
         '// An ISS frame waits: it goes to the head of the queue (the others keep their order behind it), so a beacon\n'
         '// held back meanwhile never stands before it\n'
         'bool aprs_rf_next_iss(void)\n{\n'
         '\tLOCK();\n'
         '\tuint8_t j = 0;\n'
         '\twhile (j < a.qn && a.q[j].kind != K_ISS)\n'
         '\t\tj++;\n'
         '\tbool found = j < a.qn && a.taken == 0;\n'
         '\tif (found && j > 0) {\n'
         '\t\tqueue_reverse(0, j);\n'
         '\t\tqueue_reverse(1, j);\n'
         '\t}\n'
         '\tUNLOCK();\n'
         '\treturn found;\n'
         '}\n'
         '#endif\n\n'
         'bool aprs_rf_pending(void)\n{\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('static bool aprs_can_start(const radio_sm_t *sm)\n{\n',
         'static bool aprs_can_start(const radio_sm_t *sm)\n{\n'
         '#if CONFIG_SATELLITES\n'
         '\tif (sat_held && !aprs_rf_next_iss())\n'
         '\t\treturn false;\t// DDM32 Plus: a satellite track holds the radio: only the user\'s # APRS via ISS frame\n'
         '#endif\n'),
        # An emergency (the key, the lone worker, SOS) ends a held / auto track at once, before the emergency module
        # takes the start: the user's channel back, then the emergency on it (holehunt B1)
        ('static void emergency_tick(radio_sm_t *sm, uint32_t now_ms)\n{\n',
         '#if CONFIG_SATELLITES\n'
         'static void sat_hold_end(radio_sm_t *sm, uint32_t now_ms);\t// radio_sathold.inc (DDM32 Plus)\n'
         '#endif\n'
         'static void emergency_tick(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '#if CONFIG_SATELLITES\n'
         '\tif (sat_held && dmr_emergency_start_pending()) {\n'
         '\t\tsat_hold_end(sm, now_ms);\t// DDM32 Plus: the satellite track gives the radio back for the emergency\n'
         '\t}\n'
         '#endif\n'),
        # A hold left without its track ends by itself (radio_sathold.inc sat_hold_watch, holehunt B6)
        ('void radio_sm_poll(radio_sm_t *sm, uint32_t now_ms)\n{\n',
         'void radio_sm_poll(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '#if CONFIG_SATELLITES\n'
         '\tsat_hold_watch(sm, now_ms);\t// DDM32 Plus\n'
         '#endif\n'),
    ])
    prepare_iss_tool(transform)


def prepare_iss_tool(transform):
    """Tools > ISS (docs/plus.md "Tools > ISS and the AOS pop-up"): the ISS on one page (ui_sat.c ui_iss_screen). It
    takes the number after All digital search, so only Battery (last by design, the one a radio may not have) and the
    zone offer's 0 move; every other tool keeps its number."""
    transform('core/ui/ui_nav.h', [
        ('\tSCR_SATELLITES,\t\t\t// DM32 Plus: Tools > Satellites (CONFIG_SATELLITES)\n',
         '\tSCR_SATELLITES,\t\t\t// DM32 Plus: Tools > Satellites (CONFIG_SATELLITES)\n'
         '\tSCR_ISS,\t\t\t// DDM32 Plus: Tools > ISS (CONFIG_SATELLITES)\n'),
    ])
    # Home's held digits (a channel; every digit hold there was the dead "Speed dial N is empty" toast, and in the VFO a
    # hold types the digit): the tool chosen in Settings > Keys, hold 4 = the ISS by default (ui_menus.c ui_hold_tool;
    # the ISS: its held track, else Tools > ISS, ui_sat.c ui_sat_home_iss). The hint group "4 ISS" ends the row; with a
    # caller "OK Menu" goes and the name is cut with an ellipsis to fit
    transform('core/ui/ui_app.c', [
        ('bool ui_home_key(const input_event_t *ev)\n{\n',
         '#if CONFIG_SATELLITES\n#include "ui/gfx.h"\n#include "ui/ui_screens.h"\n'
         'bool ui_hold_tool(uint8_t digit);\t// ui_menus.c (DDM32 Plus hold keys)\n'
         'const char *ui_hold_hint(bool load);\nvoid ui_hold_forget(void);\n#endif\n'
         'bool ui_home_key(const input_event_t *ev)\n{\n'),
        ('\thas_quick_key = ui_has_action(ACT_QUICK);\n',
         '\thas_quick_key = ui_has_action(ACT_QUICK);\n'
         '#if CONFIG_SATELLITES\n\tui_hold_forget();\t// DDM32 Plus: the hold keys read again\n#endif\n'),
        ('\tif (d >= 0 && ev->gesture == GESTURE_LONG && !view->vfo) {\n\t\tstatic char msg[48];\n',
         '#if CONFIG_SATELLITES\n'
         '\tif (d >= 0 && ev->gesture == GESTURE_LONG && !view->vfo && ui_hold_tool((uint8_t)d))\n'
         '\t\treturn true;\t// DDM32 Plus: a held digit opens its tool (Settings > Keys; hold 4 = the ISS)\n'
         '#endif\n'
         '\tif (d >= 0 && ev->gesture == GESTURE_LONG && !view->vfo) {\n\t\tstatic char msg[48];\n'),
        ('\tsb_str(&b, home_hint_keys());\n'
         '\tif (has_context_key && recent_n && caller_target(&t)) {\n'
         '\t\tsb_str(&b, "  # ");\n'
         '\t\tif (t.name[0]) {\n'
         '\t\t\tmemcpy(name, t.name, sizeof name - 1);\n'
         '\t\t\tname[sizeof name - 1] = 0;\n'
         '\t\t\tchar *sp_ = strchr(name, \' \');\n'
         '\t\t\tif (sp_ != NULL)\n'
         '\t\t\t\t*sp_ = 0;\t// first word of the name\n'
         '\t\t\tsb_str(&b, name);\n'
         '\t\t} else {\n'
         '\t\t\tsb_u32(&b, t.id, 1);\n'
         '\t\t}\n'
         '\t}\n',
         '#if CONFIG_SATELLITES\n'
         '\t// DDM32 Plus: "OK Menu  * Quick  4 ISS" (the held-digit shortcut, Settings > Keys; none: as upstream); with a\n'
         '\t// caller "* Quick  # Joe  4 ISS": OK Menu goes (OK opens the menu anyway) and the name is cut with an ellipsis\n'
         '\t// until the row fits, so the keys and the shortcut are never cut. A shortcut too wide for the idle row sends\n'
         '\t// OK Menu away too, and only then is itself cut with an ellipsis\n'
         '\tconst char *keys = home_hint_keys();\n'
         '\tconst char *hold = ui_hold_hint(true);\t// NULL: no digit is set (or the board has no digit keys)\n'
         '\tbool who = has_context_key && recent_n && caller_target(&t);\n'
         '\tif (hold != NULL && !has_quick_key)\n'
         '\t\tkeys = TR("Hold OK Quick");\n'
         '\telse if (hold != NULL && who)\n'
         '\t\tkeys = TR("* Quick");\n'
         '\tname[0] = 0;\n'
         '\tif (who) {\n'
         '\t\tif (t.name[0]) {\n'
         '\t\t\tmemcpy(name, t.name, sizeof name - 1);\n'
         '\t\t\tname[sizeof name - 1] = 0;\n'
         '\t\t\tchar *sp_ = strchr(name, \' \');\n'
         '\t\t\tif (sp_ != NULL)\n'
         '\t\t\t\t*sp_ = 0;\t// first word of the name\n'
         '\t\t} else {\n'
         '\t\t\tsbuf_t nb;\n'
         '\t\t\tsb_init(&nb, name, sizeof name);\n'
         '\t\t\tsb_u32(&nb, t.id, 1);\n'
         '\t\t}\n'
         '\t}\n'
         '\tchar row[sizeof home_hint];\n'
         '\tsize_t nl = strlen(name);\n'
         '\tif (nl > 0) {\t// the name cut at 11 bytes: a character left incomplete goes\n'
         '\t\tsize_t k = nl - 1;\n'
         '\t\twhile (k > 0 && ((unsigned char)name[k] & 0xC0) == 0x80)\n'
         '\t\t\tk--;\n'
         '\t\tunsigned char c0 = (unsigned char)name[k];\n'
         '\t\tsize_t len = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;\n'
         '\t\tif (k + len > nl)\n'
         '\t\t\tname[nl = k] = 0;\n'
         '\t}\n'
         '\tsbuf_t rb;\n'
         '\tfor (int pass = 0;; pass++) {\n'
         '\t\tfor (size_t n = nl;; n--) {\n'
         '\t\t\tif (n > 0 && n < nl && ((unsigned char)name[n] & 0xC0) == 0x80)\n'
         '\t\t\t\tcontinue;\t// not inside a multi-byte character\n'
         '\t\t\tsb_init(&rb, row, sizeof row);\n'
         '\t\t\tsb_str(&rb, keys);\n'
         '\t\t\tif (n > 0) {\n'
         '\t\t\t\tchar cut[sizeof name];\n'
         '\t\t\t\tmemcpy(cut, name, n);\n'
         '\t\t\t\tcut[n] = 0;\n'
         '\t\t\t\tsb_str(&rb, "  # ");\n'
         '\t\t\t\tsb_str(&rb, cut);\n'
         '\t\t\t\tif (n < nl)\n'
         '\t\t\t\t\tsb_str(&rb, GLYPH_ELLIPSIS);\n'
         '\t\t\t}\n'
         '\t\t\tif (hold != NULL) {\n'
         '\t\t\t\tsb_str(&rb, "  ");\n'
         '\t\t\t\tsb_str(&rb, hold);\n'
         '\t\t\t}\n'
         '\t\t\tif (hold == NULL || n == 0 || gfx_text_width(ui_layout.small, row) <= gfx_width() - 2)\n'
         '\t\t\t\tbreak;\n'
         '\t\t}\n'
         '\t\tif (hold == NULL || pass == 1 || who || !has_quick_key || gfx_text_width(ui_layout.small, row) <= gfx_width() - 2)\n'
         '\t\t\tbreak;\n'
         '\t\tkeys = TR("* Quick");\t// the shortcut is wide: OK Menu goes too\n'
         '\t}\n'
         '\tif (hold != NULL && gfx_text_width(ui_layout.small, row) > gfx_width() - 2) {\n'
         '\t\tchar fit[32];\t// still too wide (no caller left to cut): the shortcut itself is cut with an ellipsis\n'
         '\t\tsb_init(&rb, row, sizeof row);\n'
         '\t\tsb_str(&rb, keys);\n'
         '\t\tsb_str(&rb, "  ");\n'
         '\t\tgfx_text_fit(ui_layout.small, hold, gfx_width() - 2 - gfx_text_width(ui_layout.small, row), fit, sizeof fit);\n'
         '\t\tsb_str(&rb, fit);\n'
         '\t}\n'
         '\tstr_copy(home_hint, sizeof home_hint, row);\n'
         '#else\n'
         '\tsb_str(&b, home_hint_keys());\n'
         '\tif (has_context_key && recent_n && caller_target(&t)) {\n'
         '\t\tsb_str(&b, "  # ");\n'
         '\t\tif (t.name[0]) {\n'
         '\t\t\tmemcpy(name, t.name, sizeof name - 1);\n'
         '\t\t\tname[sizeof name - 1] = 0;\n'
         '\t\t\tchar *sp_ = strchr(name, \' \');\n'
         '\t\t\tif (sp_ != NULL)\n'
         '\t\t\t\t*sp_ = 0;\t// first word of the name\n'
         '\t\t\tsb_str(&b, name);\n'
         '\t\t} else {\n'
         '\t\t\tsb_u32(&b, t.id, 1);\n'
         '\t\t}\n'
         '\t}\n'
         '#endif\n'),
        ('static char home_hint[32];', 'static char home_hint[56];\t// DDM32 Plus: room for "* Quick  # name  7 Single-freq repeater+"'),
    ])
    # The same hint in dual watch (Home's own hint there): the ISS group must fit the 220 px row, so the second form is
    # shorter (ui_draw_hint picks the longest that fits)
    transform('core/ui/ui_screens.c', [
        ('static const char *dual_hint(const radio_view_t *v, const ui_state_t *s)\n{\n',
         '#if CONFIG_SATELLITES\nconst char *ui_hold_dual_hint(void);\t// ui_menus.c (the mwatch hint uses it too)\n#endif\n'
         'static const char *dual_hint(const radio_view_t *v, const ui_state_t *s)\n{\n'),
        ('\treturn TR("# options  Back stop");\n}',
         '#if CONFIG_SATELLITES\n'
         '\tif (!v->vfo) {\t// on a VFO letter a held digit types it: no shortcut\n'
         '\t\tconst char *hh = ui_hold_dual_hint();\t// "# options  4 ISS  Back stop"; NULL: no digit is set\n'
         '\t\tif (hh != NULL)\n'
         '\t\t\treturn hh;\n'
         '\t}\n'
         '#endif\n'
         '\treturn TR("# options  Back stop");\n}'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_sat_screen;\t// ui_sat.c (DM32 Plus)\n',
         'extern const ui_scr_def_t ui_sat_screen;\t// ui_sat.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_iss_screen;\t// ui_sat.c (DDM32 Plus)\n'),
        ('#if CONFIG_SATELLITES\n\t[SCR_SATELLITES] = &ui_sat_screen,\n#endif\n',
         '#if CONFIG_SATELLITES\n\t[SCR_SATELLITES] = &ui_sat_screen,\n\t[SCR_ISS] = &ui_iss_screen,\n#endif\n'),
        ('T_ALLSEARCH, T_BATTERY,', 'T_ALLSEARCH, T_ISS, T_BATTERY,'),
        ('(i != T_ALLSEARCH || CONFIG_ALL_SEARCH) &&', '(i != T_ALLSEARCH || CONFIG_ALL_SEARCH) && (i != T_ISS || CONFIG_SATELLITES) &&'),
        ('TRN("All digital search"), TRN("Battery"),', 'TRN("All digital search"), "ISS", TRN("Battery"),'),
        ('\tcase T_ALLSEARCH: ui_push(SCR_ALLSEARCH, 0); break;\n',
         '\tcase T_ALLSEARCH: ui_push(SCR_ALLSEARCH, 0); break;\n'
         '#if CONFIG_SATELLITES\n\tcase T_ISS: ui_push(SCR_ISS, 0); break;\n#endif\n'),
        ('\t\t[T_ALLSEARCH] = ART_ICON_P25SEARCH + 1,',
         '\t\t[T_ISS] = ART_ICON_ALMANAC + 1,\t// the sky, as Satellites\n\t\t[T_ALLSEARCH] = ART_ICON_P25SEARCH + 1,'),
    ])
    prepare_hold_keys(transform)


def prepare_hold_keys(transform):
    """Hold keys on Home (docs/plus.md "Hold keys on Home"): each digit 0-9 held on a Home channel opens a tool picked in
    Settings > Keys (ui_hold.inc; stored in settings_t.hold_tool, hold 4 = the ISS by default). The Tools list's names
    move to file scope (index 0 is "None": the settings list shows them), and its dispatch becomes tool_open."""
    n_tools = 42   # T_COUNT: the values of the settings list, 0 (None) and the tool numbers 1..41 (not the zone offer)
    transform('core/ui/ui_menus.c', [
        # The names at file scope, "None" first: the tool with number n has tool_names[n]
        ('static void tools_item(uint16_t row, ui_item_t *it)\n{\n\tstatic const char *const names[] = { ',
         '// DDM32 Plus: at file scope (Settings > Keys lists them for the hold keys); index 0 is "None", the tool T_x has\n'
         '// tool_names[T_x + 1]\n'
         'static const char *const tool_names[] = { TRN("None"), '),
        ('TRN("Zones/Chs Received") };\n\tuint16_t i = tools_row(row);',
         'TRN("Zones/Chs Received") };\n\nstatic void tools_item(uint16_t row, ui_item_t *it)\n{\n\tuint16_t i = tools_row(row);'),
        ('TR("Supply") :\n\t\t tr(names[i]));', 'TR("Supply") :\n\t\t tr(tool_names[i + 1u]));'),
        # The dispatch by tool, for the hold keys too
        ('static void tools_select(uint16_t row, bool hold)\n{\n\t(void)hold;\n\tswitch (tools_row(row)) {\n\tcase T_SCAN:',
         'static void tool_open(uint16_t t)\t// DDM32 Plus: a Tools row, or a held digit (ui_hold.inc)\n{\n\tswitch (t) {\n\tcase T_SCAN:'),
        ('\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n\tdefault: break;\n\t}\n}\n',
         '\tcase T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;\n\tdefault: break;\n\t}\n}\n\n'
         'static void tools_select(uint16_t row, bool hold)\n{\n\t(void)hold;\n\ttool_open(tools_row(row));\n}\n\n'
         '#if CONFIG_SATELLITES\n#include "ui_hold.inc"\t// DDM32 Plus hold keys\n#endif\n'),
        # Settings > Keys: the ten rows, after the others
        ('static const set_item_t s_keys[] = {\n',
         '#if CONFIG_SATELLITES\n'
         '// DDM32 Plus: the value is the hold code (0 None, else the Tools number), the name the tool\'s (tool_names)\n'
         'static const int16_t v_holdtool[] = { ' + ', '.join(str(i) for i in range(n_tools)) + ' };\n'
         '_Static_assert(sizeof v_holdtool / sizeof v_holdtool[0] == T_COUNT, "one value per tool and None");\n'
         '#define HOLD_ROW(d) LIST(hold_label[d], "hold digit long press key shortcut tool", UI_SET_HOLD_FIRST + (d), v_holdtool, tool_names)\n'
         '#endif\n'
         'static const set_item_t s_keys[] = {\n'),
        ('\tACTION(TRN("SOS key"), "emergency sos actions", SA_SOS_KEY),\t// DDM32 Plus\n#endif\n};\n',
         '\tACTION(TRN("SOS key"), "emergency sos actions", SA_SOS_KEY),\t// DDM32 Plus\n#endif\n'
         '#if CONFIG_SATELLITES\n'
         '\tHOLD_ROW(0), HOLD_ROW(1), HOLD_ROW(2), HOLD_ROW(3), HOLD_ROW(4), HOLD_ROW(5), HOLD_ROW(6), HOLD_ROW(7),\n'
         '\tHOLD_ROW(8), HOLD_ROW(9),\t// DDM32 Plus hold keys: last, so the rows above keep their places\n'
         '#endif\n};\n'),
        ('#if CONFIG_KEY_ASSIGN\n\tif (s->type != ST_ACTION && s->id >= UI_SET_KEY_FIRST && s->id <= UI_SET_KEY_LAST) {\n',
         '#if CONFIG_SATELLITES\n'
         '\tif (s->type != ST_ACTION && s->id >= UI_SET_HOLD_FIRST && s->id <= UI_SET_HOLD_LAST) {\n'
         '\t\thold_labels();\n'
         '\t\treturn !ui_has_action((uint8_t)(ACT_DIGIT_0 + (s->id - UI_SET_HOLD_FIRST)));\t// a digit this radio has no key for\n'
         '\t}\n'
         '#endif\n'
         '#if CONFIG_KEY_ASSIGN\n\tif (s->type != ST_ACTION && s->id >= UI_SET_KEY_FIRST && s->id <= UI_SET_KEY_LAST) {\n'),
        ('static void set_store(const set_item_t *s, int32_t v)\n{\n\tif (ui_svc_setting_set(s->id, v)) {\n',
         'static void set_store(const set_item_t *s, int32_t v)\n{\n\tif (ui_svc_setting_set(s->id, v)) {\n'
         '#if CONFIG_SATELLITES\n'
         '\t\tif (s->id >= UI_SET_HOLD_FIRST && s->id <= UI_SET_HOLD_LAST)\n'
         '\t\t\thold_load();\t// DDM32 Plus: the hold keys read again (here, off the draw paths)\n'
         '#endif\n'),
    ])
    # Upstream's factory reset test counts the flash bytes of one settings save (page 1 is 12 B longer now: 156 + 12 B of
    # payload + the 16 B header + 4 B commit)
    transform('tests/host/radioCoreTest.c', [
        ('sim_flash_cut_after(432);\t// the settings records (page 1: 172 + 4 bytes, page 0: 252 + 4) go in',
         'sim_flash_cut_after(444);\t// the settings records (page 1: 184 + 4 bytes, page 0: 252 + 4) go in'),
    ])
    transform('core/ui/ui_svc_stub.c', [
        ('\t[UI_SET_NOISE_REDUCTION] = 1,\n};',
         '#if CONFIG_SATELLITES\n\t[UI_SET_HOLD_FIRST + 4] = UI_HOLD_ISS,\t// DDM32 Plus: hold 4 = the ISS\n#endif\n'
         '\t[UI_SET_NOISE_REDUCTION] = 1,\n};'),
    ])
