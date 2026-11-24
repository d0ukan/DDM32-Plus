#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 14 "xband" job (docs/plus.md "Last calls", "Cross-band store and forward"; ROADMAP rows 43 and
46). Called last from plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code
is in integration/radiofoundry/lastcall/ (installed into core/ui/) and integration/radiofoundry/xband/ (core/radio/,
core/ui/); upstream files change only through the hunks below. Our own files the job touches are edited in place
(p25_lab.c / .h: the memory it lends; dota/radio_dota.inc: DOTA never takes the radio from a session;
common/ui_scratch.*: the picker's room in the shared buffer).

  LAST_CALLS  (item 43) Tools > Last call becomes a list of the last 8 calls from every mode (DMR, P25, YSF, NXDN, dPMR,
              FM caller IDs) and every watched channel (the multi-watch letter A-F, dual watch's A / B): letter, mode,
              caller, talkgroup or unit, how long ago. The newest P25 call still plays again (CALL_REPLAY's frames,
              unchanged); the other modes keep no audio (owner, 2026-11-20). Fed where the heard list is fed. Needs
              CALL_LOG and CALL_REPLAY.
  XBAND       (item 46) Tools > Cross-band: store and forward between two frequencies A and B, each in its own mode
              (2026-11-24: DMR -> DMR, NXDN -> DMR, DMR / NXDN -> FM, P25 -> P25; docs/xband-multiproto-study.md). An
              over heard on one is recorded (its AMBE+2 air frames or IMBE parameters, in the P25 worker's memory lent
              for the session) and sent on the other (the call stack, the stock codec into the FM tone path, the P25
              worker's transmitter); attended operation, the screen locked (Back stops), CROSS on Home, the CW ID, the
              band plan's refusals. Needs DMR, SFR_CWID, P25_CHANNELS, DEV_TX and CALL_REPLAY.
"""

LASTCALL = 'integration/radiofoundry/lastcall/'

LC_ON = '#if CONFIG_LAST_CALLS\n'


def prepare_last_calls(transform, install):
    for f in ('lastcall.c', 'lastcall.h', 'ui_lastcall.c', 'ui_lastcall.h'):
        install('core/ui/' + f, LASTCALL + f)

    # The switch, after DOTA's (the end of the Plus switches)
    transform('config/features.mk', [
        ('CONFIG_DOTA ?= n\nFEATURES += DOTA\n',
         'CONFIG_DOTA ?= n\nFEATURES += DOTA\n'
         'CONFIG_LAST_CALLS ?= n\nFEATURES += LAST_CALLS\n'
         'ifeq ($(CONFIG_LAST_CALLS),y)\nifneq ($(CONFIG_CALL_LOG)$(CONFIG_CALL_REPLAY),yy)\n'
         '$(error LAST_CALLS needs CALL_LOG=y and CALL_REPLAY=y)\nendif\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DOTA\n',
         '#ifndef CONFIG_LAST_CALLS\n#define CONFIG_LAST_CALLS 0\t/* DDM32 Plus last calls (docs/plus.md) */\n#endif\n'
         '#ifndef CONFIG_DOTA\n'),
    ])

    # --- The screen: Tools > Last call opens the list; the replay moved into its OK (ui_lastcall.c lc_select) ---
    transform('core/ui/ui_nav.h', [
        ('\tSCR_COUNT\n} ui_scr_t;',
         '\tSCR_LASTCALL,\t\t\t// DDM32 Plus: Tools > Last call, the last calls (CONFIG_LAST_CALLS)\n'
         '\tSCR_COUNT\n} ui_scr_t;'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_clock_screen;\t// ui_clock.c (DDM32 Plus)',
         'extern const ui_scr_def_t ui_clock_screen;\t// ui_clock.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_lastcall_screen;\t// ui_lastcall.c (DDM32 Plus)'),
        ('#if CONFIG_CLOCK_TOOLS\n\t[SCR_CLOCK] = &ui_clock_screen,\n#endif\n',
         '#if CONFIG_CLOCK_TOOLS\n\t[SCR_CLOCK] = &ui_clock_screen,\n#endif\n'
         + LC_ON + '\t[SCR_LASTCALL] = &ui_lastcall_screen,\n#endif\n'),
        ('#if CONFIG_CALL_REPLAY\n\tcase T_REPLAY: {\n',
         LC_ON + '\tcase T_REPLAY: ui_push(SCR_LASTCALL, 0); break;\t// DDM32 Plus: the list (ui_lastcall.c)\n'
         '#elif CONFIG_CALL_REPLAY\n\tcase T_REPLAY: {\n'),
    ])

    # --- The UI task's hooks: where the heard list is fed (plus_toolbox.py's lines, there whatever HEARD is) ---
    transform('core/ui/ui_app.c', [
        ('#if CONFIG_HEARD\n#include "ui/ui_heard.h"\n#endif\n',
         '#if CONFIG_HEARD\n#include "ui/ui_heard.h"\n#endif\n' + LC_ON + '#include "ui/ui_lastcall.h"\n#endif\n'),
        ('#if CONFIG_HEARD\n\tui_heard_recent(r, true);\t// DDM32 Plus heard list (ui_heard.c)\n#endif\n',
         '#if CONFIG_HEARD\n\tui_heard_recent(r, true);\t// DDM32 Plus heard list (ui_heard.c)\n#endif\n'
         + LC_ON + '\tui_lastcall_recent(r, true);\t// DDM32 Plus last calls (ui_lastcall.c)\n#endif\n'),
        ('#if CONFIG_HEARD\n\t\t\tui_heard_recent(&recent[0], false);\t// the same station\'s call goes on\n#endif\n',
         '#if CONFIG_HEARD\n\t\t\tui_heard_recent(&recent[0], false);\t// the same station\'s call goes on\n#endif\n'
         + LC_ON + '\t\t\tui_lastcall_recent(&recent[0], false);\n#endif\n'),
        ('#if CONFIG_HEARD\n\t\t\t\t\tui_heard_recent(&recent[i], false);\t// the P25 call goes on\n#endif\n',
         '#if CONFIG_HEARD\n\t\t\t\t\tui_heard_recent(&recent[i], false);\t// the P25 call goes on\n#endif\n'
         + LC_ON + '\t\t\t\t\tui_lastcall_recent(&recent[i], false);\n#endif\n'),
        ('#if CONFIG_HEARD\n\tui_heard_view(v);\t// DDM32 Plus: what the calls below are heard on\n#endif\n',
         '#if CONFIG_HEARD\n\tui_heard_view(v);\t// DDM32 Plus: what the calls below are heard on\n#endif\n'
         + LC_ON + '\tui_lastcall_view(v);\t// and on which watched channel (ui_lastcall.c)\n#endif\n'),
        ('#if CONFIG_HEARD\n\tui_heard_reset();\n#endif\n',
         '#if CONFIG_HEARD\n\tui_heard_reset();\n#endif\n' + LC_ON + '\tui_lastcall_reset();\n#endif\n'),
    ])

    # The watched channel the radio is tuned to (the ring's letter, dual watch's pane), for the list's letter. The
    # ring's own mw_rx follows a tick later (ui_dual_tick runs after Recent is fed), so this asks the view itself
    transform('core/ui/ui_dual.c', [
        # (right after multi-watch's own part, whatever follows it: air watch's)
        ('#if CONFIG_MULTI_WATCH\n#include "ui_mwatch.inc"\t// DM32 Plus\n#endif\n',
         '#if CONFIG_MULTI_WATCH\n#include "ui_mwatch.inc"\t// DM32 Plus\n#endif\n'
         + LC_ON +
         '// DDM32 Plus last calls: the watched channel v is tuned to: \'A\'..\'F\' (the ring), \'A\' / \'B\' (dual watch), 0 none\n'
         'char ui_dual_letter_here(const radio_view_t *v);\n'
         'char ui_dual_letter_here(const radio_view_t *v)\n{\n'
         '\tif (!dual()->on || v->hotspot)\n\t\treturn 0;\n'
         '#if CONFIG_MULTI_WATCH\n\tif (mw_ring()) {\n\t\tuint8_t k = mw_tuned(v);\n'
         '\t\treturn k < UI_MW_MAX ? mw_char(k) : 0;\n\t}\n#endif\n'
         '\tuint8_t t = tuned_pane(v);\n\treturn t == 0 ? \'A\' : t == 1 ? \'B\' : 0;\n}\n#endif\n'),
    ])


XBAND = 'integration/radiofoundry/xband/'

XB_ON = '#if CONFIG_XBAND\n'

# Lines for the stack check's table (tools/stack_check_edges.txt syntax; build_radiofoundry.py adds them to a Plus
# build's). radio_xband.inc, which the source scanner doesn't read, wraps the call stack's voice sink and source for a
# session and fills the engine's io: without these the radio task's chains through them are skipped. A caller is named
# plainly (the table matches callers by name; dmr_call.c's sink_lost shares its name with ambeVoice.c's, which makes no
# pointer call), a target with its file.
STACK_EDGES = (
    '# DDM32 Plus cross-band (tools/plus_xband.py)\n'
    'call sink_lost core/radio/radio_xband.inc:xb_sink_lost\n'
    'call rx_end core/radio/radio_xband.inc:xb_sink_end\n'
    'call rx_lc core/radio/radio_xband.inc:xb_sink_end core/radio/radio_xband.inc:xb_sink_begin\n'
    'call rx_recheck core/radio/radio_xband.inc:xb_sink_begin\n'
    'call rx_play core/radio/radio_xband.inc:xb_sink_frame\n'
    'call tx_fill core/radio/radio_xband.inc:xb_source_next\n'
    'call xb_sink_begin core/dmr/ambe/ambeVoice.c:sink_begin\n'
    'call xb_sink_frame core/dmr/ambe/ambeVoice.c:sink_frame\n'
    'call xb_sink_lost core/dmr/ambe/ambeVoice.c:sink_lost\n'
    'call xb_sink_end core/dmr/ambe/ambeVoice.c:sink_end\n'
    'call xband_poll core/radio/radio_xband.inc:xb_tune core/radio/radio_xband.inc:xb_key '
    'core/radio/radio_xband.inc:xb_unkey core/radio/radio_xband.inc:xb_cw core/radio/radio_xband.inc:xb_beep\n'
    'call listen core/radio/radio_xband.inc:xb_tune\n'
    'call id_next core/radio/radio_xband.inc:xb_cw core/radio/radio_xband.inc:xb_beep\n'
    # an FM over: the tone path's interrupt reads the decoded blocks, the voice bridge's audio block hands them over
    'call platform/hrc7000/hrc_afsk.c:fill radio_sstvtx_gen core/radio/radio_xband.inc:xb_gen\n'
    'call ambeVoiceAudioBlock beep_mix core/radio/radio_xband.inc:xb_mix\n'
)


def prepare_crossband(transform, install):
    """XBAND (item 46): cross-band store and forward, a mode per side (docs/plus.md "Cross-band store and forward").
    Needs DMR, SFR_CWID (the CW keyer and the callsign rule), P25_CHANNELS and DEV_TX (the P25 worker whose memory it
    borrows and whose transmitter sends a P25 over) and CALL_REPLAY (the IMBE frame packing, the memory)."""
    for dst, src in (('core/radio/xband.c', 'xband.c'), ('core/radio/xband.h', 'xband.h'),
                     ('core/radio/radio_xband.inc', 'radio_xband.inc'), ('core/ui/ui_xband.c', 'ui_xband.c'),
                     ('core/ui/ui_xband.h', 'ui_xband.h')):
        install(dst, XBAND + src)

    transform('config/features.mk', [
        ('CONFIG_LAST_CALLS ?= n\nFEATURES += LAST_CALLS\n',
         'CONFIG_LAST_CALLS ?= n\nFEATURES += LAST_CALLS\n'
         'CONFIG_XBAND ?= n\nFEATURES += XBAND\n'
         # off where a board's build lacks one (the registry asks every board), as DOTA
         'ifneq ($(CONFIG_DMR)$(CONFIG_SFR_CWID)$(CONFIG_P25_CHANNELS)$(CONFIG_DEV_TX)$(CONFIG_CALL_REPLAY),yyyyy)\n'
         'override CONFIG_XBAND := n\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_LAST_CALLS\n',
         '#ifndef CONFIG_XBAND\n#define CONFIG_XBAND 0\t/* DDM32 Plus cross-band store and forward (docs/plus.md) */\n'
         '#endif\n#ifndef CONFIG_LAST_CALLS\n'),
    ])

    # --- The radio core (radio_xband.inc) ---
    transform('core/radio/radio_sm.h', [
        ('\tRREQ_DOTA,\t\t// DDM32 Plus: arg 1 = a DOTA session starts (dota.h), 0 = it stops\n} radio_req_type_t;',
         '\tRREQ_DOTA,\t\t// DDM32 Plus: arg 1 = a DOTA session starts (dota.h), 0 = it stops\n'
         '\tRREQ_XBAND,\t\t// DDM32 Plus: arg 1 = a cross-band session starts (xband.h), 0 = it stops\n} radio_req_type_t;'),
    ])
    transform('core/radio/radio_task.c', [
        ('void radio_req_beep(uint8_t b) { post(RREQ_BEEP, b, 0); }\n',
         'void radio_req_beep(uint8_t b) { post(RREQ_BEEP, b, 0); }\n'
         + XB_ON + 'void radio_req_xband(bool on) { post(RREQ_XBAND, on, 0); }\t// DDM32 Plus (radio/xband.h)\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "radio/radio_sm.h"\n',
         '#include "radio/radio_sm.h"\n' + XB_ON +
         '#include "radio/xband.h"\t\t// DDM32 Plus cross-band store and forward (radio_xband.inc)\n'
         'static bool xb_cw_on;\t\t\t// its CW ID keys now (cw_restore brings its channel back)\n'
         '#include "hal_time.h"\n#define xb_now hal_millis()\t\t// the io calls take no time: now\n'
         '#if !RADIO_SM_CW\n#error "XBAND needs the CW keyer (SFR_CWID)"\n#endif\n'
         '#else\n#define xband_on() false\n#endif\n'),
        ('#if CONFIG_DOTA\n#include "radio/radio_dota.inc"\t// DDM32 Plus DOTA Transfer: the radio core\'s side\n#endif\n',
         '#if CONFIG_DOTA\n#include "radio/radio_dota.inc"\t// DDM32 Plus DOTA Transfer: the radio core\'s side\n#endif\n'
         + XB_ON + '#include "radio/radio_xband.inc"\t// DDM32 Plus cross-band: the radio core\'s side\n#endif\n'),
        ('#if CONFIG_DOTA\n\tif (dota_request(sm, req, now_ms))\n\t\treturn;\t// DOTA\'s own, or dropped while a transfer runs\n#endif\n',
         '#if CONFIG_DOTA\n\tif (dota_request(sm, req, now_ms))\n\t\treturn;\t// DOTA\'s own, or dropped while a transfer runs\n#endif\n'
         + XB_ON + '\tif (xb_request(sm, req, now_ms))\n\t\treturn;\t// the cross-band session\'s own, or dropped while it runs\n'
         '#endif\n'),
        ('\tif (dota_rf_poll(sm, &st, now_ms)) {\n\t\toutputs(sm);\t// DDM32 Plus DOTA keys FM: nothing else runs\n'
         '\t\treturn;\n\t}\n#endif\n',
         '\tif (dota_rf_poll(sm, &st, now_ms)) {\n\t\toutputs(sm);\t// DDM32 Plus DOTA keys FM: nothing else runs\n'
         '\t\treturn;\n\t}\n#endif\n' + XB_ON + '\txb_rf_poll(sm, now_ms);\t// DDM32 Plus cross-band (radio_xband.inc)\n#endif\n'),
        ('\tif (dota_on() || sm->aprs_phase != APRS_RF_OFF ||', '\tif (dota_on() || xband_on() || sm->aprs_phase != APRS_RF_OFF ||'),
        ('!ZONE_WRITE_BUSY() && !dota_on() &&', '!ZONE_WRITE_BUSY() && !dota_on() && !xband_on() &&'),
        # a P25 session's channel: the native receiver owns the radio and the poll is not run; the session goes on
        ('\tbeep_service(p25_lab_busy());\n#endif\n}\n',
         '\tbeep_service(p25_lab_busy());\n#endif\n' + XB_ON + '\txb_native_poll(sm, now_ms);\t// DDM32 Plus cross-band\n#endif\n}\n'),
        # The cross-band CW ID is over (or cut): back to the session's channel (the engine retunes after)
        ('#if CONFIG_CW_BEACON\n\tif (beacon.keying) {\n',
         XB_ON + '\tif (xb_cw_on) {\n\t\txb_cw_on = false;\t// DDM32 Plus cross-band: its ID is over (or cut)\n'
         '\t\tsm->board_mode = RADIO_MODE_FM;\n\t\ttune(sm, xb_now);\n\t\toutputs(sm);\n\t\tchanged(sm);\n\t\treturn;\n\t}\n'
         '#endif\n#if CONFIG_CW_BEACON\n\tif (beacon.keying) {\n'),
    ])
    # --- Tools > Cross-band (ui_xband.c), after DOTA Transfer; the lock, the tick, Home's mark ---
    transform('core/ui/ui_nav.h', [
        ('\tSCR_LASTCALL,\t\t\t// DDM32 Plus: Tools > Last call, the last calls (CONFIG_LAST_CALLS)\n',
         '\tSCR_LASTCALL,\t\t\t// DDM32 Plus: Tools > Last call, the last calls (CONFIG_LAST_CALLS)\n'
         '\tSCR_XBAND, SCR_XBAND_PICK,\t// DDM32 Plus: Tools > Cross-band, its picker (CONFIG_XBAND)\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_lastcall_screen;\t// ui_lastcall.c (DDM32 Plus)',
         'extern const ui_scr_def_t ui_lastcall_screen;\t// ui_lastcall.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_xband_screen, ui_xband_pick_screen;\t// ui_xband.c (DDM32 Plus)'),
        (LC_ON + '\t[SCR_LASTCALL] = &ui_lastcall_screen,\n#endif\n',
         LC_ON + '\t[SCR_LASTCALL] = &ui_lastcall_screen,\n#endif\n'
         + XB_ON + '\t[SCR_XBAND] = &ui_xband_screen,\n\t[SCR_XBAND_PICK] = &ui_xband_pick_screen,\n#endif\n'),
        ('T_CLOCK, T_DOTA, T_CW, T_BATTERY,', 'T_CLOCK, T_DOTA, T_XBAND, T_CW, T_BATTERY,'),
        ('(i != T_DOTA || CONFIG_DOTA) &&', '(i != T_DOTA || CONFIG_DOTA) && (i != T_XBAND || CONFIG_XBAND) &&'),
        ('TRN("DOTA Transfer"), TRN("CW decoder"),', 'TRN("DOTA Transfer"), TRN("Cross-band"), TRN("CW decoder"),'),
        ('#if CONFIG_DOTA\n\tcase T_DOTA: ui_push(SCR_DOTA, 0); break;\n#endif\n',
         '#if CONFIG_DOTA\n\tcase T_DOTA: ui_push(SCR_DOTA, 0); break;\n#endif\n'
         + XB_ON + '\tcase T_XBAND: ui_push(SCR_XBAND, 0); break;\n#endif\n'),
        ('[T_DOTA] = ART_ICON_DOTA + 1,', '[T_DOTA] = ART_ICON_DOTA + 1, [T_XBAND] = ART_ICON_XBAND + 1,'),
    ])
    transform('core/ui/ui_app.c', [
        (LC_ON + '#include "ui/ui_lastcall.h"\n#endif\n',
         LC_ON + '#include "ui/ui_lastcall.h"\n#endif\n' + XB_ON + '#include "radio/xband.h"\n#include "ui/ui_xband.h"\n#endif\n'),
        ('void ui_app_key(const input_event_t *ev)\n{\n\tdirty = true;\n\tlast_key_ms = now;\n\tlast_use_ms = now;\n'
         '\toff_warned = false;\n',
         'void ui_app_key(const input_event_t *ev)\n{\n\tdirty = true;\n\tlast_key_ms = now;\n\tlast_use_ms = now;\n'
         '\toff_warned = false;\n' + XB_ON +
         '\tif (ui_xband_lock(ev))\n\t\treturn;\t// DDM32 Plus cross-band: the screen is locked, Back alone stops it\n#endif\n'),
        ('\tif (ui_dota_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n',
         '\tif (ui_dota_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n'
         + XB_ON + '\tif (ui_xband_tick(stack[sp].scr, now))\n\t\tdirty = true;\t// DDM32 Plus cross-band: Home follows the relay\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_screens.c', [
        ('#include "config.h"\n',
         '#include "config.h"\n' + XB_ON + '#include "radio/xband.h"\n#include "ui/ui_xband.h"\n#endif\n'),
        ('\tint lx = gfx_text(f, x, ty, mw, sfg);\n',
         '\tint lx = gfx_text(f, x, ty, mw, sfg);\n' + XB_ON +
         '\tif (xband_on() && strip_fits(f, lx, rx, "CROSS"))\n'
         '\t\tlx = gfx_text(f, lx + f->w, ty, "CROSS", swarn);\t// DDM32 Plus: a cross-band session relays\n'
         '\telse\n#endif\n'),
        ('\tui_draw_bottom(s, bottom);',
         # what the relay does and how to stop it, shown whatever Settings says of hints (a toast still wins)
         XB_ON + '\tif (xband_on() && s->toast == NULL)\n'
         '\t\tui_draw_hint(ui_xband_home(bottom), false);\t// DDM32 Plus cross-band\n\telse\n#endif\n'
         '\tui_draw_bottom(s, bottom);'),
    ])


def prepare_xband(transform, install):
    prepare_last_calls(transform, install)
    prepare_crossband(transform, install)
