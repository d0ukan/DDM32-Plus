#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus faster channel scan (CONFIG_SCAN_FAST, branch fastscan, v1.0.1 candidate; docs/plus.md "Faster channel
scan"). The owner's request of 2026-11-23: the zone scan / VFO scan as fast as reasonably possible (OpenDMR's channel
scan was named as fast; its licence forbids copying, so only ideas from the lab repo's review of it were used: a
reading every few ms from shortly after the retune, a quiet step as short as its readings allow).

  - a quiet stop of a zone or VFO scan moves on once the readings taken on the radio task's 10 ms tick say so (FM and
    the native modes three readings 20-40 ms after the retune; DMR a whole 60 ms TDMA frame); anything not clearly
    quiet keeps the classic (v1.0.0) dwell, probe and hold. Engine: integration/radiofoundry/scanfast/;
  - counters for the PC: the debug link section "scanst" (the last one, so every earlier section keeps its number);
  - the switch: held 0 on Home while a zone or VFO scan runs (fast / classic), kept in RAM.
Dual watch and the multi-watch ring keep their own timing. Called last from plus_features.prepare(); a module of its
own so the shared prepare list gains one line."""

SF = 'integration/radiofoundry/scanfast/'


def prepare_fastscan(transform, install):
    install('core/radio/scanfast.h', SF + 'scanfast.h')
    install('core/radio/scanfast.c', SF + 'scanfast.c')
    install('core/radio/radio_scanfast.inc', SF + 'radio_scanfast.inc')
    install('core/radio/scanfast_view.c', SF + 'scanfast_view.c')
    install('core/ui/ui_scanfast.c', SF + 'ui_scanfast.c')

    transform('config/features.mk', [
        ('$(error MWATCH_FAST needs MULTI_WATCH=y)\nendif\nendif\n',
         '$(error MWATCH_FAST needs MULTI_WATCH=y)\nendif\nendif\n'
         'CONFIG_SCAN_FAST ?= n\nFEATURES += SCAN_FAST\n'
         'ifeq ($(CONFIG_SCAN_FAST),y)\nifneq ($(CONFIG_SCAN),y)\n'
         '$(error SCAN_FAST needs SCAN=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_MWATCH_FAST\n',
         '#ifndef CONFIG_SCAN_FAST\n#define CONFIG_SCAN_FAST 0\t/* DDM32 Plus faster channel scan (tools/plus_fastscan.py) */\n'
         '#endif\n#ifndef CONFIG_MWATCH_FAST\n'),
    ])

    transform('core/radio/radio_sm.c', [
        # the engine's header, and the radio core's side declared for scan_start (defined in radio_scanfast.inc)
        ('#include "radio/srange.h"\t// DDM32 Plus: the search\'s step rides on its start\n#endif\n',
         '#include "radio/srange.h"\t// DDM32 Plus: the search\'s step rides on its start\n#endif\n'
         '#if CONFIG_SCAN_FAST\n#include "radio/scanfast.h"\t// DDM32 Plus faster channel scan\n'
         'static void fsc_begin(radio_sm_t *sm, uint32_t now_ms);\n#endif\n'),
        # a zone or VFO scan ends: its counters stop (dual watch's are not kept)
        ('\tuint8_t kind = sc->kind;\n\tif (kind == SCAN_DUAL) {\n\t\trx2_release(sm);\n',
         '\tuint8_t kind = sc->kind;\n'
         '#if CONFIG_SCAN_FAST\n\tif (kind != SCAN_DUAL)\n\t\tfs_end(now_ms);\t// DDM32 Plus faster scan: its counters\n#endif\n'
         '\tif (kind == SCAN_DUAL) {\n\t\trx2_release(sm);\n'),
        # ... and starts (scan_start, just before its first stop)
        ('\t\tif (next < 0 || next == sm->view.zone_pos) {\n\t\t\tscan_end(sc);\n\t\t\treturn;\n\t\t}\n\t}\n'
         '\tactivity(sm, now_ms);\n\tscan_step(sm, now_ms);\n}\n',
         '\t\tif (next < 0 || next == sm->view.zone_pos) {\n\t\t\tscan_end(sc);\n\t\t\treturn;\n\t\t}\n\t}\n'
         '#if CONFIG_SCAN_FAST\n\tfsc_begin(sm, now_ms);\t// DDM32 Plus faster scan\n#endif\n'
         '\tactivity(sm, now_ms);\n\tscan_step(sm, now_ms);\n}\n'),
        ('// Every poll while scanning: pause on the monitor, what the receiver hears, move on\nstatic void scan_service(',
         '#if CONFIG_SCAN_FAST\n#include "radio/radio_scanfast.inc"\t// DDM32 Plus faster channel scan\n#endif\n\n'
         '// Every poll while scanning: pause on the monitor, what the receiver hears, move on\nstatic void scan_service('),
        # the readings on the 10 ms tick
        ('void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms)\n{\n',
         'void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '#if CONFIG_SCAN_FAST\n\tfsc_tick(sm, now_ms);\t// DDM32 Plus faster scan: a quiet stop moves on here\n#endif\n'),
        # the poll's squelch doesn't open while the tick's retune is younger than a classic stop's first poll (only the
        # opening: VOX, roaming and the floor tracking run as always)
        ('\t\tscan_talked(&sm->scan, now_ms);\t// transmitting: the wait after it starts when it ends\n\t}\n#endif\n#endif\n',
         '\t\tscan_talked(&sm->scan, now_ms);\t// transmitting: the wait after it starts when it ends\n\t}\n#endif\n'
         '#if CONFIG_SCAN_FAST\n'
         '\tbool fsc_hush = fsc_guard(sm, now_ms);\t// DDM32 Plus faster scan: the squelch waits after the tick\'s retune\n'
         '#endif\n#endif\n'),
        ('\tbool opens = (listening || (sm->view.tone_ok && (sq == 0 || st.noise < open_below))) && !dual_peeking(sm);\n',
         '\tbool opens = (listening || (sm->view.tone_ok && (sq == 0 || st.noise < open_below))) && !dual_peeking(sm);\n'
         '#if CONFIG_SCAN_FAST\n'
         '\topens = opens && !fsc_hush;\t// DDM32 Plus faster scan: not on a reading right after the tick\'s retune\n'
         '#endif\n'),
    ])

    # The switch: held 0 on Home while a zone or VFO scan runs (a short 0 still skips, ui_scanx.c)
    transform('core/ui/ui_nav.h', [
        ('bool ui_scx_home_key(const input_event_t *ev);',
         'bool ui_fscan_home_key(const input_event_t *ev);\t// DDM32 Plus: held 0 while scanning: fast / classic\n'
         'bool ui_scx_home_key(const input_event_t *ev);'),
    ])
    transform('core/ui/ui_app.c', [
        ('#if CONFIG_SCAN_PRIO\n\tif (ui_scx_home_key(ev))',
         '#if CONFIG_SCAN_FAST\n\tif (ui_fscan_home_key(ev))\t// DDM32 Plus faster scan: held 0 switches it\n\t\treturn true;\n#endif\n'
         '#if CONFIG_SCAN_PRIO\n\tif (ui_scx_home_key(ev))'),
    ])

    # The PC: a section "scanst" (31), after alls (each earlier keeps its number)
    transform('core/debuglink/debuglink.h', [
        ('\tDL_VIEW_ALLS,\t\t// DDM32 Plus: the all digital search\'s state and found list (searchlog_view.c)\n',
         '\tDL_VIEW_ALLS,\t\t// DDM32 Plus: the all digital search\'s state and found list (searchlog_view.c)\n'
         '\tDL_VIEW_SCANST,\t\t// DDM32 Plus: the zone / VFO scan\'s counters (scanfast_view.c)\n'),
    ])
    transform('core/debuglink/debuglink_view.c', [
        ('"net_link", "cps", "p25s", "dmrs", "alls" };\n', '"net_link", "cps", "p25s", "dmrs", "alls", "scanst" };\n'),
        ('\tcase DL_VIEW_ALLS:\n\t\tsearchlog_view_all(t);\t// DDM32 Plus all digital search\n\t\tbreak;\n',
         '\tcase DL_VIEW_ALLS:\n\t\tsearchlog_view_all(t);\t// DDM32 Plus all digital search\n\t\tbreak;\n'
         '\tcase DL_VIEW_SCANST: {\n'
         '\t\tvoid scanfast_view(dl_text_t *t);\t// DDM32 Plus faster channel scan (scanfast_view.c)\n'
         '\t\tscanfast_view(t);\n\t\tbreak;\n\t}\n'),
    ])
