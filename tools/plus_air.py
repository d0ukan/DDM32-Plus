#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus round 14 air items (docs/plus.md "Round 14: air band and spectrum", branch r14-air). Called last from
plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code is in
integration/radiofoundry/air/ (installed into core/radio/ and core/ui/); upstream files and the shared Plus files
change only through the hunks below.

  AIR_WATCH  (item 44b) Tools > Airfields, # on an airfield's page: up to 6 of its frequencies on the multi-watch ring,
             A..F each its own VFO (AM: the air band), hopping as multi-watch does; its ATIS last, holding the ring 5 s
             at most and then passed over 60 s; stopping it brings the user's watch back as it was. Needs MULTI_WATCH,
             VFO_MODES and AIRFIELDS. Home's status bar and dual watch's VFO pane say AM for the air band (they said
             FM), and the priority channel is never a stop of its ring (radio_scanprio.inc).
  SPECTRUM_WF3D (item 45) Tools > Spectrum's third view: the waterfall's rows as a ridge plot in perspective (* cycles
             waterfall, 3D, bars alone); the waterfall's own heap rows, no new RAM. Needs SPECTRUM_WF.
"""

AIR = 'integration/radiofoundry/air/'
TOOLBOX = 'integration/radiofoundry/toolbox/'


def prepare_air(transform, install):
    install('core/radio/radio_airwatch.inc', AIR + 'radio_airwatch.inc')
    install('core/ui/ui_airwatch.inc', AIR + 'ui_airwatch.inc')
    install('core/ui/wf3d.c', TOOLBOX + 'wf3d.c')
    install('core/ui/wf3d.h', TOOLBOX + 'wf3d.h')

    # The switch, after DOTA's (the end of the Plus switches); off where a build lacks what it needs
    transform('config/features.mk', [
        ('CONFIG_DOTA ?= n\nFEATURES += DOTA\n',
         'CONFIG_DOTA ?= n\nFEATURES += DOTA\n'
         'CONFIG_AIR_WATCH ?= n\nFEATURES += AIR_WATCH\n'
         'ifneq ($(CONFIG_MULTI_WATCH)$(CONFIG_VFO_MODES)$(CONFIG_AIRFIELDS),yyy)\n'
         'override CONFIG_AIR_WATCH := n\nendif\n'
         'CONFIG_SPECTRUM_WF3D ?= n\nFEATURES += SPECTRUM_WF3D\n'
         'ifneq ($(CONFIG_SPECTRUM_WF),y)\noverride CONFIG_SPECTRUM_WF3D := n\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_DOTA\n#define CONFIG_DOTA 0\t/* DDM32 Plus DOTA Transfer (docs/dota.md) */\n#endif\n',
         '#ifndef CONFIG_DOTA\n#define CONFIG_DOTA 0\t/* DDM32 Plus DOTA Transfer (docs/dota.md) */\n#endif\n'
         '#ifndef CONFIG_AIR_WATCH\n#define CONFIG_AIR_WATCH 0\t/* DDM32 Plus air watch (item 44b) */\n#endif\n'
         '#ifndef CONFIG_SPECTRUM_WF3D\n#define CONFIG_SPECTRUM_WF3D 0\t/* DDM32 Plus 3D waterfall (item 45) */\n#endif\n'),
    ])

    # ---- The radio core (radio_airwatch.inc): two requests, the flag, nothing noted while it runs ----
    transform('core/radio/radio.h', [
        ('bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel);\n',
         'bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel);\n'
         '#if CONFIG_AIR_WATCH\n'
         '// DDM32 Plus air watch (radio_airwatch.inc): VFO idx (0..5, a ring letter\'s own) takes an air band frequency;\n'
         '// the first since the last end keeps the user\'s state as a power off does. The end brings it back\n'
         'void radio_req_air_vfo(uint32_t arg, uint32_t hz, uint16_t tone10);\n'
         'void radio_req_air_end(void);\n'
         '// arg: the VFO (bits 0-2) and these. DDM32 Plus auto scan (radio_airwatch.inc): the letter\'s kind, AM (the air\n'
         '// band, 0), FM or DMR (a repeater as listed); bits 8-13 DMR: colour code (8-11) and slot (12-13); bits 14-31\n'
         '// TX - RX in 1.25 kHz steps, signed. tone10: an FM letter\'s access tone as listed (Hz x 10, 0 none)\n'
         '#define RADIO_AIR_KIND_SHIFT\t3\n'
         '#define RADIO_AIR_FM\t0x08u\n'
         '#define RADIO_AIR_DMR\t0x10u\n'
         '#define RADIO_AIR_NARROW\t0x20u\t// FM 12.5 kHz (else 25)\n'
         '#define RADIO_AIR_REPLACE\t0x40u\t// a refresh: refused while the ring holds a signal (or waits after one) or TX\n'
         '#define RADIO_AIR_ATIS\t0x80u\t// idx | this: an ATIS (its letter held 5 s at most, then passed over 60 s)\n'
         '#define RADIO_AIR_P_SHIFT\t8\n'
         '#define RADIO_AIR_OFF_SHIFT\t14\n'
         '// The request\'s arg2: an air letter\'s frequency; a repeater letter\'s RX in 1.25 kHz steps (bits 0-19) and its\n'
         '// access tone (bits 20-31). 0 (refused by the band check): one it can\'t carry (RX off that grid)\n'
         'static inline uint32_t radio_air_arg2(uint32_t arg, uint32_t hz, uint16_t tone10)\n{\n'
         '\tif ((arg & (RADIO_AIR_FM | RADIO_AIR_DMR)) == 0)\n\t\treturn hz;\n'
         '\tif (hz % 1250u != 0u || hz / 1250u > 0xFFFFFu || tone10 > 0xFFFu)\n\t\treturn 0;\n'
         '\treturn hz / 1250u | (uint32_t)tone10 << 20;\n}\n'
         '#endif\n'),
    ])
    transform('core/radio/radio_sm.h', [
        ('\tRREQ_MWATCH,\t\t// DM32 Plus multi-watch: the ring radio_req_mwatch left (radio_mwatch_take)\n',
         '\tRREQ_MWATCH,\t\t// DM32 Plus multi-watch: the ring radio_req_mwatch left (radio_mwatch_take)\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tRREQ_AIR_VFO,\t\t// DDM32 Plus air watch: arg VFO 0..5, arg2 its frequency (radio_airwatch.inc)\n'
         '\tRREQ_AIR_END,\t\t// ... the end: the user\'s VFOs and position as kept\n'
         '#endif\n'),
        ('\tuint8_t dual_look;\t\t// ... the one looked at last (the looks take the others in turn)\n',
         '\tuint8_t dual_look;\t\t// ... the one looked at last (the looks take the others in turn)\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tbool air;\t\t\t// DDM32 Plus air watch: the VFOs hold an airfield\'s frequencies, nothing is noted\n'
         '\tuint8_t air_atis;\t\t// ... the VFOs (bit 0..5) holding an ATIS (radio_airwatch.inc air_atis_poll)\n'
         '\tuint32_t air_held_ms, air_cool_ms;\t// ... an ATIS letter held since (0: not held), passed over until\n'
         '\tuint32_t air_talk;\t\t// ... the user\'s DMR VFO talk target at its start (a DMR letter\'s; bit 31 private)\n'
         '#endif\n'),
    ])
    transform('core/radio/radio_task.c', [
        ('bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel)\n{\n',
         '#if CONFIG_AIR_WATCH\n'
         '// DDM32 Plus air watch: up to six of these and the ring at once; waits a little for room rather than lose one\n'
         'void radio_req_air_vfo(uint32_t arg, uint32_t hz, uint16_t tone10)\n{\n'
         '\tpost_pair(RREQ_AIR_VFO, (int32_t)arg, radio_air_arg2(arg, hz, tone10), pdMS_TO_TICKS(20));\n}\n\n'
         'void radio_req_air_end(void)\n{\n'
         '\tpost_pair(RREQ_AIR_END, 0, 0, pdMS_TO_TICKS(20));\n}\n'
         '#endif\n'
         'bool radio_mwatch_take(radio_dual_ref_t ref[], uint8_t *n, uint8_t *sel)\n{\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('static void persist(radio_sm_t *sm, uint32_t now_ms)\n{\n\tif (sm->hotspot) {\n',
         'static void persist(radio_sm_t *sm, uint32_t now_ms)\n{\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tif (sm->air) {\n'
         '\t\treturn;\t\t// DDM32 Plus air watch: the user\'s state stays as noted before it (radio_airwatch.inc)\n'
         '\t}\n'
         '#endif\n'
         '\tif (sm->hotspot) {\n'),
        ('void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n',
         '#if CONFIG_AIR_WATCH\n#include "radio/radio_airwatch.inc"\t// DDM32 Plus air watch (item 44b)\n#endif\n\n'
         'void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n'),
        # An ATIS letter of the air watch holds the ring 5 s at most, then is passed over 60 s (radio_airwatch.inc)
        ('// Every poll while scanning: pause on the monitor, what the receiver hears, move on\nstatic void scan_service(',
         '#if CONFIG_AIR_WATCH\n'
         'static bool air_atis_poll(radio_sm_t *sm, bool *active, bool *carrier, bool *sync, uint32_t now_ms);\n'
         'static void air_session_end(radio_sm_t *sm, uint32_t now_ms);\t// radio_airwatch.inc: DOTA, cross-band\n'
         'static void air_scan_first(radio_sm_t *sm, uint8_t what, uint32_t now_ms);\t// ... a scan of the user\'s\n'
         'static void air_talk_back(radio_sm_t *sm);\t// ... PTT on the letter heard\n'
         '#endif\n\n'
         '// Every poll while scanning: pause on the monitor, what the receiver hears, move on\nstatic void scan_service('),
        ('\tsc->cfg.carrier_hold_ms = dmr ? RADIO_SCAN_DMR_CARRIER_MS : 0;\n\tuint8_t was = sc->phase;\n',
         '\tsc->cfg.carrier_hold_ms = dmr ? RADIO_SCAN_DMR_CARRIER_MS : 0;\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tif (air_atis_poll(sm, &active, &carrier, &sync, now_ms)) {\n'
         '\t\tscan_step(sm, now_ms);\t// DDM32 Plus air watch: an ATIS held its letter long enough\n'
         '\t\treturn;\n'
         '\t}\n'
         '#endif\n'
         '\tuint8_t was = sc->phase;\n'),
        ('#if CONFIG_MULTI_WATCH\n\tcase RREQ_MWATCH:\n\t\tmw_request(sm, now_ms);\n\t\tbreak;\n#endif\n',
         '#if CONFIG_MULTI_WATCH\n\tcase RREQ_MWATCH:\n\t\tmw_request(sm, now_ms);\n\t\tbreak;\n#endif\n'
         '#if CONFIG_AIR_WATCH\n\tcase RREQ_AIR_VFO:\n\tcase RREQ_AIR_END:\n\t\tair_request(sm, req, now_ms);\n'
         '\t\tbreak;\n#endif\n'),
    ])

    # ---- The UI (ui_airwatch.inc at the end of ui_dual.c, after ui_mwatch.inc) ----
    transform('core/ui/ui_svc.h', [
        ('bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel);\n',
         'bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel);\n'
         '#if CONFIG_AIR_WATCH\n'
         '// DDM32 Plus air watch: VFO idx (0..5) takes hz, the user\'s state kept (radio_req_air_vfo); the end brings it back\n'
         'void ui_svc_air_vfo(uint32_t arg, uint32_t hz, uint16_t tone10);\n'
         'void ui_svc_air_end(void);\n'
         '#define UI_AIR_ATIS\t0x80u\t// idx | this: an ATIS (RADIO_AIR_ATIS)\n'
         '#define UI_AIR_FM\t0x08u\t// DDM32 Plus auto scan: the RADIO_AIR_* bits (radio.h)\n'
         '#define UI_AIR_DMR\t0x10u\n'
         '#define UI_AIR_NARROW\t0x20u\n'
         '#define UI_AIR_REPLACE\t0x40u\n'
         '#define UI_AIR_P_SHIFT\t8\n'
         '#define UI_AIR_OFF_SHIFT\t14\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_svc_stub.c', [
        ('WEAK bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel) { (void)ref; (void)n; (void)sel; return false; }\n',
         'WEAK bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel) { (void)ref; (void)n; (void)sel; return false; }\n'
         '#if CONFIG_AIR_WATCH\n'
         'WEAK void ui_svc_air_vfo(uint32_t arg, uint32_t hz, uint16_t tone10) { (void)arg; (void)hz; (void)tone10; }\n'
         'WEAK void ui_svc_air_end(void) {}\n'
         '#endif\n'),
    ])
    transform('core/ui/ui_svc_radio.c', [
        ('bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel)\n{\n',
         '#if CONFIG_AIR_WATCH\n'
         '_Static_assert(UI_AIR_ATIS == RADIO_AIR_ATIS, "ui_svc.h: the ATIS mark is the radio core\'s");\n'
         '_Static_assert(UI_AIR_FM == RADIO_AIR_FM && UI_AIR_DMR == RADIO_AIR_DMR && UI_AIR_NARROW == RADIO_AIR_NARROW &&\n'
         '\t       UI_AIR_REPLACE == RADIO_AIR_REPLACE && UI_AIR_P_SHIFT == RADIO_AIR_P_SHIFT &&\n'
         '\t       UI_AIR_OFF_SHIFT == RADIO_AIR_OFF_SHIFT, "ui_svc.h: the letter bits are the radio core\'s");\n'
         'void ui_svc_air_vfo(uint32_t arg, uint32_t hz, uint16_t tone10)\n{\n\tradio_req_air_vfo(arg, hz, tone10);\n}\n\n'
         'void ui_svc_air_end(void)\n{\n\tradio_req_air_end();\n}\n'
         '#endif\n\n'
         'bool ui_svc_mwatch(const ui_dual_ref_t ref[], uint8_t n, uint8_t sel)\n{\n'),
    ])
    transform('core/ui/ui_dual.c', [
        ('static void mw_follow_zones(uint32_t from, uint32_t to);\n#endif\n#else\n',
         'static void mw_follow_zones(uint32_t from, uint32_t to);\n#endif\n'
         '#if CONFIG_AIR_WATCH\n'
         'static bool mw_air;\t\t// DDM32 Plus air watch (ui_airwatch.inc): the ring is an airfield\'s, nothing is saved\n'
         'static uint8_t mw_air_kind;\t// ... the auto scan\'s (AS_AIR, AS_RPT; 0 an airfield\'s): its letters keep labels\n'
         'static bool mw_air_end(void);\n'
         '#define MW_AIR_OFF\tTR("Air watch off")\t// its string here: tools/i18n.py reads .c and .h files only\n'
         '#endif\n'
         '#else\n'),
        ('static void state_save(void)\n{\n\tconst ui_dual_t *d = dual();\n',
         'static void state_save(void)\n{\n'
         '#if CONFIG_AIR_WATCH\n'
         '\tif (mw_air)\n\t\treturn;\t// DDM32 Plus air watch: the user\'s watch stays as saved before it\n'
         '#endif\n'
         '\tconst ui_dual_t *d = dual();\n'),
        ('\tif (!d->on)\n\t\treturn resume_poll(d, v) || adopt_poll(d, v);\n',
         '#if CONFIG_AIR_WATCH\n'
         '\tif (!d->on && mw_air)\n'
         '\t\treturn mw_air_end();\t// DDM32 Plus: the air watch stopped: the user\'s watch as it was\n'
         '#endif\n'
         '\tif (!d->on)\n\t\treturn resume_poll(d, v) || adopt_poll(d, v);\n'),
        ('#if CONFIG_MULTI_WATCH\n#include "ui_mwatch.inc"\t// DM32 Plus\n#endif\n',
         '#if CONFIG_MULTI_WATCH\n#include "ui_mwatch.inc"\t// DM32 Plus\n#endif\n'
         '#if CONFIG_AIR_WATCH\n#include "ui_airwatch.inc"\t// DDM32 Plus air watch (item 44b)\n#endif\n'),
    ])

    # Dual watch / the air watch on Home: a pane's FM channel or VFO in the air band is received in AM (radio_am.h).
    # Upstream's status bar word said "FM" (or DMR) for the selected pane and a VFO pane read "VFO FM": the air watch's
    # letters showed FM while the radio received them in AM (owner's beta.7 report)
    transform('core/ui/ui_screens.c', [
        ('static bool strip_fits(const font_t *f, int lx, int rx, const char *word)\n',
         '#if CONFIG_DUAL_WATCH && CONFIG_AM_RX\n'
         '#include "radio/radio_am.h"\n'
         '// DDM32 Plus: a pane received in AM, as the radio core decides (radio_am_channel: an FM channel or VFO in the air\n'
         '// band on a radio that receives AM; the air watch\'s letters)\n'
         'static bool dual_pane_am(const ui_dual_pane_t *p)\n'
         '{\n'
         '\treturn p->mode == RADIO_MODE_FM && radio_am_band(p->rx_hz) && radio_am_capable();\n'
         '}\n'
         '#endif\n\n'
         'static bool strip_fits(const font_t *f, int lx, int rx, const char *word)\n'),
        ('\t\tmw = s->dual.pane[s->dual.sel].mode == RADIO_MODE_DMR ? "DMR" : "FM";\n',
         '#if CONFIG_AM_RX\n'
         '\t\tmw = s->dual.pane[s->dual.sel].mode == RADIO_MODE_DMR ? "DMR" :\n'
         '\t\t     dual_pane_am(&s->dual.pane[s->dual.sel]) ? "AM" : "FM";\t// DDM32 Plus: the air band in AM\n'
         '#else\n'
         '\t\tmw = s->dual.pane[s->dual.sel].mode == RADIO_MODE_DMR ? "DMR" : "FM";\n'
         '#endif\n'),
        ('\t\tsb_str(&b, "VFO FM");\n',
         '#if CONFIG_AM_RX\n'
         '\t\tsb_str(&b, dual_pane_am(p) ? "VFO AM" : "VFO FM");\t// DDM32 Plus: the air band in AM\n'
         '#else\n'
         '\t\tsb_str(&b, "VFO FM");\n'
         '#endif\n'),
    ])
