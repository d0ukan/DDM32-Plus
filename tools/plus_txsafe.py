#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus transmit safety, second round (docs/plus.md "Pre-release hole hunt: transmit safety"). Called from
plus_features.prepare() after plus_sstvtx; a module of its own so the shared prepare list gains one line. Runs last on
purpose: it anchors on text the earlier Plus hunks made.

- radio_moving(): one rule for every feature that keys by itself (SSTV send, the CW beacon): never while a scan, a
  dual / multi-watch, the air watch, a search, the spectrum or trunk following moves the receiver.
- CW beacon: refused while radio_moving(), and outside the band plan (xband_check_hz), each time it keys.
- CONFIG_VFO_PLAN: the VFO transmits only in the amateur bands (radio_sm_tx_check, TXDENY_VFO_PLAN): 144-146 /
  430-440 MHz, never PMR446, never simplex on a repeater output segment; a repeater input with a split is fine.
  Codeplug channels are the user's (CPS) and are not affected.
- RREQ_VFO_TX: an explicit split ends a search find's receive only (FM / DMR VFO).
- SFR repeater role with its CW ID on and no callsign as the radio name: not started (no unidentified repeating);
  the role page says why (radio_dmr_view_t.sfr_nocall)."""

ON_PLAN = '#if CONFIG_VFO_PLAN\n'


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
    transform('config/features.mk', [
        ('CONFIG_VFO_MODES ?= n\nFEATURES += VFO_MODES\n',
         'CONFIG_VFO_PLAN ?= n\nFEATURES += VFO_PLAN\nCONFIG_VFO_MODES ?= n\nFEATURES += VFO_MODES\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_VFO_MODES\n',
         '#ifndef CONFIG_VFO_PLAN\n#define CONFIG_VFO_PLAN 0\t/* DDM32 Plus: the VFO transmits in the amateur bands only */\n'
         '#endif\n#ifndef CONFIG_VFO_MODES\n'),
    ])
    transform('core/radio/radio.h', [
        ('\tTXDENY_NO_CALL,\t// DM32 Plus YSF transmit: the radio name is not a callsign (CONFIG_YSF_TX)\n',
         '\tTXDENY_NO_CALL,\t// DM32 Plus YSF transmit: the radio name is not a callsign (CONFIG_YSF_TX)\n'
         '\tTXDENY_VFO_PLAN,\t// DDM32 Plus: the VFO outside the amateur bands (CONFIG_VFO_PLAN)\n'),
        ('\tbool sfr_cw;\t\t// the repeater role keys its CW ID now (CONFIG_SFR_CWID, sfr.md 4, CW ID)\n',
         '\tbool sfr_cw;\t\t// the repeater role keys its CW ID now (CONFIG_SFR_CWID, sfr.md 4, CW ID)\n'
         '\tbool sfr_nocall;\t// DDM32 Plus: the role wasn\'t started: its CW ID is on and the radio name is no callsign\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('\tdefault: sb_str(&b, TR("Not allowed here")); break;\n',
         ON_PLAN + '\tcase TXDENY_VFO_PLAN: sb_str(&b, TR("VFO: outside the amateur bands")); break;\n#endif\n'
         '\tdefault: sb_str(&b, TR("Not allowed here")); break;\n'),
        ('\tcase TXDENY_BAND: return v->vfo ?',
         ON_PLAN + '\tcase TXDENY_VFO_PLAN:\n#endif\n\tcase TXDENY_BAND: return v->vfo ?'),
    ])
    transform('core/ui/ui_menus.c', [
        ('!d->sfr_rpt ? TR("Off: transmit not allowed now") :',
         '!d->sfr_rpt ? (d->sfr_nocall ? TR("Set your callsign (the radio name) in qdmr.") :\n'
         '\t\t\t\tTR("Off: transmit not allowed now")) :'),
    ])
    transform('core/radio/radio_sm.c', [
        # The VFO's band plan, then its place in the TX checks (after receive only)
        ('uint8_t radio_sm_tx_check(const radio_sm_t *sm)\n{\n',
         ON_PLAN + '// DDM32 Plus (hole hunt): the VFO transmits in the amateur bands only (IARU Region 1, Turkey): 144-146 and\n'
         '// 430-440 MHz, never PMR446, never simplex on a repeater output segment (145.600-145.7875, 438.650-439.425 MHz,\n'
         '// half a channel either side); a repeater input with the VFO\'s split is fine. Satellites and APRS are allowed\n'
         '#ifdef VFO_PLAN_TEST\nextern bool vfo_plan_test_on;\t// host tests: upstream\'s suites keep their VFO anywhere\n'
         '#define VFO_PLAN_ON vfo_plan_test_on\n#else\n#define VFO_PLAN_ON true\n#endif\n'
         'static bool vfo_plan_refused(uint32_t rx, uint32_t tx)\n{\n'
         '\tif (!VFO_PLAN_ON)\n\t\treturn false;\n'
         '\tif (tx >= 446000000u && tx <= 446200000u)\n\t\treturn true;\n'
         '\tif (!((tx >= 144000000u && tx <= 146000000u) || (tx >= 430000000u && tx <= 440000000u)))\n\t\treturn true;\n'
         '\treturn rx == tx && ((tx >= 145593750u && tx <= 145793750u) || (tx >= 438643750u && tx <= 439431250u));\n'
         '}\n#endif\n\n'
         'uint8_t radio_sm_tx_check(const radio_sm_t *sm)\n{\n'),
        ('\tif (ch->rx_only) {\n\t\treturn TXDENY_RX_ONLY;\n\t}\n',
         '\tif (ch->rx_only) {\n\t\treturn TXDENY_RX_ONLY;\n\t}\n'
         + ON_PLAN + '\tif (sm->view.vfo && vfo_plan_refused(ch->rx_hz, ch->tx_hz)) {\n'
         '\t\treturn TXDENY_VFO_PLAN;\t// DDM32 Plus: a codeplug channel is the user\'s (CPS), the VFO is ours\n\t}\n#endif\n'),
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
         '\treturn on;\n}\n#endif\n'
         '#if CONFIG_XBAND || CONFIG_DOTA || CONFIG_SSTV_TX\n'
         'uint8_t xband_check_hz(uint32_t hz);\t// radio/xband.h: 0 = the band plan allows it\n#endif\n\n'
         "#if CONFIG_CW_BEACON\n// Key the beacon's message now, or refuse"),
        ('\tbool busy = sm->hotspot || sm->ptt || sm->view.state == RS_TX || sm->cw_phase != CW_OFF ||\n'
         '\t\t    board.radio->tx_tone == NULL || codeplug_mode_p25(sm->view.ch.mode);\n',
         '\tbool busy = sm->hotspot || sm->ptt || sm->view.state == RS_TX || sm->cw_phase != CW_OFF ||\n'
         '\t\t    board.radio->tx_tone == NULL || codeplug_mode_p25(sm->view.ch.mode);\n'
         '\tbusy = busy || radio_moving(sm);\t// DDM32 Plus: never the letter a scan / the ring is on, every time it keys\n'
         '#if CONFIG_XBAND || CONFIG_DOTA || CONFIG_SSTV_TX\n'
         '\tif (deny == TXDENY_NONE && xband_check_hz(sm->view.ch.tx_hz) != 0)\n'
         '\t\tdeny = TXDENY_BAND;\t// DDM32 Plus: the band plan, unattended keying\n#endif\n'),
        # An explicit split ends a search find's receive only (the user set the repeater's offset)
        ('\t\tsm->vfo_ch.tx_hz = tx;\n\t\tsm->view.ch.tx_hz = tx;\n',
         '\t\tsm->vfo_ch.tx_hz = tx;\n\t\tsm->view.ch.tx_hz = tx;\n'
         '#if CONFIG_VFO_MODES\n'
         '\t\tif (tx != sm->vfo_ch.rx_hz && !vfox_native(sm->vfo_ch.mode)) {\n'
         '\t\t\tsm->vfo_ch.rx_only = false;\t// DDM32 Plus: a find\'s receive only ends with a split set\n'
         '\t\t\tsm->view.ch.rx_only = false;\n\t\t}\n#endif\n'),
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
