#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus all digital search (CONFIG_ALL_SEARCH, docs/plus.md "All digital search"; owner 2026-11-21): Tools > All
digital search sweeps a range like the P25 and DMR searches and probes a hit in the owner's priority order P25 > DMR >
YSF > NXDN; the first confirmed mode wins. Receive only.

It is the DMR search's engine with a second kind (spectrum/dmrsearch.{c,h}: sweep, skip list, revisits, found list,
call, hang and resume shared; the two kinds share its RAM and starting the other kind clears the list) and the DMR
search's screen with a flag (spectrum/ui_dmrsearch.c). Each mode is probed by the receiver it already has in this
tree: P25 and YSF by the native C4FM receiver (native/native_lab.c, the P25 search's take and give back), DMR in the
DMR board mode with the call stack hearing everything (the DMR search's), NXDN on the FM path by the software 4FSK
modem (radio_nxdn.c). The radio core's side is spectrum/radio_alls.inc. The PC's search log gains a third debug link
section "alls" (30, searchlog_view.c).

Called last from plus_features.prepare() (after plus_scan and plus_searchlog, whose text it extends); a module of its
own so the shared prepare list gains one line. Upstream files change only through the hunks below."""

ON = '#if CONFIG_ALL_SEARCH\n'


def prepare_alldigital(transform, install):
    install('core/radio/radio_alls.inc', 'integration/radiofoundry/spectrum/radio_alls.inc')
    transform('config/features.mk', [
        ('$(error SEARCH_FAST needs P25_SEARCH=y or DMR_SEARCH=y)\nendif\nendif\n',
         '$(error SEARCH_FAST needs P25_SEARCH=y or DMR_SEARCH=y)\nendif\nendif\n'
         'CONFIG_ALL_SEARCH ?= n\nFEATURES += ALL_SEARCH\n'
         'ifeq ($(CONFIG_ALL_SEARCH),y)\n'
         'ifneq ($(CONFIG_DMR_SEARCH)$(CONFIG_P25_SEARCH)$(CONFIG_P25_SCAN)$(CONFIG_P25_CALLER),yyyy)\n'
         '$(error ALL_SEARCH needs DMR_SEARCH=y, P25_SEARCH=y, P25_SCAN=y and P25_CALLER=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DMR_SEARCH\n#define CONFIG_DMR_SEARCH 0\n#endif\n',
         '#ifndef CONFIG_DMR_SEARCH\n#define CONFIG_DMR_SEARCH 0\n#endif\n'
         '#ifndef CONFIG_ALL_SEARCH\n#define CONFIG_ALL_SEARCH 0\t/* DDM32 Plus all digital search (tools/plus_alldigital.py) */\n'
         '#endif\n'),
    ])

    # --- The request ---
    transform('core/radio/radio_sm.h', [
        ('\tRREQ_DMR_SEARCH,\t// DM32 Plus: arg low Hz (0 = stop), arg2 high Hz (radio/dmrsearch.h)\n',
         '\tRREQ_DMR_SEARCH,\t// DM32 Plus: arg low Hz (0 = stop), arg2 high Hz (radio/dmrsearch.h)\n'
         '\tRREQ_ALL_SEARCH,\t// DDM32 Plus: the all digital search, as RREQ_DMR_SEARCH (radio/dmrsearch.h)\n'),
    ])
    transform('core/radio/radio_task.c', [
        ('{ post_pair(RREQ_DMR_SEARCH, (int32_t)lo_hz, hi_hz, 0); }\n#endif\n',
         '{ post_pair(RREQ_DMR_SEARCH, (int32_t)lo_hz, hi_hz, 0); }\n#endif\n'
         + ON + 'void radio_req_all_search(uint32_t lo_hz, uint32_t hi_hz)\n'
         '{ post_pair(RREQ_ALL_SEARCH, (int32_t)lo_hz, hi_hz, 0); }\n#endif\n'),
    ])

    # --- The radio core ---
    transform('core/radio/radio_sm.c', [
        # Hole hunt B3: no search over a dual / multi-watch or the air watch. Their requests stopped the ring with
        # scan_stop(false) and left sm->air set: the UI then ended the air watch (RREQ_AIR_END), which ended the new
        # search too, or nothing was noted (persist) until a power cycle. Refused; the screens say "Stop the scan or
        # watch first" (ui_dmrsearch.c, ui_p25search.c). A plain scan is still stopped by a search, as before
        ('#include "radio/radio_sm.h"\n',
         '#include "radio/radio_sm.h"\n'
         '#if CONFIG_AIR_WATCH\n'
         '#define SEARCH_WATCH_ON(sm) ((sm)->air || (scan_on(&(sm)->scan) && (sm)->scan.kind == SCAN_DUAL))\n'
         '#else\n#define SEARCH_WATCH_ON(sm) (scan_on(&(sm)->scan) && (sm)->scan.kind == SCAN_DUAL)\n#endif\n'),
        ('\t\tif (busy || sm->ptt || aprs_busy || board.dmr == NULL)\n\t\t\tbreak;\n',
         '\t\tif (busy || sm->ptt || aprs_busy || board.dmr == NULL || SEARCH_WATCH_ON(sm))\n\t\t\tbreak;\n'),
        ('\t\t\t\tp25s_stop();\n\t\t\t\ttune(sm, now_ms);\n\t\t\t}\n\t\t\tbreak;\n\t\t}\n\t\tbool aprs_busy = false;\n'
         '#if CONFIG_APRS\n\t\taprs_busy = sm->aprs_phase != APRS_RF_OFF;\n#endif\n\t\tif (busy || sm->ptt || aprs_busy)\n',
         '\t\t\t\tp25s_stop();\n\t\t\t\ttune(sm, now_ms);\n\t\t\t}\n\t\t\tbreak;\n\t\t}\n\t\tbool aprs_busy = false;\n'
         '#if CONFIG_APRS\n\t\taprs_busy = sm->aprs_phase != APRS_RF_OFF;\n#endif\n'
         '\t\tif (busy || sm->ptt || aprs_busy || SEARCH_WATCH_ON(sm))\n'),
        # its side, before the DMR search's poll, which hands this kind over
        ("// Every poll while searching: the sweep's reading, or what the probe heard; retune when the engine moves on\n"
         'static void __attribute__((noinline)) dmrs_service(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '\tstatic uint32_t shown_ms;\n\tuint32_t hz;\n',
         ON + '#include "radio/radio_alls.inc"\t// DDM32 Plus all digital search: the radio core\'s side\n#endif\n\n'
         "// Every poll while searching: the sweep's reading, or what the probe heard; retune when the engine moves on\n"
         'static void __attribute__((noinline)) dmrs_service(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '\tstatic uint32_t shown_ms;\n\tuint32_t hz;\n'
         + ON + '\tif (dmrs_kind() == DMRS_KIND_ALL) {\n\t\talls_service(sm, now_ms, &shown_ms);\n\t\treturn;\n\t}\n'
         '#endif\n'),
        # the search's own request doesn't end it; the other kind's does
        ('\tif (!alert && req->type != RREQ_DMR_SEARCH && req->type != RREQ_SETTINGS && req->type != RREQ_DMR_FOCUS &&\n'
         '\t    dmrs_on()) {\n',
         ON + '\tif (!alert && !dmrs_own_request(req) && req->type != RREQ_SETTINGS && req->type != RREQ_DMR_FOCUS &&\n'
         '\t    dmrs_on()) {\n#else\n'
         '\tif (!alert && req->type != RREQ_DMR_SEARCH && req->type != RREQ_SETTINGS && req->type != RREQ_DMR_FOCUS &&\n'
         '\t    dmrs_on()) {\n#endif\n'),
        ('#if CONFIG_DMR_SEARCH\n\tcase RREQ_DMR_SEARCH: {',
         ON + '\tcase RREQ_ALL_SEARCH:\n\t\talls_request(sm, req, busy, now_ms);\n\t\tbreak;\n#endif\n'
         '#if CONFIG_DMR_SEARCH\n\tcase RREQ_DMR_SEARCH: {'),
        # the FM path's listeners keep off while it has the receiver; NXDN listens for its probe
        ('#if CONFIG_P25_SEARCH\n\t\tfmid = fmid && !p25s_on();\n#endif\n',
         '#if CONFIG_P25_SEARCH\n\t\tfmid = fmid && !p25s_on();\n#endif\n' + ON + '\t\tfmid = fmid && !dmrs_on();\n#endif\n'),
        ('#if CONFIG_P25_SEARCH\n\t\tmsg = msg && !p25s_on();\n#endif\n',
         '#if CONFIG_P25_SEARCH\n\t\tmsg = msg && !p25s_on();\n#endif\n' + ON + '\t\tmsg = msg && !dmrs_on();\n#endif\n'),
        ('#if CONFIG_P25_SEARCH\n\t\tdpmr = dpmr && !p25s_on();\n#endif\n',
         '#if CONFIG_P25_SEARCH\n\t\tdpmr = dpmr && !p25s_on();\n#endif\n' + ON + '\t\tdpmr = dpmr && !dmrs_on();\n#endif\n'),
        ('\t\tradio_nxdn_poll(nxdn, sm->view.ch.rx_hz, (uint8_t)sm->view.ch.p25_tg, now_ms);\n',
         ON + '\t\t// DDM32 Plus all digital search: its NXDN probe, any RAN (the channel\'s own listening waits)\n'
         '\t\tif (dmrs_on()) {\n\t\t\tuint32_t probe_hz = dmrs_nxdn_hz();\n'
         '\t\t\tradio_nxdn_poll(probe_hz != 0 && sm->board_mode == RADIO_MODE_FM, probe_hz, 0, now_ms);\n'
         '\t\t} else\n#endif\n'
         '\t\tradio_nxdn_poll(nxdn, sm->view.ch.rx_hz, (uint8_t)sm->view.ch.p25_tg, now_ms);\n'),
    ])

    # --- Tools > All digital search (the DMR search's screen with a flag, ui_dmrsearch.c), after CW decoder ---
    transform('core/ui/ui_nav.h', [
        ('\tSCR_XBAND, SCR_XBAND_PICK,\t// DDM32 Plus: Tools > Cross-band, its picker (CONFIG_XBAND)\n',
         '\tSCR_XBAND, SCR_XBAND_PICK,\t// DDM32 Plus: Tools > Cross-band, its picker (CONFIG_XBAND)\n'
         '\tSCR_ALLSEARCH,\t\t\t// DDM32 Plus: Tools > All digital search (CONFIG_ALL_SEARCH)\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_xband_screen, ui_xband_pick_screen;\t// ui_xband.c (DDM32 Plus)',
         'extern const ui_scr_def_t ui_xband_screen, ui_xband_pick_screen;\t// ui_xband.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_allsearch_screen;\t// ui_dmrsearch.c (DDM32 Plus)'),
        ('\t[SCR_XBAND_PICK] = &ui_xband_pick_screen,\n#endif\n',
         '\t[SCR_XBAND_PICK] = &ui_xband_pick_screen,\n#endif\n'
         + ON + '\t[SCR_ALLSEARCH] = &ui_allsearch_screen,\n#endif\n'),
        ('T_XBAND, T_CW, T_BATTERY,', 'T_XBAND, T_CW, T_ALLSEARCH, T_BATTERY,'),
        ('(i != T_CW || CONFIG_CW_RX) &&', '(i != T_CW || CONFIG_CW_RX) && (i != T_ALLSEARCH || CONFIG_ALL_SEARCH) &&'),
        ('TRN("CW decoder"), TRN("Battery"),', 'TRN("CW decoder"), TRN("All digital search"), TRN("Battery"),'),
        ('\tcase T_CW: ui_push(SCR_CW, 0); break;\n',
         '\tcase T_CW: ui_push(SCR_CW, 0); break;\n\tcase T_ALLSEARCH: ui_push(SCR_ALLSEARCH, 0); break;\n'),
        ('\t\t[T_CW] = ART_ICON_BEACON + 1,', '\t\t[T_ALLSEARCH] = ART_ICON_P25SEARCH + 1,\t// the searches\' magnifier\n'
         '\t\t[T_CW] = ART_ICON_BEACON + 1,'),
    ])
    # * opens the range editor there too (as on the two other search screens)
    transform('core/ui/ui_app.c', [
        ('(stack[sp].scr == SCR_P25SEARCH || stack[sp].scr == SCR_DMRSEARCH)',
         '(stack[sp].scr == SCR_P25SEARCH || stack[sp].scr == SCR_DMRSEARCH ||\n'
         '\t\t\t\t\t\t  stack[sp].scr == SCR_ALLSEARCH)'),
    ])

    # --- The PC's search log: a third section "alls" (30), after p25s and dmrs (each earlier keeps its number) ---
    transform('core/debuglink/debuglink.h', [
        ('\tDL_VIEW_DMRS,\t\t// DDM32 Plus: the DMR search\'s state and found list (searchlog_view.c)\n',
         '\tDL_VIEW_DMRS,\t\t// DDM32 Plus: the DMR search\'s state and found list (searchlog_view.c)\n'
         '\tDL_VIEW_ALLS,\t\t// DDM32 Plus: the all digital search\'s state and found list (searchlog_view.c)\n'),
    ])
    transform('core/debuglink/debuglink_view.c', [
        ('"net_link", "cps", "p25s", "dmrs" };\n', '"net_link", "cps", "p25s", "dmrs", "alls" };\n'),
        ('\tcase DL_VIEW_DMRS:\n\t\tsearchlog_view_dmr(t);\n\t\tbreak;\n',
         '\tcase DL_VIEW_DMRS:\n\t\tsearchlog_view_dmr(t);\n\t\tbreak;\n'
         '\tcase DL_VIEW_ALLS:\n\t\tsearchlog_view_all(t);\t// DDM32 Plus all digital search\n\t\tbreak;\n'),
    ])
