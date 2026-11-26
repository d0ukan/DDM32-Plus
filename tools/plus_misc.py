# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus round 3 "misc" job, applied last by tools/plus_features.py prepare_misc (docs/plus.md):

- Tools > Airfields (CONFIG_AIRFIELDS; needs AM_RX and APRS): the Turkish airfields of OurAirports nearest first, their
  frequencies; OK tunes the VFO as Tools > Air band does (AM receive, never TX). Table: tools/airfields.py, committed.
- Tools > Marine VHF (CONFIG_MARINE): the ITU RR Appendix 18 channels; OK puts the VFO on the coast frequency, FM 25 kHz,
  and the radio core refuses TX while the VFO is on a marine frequency (radio_sm_tx_check: TXDENY_RX_ONLY).
- QSO log (CONFIG_QSO_LOG; needs CALL_LOG): * on Recent marks a QSO, an automatic mark for calls we answered, and the
  debug link's read only 0x21 [14] export of the history.
- Battery history (CONFIG_BATT_GRAPH): Tools > Battery draws the last 3.2 hours and the time left.
- PC live panel (CONFIG_PC_LIVE): debug link 0x21 [13] (the spectrum sweep, read only, not counted as a look) and the
  GET_VIEW key plus_mode.
"""


def prepare(transform, install):
    install('core/ui/ui_misc.h', 'integration/radiofoundry/misc/ui_misc.h')
    transform('config/features.mk', [
        ('$(error P25_MODE2 needs P25_CHANNELS=y)\nendif\nendif\n',
         '$(error P25_MODE2 needs P25_CHANNELS=y)\nendif\nendif\n'
         'CONFIG_AIRFIELDS ?= n\nFEATURES += AIRFIELDS\n'
         'ifeq ($(CONFIG_AIRFIELDS),y)\nifneq ($(CONFIG_AM_RX)$(CONFIG_APRS)$(CONFIG_VFO),yyy)\n'
         '$(error AIRFIELDS needs AM_RX=y, APRS=y and VFO=y)\nendif\nendif\n'
         'CONFIG_MARINE ?= n\nFEATURES += MARINE\n'
         'ifeq ($(CONFIG_MARINE),y)\nifneq ($(CONFIG_VFO),y)\n$(error MARINE needs VFO=y)\nendif\nendif\n'
         'CONFIG_QSO_LOG ?= n\nFEATURES += QSO_LOG\n'
         'ifeq ($(CONFIG_QSO_LOG),y)\nifneq ($(CONFIG_CALL_LOG)$(CONFIG_DMR),yy)\n'
         '$(error QSO_LOG needs CALL_LOG=y and DMR=y)\nendif\nendif\n'
         'CONFIG_BATT_GRAPH ?= n\nFEATURES += BATT_GRAPH\n'
         'CONFIG_PC_LIVE ?= n\nFEATURES += PC_LIVE\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_P25_MODE2\n',
         '#ifndef CONFIG_AIRFIELDS\n#define CONFIG_AIRFIELDS 0\n#endif\n#ifndef CONFIG_MARINE\n#define CONFIG_MARINE 0\n#endif\n'
         '#ifndef CONFIG_QSO_LOG\n#define CONFIG_QSO_LOG 0\n#endif\n#ifndef CONFIG_BATT_GRAPH\n#define CONFIG_BATT_GRAPH 0\n'
         '#endif\n#ifndef CONFIG_PC_LIVE\n#define CONFIG_PC_LIVE 0\n#endif\n#ifndef CONFIG_P25_MODE2\n'),
    ])
    prepare_screens(transform, install)
    prepare_marine_guard(transform, install)
    prepare_qso(transform, install)
    prepare_battery(transform, install)
    prepare_pc_live(transform)


def prepare_screens(transform, install):
    """Tools > Airfields and Tools > Marine VHF (tools 29 and 30, after Auto repeater; Battery and the zone offer move
    two on)"""
    install('core/ui/airfield.h', 'integration/radiofoundry/air/airfield.h')
    install('core/ui/airfield.c', 'integration/radiofoundry/air/airfield.c')
    install('core/ui/airfield_data.h', 'integration/radiofoundry/air/airfield_data.h')
    # (airfield_data.c is the map pack's since 1.1: tools/refpack.py and the host tests read it where it is committed)
    install('core/ui/ui_airfield.c', 'integration/radiofoundry/air/ui_airfield.c')
    install('core/radio/marine.h', 'integration/radiofoundry/marine/marine.h')
    install('core/radio/marine.c', 'integration/radiofoundry/marine/marine.c')
    install('core/ui/ui_marine.c', 'integration/radiofoundry/marine/ui_marine.c')
    install('core/ui/battgraph.h', 'integration/radiofoundry/battery/battgraph.h')
    install('core/ui/battgraph.c', 'integration/radiofoundry/battery/battgraph.c')
    install('core/ui/ui_battgraph.c', 'integration/radiofoundry/battery/ui_battgraph.c')
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;',
         '\tSCR_AIRFIELDS,\t\t\t// DM32 Plus: Tools > Airfields (CONFIG_AIRFIELDS)\n'
         '\tSCR_MARINE,\t\t\t// DM32 Plus: Tools > Marine VHF (CONFIG_MARINE)\n'
         '\tSCR_BATTGRAPH,\t\t\t// DM32 Plus: Tools > Battery with its history (CONFIG_BATT_GRAPH)\n'
         '\tSCR_COUNT\n} ui_scr_t;'),
    ])
    transform('core/ui/ui_menus.c', [
        ('#include "radio/radio_am.h"\n',
         '#include "radio/radio_am.h"\n#if CONFIG_BATT_GRAPH || CONFIG_QSO_LOG\n#include "ui/ui_misc.h"\n#endif\n'
         '#if CONFIG_QSO_LOG\n#include "ui/qso_log.h"\n#endif\n'),
        ('extern const ui_scr_def_t ui_rpt_screen;\t// ui_rpt.c (DM32 Plus)\n',
         'extern const ui_scr_def_t ui_rpt_screen;\t// ui_rpt.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_airfield_screen;\t// ui_airfield.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_marine_screen;\t// ui_marine.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_battgraph_screen;\t// ui_battgraph.c (DM32 Plus)\n'),
        ('#if CONFIG_REPEATERS\n\t[SCR_REPEATERS] = &ui_rpt_screen,\n#endif\n',
         '#if CONFIG_REPEATERS\n\t[SCR_REPEATERS] = &ui_rpt_screen,\n#endif\n'
         '#if CONFIG_AIRFIELDS\n\t[SCR_AIRFIELDS] = &ui_airfield_screen,\n#endif\n'
         '#if CONFIG_MARINE\n\t[SCR_MARINE] = &ui_marine_screen,\n#endif\n'
         '#if CONFIG_BATT_GRAPH\n\t[SCR_BATTGRAPH] = &ui_battgraph_screen,\n#endif\n'),
        ('T_AUTORPT, ', 'T_AUTORPT, T_AIRFIELDS, T_MARINE, '),
        ('(i != T_AUTORPT || CONFIG_AUTO_RPT) &&',
         '(i != T_AUTORPT || CONFIG_AUTO_RPT) && (i != T_AIRFIELDS || (CONFIG_AIRFIELDS && radio_am_capable())) &&'
         ' (i != T_MARINE || CONFIG_MARINE) &&'),
        ('TRN("Auto repeater"), ', 'TRN("Auto repeater"), TRN("Airfields"), TRN("Marine VHF"), '),
        ('\t\t[T_AUTORPT] = ART_ICON_GOTO + 1,\t// and to the nearest on the way\n',
         '\t\t[T_AUTORPT] = ART_ICON_GOTO + 1,\t// and to the nearest on the way\n'
         '\t\t[T_AIRFIELDS] = ART_ICON_AIRBAND + 1,\t// the aircraft\n'
         '\t\t[T_MARINE] = ART_ICON_GOTO + 1,\t// the compass\n'),
        ('\tcase T_REPEATERS: ui_push(SCR_REPEATERS, 0); break;\n',
         '\tcase T_REPEATERS: ui_push(SCR_REPEATERS, 0); break;\n'
         '\tcase T_AIRFIELDS: ui_push(SCR_AIRFIELDS, 0); break;\n'
         '\tcase T_MARINE: ui_push(SCR_MARINE, 0); break;\n'),
        ('\tcase T_BATTERY: ui_push(SCR_INFO, INFO_BATTERY); break;',
         '#if CONFIG_BATT_GRAPH\n'
         '\tcase T_BATTERY: ui_push(ui_battgraph_wanted() ? SCR_BATTGRAPH : SCR_INFO, INFO_BATTERY); break;\n'
         '#else\n\tcase T_BATTERY: ui_push(SCR_INFO, INFO_BATTERY); break;\n#endif'),
    ])


def prepare_marine_guard(transform, install):
    """A marine VHF frequency in the VFO never transmits (marine.h): the RX only refusal, after the air band's"""
    transform('core/radio/radio_sm.c', [
        ('#include "radio/radio_sm.h"\n',
         '#include "radio/radio_sm.h"\n#if CONFIG_MARINE\n#include "radio/marine.h"\n#endif\n'),
        ('\t\treturn TXDENY_AM;\t// the air band is receive only, whatever the codeplug says\n\t}\n#endif\n',
         '\t\treturn TXDENY_AM;\t// the air band is receive only, whatever the codeplug says\n\t}\n#endif\n'
         '#if CONFIG_MARINE\n'
         '\t// DM32 Plus: the VFO on a marine VHF channel (Tools > Marine VHF, or typed) receives only: the maritime\n'
         '\t// mobile service is not an amateur band. Codeplug channels are not affected\n'
         '\tif (sm->view.vfo && (marine_hz(ch->rx_hz) || marine_hz(ch->tx_hz)))\n'
         '\t\treturn TXDENY_RX_ONLY;\n'
         '#endif\n'),
    ])


def prepare_qso(transform, install):
    """QSO log: the marks beside Recent (ui_app.c's list), * on Recent, the star on a marked row"""
    install('core/ui/qso_log.h', 'integration/radiofoundry/qso/qso_log.h')
    install('core/ui/qso_log.c', 'integration/radiofoundry/qso/qso_log.c')
    install('core/ui/ui_qso.c', 'integration/radiofoundry/qso/ui_qso.c')
    transform('core/ui/ui_app.c', [
        ('#include "input/input.h"\n',
         '#include "input/input.h"\n#if CONFIG_QSO_LOG\n#include "ui/qso_log.h"\n#endif\n'
         '#if CONFIG_BATT_GRAPH\n#include "ui/ui_misc.h"\n#endif\n'),
        # The marks move with the list (the same station again moves up with its mark; a new one starts unmarked)
        ('\tif (i == recent_n && recent_n < UI_RECENT_MAX)\n\t\trecent_n++;\n',
         '#if CONFIG_QSO_LOG\n\tbool qso_dup = i < recent_n;\n#endif\n'
         '\tif (i == recent_n && recent_n < UI_RECENT_MAX)\n\t\trecent_n++;\n'),
        ('\tmemmove(&recent[1], &recent[0], i * sizeof recent[0]);\n\trecent[0] = *r;\n',
         '\tmemmove(&recent[1], &recent[0], i * sizeof recent[0]);\n\trecent[0] = *r;\n'
         '#if CONFIG_QSO_LOG\n\tqso_log_shift(i, qso_dup);\n#endif\n'),
        # Every change to Recent between begin and end: the debug link's export copies a consistent list
        ('#if CONFIG_DMR\n\trecent_update(v);\n#if CONFIG_CALL_LOG\n\tcall_log_update();\n#endif\n',
         '#if CONFIG_DMR\n#if CONFIG_QSO_LOG\n\tqso_log_begin();\n#endif\n'
         '\trecent_update(v);\n#if CONFIG_CALL_LOG\n\tcall_log_update();\n#endif\n'
         '#if CONFIG_QSO_LOG\n\tqso_log_end(v, now);\n#endif\n'),
        ('\trecent_n = 0;\n\trecent_seen_ms = 0;\n\trecent_tx = false;\n',
         '\trecent_n = 0;\n\trecent_seen_ms = 0;\n\trecent_tx = false;\n'
         '#if CONFIG_QSO_LOG\n\tqso_log_reset();\n#endif\n#if CONFIG_BATT_GRAPH\n\tui_battgraph_reset();\n#endif\n'),
        # On Recent, * is the QSO mark (ui_qso.c), not the quick settings
        ('stack[sp].scr != SCR_DIAG && stack[sp].scr != SCR_HOME && !typing) {\n\t\tui_push(SCR_QUICK, 0);',
         'stack[sp].scr != SCR_DIAG && stack[sp].scr != SCR_HOME && !typing &&\n'
         '\t\t\t!(CONFIG_QSO_LOG && stack[sp].scr == SCR_RECENT)) {\n\t\tui_push(SCR_QUICK, 0);'),
    ])
    transform('core/ui/ui_menus.c', [
        ('static void recent_item(uint16_t i, ui_item_t *it)\n{\n\tconst ui_recent_t *r = ui_recent_get((uint8_t)i);\n'
         '\tif (r == NULL)\n\t\treturn;\n',
         'static void recent_item(uint16_t i, ui_item_t *it)\n{\n\tconst ui_recent_t *r = ui_recent_get((uint8_t)i);\n'
         '\tif (r == NULL)\n\t\treturn;\n'
         '#if CONFIG_QSO_LOG\n\tif (qso_log_marked((uint8_t)i))\n\t\tit->num = \'*\';\t// DM32 Plus: a QSO\n#endif\n'),
        ('.prefix = TRN("Recent"), .hint = TRN("PTT talk  # more  Back"),',
         '.prefix = TRN("Recent"),\n'
         '#if CONFIG_QSO_LOG\n\t.hint = TRN("PTT talk  * QSO  # more"), .key = ui_qso_recent_key,\n#else\n'
         '\t.hint = TRN("PTT talk  # more  Back"),\n#endif\n'),
    ])


def prepare_battery(transform, install):
    """Tools > Battery's history: a sample every 2 minutes from the UI task's tick, whatever screen is open"""
    transform('core/ui/ui_app.c', [
        ('\tbattery_tick(v);\n',
         '\tbattery_tick(v);\n#if CONFIG_BATT_GRAPH\n\tui_battgraph_tick(v, now);\t// DM32 Plus: Tools > Battery\'s history\n'
         '#endif\n'),
    ])


def prepare_pc_live(transform):
    """GET_VIEW's plus_mode: what the radio really receives (am in the air band), for the PC live panel. A new key: the
    view's version stays (debuglink_view.c: "new keys may appear anywhere in a section")"""
    transform('core/debuglink/debuglink_view.c', [
        ('\tdl_text(t, "am=%u\\nbcast=%u\\nbcast_fault=%u\\nbcast_khz=%u\\n", v->am,',
         '#if CONFIG_PC_LIVE\n'
         '\tdl_text(t, "plus_mode=%s\\n", v->am ? "am" : codeplug_mode_dpmr(v->ch.mode) ? "dpmr" :\n'
         '\t\tcodeplug_mode_nxdn(v->ch.mode) ? "nxdn" : codeplug_mode_ysf(v->ch.mode) ? "ysf" :\n'
         '\t\tcodeplug_mode_p25(v->ch.mode) ? "p25" : v->ch.mode == RADIO_MODE_DMR ? "dmr" : "fm");\n'
         '#endif\n'
         '\tdl_text(t, "am=%u\\nbcast=%u\\nbcast_fault=%u\\nbcast_khz=%u\\n", v->am,'),
    ])
