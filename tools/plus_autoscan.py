#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus auto airfield scan and auto repeater scan (docs/autoscan-design.md, branch autoscan), and the air watch
fixes it needed. Called last from plus_features.prepare(): it changes text the earlier prepares made (the Tools list,
the hold keys, PTT in dual watch).

  AUTO_SCAN  Tools > Air scan and Tools > Repeater scan (after ISS; Battery and the zone offer move two on, a stored
             hold code for Battery with them): the nearest airfield frequencies (100 km) or FM / DMR repeaters
             (60 km) of the map pack on the multi-watch ring, A..F each its own VFO, refreshed as the GPS position
             moves (integration/radiofoundry/map/autoscan.c, ui_autoscan.c, air/ui_airwatch.inc). Needs AIR_WATCH,
             REPEATERS, GPS, VFO_FM, VFO_DMR and DATAPACK. A held digit set to one of them works on a VFO too.
  Air watch  (every air watch ring, CONFIG_AIR_WATCH): PTT while the ring holds or waits on a letter talks on that
             letter (radio_airwatch.inc air_talk_back); a zone / VFO scan or dual watch asked for ends the air state
             first (air_scan_first); DOTA and cross-band end it before they take the radio (radio_dota.inc,
             radio_xband.inc air_session_end)."""

MAP = 'integration/radiofoundry/map/'


def prepare_autoscan(transform, install):
    install('core/ui/autoscan.h', MAP + 'autoscan.h')
    install('core/ui/autoscan.c', MAP + 'autoscan.c')
    install('core/ui/ui_autoscan.c', MAP + 'ui_autoscan.c')

    transform('config/features.mk', [
        ('CONFIG_SPECTRUM_WF3D ?= n\nFEATURES += SPECTRUM_WF3D\n',
         'CONFIG_AUTO_SCAN ?= n\nFEATURES += AUTO_SCAN\n'
         'ifneq ($(CONFIG_AIR_WATCH)$(CONFIG_REPEATERS)$(CONFIG_GPS)$(CONFIG_VFO_FM)$(CONFIG_VFO_DMR)$(CONFIG_DATAPACK),'
         'yyyyyy)\noverride CONFIG_AUTO_SCAN := n\nendif\n'
         'CONFIG_SPECTRUM_WF3D ?= n\nFEATURES += SPECTRUM_WF3D\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_AIR_WATCH\n',
         '#ifndef CONFIG_AUTO_SCAN\n#define CONFIG_AUTO_SCAN 0\t/* DDM32 Plus auto airfield / repeater scan (tools/plus_autoscan.py) */\n'
         '#endif\n#ifndef CONFIG_AIR_WATCH\n'),
    ])

    # ---- The radio core: PTT on the letter heard; a scan of the user's ends the air state first ----
    transform('core/radio/radio_sm.c', [
        ('\tif (dual_running(sm)) {\n\t\tdual_talk(sm, now_ms);\n#if CONFIG_P25_CHANNELS\n',
         '\tif (dual_running(sm)) {\n'
         '#if CONFIG_AIR_WATCH\n'
         '\t\tair_talk_back(sm);\t// DDM32 Plus: the air watch / auto scan: the letter heard (radio_airwatch.inc)\n'
         '#endif\n'
         '\t\tdual_talk(sm, now_ms);\n#if CONFIG_P25_CHANNELS\n'),
        ('\tif (sm->view.state == RS_TX || sm->hotspot || sm->ptt) {\n\t\treturn;\n\t}\n\tswitch (req->arg) {\n'
         '\tcase RSCAN_STOP:\n',
         '\tif (sm->view.state == RS_TX || sm->hotspot || sm->ptt) {\n\t\treturn;\n\t}\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tair_scan_first(sm, (uint8_t)req->arg, now_ms);\t// DDM32 Plus: the air watch ends before the user\'s scan\n'
         '#endif\n'
         '\tswitch (req->arg) {\n\tcase RSCAN_STOP:\n'),
    ])

    # ---- The UI: the ring's side in ui_airwatch.inc, the tick, the Tools entries, the held digit on a VFO ----
    transform('core/ui/ui_dual.c', [
        ('#include "ui_airwatch.inc"\t// DDM32 Plus air watch (item 44b)\n',
         '#if CONFIG_AUTO_SCAN\n#include "ui/autoscan.h"\t// DDM32 Plus auto scan: its ring (ui_airwatch.inc)\n#endif\n'
         '#include "ui_airwatch.inc"\t// DDM32 Plus air watch (item 44b)\n'),
    ])
    transform('core/ui/ui_app.c', [
        ('#if CONFIG_AUTO_RPT || CONFIG_MAP_TRACK\n#include "ui/mapnav.h"\n#endif\n',
         '#if CONFIG_AUTO_RPT || CONFIG_MAP_TRACK\n#include "ui/mapnav.h"\n#endif\n'
         '#if CONFIG_AUTO_SCAN\n#include "ui/autoscan.h"\t// DDM32 Plus auto scan\n#endif\n'),
        ('\tif (ui_mapnav_tick(now))\n\t\tdirty = true;\n#endif\n',
         '\tif (ui_mapnav_tick(now))\n\t\tdirty = true;\n#endif\n'
         '#if CONFIG_AUTO_SCAN\n'
         '\t// DDM32 Plus auto scan: its refresh, once a second while on (ui_autoscan.c)\n'
         '\tif (ui_autoscan_tick(now))\n\t\tdirty = true;\n#endif\n'),
        ('bool ui_hold_tool(uint8_t digit);\t// ui_menus.c (DDM32 Plus hold keys)\n',
         'bool ui_hold_tool(uint8_t digit);\t// ui_menus.c (DDM32 Plus hold keys)\n'
         '#if CONFIG_AUTO_SCAN\nbool ui_hold_any(uint8_t digit);\t// ui_hold.inc\n#endif\n'),
        ('\tif (d >= 0 && ev->gesture == GESTURE_LONG && !view->vfo && ui_hold_tool((uint8_t)d))\n',
         '#if CONFIG_AUTO_SCAN\n'
         '\t// (DDM32 Plus: an auto scan\'s digit on a VFO too, not while a frequency is typed: its letters are VFOs)\n'
         '\tif (d >= 0 && ev->gesture == GESTURE_LONG && (!view->vfo || (!ui_st.entry && ui_hold_any((uint8_t)d))) &&\n'
         '\t    ui_hold_tool((uint8_t)d))\n'
         '#else\n'
         '\tif (d >= 0 && ev->gesture == GESTURE_LONG && !view->vfo && ui_hold_tool((uint8_t)d))\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('#if CONFIG_AUTO_RPT\n#include "ui/mapnav.h"\n#endif\n',
         '#if CONFIG_AUTO_RPT\n#include "ui/mapnav.h"\n#endif\n'
         '#if CONFIG_AUTO_SCAN\n#include "ui/autoscan.h"\n#endif\n'),
        ('T_ISS, T_BATTERY,', 'T_ISS, T_AIRSCAN, T_RPTSCAN, T_BATTERY,'),
        ('(i != T_ISS || CONFIG_SATELLITES) &&',
         '(i != T_ISS || CONFIG_SATELLITES) && (i != T_AIRSCAN || (CONFIG_AUTO_SCAN && radio_am_capable())) &&'
         ' (i != T_RPTSCAN || CONFIG_AUTO_SCAN) &&'),
        ('"ISS", TRN("Battery"),', '"ISS", TRN("Air scan"), TRN("Repeater scan"), TRN("Battery"),'),
        ('\t\t[T_ISS] = ART_ICON_ALMANAC + 1,\t// the sky, as Satellites\n',
         '\t\t[T_ISS] = ART_ICON_ALMANAC + 1,\t// the sky, as Satellites\n'
         '\t\t[T_AIRSCAN] = ART_ICON_AIRBAND + 1,\t// the aircraft, as Airfields\n'
         '\t\t[T_RPTSCAN] = ART_ICON_GOTO + 1,\t// the way, as Repeaters\n'),
        ('#if CONFIG_AUTO_RPT\n\tcase T_AUTORPT:\n\t\tsb_str(&b, ui_mapnav_auto_on() ? TR("On") : TR("Off"));\n\t\tbreak;\n'
         '#endif\n',
         '#if CONFIG_AUTO_RPT\n\tcase T_AUTORPT:\n\t\tsb_str(&b, ui_mapnav_auto_on() ? TR("On") : TR("Off"));\n\t\tbreak;\n'
         '#endif\n'
         '#if CONFIG_AUTO_SCAN\n'
         '\tcase T_AIRSCAN:\n\t\tsb_str(&b, ui_autoscan_on() == AS_AIR ? TR("On") : TR("Off"));\n\t\tbreak;\n'
         '\tcase T_RPTSCAN:\n\t\tsb_str(&b, ui_autoscan_on() == AS_RPT ? TR("On") : TR("Off"));\n\t\tbreak;\n'
         '#endif\n'),
        ('#if CONFIG_AUTO_RPT\n\tcase T_AUTORPT: ui_mapnav_auto_toggle(); break;\n#endif\n',
         '#if CONFIG_AUTO_RPT\n\tcase T_AUTORPT: ui_mapnav_auto_toggle(); break;\n#endif\n'
         '#if CONFIG_AUTO_SCAN\n'
         '\tcase T_AIRSCAN: ui_autoscan_toggle(AS_AIR); break;\t// DDM32 Plus auto scan (ui_autoscan.c)\n'
         '\tcase T_RPTSCAN: ui_autoscan_toggle(AS_RPT); break;\n'
         '#endif\n'),
        # Settings > Keys: the hold codes' values, one per tool and None (tools/plus_satauto.py made 0..41)
        ('40, 41 };\n', '40, 41, 42, 43 };\n'),
    ])
