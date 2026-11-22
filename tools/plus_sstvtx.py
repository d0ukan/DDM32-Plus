#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus SSTV send (CONFIG_SSTV_TX, docs/plus.md "SSTV send"): Tools > SSTV > OK sends one built-in test
picture (the DDM32 Plus card with the radio's callsign) in Robot 36, Martin M1 or PD120 on the FM simplex channel in
use, after an explicit OK. Called last from plus_features.prepare(); a module of its own so the shared prepare list
gains one line. Our code is in integration/radiofoundry/sstv/ (sstv_tx.c: the picture's audio, made on the fly;
radio_sstvtx.c / .inc: the radio core's side; ui_sstvtx.c: the page); upstream files change only through the hunks
below: board.afsk gains tx_pcm (the APRS tone path, the HR_C7000's prompt FIFO, fed from a generator instead of
the AFSK modulator), the request, the screen and the hooks. Needs SSTV, APRS (the tone path) and PLUS_ART (the logo)."""

SSTV = 'integration/radiofoundry/sstv/'
ON = '#if CONFIG_SSTV_TX\n'

# Lines for the stack check's table (tools/stack_check_edges.txt syntax; build_radiofoundry.py adds them to a Plus
# build's): the tone path's interrupt calls the generator through a pointer
STACK_EDGES = (
    '# DDM32 Plus SSTV send (tools/plus_sstvtx.py)\n'
    'call platform/hrc7000/hrc_afsk.c:fill radio_sstvtx_gen\n'
)

FILES = [
    ('core/radio/sstv_tx.h', 'sstv_tx.h'),
    ('core/radio/sstv_tx.c', 'sstv_tx.c'),
    ('core/radio/radio_sstvtx.h', 'radio_sstvtx.h'),
    ('core/radio/radio_sstvtx.c', 'radio_sstvtx.c'),
    ('core/radio/radio_sstvtx.inc', 'radio_sstvtx.inc'),
    ('core/ui/ui_sstvtx.c', 'ui_sstvtx.c'),
]


def prepare_sstvtx(transform, install):
    for dst, src in FILES:
        install(dst, SSTV + src)

    # The switch, after DOTA's (off where a board's build lacks a part it needs: the registry asks every board)
    transform('config/features.mk', [
        ('override CONFIG_DOTA := n\nendif\n',
         'override CONFIG_DOTA := n\nendif\n'
         'CONFIG_SSTV_TX ?= n\nFEATURES += SSTV_TX\n'
         'ifneq ($(CONFIG_SSTV)$(CONFIG_APRS)$(CONFIG_PLUS_ART),yyy)\noverride CONFIG_SSTV_TX := n\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_SSTV\n',
         '#ifndef CONFIG_SSTV_TX\n#define CONFIG_SSTV_TX 0\t/* DDM32 Plus SSTV send (docs/plus.md) */\n#endif\n'
         '#ifndef CONFIG_SSTV\n'),
    ])

    # --- The tone path: PCM from a generator through the prompt FIFO (hal_afsk.h tx_pcm) ---
    transform('hal/hal_afsk.h', [
        ('\tvoid (*click)(void);\n',
         '\tvoid (*click)(void);\n'
         '\t// DDM32 Plus (SSTV send; NULL where a board lacks it): play PCM from gen in place of AFSK tones, at level as\n'
         '\t// for tx_start. gen writes up to max samples (8 kHz, peak amp) and returns how many, fewer only at the end;\n'
         '\t// it runs in the board\'s interrupt. Then tx_busy and tx_stop as for tones\n'
         '\tbool (*tx_pcm)(uint16_t (*gen)(int16_t *out, uint16_t max, int16_t amp), uint8_t level);\n'),
    ])
    transform('platform/hrc7000/hrc_afsk.c', [
        ('static bool irq_set;\n',
         'static bool irq_set;\n'
         '// DDM32 Plus (SSTV send): the PCM source in place of afsk_mod while set (tx_pcm; cleared by tx_stop)\n'
         'static uint16_t (*volatile pcm_gen)(int16_t *out, uint16_t max, int16_t amp);\n'),
        ('\t\t\tsize_t n = afsk_mod(&mod, tones, nbits, amp, chunk, C7_PROMPT_CHUNK);\n',
         '\t\t\tsize_t n = pcm_gen != NULL ? pcm_gen(chunk, C7_PROMPT_CHUNK, amp) :\n'
         '\t\t\t\t\t\t      afsk_mod(&mod, tones, nbits, amp, chunk, C7_PROMPT_CHUNK);\n'),
        ('\thrc_timer_stop(TIMER_AFSK);\n\tbusy = false;\n\thrc_irq_restore(s);\n',
         '\thrc_timer_stop(TIMER_AFSK);\n\tbusy = false;\n\tpcm_gen = NULL;\n\thrc_irq_restore(s);\n'),
        ('// DDM32 Plus (DOTA): the next buffer, while the current one plays and none waits\n',
         '// DDM32 Plus (SSTV send): tx_start\'s path with the generator in place of the tones\n'
         'static bool tx_pcm(uint16_t (*gen)(int16_t *out, uint16_t max, int16_t amp), uint8_t level)\n{\n'
         '\tstatic const uint8_t none;\n'
         '\tif (gen == NULL || busy)\n\t\treturn false;\n'
         '\tpcm_gen = gen;\n'
         '\tif (!tx_start(&none, 1, AFSK_BAUD, AFSK_MARK_HZ, AFSK_SPACE_HZ, level)) {\n'
         '\t\tpcm_gen = NULL;\n\t\treturn false;\n\t}\n\treturn true;\n}\n\n'
         '// DDM32 Plus (DOTA): the next buffer, while the current one plays and none waits\n'),
        ('\t.click = click,\n', '\t.click = click,\n\t.tx_pcm = tx_pcm,\n'),
    ])

    # --- The callsign check (hole hunt B16): SSTV's, the cross-band's and the CW ID's identification. Upstream's rule (4
    # to 7 letters and digits, one of each) took "DM32UV", the factory radio name; now it must be shaped like an ITU
    # callsign: a prefix (a letter, two letters, letter + digit, digit + letter, digit + two letters), one digit, 1-4
    # letters. "TA2DOU/P" gives TA2DOU as before (the word ends at '/', '-' or a space). Special-event calls (second
    # round): 2-4 digits after a one / two character prefix ending in a letter (TC100A, YM100TC, W100AW, TC18MRT, TC1453);
    # two digits take three letters or more and only four digits may stand alone, so radio model names (DM32UV, AT878,
    # MD380, UV82, BF888) stay refused ---
    transform('core/radio/radio_cwid.c', [
        ('uint8_t radio_cwid_callsign(const char *name, char *out, uint8_t cap)\n{\n',
         '// DDM32 Plus: s (n characters, A-Z and 0-9 only) shaped like an ITU callsign: prefix, one digit, 1-4 letters;\n'
         '// or a special-event call: a one / two character prefix ending in a letter, 2-4 digits, letters (see below)\n'
         'static bool call_shape(const char *s, uint8_t n)\n{\n'
         '\tuint8_t e = n;\n'
         '\twhile (e > 0 && s[e - 1u] >= \'A\')\n\t\te--;\t\t// the suffix: letters\n'
         '\tuint8_t suf = (uint8_t)(n - e);\n'
         '\tif (suf > 4u || e < 2u)\n\t\treturn false;\n'
         '\tif (suf != 0 && e <= 4u) {\t// one digit, s[e - 1]; the prefix s[0..e - 1)\n'
         '\t\tuint8_t p = (uint8_t)(e - 1u);\n'
         '\t\tbool d0 = s[0] <= \'9\', d1 = s[1] <= \'9\';\n'
         '\t\tif (p == 1u ? !d0 : p == 2u ? !(d0 && d1) : d0 && !d1 && s[2] >= \'A\')\n\t\t\treturn true;\n\t}\n'
         '\tuint8_t d = e;\n'
         '\twhile (d > 0 && s[d - 1u] <= \'9\')\n\t\td--;\t\t// the number; s[0..d) the prefix\n'
         '\tuint8_t k = (uint8_t)(e - d);\n'
         '\tif (k < 2u || k > 4u || d == 0 || d > 2u || s[d - 1u] < \'A\')\n\t\treturn false;\n'
         '\treturn k == 4u || (k == 3u ? suf >= 1u : suf >= 3u);\n}\n\n'
         'uint8_t radio_cwid_callsign(const char *name, char *out, uint8_t cap)\n{\n'),
        ('\tif (!word_end(name[n]) || n < RADIO_CWID_CALL_MIN || !letter || !digit) {\n',
         '\tif (!word_end(name[n]) || n < RADIO_CWID_CALL_MIN || !letter || !digit || !call_shape(out, n)) {\n'),
    ])

    # --- The radio core (radio_sstvtx.inc) ---
    transform('core/radio/radio_sm.h', [
        ('\tRREQ_XBAND,\t\t// DDM32 Plus: arg 1 = a cross-band session starts (xband.h), 0 = it stops\n',
         '\tRREQ_XBAND,\t\t// DDM32 Plus: arg 1 = a cross-band session starts (xband.h), 0 = it stops\n'
         '\tRREQ_SSTV_TX,\t\t// DDM32 Plus: arg the SSTV send mode (radio_sstvtx.h), SSTV_TX_MODES = stop\n'),
    ])
    transform('core/radio/radio_task.c', [
        ('{ post_pair(RREQ_CW_BEACON, kind, interval, 0); }\n#endif\n',
         '{ post_pair(RREQ_CW_BEACON, kind, interval, 0); }\n#endif\n'
         + ON + '#include "radio/radio_sstvtx.h"\n'
         'void radio_req_sstv_tx(uint8_t mode) { post_pair(RREQ_SSTV_TX, mode, 0, 0); }\t// DDM32 Plus SSTV send\n'
         '#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "radio/radio_sm.h"\n',
         '#include "radio/radio_sm.h"\n'
         + ON + '#include "radio/radio_sstvtx.h"\t// DDM32 Plus SSTV send (radio_sstvtx.inc)\n'
         '#include "radio/radio_cwid.h"\n#endif\n'),
        ('void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n',
         ON + '#include "radio/radio_sstvtx.inc"\t// DDM32 Plus SSTV send: the radio core\'s side\n#endif\n\n'
         'void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n'
         + ON + '\tif (sstvtx_request(sm, req, now_ms))\n'
         '\t\treturn;\t// SSTV send\'s own, or the PTT press that stopped it (any other stops it, then goes on)\n'
         '#endif\n'),
        ('\tif (dota_rf_poll(sm, &st, now_ms)) {\n\t\toutputs(sm);\t// DDM32 Plus DOTA keys FM: nothing else runs\n'
         '\t\treturn;\n\t}\n#endif\n',
         '\tif (dota_rf_poll(sm, &st, now_ms)) {\n\t\toutputs(sm);\t// DDM32 Plus DOTA keys FM: nothing else runs\n'
         '\t\treturn;\n\t}\n#endif\n'
         + ON + '\tif (sstvtx_poll(sm, now_ms)) {\n\t\toutputs(sm);\t// DDM32 Plus SSTV send keys FM: nothing else runs\n'
         '\t\treturn;\n\t}\n#endif\n'),
    ])

    # --- The page: Tools > SSTV > OK ---
    transform('core/ui/ui_nav.h', [
        ('\tSCR_SSTV,\t\t\t// DM32 Plus: Tools > SSTV (CONFIG_SSTV)\n',
         '\tSCR_SSTV,\t\t\t// DM32 Plus: Tools > SSTV (CONFIG_SSTV)\n'
         '\tSCR_SSTV_TX,\t\t\t// DDM32 Plus: Tools > SSTV > OK, send the test picture (CONFIG_SSTV_TX)\n'),
        ('bool ui_sstv_tick(void);\n#endif\n',
         'bool ui_sstv_tick(void);\n#endif\n'
         + ON + '// DDM32 Plus SSTV send (ui_sstvtx.c), from ui_msg_tick every tick with the top screen: true: draw again\n'
         'bool ui_sstvtx_tick(uint8_t scr);\n#endif\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_sstv_screen;\t// ui_sstv.c (DM32 Plus)\n',
         'extern const ui_scr_def_t ui_sstv_screen;\t// ui_sstv.c (DM32 Plus)\n'
         'extern const ui_scr_def_t ui_sstvtx_screen;\t// ui_sstvtx.c (DDM32 Plus)\n'),
        ('\t[SCR_SSTV] = &ui_sstv_screen,\n',
         '\t[SCR_SSTV] = &ui_sstv_screen,\n#if CONFIG_SSTV_TX\n\t[SCR_SSTV_TX] = &ui_sstvtx_screen,\n#endif\n'),
    ])
