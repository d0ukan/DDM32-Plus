#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 13 "ideas 1" job (docs/plus.md): four field tools, each behind its own switch. Called last from
plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code is in
integration/radiofoundry/fieldx/ (installed into core/ui/), plus small additions to our own screens (spectrum/
ui_p25search.c, ui_dmrsearch.c, ui_goto.c, map/ui_map.c); upstream files change only through the hunks below.

  FIND_SAVE  (item 23) P25 / DMR search finds saved as channels in a zone "Bulunanlar N" through the zone editor's
             own queue (ZONE_OP_NEW, ZONE_OP_ADD_NEW); needs ZONE_EDIT and a search.
  NAV_FOLLOW (item 28) "find my friend": Go-To follows one APRS station (the map's pick, the heard list).
  LOCATOR    (item 30) Go-To's locator calculator page (# there): a Maidenhead locator to lat / lon, distance and
             bearing, saved as a waypoint; needs NAV and the APRS message text entry.
  SOS_KEY    (item 33) the key function "SOS" (Settings > Keys) and Settings > Keys > SOS key (its actions; Tools >
             SOS key until round 14): the confirmation page, then the chosen actions (APRS SOS, DMR emergency, CW SOS once); RAM only, all off at power on.
"""

FIELDX = 'integration/radiofoundry/fieldx/'


def prepare_fieldx(transform, install):
    for f in ('findsave.c', 'findsave.h', 'ui_findsave.c', 'follow.c', 'follow.h', 'locator.c', 'locator.h',
              'ui_locator.c', 'sos.c', 'sos.h', 'ui_sos.c'):
        install('core/ui/' + f, FIELDX + f)

    # The switches. After P25_SCAN's lines: every switch these depend on is set by then
    transform('config/features.mk', [
        ('$(error P25_SCAN needs P25_CHANNELS=y and SCAN=y)\nendif\nendif\n',
         '$(error P25_SCAN needs P25_CHANNELS=y and SCAN=y)\nendif\nendif\n'
         'CONFIG_FIND_SAVE ?= n\nFEATURES += FIND_SAVE\n'
         'ifeq ($(CONFIG_FIND_SAVE),y)\nifneq ($(CONFIG_ZONE_EDIT),y)\n$(error FIND_SAVE needs ZONE_EDIT=y)\nendif\n'
         'ifneq ($(findstring y,$(CONFIG_P25_SEARCH)$(CONFIG_DMR_SEARCH)),y)\n'
         '$(error FIND_SAVE needs P25_SEARCH=y or DMR_SEARCH=y)\nendif\nendif\n'
         'CONFIG_NAV_FOLLOW ?= n\nFEATURES += NAV_FOLLOW\n'
         'ifeq ($(CONFIG_NAV_FOLLOW),y)\nifneq ($(CONFIG_NAV)$(CONFIG_APRS),yy)\n'
         '$(error NAV_FOLLOW needs NAV=y and APRS=y)\nendif\nendif\n'
         'CONFIG_LOCATOR ?= n\nFEATURES += LOCATOR\n'
         'ifeq ($(CONFIG_LOCATOR),y)\nifneq ($(CONFIG_NAV)$(CONFIG_APRS_MSG),yy)\n'
         '$(error LOCATOR needs NAV=y and APRS_MSG=y)\nendif\nendif\n'
         'CONFIG_SOS_KEY ?= n\nFEATURES += SOS_KEY\n'
         'ifeq ($(CONFIG_SOS_KEY),y)\nifneq ($(CONFIG_KEY_ASSIGN),y)\n$(error SOS_KEY needs KEY_ASSIGN=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_RPT_PROV\n',
         '#ifndef CONFIG_FIND_SAVE\n#define CONFIG_FIND_SAVE 0\n#endif\n#ifndef CONFIG_NAV_FOLLOW\n#define CONFIG_NAV_FOLLOW 0\n'
         '#endif\n#ifndef CONFIG_LOCATOR\n#define CONFIG_LOCATOR 0\n#endif\n#ifndef CONFIG_SOS_KEY\n'
         '#define CONFIG_SOS_KEY 0\n#endif\n#ifndef CONFIG_RPT_PROV\n'),
    ])

    # --- SOS key: a key function and its input action (appended: stored values are never renumbered) ---
    transform('hal/hal_input.h', [
        ('\tACT_COUNT\n} input_action_t;',
         '\tACT_SOS,\t\t// DDM32 Plus: the one-key SOS (CONFIG_SOS_KEY, core/ui/sos.h); its long press confirms\n'
         '\tACT_COUNT\n} input_action_t;'),
    ])
    transform('core/input/keyassign.h', [
        ('\tKEYFN_COUNT\n} keyfn_t;',
         '\tKEYFN_SOS,\t\t// DDM32 Plus: the SOS page (CONFIG_SOS_KEY): press or long press; set to the press, its\n'
         '\t\t\t// long press (unless set) confirms that page\n\tKEYFN_COUNT\n} keyfn_t;'),
    ])
    transform('core/input/keyassign.c', [
        ('[KEYFN_AB] = ACT_AB,\n};',
         '[KEYFN_AB] = ACT_AB,\n#if CONFIG_SOS_KEY\n\t[KEYFN_SOS] = ACT_SOS,\n#endif\n};'),
        ('\t\treturn false;\t// retired\n\t}\n',
         '\t\treturn false;\t// retired\n\t}\n#if !CONFIG_SOS_KEY\n\tif (fn == KEYFN_SOS) {\n\t\treturn false;\t'
         '// DDM32 Plus: not in this build\n\t}\n#endif\n'),
        ('\t\t\tif (hold != KEYFN_DEFAULT) {\n\t\t\t\tm->long_action = fn_action[hold];\n\t\t\t}\n',
         '\t\t\tif (hold != KEYFN_DEFAULT) {\n\t\t\t\tm->long_action = fn_action[hold];\n\t\t\t}\n'
         '#if CONFIG_SOS_KEY\n\t\t\telse if (press == KEYFN_SOS) {\n'
         '\t\t\t\tm->long_action = ACT_SOS;\t// DDM32 Plus: the SOS key held confirms its page\n\t\t\t}\n#endif\n'),
    ])

    # --- The screens (appended), their table entries, Tools > SOS key, the key function in Settings > Keys ---
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;',
         '\tSCR_SOS, SCR_SOS_SET,\t\t// DDM32 Plus: the SOS key\'s page, Tools > SOS key (CONFIG_SOS_KEY)\n'
         '\tSCR_LOCATOR,\t\t\t// DDM32 Plus: Go-To\'s locator calculator (CONFIG_LOCATOR)\n'
         '\tSCR_COUNT\n} ui_scr_t;'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_cw_screen, ui_cwtrain_screen;\t// ui_cw.c (DM32 Plus)',
         'extern const ui_scr_def_t ui_cw_screen, ui_cwtrain_screen;\t// ui_cw.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_sos_screen, ui_sos_set_screen;\t// ui_sos.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_locator_screen;\t// ui_locator.c (DDM32 Plus)'),
        ('#if CONFIG_CW_RX\n\t[SCR_CW] = &ui_cw_screen,\n\t[SCR_CW_TRAIN] = &ui_cwtrain_screen,\n#endif\n',
         '#if CONFIG_CW_RX\n\t[SCR_CW] = &ui_cw_screen,\n\t[SCR_CW_TRAIN] = &ui_cwtrain_screen,\n#endif\n'
         '#if CONFIG_SOS_KEY\n\t[SCR_SOS] = &ui_sos_screen,\n\t[SCR_SOS_SET] = &ui_sos_set_screen,\n#endif\n'
         '#if CONFIG_LOCATOR\n\t[SCR_LOCATOR] = &ui_locator_screen,\n#endif\n'),
        # The tool slot T_SOS stays (later tools' hunks name it) but is never listed: the SOS key's actions moved to
        # Settings > Keys (round 14 item 48, owner 2026-11-20; below)
        ('T_SELCALL, T_CW, T_BATTERY,', 'T_SELCALL, T_SOS, T_CW, T_BATTERY,'),
        ('(i != T_CW || CONFIG_CW_RX) &&', 'i != T_SOS && (i != T_CW || CONFIG_CW_RX) &&'),
        ('TRN("DTMF / SelCall"), TRN("CW decoder"),', 'TRN("DTMF / SelCall"), TRN("SOS key"), TRN("CW decoder"),'),
        # Settings > Keys: "SOS key" after the key rows opens the actions page (SCR_SOS_SET)
        ('SA_LANGUAGE, SA_SCAN_PRIO };', 'SA_LANGUAGE, SA_SCAN_PRIO, SA_SOS_KEY };'),
        ('\tKEY_ROWS(8),\n#endif\n};\n',
         '\tKEY_ROWS(8),\n#endif\n#if CONFIG_SOS_KEY\n\tACTION(TRN("SOS key"), "emergency sos actions", SA_SOS_KEY),\t// DDM32 Plus\n'
         '#endif\n};\n'),
        ('\t\tcase SA_LANGUAGE: ui_push(SCR_LANGUAGE, 0); break;\n',
         '\t\tcase SA_LANGUAGE: ui_push(SCR_LANGUAGE, 0); break;\n'
         '#if CONFIG_SOS_KEY\n\t\tcase SA_SOS_KEY: ui_push(SCR_SOS_SET, 0); break;\t// DDM32 Plus\n#endif\n'),
        ('\t\t[T_CW] = ART_ICON_CW + 1,\t// the key\n',
         '\t\t[T_SOS] = ART_ICON_SOS + 1,\t// the warning triangle\n\t\t[T_CW] = ART_ICON_CW + 1,\t// the key\n'),
        # Settings > Keys: "SOS" for a press and a long press (not assigned by default: Default stays)
        ('\t\t\t\t\t KEYFN_AB, KEYFN_DUAL_WATCH,\n#endif\n};\nstatic const char *const n_keyfn_press[]',
         '\t\t\t\t\t KEYFN_AB, KEYFN_DUAL_WATCH,\n#endif\n#if CONFIG_SOS_KEY\n\t\t\t\t\t KEYFN_SOS,\n#endif\n};\n'
         'static const char *const n_keyfn_press[]'),
        ('\t\t\t\t\t     TRN("Dual watch"),\n#endif\n\n#endif\n};\nstatic const int16_t v_keyfn_hold[]',
         '\t\t\t\t\t     TRN("Dual watch"),\n#endif\n\n#endif\n#if CONFIG_SOS_KEY\n\t\t\t\t\t     "SOS",\n#endif\n};\n'
         'static const int16_t v_keyfn_hold[]'),
        ('\t\t\t\t\tKEYFN_AB, KEYFN_DUAL_WATCH,\n#endif\n};\nstatic const char *const n_keyfn_hold[]',
         '\t\t\t\t\tKEYFN_AB, KEYFN_DUAL_WATCH,\n#endif\n#if CONFIG_SOS_KEY\n\t\t\t\t\tKEYFN_SOS,\n#endif\n};\n'
         'static const char *const n_keyfn_hold[]'),
        ('\t\t\t\t\t     TRN("Dual watch"),\n#endif\n\n#endif\n};\n#endif\nstatic const int16_t v_updown[]',
         '\t\t\t\t\t     TRN("Dual watch"),\n#endif\n\n#endif\n#if CONFIG_SOS_KEY\n\t\t\t\t\t    "SOS",\n#endif\n};\n'
         '#endif\nstatic const int16_t v_updown[]'),
        # The APRS message text entry titles the locator's (arg 2)
        ('\tsb_str(&b, ui_frame()->arg != 0 ? TR("Text") : TR("To"));\n',
         '#if CONFIG_LOCATOR\n'
         '\tsb_str(&b, ui_frame()->arg == 2 ? TR("Locator") : ui_frame()->arg != 0 ? TR("Text") : TR("To"));\n'
         '#else\n\tsb_str(&b, ui_frame()->arg != 0 ? TR("Text") : TR("To"));\n#endif\n'),
    ])

    # --- The UI task's hooks: the SOS key before the key lock (as the emergency key), the ticks ---
    transform('core/ui/ui_app.c', [
        ('#if CONFIG_AUTO_RPT || CONFIG_MAP_TRACK\n#include "ui/mapnav.h"\n#endif\n',
         '#if CONFIG_AUTO_RPT || CONFIG_MAP_TRACK\n#include "ui/mapnav.h"\n#endif\n'
         '#if CONFIG_FIND_SAVE\n#include "ui/findsave.h"\n#endif\n#if CONFIG_NAV_FOLLOW\n#include "ui/follow.h"\n#endif\n'
         '#if CONFIG_SOS_KEY\n#include "ui/sos.h"\n#endif\n'),
        ('\tif (emergency_key(ev))\n\t\treturn;\n',
         '\tif (emergency_key(ev))\n\t\treturn;\n'
         '#if CONFIG_SOS_KEY\n\t// DDM32 Plus one-key SOS (ui/sos.h): its page, and the key held there confirms; like the emergency key it\n'
         '\t// works with the keys locked\n\tif (ui_sos_key(ev))\n\t\treturn;\n#endif\n'),
        ('\tif (ui_mapnav_tick(now))\n\t\tdirty = true;\n#endif\n',
         '\tif (ui_mapnav_tick(now))\n\t\tdirty = true;\n#endif\n'
         '#if CONFIG_FIND_SAVE\n\t// DDM32 Plus: search finds being saved, one zone editor write at a time (ui_findsave.c)\n'
         '\tif (ui_findsave_tick())\n\t\tdirty = true;\n#endif\n'
         '#if CONFIG_NAV_FOLLOW\n\t// DDM32 Plus: the APRS station Go-To follows, looked up once a second (ui_goto.c)\n'
         '\tif (ui_follow_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n'
         '#if CONFIG_SOS_KEY\n\t// DDM32 Plus one-key SOS: its actions\' progress and toasts (ui_sos.c)\n'
         '\tif (ui_sos_tick(now))\n\t\tdirty = true;\n#endif\n'),
    ])

    # --- Find my friend: Tools > APRS > Stations heard, OK follows the station in Go-To ---
    transform('core/ui/ui_aprs.c', [
        ('const ui_scr_def_t ui_aprs_heard_screen = {\n'
         '\t.name = "aprs-heard", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Heard"), .hint = TRN("Back"),\n',
         '#if CONFIG_NAV_FOLLOW\n#include "ui/follow.h"\n'
         '// DDM32 Plus find my friend (ui/follow.h): OK follows the station in Tools > Go-To\n'
         'static void heard_select(uint16_t i, bool hold)\n{\n\t(void)hold;\n\taprs_station_t s;\n'
         '\tif (ui_svc_aprs_station((uint8_t)i, &s)) {\n\t\tui_follow_start(s.call);\n\t}\n}\n#endif\n\n'
         'const ui_scr_def_t ui_aprs_heard_screen = {\n'
         '#if CONFIG_NAV_FOLLOW\n'
         '\t.name = "aprs-heard", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Heard"), .hint = TRN("OK follow  Back"),\n'
         '\t.select = heard_select,\n#else\n'
         '\t.name = "aprs-heard", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Heard"), .hint = TRN("Back"),\n#endif\n'),
    ])
