#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 15 "scan" job (docs/plus.md "Search ranges and speed"): the owner's requests after using
v1.0.0-beta.3. Called last from plus_features.prepare(); a module of its own so the shared prepare list gains one line.
Our code is in integration/radiofoundry/srange/ (installed into core/radio and core/ui), plus changes to our own
screens and engines (spectrum/ui_p25search.c, ui_dmrsearch.c, p25search.*, dmrsearch.*); upstream files change only
through the hunks below.

  SEARCH_RANGE  (item 1) Tools > P25 search and DMR search: a manual range typed in an editor (start, end, a step of
                2.5 kHz to 1 MHz, the range moved by a step or 1 MHz), remembered with the step while the radio is on.
                The step rides on the search request's start frequency (radio/srange.h). Needs P25_SEARCH or
                DMR_SEARCH.
  SEARCH_FAST   (item 2) the searches step faster: a step's first reading at the poll after the retune gates it (a
                quiet P25 step one poll instead of two, a DMR one three instead of four); a strong first reading is
                read again settled, and when it turns out quiet the step before is looked at again (a slow reading);
                a carrier found again not to be P25 / DMR is skipped for 30 s instead of 10. In the engines
                (spectrum/p25search.c, dmrsearch.c). Needs P25_SEARCH or DMR_SEARCH.
  MWATCH_FAST   (item 3) the multi-watch ring of 3..6 channels goes round faster: a stop with nothing heard ends at the
                first reading after the settle (DMR: the third), a P25 channel on it is found by its strength too and
                given back by the zone scan's rules (no frame: 1.2 s; in its cooldown passed over). Dual watch's pair
                keeps upstream's timing. Needs MULTI_WATCH.
  SEARCH_SURE   (searchfix, docs/plus.md "Search reliability") the P25, DMR and all digital searches catch a real
                signal surely: the floor learned from the first step and kept down (it never deadlocks above the
                noise), a strong step followed to the middle of its run before it is probed (the peak of a radio
                close by, a plateau, a half step), the carrier's skirt not probed again, a direct mode carrier held
                1.1 s (DMR) and P25 1.2 s, a carrier that went off the air during its probe never skipped, the
                steps a second on the search screen. The radio core hands the engines the level while they probe.
                In the engines (spectrum/p25search.c, dmrsearch.c, searchsure.h). Needs SEARCH_FAST.
  SEARCH_HYBRID (branch fastsrch, docs/plus.md "Faster searches (beta.9 candidate)") counters the PC reads (the X.st
                line of the search log sections) and the "fast" algorithm, chosen on the search screens (held 0;
                "classic" is beta.8's): the DMR and all digital sweeps read a quiet step in two polls from samples on
                the radio task's 10 ms tick; carriers left undecided are read again between steps; a steady carrier
                not digital twice in a row is skipped for minutes; a DMR probe of a steady carrier ends after 0.4 s.
                In spectrum/searchhy.* and the engines; the tick hook below. Needs SEARCH_SURE.
"""

SRANGE = 'integration/radiofoundry/srange/'


def prepare_scan(transform, install):
    install('core/radio/srange.h', SRANGE + 'srange.h')
    install('core/radio/searchsure.h', 'integration/radiofoundry/spectrum/searchsure.h')
    install('core/radio/searchhy.h', 'integration/radiofoundry/spectrum/searchhy.h')
    install('core/radio/searchhy.c', 'integration/radiofoundry/spectrum/searchhy.c')
    for f in ('ui_srange.c', 'ui_srange.h'):
        install('core/ui/' + f, SRANGE + f)

    # The switch, after DMR_SEARCH's own lines (both searches' switches are set by then)
    transform('config/features.mk', [
        ('$(error DMR_SEARCH needs DMR=y and DMR_ANY=y)\nendif\nendif\n',
         '$(error DMR_SEARCH needs DMR=y and DMR_ANY=y)\nendif\nendif\n'
         'CONFIG_SEARCH_RANGE ?= n\nFEATURES += SEARCH_RANGE\n'
         'ifeq ($(CONFIG_SEARCH_RANGE),y)\nifeq ($(CONFIG_P25_SEARCH)$(CONFIG_DMR_SEARCH),nn)\n'
         '$(error SEARCH_RANGE needs P25_SEARCH=y or DMR_SEARCH=y)\nendif\nendif\n'
         'CONFIG_SEARCH_FAST ?= n\nFEATURES += SEARCH_FAST\n'
         'ifeq ($(CONFIG_SEARCH_FAST),y)\nifeq ($(CONFIG_P25_SEARCH)$(CONFIG_DMR_SEARCH),nn)\n'
         '$(error SEARCH_FAST needs P25_SEARCH=y or DMR_SEARCH=y)\nendif\nendif\n'
         'CONFIG_SEARCH_SURE ?= n\nFEATURES += SEARCH_SURE\n'
         'ifeq ($(CONFIG_SEARCH_SURE),y)\nifneq ($(CONFIG_SEARCH_FAST),y)\n'
         '$(error SEARCH_SURE needs SEARCH_FAST=y)\nendif\nendif\n'
         'CONFIG_SEARCH_HYBRID ?= n\nFEATURES += SEARCH_HYBRID\n'
         'ifeq ($(CONFIG_SEARCH_HYBRID),y)\nifneq ($(CONFIG_SEARCH_SURE),y)\n'
         '$(error SEARCH_HYBRID needs SEARCH_SURE=y)\nendif\nendif\n'),
        # after MULTI_WATCH's own lines
        ('$(error MULTI_WATCH needs DUAL_WATCH=y)\nendif\nendif\n',
         '$(error MULTI_WATCH needs DUAL_WATCH=y)\nendif\nendif\n'
         'CONFIG_MWATCH_FAST ?= n\nFEATURES += MWATCH_FAST\n'
         'ifeq ($(CONFIG_MWATCH_FAST),y)\nifneq ($(CONFIG_MULTI_WATCH),y)\n'
         '$(error MWATCH_FAST needs MULTI_WATCH=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DMR_SEARCH\n#define CONFIG_DMR_SEARCH 0\n#endif\n',
         '#ifndef CONFIG_DMR_SEARCH\n#define CONFIG_DMR_SEARCH 0\n#endif\n'
         '#ifndef CONFIG_SEARCH_RANGE\n#define CONFIG_SEARCH_RANGE 0\t/* DDM32 Plus search ranges (tools/plus_scan.py) */\n'
         '#endif\n#ifndef CONFIG_SEARCH_FAST\n#define CONFIG_SEARCH_FAST 0\t/* DDM32 Plus faster searches (tools/plus_scan.py) */\n'
         '#endif\n#ifndef CONFIG_MWATCH_FAST\n#define CONFIG_MWATCH_FAST 0\t/* DDM32 Plus faster ring (tools/plus_scan.py) */\n'
         '#endif\n#ifndef CONFIG_SEARCH_SURE\n#define CONFIG_SEARCH_SURE 0\t/* DDM32 Plus surer searches (tools/plus_scan.py) */\n'
         '#endif\n#ifndef CONFIG_SEARCH_HYBRID\n#define CONFIG_SEARCH_HYBRID 0\t/* DDM32 Plus faster searches, beta.9 (tools/plus_scan.py) */\n'
         '#endif\n'),
    ])

    # SEARCH_SURE: the engines get the level while they probe (a carrier that went off the air is not skipped; a direct
    # mode DMR carrier's rise and fall holds the probe longer). The DMR search's probe reads it with what the stack
    # heard; the P25 search's while the native receiver has the frequency (the radio core reads the status on every
    # poll for a P25 channel's receive too)
    transform('core/radio/radio_sm.c', [
        ('\t\tdmrs_heard_t h;\n\t\tdmrs_heard_read(&h);\n\t\thz = dmrs_probe(&h, now_ms);\n',
         '\t\tdmrs_heard_t h;\n\t\tdmrs_heard_read(&h);\n'
         '#if CONFIG_SEARCH_SURE\n'
         '\t\tradio_status_t st;\n\t\tboard.radio->status(&st);\n'
         '\t\th.rssi_dbm = st.rssi_dbm;\t// DDM32 Plus: the level while probing (radio/searchsure.h)\n'
         '#endif\n'
         '\t\thz = dmrs_probe(&h, now_ms);\n'),
        ('\t\tif (now_ms - p25s_shown_ms >= 250u) {\n',
         '#if CONFIG_SEARCH_SURE\n'
         '\t\telse if (p25s_phase() == P25S_PROBE) {\n'
         '\t\t\tboard.radio->status(&st);\t// DDM32 Plus: the level while the native receiver probes\n'
         '\t\t\tp25s_level(st.rssi_dbm, now_ms);\n'
         '\t\t}\n'
         '#endif\n'
         '\t\tif (now_ms - p25s_shown_ms >= 250u) {\n'),
    ])

    # SEARCH_HYBRID: the DMR and all digital sweeps' samples on the 10 ms tick (radio/dmrsearch.h DMRS_SAMPLE_MS); only
    # while a fast search's step waits for them, so nothing else reads the FD6818 more often
    transform('core/radio/radio_sm.c', [
        ('void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms)\n{\n',
         'void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '#if CONFIG_SEARCH_HYBRID && CONFIG_DMR_SEARCH\n'
         '\tif (dmrs_sampling(now_ms)) {\n'
         '\t\tradio_status_t st;\t// DDM32 Plus faster search: the sweep step\'s level, sampled every tick\n'
         '\t\tboard.radio->status(&st);\n'
         '\t\tdmrs_sample(st.rssi_dbm, now_ms);\n'
         '\t}\n'
         '#endif\n'),
    ])

    # Item 1: * on the two search screens opens their range editor, not quick settings
    transform('core/ui/ui_app.c', [
        ('!(CONFIG_MAP_COVERAGE && stack[sp].scr == SCR_MAP)) {',
         '!(CONFIG_MAP_COVERAGE && stack[sp].scr == SCR_MAP) &&\n'
         '\t\t\t!(CONFIG_SEARCH_RANGE && (stack[sp].scr == SCR_P25SEARCH || stack[sp].scr == SCR_DMRSEARCH))) {'),
    ])
    # Item 1: the searches sweep with the step the request's start names (radio/srange.h); untagged: 12.5 kHz
    transform('core/radio/radio_sm.c', [
        ('#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n',
         '#if CONFIG_DMR_SEARCH\n#include "radio/dmrsearch.h"\n#endif\n'
         '#if CONFIG_SEARCH_RANGE\n#include "radio/srange.h"\t// DDM32 Plus: the search\'s step rides on its start\n#endif\n'),
        ('\t\tboard.radio->rx(p25s_start((uint32_t)req->arg, req->arg2, 12500u, now_ms), RADIO_BW_12K5, &search_no_tone);\n',
         '#if CONFIG_SEARCH_RANGE\n'
         '\t\tboard.radio->rx(p25s_start(srange_lo_of((uint32_t)req->arg), req->arg2,\n'
         '\t\t\t\t\t   srange_step_hz(srange_tag_of((uint32_t)req->arg)), now_ms), RADIO_BW_12K5, &search_no_tone);\n'
         '#else\n'
         '\t\tboard.radio->rx(p25s_start((uint32_t)req->arg, req->arg2, 12500u, now_ms), RADIO_BW_12K5, &search_no_tone);\n'
         '#endif\n'),
        ('\t\tuint32_t hz = dmrs_start((uint32_t)req->arg, req->arg2, 12500u, now_ms);\n',
         '#if CONFIG_SEARCH_RANGE\n'
         '\t\tuint32_t hz = dmrs_start(srange_lo_of((uint32_t)req->arg), req->arg2,\n'
         '\t\t\t\t\t  srange_step_hz(srange_tag_of((uint32_t)req->arg)), now_ms);\n'
         '#else\n'
         '\t\tuint32_t hz = dmrs_start((uint32_t)req->arg, req->arg2, 12500u, now_ms);\n'
         '#endif\n'),
    ])

    # Item 3: the faster multi-watch ring (3..6 channels; dual watch's pair keeps upstream's timing). A stop with
    # nothing heard ends at the first reading after the settle (DMR: the third); anything heard keeps the dwell, the
    # probe and the hold. P25 on the ring: its carrier by the strength too and the zone scan's give back and cooldown
    # pass-over (native_lab.c scan_by_frames)
    transform('core/radio/radio_sm.c', [
        ('// Every poll while scanning: pause on the monitor, what the receiver hears, move on\nstatic void scan_service(',
         '#if CONFIG_MWATCH_FAST && CONFIG_MULTI_WATCH\n'
         '// DDM32 Plus faster ring (docs/plus.md "The multi-watch ring"): a ring of 3..6 channels, not dual watch\'s pair\n'
         '#define MW_FAST(sm) ((sm)->scan.kind == SCAN_DUAL && dual_count(sm) > 2)\n'
         '_Static_assert(RADIO_MW_SETTLE_MS == SCAN_SETTLE_MS, "radio_mwatch.h: the ring estimate\'s settle is the scan\'s");\n'
         '#else\n#define MW_FAST(sm) false\n#endif\n\n'
         '// Every poll while scanning: pause on the monitor, what the receiver hears, move on\nstatic void scan_service('),
        ('\tif (sc->kind == SCAN_ZONE && codeplug_mode_p25(sm->view.ch.mode) && now_ms - sc->tuned_ms >= SCAN_SETTLE_MS) {\n',
         '\tif ((sc->kind == SCAN_ZONE || MW_FAST(sm)) && codeplug_mode_p25(sm->view.ch.mode) &&\n'
         '\t    now_ms - sc->tuned_ms >= SCAN_SETTLE_MS) {\n'),
        ('\t\tif (native_lab_scan_stop(sm->view.zone_pos, (uint8_t)sc->steps, carrier || strong)) {\n',
         '\t\tif (native_lab_scan_stop(native_lab_scan_key(sm), (uint8_t)sc->steps, carrier || strong)) {\n'),
        ('\tuint8_t was = sc->phase;\n\tswitch (scan_poll(sc, now_ms, active, carrier, sync)) {\n',
         '\tuint8_t was = sc->phase;\n'
         '#if CONFIG_MWATCH_FAST && CONFIG_MULTI_WATCH\n'
         '\t// The ring: nothing heard on this stop since the settle (no carrier, no sync, no signal to hold): its dwell is\n'
         '\t// the first reading after the settle, a DMR stop\'s the third (radio_mwatch.h radio_mw_quiet_ms)\n'
         '\tuint16_t dwell = sc->cfg.dwell_ms;\n'
         '\tif (MW_FAST(sm) && sc->phase == SCAN_PH_LISTEN && !sc->heard && !carrier && !sync && !active) {\n'
         '\t\tuint32_t quiet = radio_mw_quiet_ms(dual_count(sm), dmr);\n'
         '\t\tif (quiet < dwell)\n'
         '\t\t\tsc->cfg.dwell_ms = (uint16_t)quiet;\n'
         '\t}\n'
         '\tscan_act_t act = scan_poll(sc, now_ms, active, carrier, sync);\n'
         '\tsc->cfg.dwell_ms = dwell;\n'
         '\tswitch (act) {\n'
         '#else\n'
         '\tswitch (scan_poll(sc, now_ms, active, carrier, sync)) {\n'
         '#endif\n'),
    ])
