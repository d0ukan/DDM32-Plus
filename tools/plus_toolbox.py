#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 13 "ideas 2" job (docs/plus.md): a static RAM reclaim, then five tools, each behind its own switch.
Called last from plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code is in
integration/radiofoundry/common/ (the shared screen buffer) and integration/radiofoundry/toolbox/ (installed into
core/ui/), plus changes to our own screens (spectrum/ui_spectrum.c, map/ui_map.c, map/ui_mapnav.c, map/mapnav.*);
upstream files change only through the hunks below.

  RAM reclaim  the six tool screens' copies (spectrum, DMR / P25 search, DMR / P25 trunk, the message page) share one
               buffer (core/ui/ui_scratch.c); the debug log ring is 512 B (plus_features.PLUS_RAM_TRIM).
  HEARD        (item 24) Tools > Heard list: every station heard (P25, DMR, YSF, NXDN, dPMR), merged, newest first,
               OK opens one, a P25 / DMR one tunes the VFO; fed where Recent is fed. Needs CALL_LOG.
  SPECTRUM_WF  (item 26) Tools > Spectrum's waterfall (* on / off): the last 40 sweeps in a heap buffer the screen holds
               while it is open. Needs SPECTRUM.
  MAP_COVERAGE (item 27) the map track's points coloured by the signal when each was taken (* on the map). Needs
               MAP_TRACK.
  GPS_DASH     (item 29) Tools > GPS: speed, heading, altitude, trip distance, top speed, fix. Needs GPS.
  CLOCK_TOOLS  (item 34) Tools > Clock: stopwatch with laps, countdown, alarm at a time of day (GPS time); the countdown
               and the alarm sound and show a banner on any screen.
"""

COMMON = 'integration/radiofoundry/common/'
TOOLBOX = 'integration/radiofoundry/toolbox/'


def prepare_toolbox(transform, install):
    for f in ('ui_scratch.c', 'ui_scratch.h'):
        install('core/ui/' + f, COMMON + f)
    for f in ('heard.c', 'heard.h', 'ui_heard.c', 'ui_heard.h', 'wfall.c', 'wfall.h', 'tripm.c', 'tripm.h',
              'ui_gpsdash.c', 'ui_gpsdash.h', 'clockx.c', 'clockx.h', 'ui_clock.c', 'ui_clock.h'):
        install('core/ui/' + f, TOOLBOX + f)

    # The switches. After SOS_KEY's lines (the end of the Plus switches): every switch these depend on is set by then
    transform('config/features.mk', [
        ('$(error SOS_KEY needs KEY_ASSIGN=y)\nendif\nendif\n',
         '$(error SOS_KEY needs KEY_ASSIGN=y)\nendif\nendif\n'
         'CONFIG_HEARD ?= n\nFEATURES += HEARD\n'
         'ifeq ($(CONFIG_HEARD),y)\nifneq ($(CONFIG_CALL_LOG),y)\n$(error HEARD needs CALL_LOG=y)\nendif\nendif\n'
         'CONFIG_SPECTRUM_WF ?= n\nFEATURES += SPECTRUM_WF\n'
         'ifeq ($(CONFIG_SPECTRUM_WF),y)\nifneq ($(CONFIG_SPECTRUM),y)\n$(error SPECTRUM_WF needs SPECTRUM=y)\nendif\n'
         'endif\n'
         'CONFIG_MAP_COVERAGE ?= n\nFEATURES += MAP_COVERAGE\n'
         'ifeq ($(CONFIG_MAP_COVERAGE),y)\nifneq ($(CONFIG_MAP_TRACK),y)\n$(error MAP_COVERAGE needs MAP_TRACK=y)\n'
         'endif\nendif\n'
         'CONFIG_GPS_DASH ?= n\nFEATURES += GPS_DASH\n'
         'ifeq ($(CONFIG_GPS_DASH),y)\nifneq ($(CONFIG_GPS),y)\n$(error GPS_DASH needs GPS=y)\nendif\nendif\n'
         'CONFIG_CLOCK_TOOLS ?= n\nFEATURES += CLOCK_TOOLS\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_RPT_PROV\n',
         '#ifndef CONFIG_HEARD\n#define CONFIG_HEARD 0\n#endif\n#ifndef CONFIG_SPECTRUM_WF\n#define CONFIG_SPECTRUM_WF 0\n'
         '#endif\n#ifndef CONFIG_MAP_COVERAGE\n#define CONFIG_MAP_COVERAGE 0\n#endif\n#ifndef CONFIG_GPS_DASH\n'
         '#define CONFIG_GPS_DASH 0\n#endif\n#ifndef CONFIG_CLOCK_TOOLS\n#define CONFIG_CLOCK_TOOLS 0\n#endif\n'
         '#ifndef CONFIG_RPT_PROV\n'),
    ])

    # --- The screens (appended), their table entries, the Tools entries after SOS key ---
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;',
         '\tSCR_HEARD, SCR_HEARD_ONE,\t// DDM32 Plus: Tools > Heard list, one station (CONFIG_HEARD)\n'
         '\tSCR_GPSDASH,\t\t\t// DDM32 Plus: Tools > GPS (CONFIG_GPS_DASH)\n'
         '\tSCR_CLOCK,\t\t\t// DDM32 Plus: Tools > Clock (CONFIG_CLOCK_TOOLS)\n'
         '\tSCR_COUNT\n} ui_scr_t;'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_locator_screen;\t// ui_locator.c (DDM32 Plus)',
         'extern const ui_scr_def_t ui_locator_screen;\t// ui_locator.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_heard_screen, ui_heard_one_screen;\t// ui_heard.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_gpsdash_screen;\t// ui_gpsdash.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_clock_screen;\t// ui_clock.c (DDM32 Plus)'),
        ('#if CONFIG_LOCATOR\n\t[SCR_LOCATOR] = &ui_locator_screen,\n#endif\n',
         '#if CONFIG_LOCATOR\n\t[SCR_LOCATOR] = &ui_locator_screen,\n#endif\n'
         '#if CONFIG_HEARD\n\t[SCR_HEARD] = &ui_heard_screen,\n\t[SCR_HEARD_ONE] = &ui_heard_one_screen,\n#endif\n'
         '#if CONFIG_GPS_DASH\n\t[SCR_GPSDASH] = &ui_gpsdash_screen,\n#endif\n'
         '#if CONFIG_CLOCK_TOOLS\n\t[SCR_CLOCK] = &ui_clock_screen,\n#endif\n'),
        # Tools > Heard list, GPS, Clock after SOS key (CW decoder and Battery stay the last two)
        ('T_SELCALL, T_SOS, T_CW, T_BATTERY,', 'T_SELCALL, T_SOS, T_HEARD, T_GPSDASH, T_CLOCK, T_CW, T_BATTERY,'),
        ('i != T_SOS &&',
         'i != T_SOS && (i != T_HEARD || CONFIG_HEARD) && (i != T_GPSDASH || CONFIG_GPS_DASH) && '
         '(i != T_CLOCK || CONFIG_CLOCK_TOOLS) &&'),
        ('TRN("SOS key"), TRN("CW decoder"),', 'TRN("SOS key"), TRN("Heard list"), "GPS", CONFIG_CLOCK_TOOLS ? TRN("Clock") : TRN("None"), '
         'TRN("CW decoder"),'),
        ('\tcase T_CW: ui_push(SCR_CW, 0); break;\n',
         '#if CONFIG_HEARD\n\tcase T_HEARD: ui_push(SCR_HEARD, 0); break;\n#endif\n'
         '#if CONFIG_GPS_DASH\n\tcase T_GPSDASH: ui_push(SCR_GPSDASH, 0); break;\n#endif\n'
         '#if CONFIG_CLOCK_TOOLS\n\tcase T_CLOCK: ui_push(SCR_CLOCK, 0); break;\n#endif\n'
         '\tcase T_CW: ui_push(SCR_CW, 0); break;\n'),
        ('\t\t[T_SOS] = ART_ICON_SOS + 1,\t// the warning triangle\n',
         '\t\t[T_SOS] = ART_ICON_SOS + 1,\t// the warning triangle\n'
         '\t\t[T_HEARD] = ART_ICON_RECENT + 1,\t// a list of calls, as Recent\n'
         '\t\t[T_GPSDASH] = ART_ICON_POSITION + 1, [T_CLOCK] = ART_ICON_ALMANAC + 1,\t// the GPS\'s; the sky\'s clock\n'),
    ])

    # --- The UI task's hooks ---
    transform('core/ui/ui_app.c', [
        ('#if CONFIG_SOS_KEY\n#include "ui/sos.h"\n#endif\n',
         '#if CONFIG_SOS_KEY\n#include "ui/sos.h"\n#endif\n'
         '#if CONFIG_HEARD\n#include "ui/ui_heard.h"\n#endif\n#if CONFIG_SPECTRUM_WF\n#include "ui/wfall.h"\n#endif\n'
         '#if CONFIG_GPS_DASH\n#include "ui/ui_gpsdash.h"\n#endif\n#if CONFIG_CLOCK_TOOLS\n#include "ui/ui_clock.h"\n'
         '#endif\n'),
        # Heard list: every call Recent takes in, and a DMR / P25 call going on (heard again)
        ('\trecent[0] = *r;\n#if CONFIG_QSO_LOG\n\tqso_log_shift(i, qso_dup);\n#endif\n',
         '\trecent[0] = *r;\n#if CONFIG_QSO_LOG\n\tqso_log_shift(i, qso_dup);\n#endif\n'
         '#if CONFIG_HEARD\n\tui_heard_recent(r, true);\t// DDM32 Plus heard list (ui_heard.c)\n#endif\n'),
        ('\t\t} else {\n\t\t\trecent[0].ms = d->last_ms;\n\t\t\trecent_gps(&recent[0], &d->last);\n\t\t}\n',
         '\t\t} else {\n\t\t\trecent[0].ms = d->last_ms;\n\t\t\trecent_gps(&recent[0], &d->last);\n'
         '#if CONFIG_HEARD\n\t\t\tui_heard_recent(&recent[0], false);\t// the same station\'s call goes on\n#endif\n'
         '\t\t}\n'),
        ('\t\t\t\t\trecent[i].ms = now;\n\t\t\t\t\tdirty = true;\n\t\t\t\t\tbreak;\n',
         '\t\t\t\t\trecent[i].ms = now;\n\t\t\t\t\tdirty = true;\n'
         '#if CONFIG_HEARD\n\t\t\t\t\tui_heard_recent(&recent[i], false);\t// the P25 call goes on\n#endif\n'
         '\t\t\t\t\tbreak;\n'),
        ('\trecent_update(v);\n',
         '#if CONFIG_HEARD\n\tui_heard_view(v);\t// DDM32 Plus: what the calls below are heard on\n#endif\n'
         '\trecent_update(v);\n'),
        ('#if CONFIG_QSO_LOG\n\tqso_log_reset();\n#endif\n',
         '#if CONFIG_QSO_LOG\n\tqso_log_reset();\n#endif\n#if CONFIG_HEARD\n\tui_heard_reset();\n#endif\n'),
        # The ticks: the waterfall's buffer back once its screen is left, the trip meter and the clock's GPS time
        ('\tif (ui_sos_tick(now))\n\t\tdirty = true;\n#endif\n',
         '\tif (ui_sos_tick(now))\n\t\tdirty = true;\n#endif\n'
         '#if CONFIG_SPECTRUM_WF\n\tui_spectrum_wf_tick(stack[sp].scr);\t// DDM32 Plus: the waterfall\'s heap buffer\n'
         '#endif\n'
         '#if CONFIG_GPS_DASH\n\t// DDM32 Plus Tools > GPS: the trip meter, once a second whatever is open (ui_gpsdash.c)\n'
         '\tif (ui_gpsdash_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n'
         '#if CONFIG_CLOCK_TOOLS\n\t// DDM32 Plus Tools > Clock: the GPS time once a second, the running stopwatch shown\n'
         '\tif (ui_clock_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n'),
        # The countdown's end and the alarm: a tone and a banner on any screen, as the satellite pass alarm
        ('\tif (ui_st.toast != NULL && (int32_t)(now - toast_until) >= 0) {\n',
         '#if CONFIG_CLOCK_TOOLS\n'
         '\t// DDM32 Plus Tools > Clock: the countdown\'s end or the alarm (ui_clock.c) has sounded; its text on any screen\n'
         '\tconst char *clock_alarm = ui_clock_alarm(now);\n'
         '\tif (clock_alarm != NULL) {\n'
         '\t\tif (ui_st.banner.hold)\n'
         '\t\t\tui_toast(clock_alarm);\t// the zone offer stays up: it waits for the user\n'
         '\t\telse\n'
         '\t\t\tbanner(UI_BANNER_INFO, false, 0, clock_alarm, NULL);\n'
         '\t\twake_req = true;\n'
         '\t\tdirty = true;\n'
         '\t}\n'
         '#endif\n'
         '\tif (ui_st.toast != NULL && (int32_t)(now - toast_until) >= 0) {\n'),
        # * is the waterfall's key on Tools > Spectrum and Coverage's on the map: no quick settings from there
        ('\t\t\t!(CONFIG_QSO_LOG && stack[sp].scr == SCR_RECENT)) {\n',
         '\t\t\t!(CONFIG_QSO_LOG && stack[sp].scr == SCR_RECENT) &&\n'
         '\t\t\t!(CONFIG_SPECTRUM_WF && stack[sp].scr == SCR_SPECTRUM) && !(CONFIG_MAP_COVERAGE && stack[sp].scr == SCR_MAP)) {\n'),
    ])
