#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus transmit safety, second round (docs/plus.md "Pre-release hole hunt: transmit safety"). Called from
plus_features.prepare() after plus_sstvtx; a module of its own so the shared prepare list gains one line. Runs last on
purpose: it anchors on text the earlier Plus hunks made.

- radio_moving(): one rule for every feature that keys by itself (SSTV send, the CW beacon): never while a scan, a
  dual / multi-watch, the air watch, a search, the spectrum or trunk following moves the receiver.
- CW beacon: refused while radio_moving(), each time it keys (no band plan: owner decision 2026-11-22, only DOTA
  refuses frequencies outside the amateur bands).
- SFR repeater role with its CW ID on and no callsign as the radio name: not started (no unidentified repeating);
  the role page says why (radio_dmr_view_t.sfr_nocall)."""

def prepare_txsafe(transform, install):
    # Upstream's own expectation changes on purpose: a repeater role with its CW ID on and no callsign is not started
    # (it repeated unidentified, counting the ID as skipped)
    transform('tests/host/radioCoreTest.c', [
        ('\t// No callsign in the radio name (a personal name): nothing keyed, counted as skipped, the role goes on\n'
         '\tstrcpy(sm.radio_name, "Base 1");\n\tchans[4].sfr_flag1 = SFR_CHANNEL_FLAG1_ENABLED;\n\treload_channel();\n'
         '\tCHECK(sm.sfr_rpt);\n\tsfr_call_through();\n\trun(6000);\n'
         '\tCHECK(sm.cw_phase == 0 && sm.view.dmr.sfr_cwid_cut == 2 && sm.sfr_rpt && hw_mode == RADIO_MODE_DMR);\n',
         '\t// DDM32 Plus: no callsign in the radio name (a personal name): the role is not started (never repeats\n'
         '\t// unidentified), the page says why; an ordinary DMR channel, nothing keyed\n'
         '\tstrcpy(sm.radio_name, "Base 1");\n\tchans[4].sfr_flag1 = SFR_CHANNEL_FLAG1_ENABLED;\n\treload_channel();\n'
         '\tCHECK(!sm.sfr_rpt && sm.view.dmr.sfr_nocall);\n\trun(6000);\n'
         '\tCHECK(sm.cw_phase == 0 && sm.view.dmr.sfr_cwid_cut == 1 && !sm.sfr_rpt && hw_mode == RADIO_MODE_DMR);\n'),
    ])
    transform('core/radio/radio.h', [
        ('\tbool sfr_cw;\t\t// the repeater role keys its CW ID now (CONFIG_SFR_CWID, sfr.md 4, CW ID)\n',
         '\tbool sfr_cw;\t\t// the repeater role keys its CW ID now (CONFIG_SFR_CWID, sfr.md 4, CW ID)\n'
         '\tbool sfr_nocall;\t// DDM32 Plus: the role wasn\'t started: its CW ID is on and the radio name is no callsign\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('!d->sfr_rpt ? TR("Off: transmit not allowed now") :',
         '!d->sfr_rpt ? (d->sfr_nocall ? TR("Set your callsign (the radio name) in qdmr.") :\n'
         '\t\t\t\tTR("Off: transmit not allowed now")) :'),
    ])
    transform('core/radio/radio_sm.c', [
        # One rule for every feature that keys by itself
        ("#if CONFIG_CW_BEACON\n// Key the beacon's message now, or refuse",
         '#if CONFIG_CW_BEACON || CONFIG_SSTV_TX\n'
         '// DDM32 Plus (hole hunt A1 / B7): something moves the receiver by itself: a scan, a dual / multi-watch or the air\n'
         '// watch (the ring would key a letter nobody checked), a search, the spectrum, trunk following (their polls run\n'
         '// first and retune). A feature that keys by itself (SSTV send, the CW beacon) refuses then\n'
         'static bool radio_moving(const radio_sm_t *sm)\n{\n\tbool on = false;\n'
         '#if CONFIG_SCAN\n\ton = scan_on(&sm->scan);\n#endif\n'
         '#if CONFIG_AIR_WATCH\n\ton = on || sm->air;\n#endif\n'
         '#if CONFIG_DMR_SEARCH\n\ton = on || dmrs_on();\t// the DMR and the all digital search\n#endif\n'
         '#if CONFIG_P25_SEARCH\n\ton = on || p25s_on();\n#endif\n'
         '#if CONFIG_SPECTRUM\n\ton = on || spectrum_on();\n#endif\n'
         '#if CONFIG_DMR_TRUNK\n\ton = on || dmrtk_on();\n#endif\n'
         '#if CONFIG_P25_TRUNK\n\ton = on || p25tk_on();\n#endif\n'
         '\treturn on;\n}\n#endif\n\n'
         "#if CONFIG_CW_BEACON\n// Key the beacon's message now, or refuse"),
        ('\tbool busy = sm->hotspot || sm->ptt || sm->view.state == RS_TX || sm->cw_phase != CW_OFF ||\n'
         '\t\t    board.radio->tx_tone == NULL || codeplug_mode_p25(sm->view.ch.mode);\n',
         '\tbool busy = sm->hotspot || sm->ptt || sm->view.state == RS_TX || sm->cw_phase != CW_OFF ||\n'
         '\t\t    board.radio->tx_tone == NULL || codeplug_mode_p25(sm->view.ch.mode);\n'
         '\tbusy = busy || radio_moving(sm);\t// DDM32 Plus: never the letter a scan / the ring is on, every time it keys\n'),
        # SFR: the CW ID switched off after a refused start: the check runs again (the role may start now)
        ('\tactivity(sm, now_ms);\n#if CONFIG_DMR\n\tif (reply_changed && sm->dmr_running) {\n',
         '#if CONFIG_SFR_CWID\n'
         '\tif (sm->view.dmr.sfr_nocall && sm->set.sfr_cwid == 0) {\n'
         '\t\tsm->view.dmr.sfr_nocall = false;\t// DDM32 Plus: no ID wanted any more: the channel tuned again\n'
         '\t\ttune(sm, now_ms);\n\t}\n#endif\n'
         '\tactivity(sm, now_ms);\n#if CONFIG_DMR\n\tif (reply_changed && sm->dmr_running) {\n'),
        # SFR: no unidentified repeating
        ('\tif (sfr_rpt_wait(sm)) {\n\t\tsm->sfr_denied = true;\t// radio_sm_tick starts the role once it may\n',
         '#if CONFIG_SFR_CWID\n'
         '\tsm->view.dmr.sfr_nocall = false;\n'
         '\tif (sm->set.sfr_cwid != 0 && !SFR_RPT_RESUMING(sm)) {\n'
         '\t\tchar call[RADIO_CWID_CALL_MAX + 1];\n'
         '\t\tif (radio_cwid_callsign(sm->radio_name, call, sizeof call) == 0) {\n'
         '\t\t\tsm->view.dmr.sfr_nocall = true;\t// DDM32 Plus: its CW ID is on, no callsign: never repeat unidentified\n'
         '\t\t\treturn false;\n\t\t}\n\t}\n#endif\n'
         '\tif (sfr_rpt_wait(sm)) {\n\t\tsm->sfr_denied = true;\t// radio_sm_tick starts the role once it may\n'),
    ])
