#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus DOTA Transfer (docs/dota.md): channels, zones and the DMR contacts / RX group lists they use, sent from
one radio to another over FM (AFSK 1200 in AX.25 UI frames, CTCSS) or DMR (unconfirmed UDP over the data service).
Called last from plus_features.prepare(); a module of its own so the shared prepare list gains one line. Our code is
in integration/radiofoundry/dota/ (installed into core/radio/, core/codeplug/ and core/ui/); upstream files change only
through the hunks below. Switch CONFIG_DOTA (needs APRS, APRS_RX, APRS_MSG, DMR and ZONE_EDIT)."""

DOTA = 'integration/radiofoundry/dota/'

# Lines for the stack check's table (tools/stack_check_edges.txt syntax; build_radiofoundry.py adds them to a Plus
# build's). The session's transport calls (dota_io_t.key / .beep) are filled in radio_dota.inc, which the source scanner
# doesn't read: without these the radio task's chains through them are skipped. The receiver's storage job runs on the
# start-up task through storage_job_poll's pointer; the scanner finds it from storage_job_post, named here as well so
# the job is counted even if that call ever moves out of its sight.
STACK_EDGES = (
    '# DDM32 Plus DOTA (tools/plus_dota.py)\n'
    'call finish_send core/radio/radio_dota.inc:dota_key\n'
    'call send_start core/radio/radio_dota.inc:dota_key\n'
    'call conclude core/radio/radio_dota.inc:dota_beep\n'
    'call dota_poll core/radio/radio_dota.inc:dota_beep core/radio/radio_dota.inc:dota_key\n'
    'call storage_job_poll dota_job\n'
)

# (installed path, our file)
FILES = [
    ('core/radio/dota_proto.h', 'dota_proto.h'),
    ('core/radio/dota_proto.c', 'dota_proto.c'),
    ('core/radio/dota_rec.h', 'dota_rec.h'),
    ('core/radio/dota_rec.c', 'dota_rec.c'),
    ('core/radio/dota_import.h', 'dota_import.h'),
    ('core/radio/dota_import.c', 'dota_import.c'),
    ('core/codeplug/codeplug_dota.inc', 'codeplug_dota.inc'),
    ('core/radio/dota.h', 'dota.h'),
    ('core/radio/dota.c', 'dota.c'),
    ('core/radio/radio_dota.inc', 'radio_dota.inc'),
    ('core/ui/ui_dota.c', 'ui_dota.c'),
    ('core/ui/ui_dota.h', 'ui_dota.h'),
]

ON = '#if CONFIG_DOTA\n'


def prepare_dota(transform, install):
    for dst, src in FILES:
        install(dst, DOTA + src)

    # The switch, after Clock tools' (the end of the Plus switches)
    transform('config/features.mk', [
        ('CONFIG_CLOCK_TOOLS ?= n\nFEATURES += CLOCK_TOOLS\n',
         'CONFIG_CLOCK_TOOLS ?= n\nFEATURES += CLOCK_TOOLS\n'
         'CONFIG_DOTA ?= n\nFEATURES += DOTA\n'
         # needs APRS, APRS_RX, APRS_MSG (the text entry), DMR and ZONE_EDIT: off where a board's build lacks one (the
         # registry asks every board)
         'ifneq ($(CONFIG_APRS)$(CONFIG_APRS_RX)$(CONFIG_APRS_MSG)$(CONFIG_DMR)$(CONFIG_ZONE_EDIT),yyyyy)\n'
         'override CONFIG_DOTA := n\nendif\n'),
    ])
    transform('core/config.h', [
        ('#ifndef CONFIG_CLOCK_TOOLS\n#define CONFIG_CLOCK_TOOLS 0\n#endif\n',
         '#ifndef CONFIG_CLOCK_TOOLS\n#define CONFIG_CLOCK_TOOLS 0\n#endif\n'
         '#ifndef CONFIG_DOTA\n#define CONFIG_DOTA 0\t/* DDM32 Plus DOTA Transfer (docs/dota.md) */\n#endif\n'),
    ])

    # The codeplug's staging (docs/dota.md 10, codeplug_dota.inc): codeplug_set_channel's body, able to leave the
    # in-use bit (and the serial) alone; the staging and commit calls at the end of the file
    transform('core/codeplug/codeplug.c', [
        ('bool codeplug_set_channel(uint16_t number, const channel_t *ch)\n{\n\tuint8_t r[CH_SIZE];',
         '// live false (DDM32 Plus DOTA staging, codeplug_dota.inc): the record and its tag, not its in-use bit\n'
         'static bool set_channel_as(uint16_t number, const channel_t *ch, bool live)\n{\n\tuint8_t r[CH_SIZE];'),
        ('\tif (ok && !is_used(number)) {\n\t\tset_used(number, true);\t\t// the record first, then its in-use bit\n',
         '\tif (ok && live && !is_used(number)) {\n\t\tset_used(number, true);\t\t// the record first, then its in-use bit\n'),
        ('\tif (ok) {\n\t\tfind_max();\n\t\tserial++;\n\t}\n\tstorage_unlock();\n\treturn ok;\n}\n\n'
         'bool codeplug_delete_channel(uint16_t number)\n',
         '\tif (ok && live) {\n\t\tfind_max();\n\t\tserial++;\n\t}\n\tstorage_unlock();\n\treturn ok;\n}\n\n'
         'bool codeplug_set_channel(uint16_t number, const channel_t *ch)\n{\n\treturn set_channel_as(number, ch, true);\n}\n\n'
         'bool codeplug_delete_channel(uint16_t number)\n'),
        ('\n#include "classic_standard.inc"\n',
         '\n#include "classic_standard.inc"\n'
         '#if CONFIG_DOTA\n#include "codeplug_dota.inc"\t// DDM32 Plus DOTA Transfer: staging and the commit (docs/dota.md 10)\n'
         '#endif\n'),
    ])

    # --- FM: the AFSK path (docs/dota.md 4.2) ---
    # A buffer queued behind the one playing (one transmission of many frames from two halves), the sender's speaker
    # monitor of its own tones, and the amplifier's click for the DMR sender (docs/dota.md 13)
    transform('hal/hal_afsk.h', [
        ('\tvoid (*tx_stop)(void);\n',
         '\tvoid (*tx_stop)(void);\n'
         '\t// DDM32 Plus (DOTA, docs/dota.md 4.2, 13; NULL where a board lacks them): queue the next tone buffer behind the\n'
         '\t// one playing (false when one waits already or the transmission ended), whether one waits, the speaker\n'
         '\t// playing our own tones while we transmit, and one click of the speaker amplifier\n'
         '\tbool (*tx_more)(const uint8_t *tones, uint16_t nbits);\n'
         '\tbool (*tx_queued)(void);\n'
         '\tvoid (*monitor)(bool on);\n'
         '\tvoid (*click)(void);\n'),
    ])
    transform('platform/hrc7000/hrc_afsk.h', [
        ('\tint16_t amp_per_level;\n} hrc_afsk_board_t;',
         '\tint16_t amp_per_level;\n'
         '\t// DDM32 Plus (DOTA): the speaker on our own tones while transmitting; one amplifier click. May be NULL\n'
         '\tvoid (*monitor)(bool on);\n\tvoid (*click)(void);\n} hrc_afsk_board_t;'),
    ])
    transform('platform/hrc7000/hrc_afsk.c', [
        ('static bool irq_set;\n',
         'static bool irq_set;\n'
         '// DDM32 Plus (DOTA): the buffer that plays next, queued by tx_more while one plays\n'
         'static const uint8_t *volatile next;\nstatic volatile uint16_t next_bits;\n'),
        ('\t\t\tsize_t n = afsk_mod(&mod, tones, nbits, amp, chunk, C7_PROMPT_CHUNK);\n',
         '\t\t\tsize_t n = afsk_mod(&mod, tones, nbits, amp, chunk, C7_PROMPT_CHUNK);\n'
         '\t\t\tif (n < C7_PROMPT_CHUNK && next != NULL) {\n'
         '\t\t\t\t// DDM32 Plus (DOTA): on into the queued buffer in the same chunk, the phase going on\n'
         '\t\t\t\ttones = next;\n\t\t\t\tnbits = next_bits;\n\t\t\t\tnext = NULL;\n'
         '\t\t\t\tmod.sample = 0;\n\t\t\t\tmod.bit = 0;\n'
         '\t\t\t\tn += afsk_mod(&mod, tones, nbits, amp, chunk + n, C7_PROMPT_CHUNK - n);\n\t\t\t}\n'),
        ('\tdone = false;\n\tpending = false;\n\tdrain = 0;\n',
         '\tdone = false;\n\tpending = false;\n\tdrain = 0;\n\tnext = NULL;\n'),
        ('#if CONFIG_APRS_RX && CONFIG_DMR\nstatic bool rx_start(void)\n',
         '// DDM32 Plus (DOTA): the next buffer, while the current one plays and none waits\n'
         'static bool tx_more(const uint8_t *t, uint16_t n)\n{\n'
         '\tuint32_t s = hrc_irq_save();\n'
         '\tbool ok = busy && !done && next == NULL && n != 0;\n'
         '\tif (ok) {\n\t\tnext_bits = n;\n\t\tnext = t;\n\t}\n'
         '\thrc_irq_restore(s);\n\treturn ok;\n}\n\n'
         'static bool tx_queued(void)\n{\n\treturn next != NULL;\n}\n\n'
         'static void monitor(bool on)\n{\n\tif (hooks != NULL && hooks->monitor != NULL)\n\t\thooks->monitor(on);\n}\n\n'
         'static void click(void)\n{\n\tif (hooks != NULL && hooks->click != NULL)\n\t\thooks->click();\n}\n\n'
         '#if CONFIG_APRS_RX && CONFIG_DMR\nstatic bool rx_start(void)\n'),
        ('\t.tx_stop = tx_stop,\n',
         '\t.tx_stop = tx_stop,\n\t.tx_more = tx_more,\n\t.tx_queued = tx_queued,\n\t.monitor = monitor,\n\t.click = click,\n'),
    ])
    transform('boards/dm32/radio_dm32.c', [
        ('#define AMP_BCAST\t0x04u\t// the broadcast receiver (dm32_bcast_audio)\n',
         '#define AMP_BCAST\t0x04u\t// the broadcast receiver (dm32_bcast_audio)\n'
         '#define AMP_DOTA\t0x08u\t// DDM32 Plus DOTA: our own AFSK while transmitting, a click (docs/dota.md 13)\n'),
        ('static bool afsk_tx_route(bool on);\n',
         'static bool afsk_tx_route(bool on);\nstatic void afsk_monitor(bool on);\nstatic void speaker_click(void);\n'
         'static bool afsk_mon;\t\t// DDM32 Plus DOTA: the sender hears its own AFSK (afsk_monitor)\n'),
        ('\t.tx_route = afsk_tx_route, .listen = aprs_listen_route, .amp_per_level = 55,\n',
         '\t.tx_route = afsk_tx_route, .listen = aprs_listen_route, .amp_per_level = 55,\n'
         '\t.monitor = afsk_monitor, .click = speaker_click,\n'),
        ('\tafsk_tx = on;\n\thrc7000_codec_mute(&modem, on, false);\n\tring_play();\n\treturn true;\n}\n',
         '\tafsk_tx = on;\n\thrc7000_codec_mute(&modem, on, false);\n\tring_play();\n'
         '\tamp(AMP_DOTA, on && afsk_mon);\t// DDM32 Plus DOTA: the speaker hears the prompt FIFO\'s tones too\n'
         '\treturn true;\n}\n\n'
         '// DDM32 Plus DOTA (docs/dota.md 13): the prompt FIFO also plays to the codec DAC (modem-hrc7000.md 8.4), so the\n'
         '// amplifier on during an AFSK transmission lets the sender hear its own tones, at the knob\'s level. Unverified\n'
         '// on the radio: the test row D-S1 decides\n'
         'static void afsk_monitor(bool on)\n{\n\tafsk_mon = on;\n\tamp(AMP_DOTA, on && afsk_tx);\n}\n\n'
         '// DDM32 Plus DOTA: one click of the amplifier (its switch-on pop). While DMR transmits, the speaker PCM is held off\n'
         '// the prompt FIFO, which feeds the modulator (modem-hrc7000.md 8.4): this is the sound left that never reaches the\n'
         '// air. Unverified on the radio (test row D-S2)\n'
         'static void speaker_click(void)\n{\n\tamp(AMP_DOTA, true);\n\thal_delay_us(300);\n\tamp(AMP_DOTA, afsk_tx && afsk_mon);\n}\n'),
    ])
    transform('core/aprs/aprs.c', [
        ('#include "codeplug/codeplug.h"\n',
         '#include "codeplug/codeplug.h"\n' + ON + '#include "radio/dota.h"\t// DDM32 Plus DOTA: its frames on its frequency\n#endif\n'),
        ('\t(void)ctx;\n\taprs_rx_frame(frame, n, a.now);\n',
         '\t(void)ctx;\n' + ON + '\tif (dota_on()) {\n\t\tdota_fm_frame(frame, n);\t// DDM32 Plus: a DOTA session listens\n\t\treturn;\n\t}\n#endif\n'
         '\taprs_rx_frame(frame, n, a.now);\n'),
        ('\tbool want = a.cfg.rx_on && r->fm_rx && r->rx_hz != 0 && r->rx_hz == aprs_cfg_freq(&a.cfg) &&\n'
         '\t\t    (own || rx_by_stream());\n',
         '\tbool want = a.cfg.rx_on && r->fm_rx && r->rx_hz != 0 && r->rx_hz == aprs_cfg_freq(&a.cfg) &&\n'
         '\t\t    (own || rx_by_stream());\n'
         + ON + '\twant = want || (r->fm_rx && own && dota_fm_listening(r->rx_hz));\t// DDM32 Plus: DOTA over FM\n#endif\n'),
    ])

    # --- DMR: DOTA's UDP port, and a count of data blocks heard for the receiver's crackle (docs/dota.md 5.2) ---
    transform('core/dmr/dmrDataService.c', [
        ('#if CONFIG_ZONE_SHARE\n#include "dmr/zoneShare.h"\n#endif\n',
         '#if CONFIG_ZONE_SHARE\n#include "dmr/zoneShare.h"\n#endif\n' + ON + '#include "radio/dota.h"\n#endif\n'),
        ('\t\tdmrDataServiceZoneShareRx(packet->src, packet->group, udp.payload, udp.length);\n\t}\n#endif\n',
         '\t\tdmrDataServiceZoneShareRx(packet->src, packet->group, udp.payload, udp.length);\n\t}\n#endif\n'
         + ON + '\t// DDM32 Plus DOTA Transfer: its frames, private to us\n'
         '\tif (!toPc && isUDP && (udp.dstPort == DOTA_PORT) && !packet->group)\n\t{\n'
         '\t\tdota_dmr_rx(packet->src, udp.payload, udp.length);\n\t}\n#endif\n'),
        ('void dmrDataServiceResetStats(void)\n{\n',
         'uint32_t dmrDataServiceRxBlockCount(void)\n{\n\treturn stats.rxBlocks;\t// DDM32 Plus DOTA: the crackle\n}\n\n'
         'void dmrDataServiceResetStats(void)\n{\n'),
    ])
    transform('core/dmr/dmrDataService.h', [
        ('void dmrDataServiceResetStats(void);\n',
         'void dmrDataServiceResetStats(void);\n'
         '// DDM32 Plus DOTA: data blocks heard since start-up (no copy of the statistics on the radio task\'s stack)\n'
         'uint32_t dmrDataServiceRxBlockCount(void);\n'),
    ])

    # --- Sounds (docs/dota.md 13): "dit dit diiit", and the data crackle ---
    transform('core/radio/beep.h', [
        ('\tBEEP_MORSE,\t\t// DM32 Plus CW practice: the Morse set with beep_set_morse (a dot without one)\n',
         '\tBEEP_MORSE,\t\t// DM32 Plus CW practice: the Morse set with beep_set_morse (a dot without one)\n'
         '\tBEEP_DOTA_MARK,\t\t// DDM32 Plus DOTA: a transfer starts / ends well ("dit dit diiit")\n'
         '\tBEEP_DOTA_DATA,\t\t// DDM32 Plus DOTA: data on the air (a crackle of short ticks)\n'),
    ])
    transform('core/radio/beep.c', [
        ('static const beep_seg_t p_message[] = { { 1000, 60 }, { 0, 30 }, { 1300, 60 }, { 0, 30 }, { 1700, 90 } };\n',
         'static const beep_seg_t p_message[] = { { 1000, 60 }, { 0, 30 }, { 1300, 60 }, { 0, 30 }, { 1700, 90 } };\n'
         '// DDM32 Plus DOTA (docs/dota.md 13)\n'
         'static const beep_seg_t p_dota_mark[] = { { 1200, 70 }, { 0, 60 }, { 1200, 70 }, { 0, 60 }, { 1600, 300 } };\n'
         'static const beep_seg_t p_dota_data[] = { { 3000, 4 }, { 0, 30 }, { 2200, 3 }, { 0, 45 }, { 3500, 5 }, { 0, 25 } };\n'),
        ('\t[BEEP_MORSE] = PAT(p_morse),\n',
         '\t[BEEP_MORSE] = PAT(p_morse),\n\t[BEEP_DOTA_MARK] = PAT(p_dota_mark),\n\t[BEEP_DOTA_DATA] = PAT(p_dota_data),\n'),
    ])

    # --- The radio core (docs/dota.md 11.3, radio_dota.inc) ---
    transform('core/radio/radio_sm.h', [
        ('#endif\n} radio_req_type_t;', '#endif\n\tRREQ_DOTA,\t\t// DDM32 Plus: arg 1 = a DOTA session starts (dota.h), 0 = it stops\n} radio_req_type_t;'),
    ])
    transform('core/radio/radio_task.c', [
        ('void radio_req_beep(uint8_t b) { post(RREQ_BEEP, b, 0); }\n',
         'void radio_req_beep(uint8_t b) { post(RREQ_BEEP, b, 0); }\n'
         + ON + 'void radio_req_dota(bool on) { post(RREQ_DOTA, on, 0); }\t// DDM32 Plus (radio/dota.h)\n#endif\n'),
    ])
    transform('core/radio/radio_sm.c', [
        ('#include "radio/radio_sm.h"\n',
         '#include "radio/radio_sm.h"\n'
         + ON + '#include "radio/dota.h"\t\t// DDM32 Plus DOTA Transfer (docs/dota.md 11.3)\n#else\n#define dota_on() false\n#endif\n'),
        ('void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n',
         ON + '#include "radio/radio_dota.inc"\t// DDM32 Plus DOTA Transfer: the radio core\'s side\n#endif\n\n'
         'void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{\n'
         + ON + '\tif (dota_request(sm, req, now_ms))\n\t\treturn;\t// DOTA\'s own, or dropped while a transfer runs\n#endif\n'),
        ('\t\taprs_rf_service(sm, &st, now_ms);\t// on the APRS frequency: no squelch, scan or time-out here\n'
         '\t\toutputs(sm);\n\t\treturn;\n\t}\n#endif\n',
         '\t\taprs_rf_service(sm, &st, now_ms);\t// on the APRS frequency: no squelch, scan or time-out here\n'
         '\t\toutputs(sm);\n\t\treturn;\n\t}\n#endif\n'
         + ON + '\tif (dota_rf_poll(sm, &st, now_ms)) {\n\t\toutputs(sm);\t// DDM32 Plus DOTA keys FM: nothing else runs\n'
         '\t\treturn;\n\t}\n#endif\n'),
        ('\tif (sm->aprs_phase != APRS_RF_OFF || sm->ptt || sm->hotspot ||',
         '\tif (dota_on() || sm->aprs_phase != APRS_RF_OFF || sm->ptt || sm->hotspot ||'),
        ('\t\tfmid = fmid && !spectrum_on();\n', '\t\tfmid = fmid && !spectrum_on() && !dota_on();\n'),
        ('\t\tmsg = msg && !spectrum_on();\n', '\t\tmsg = msg && !spectrum_on() && !dota_on();\n'),
        ('\t\tnxdn = nxdn && !spectrum_on();\n', '\t\tnxdn = nxdn && !spectrum_on() && !dota_on();\n'),
        ('\t\tdpmr = dpmr && !spectrum_on();\n', '\t\tdpmr = dpmr && !spectrum_on() && !dota_on();\n'),
        ('ZONES_MOVED(sm)) && !ZONE_WRITE_BUSY() &&', 'ZONES_MOVED(sm)) && !ZONE_WRITE_BUSY() && !dota_on() &&'),
    ])

    # --- Tools > DOTA Transfer (docs/dota.md 12, ui_dota.c), after Clock ---
    transform('core/ui/ui_nav.h', [
        ('\tSCR_CLOCK,\t\t\t// DDM32 Plus: Tools > Clock (CONFIG_CLOCK_TOOLS)\n',
         '\tSCR_CLOCK,\t\t\t// DDM32 Plus: Tools > Clock (CONFIG_CLOCK_TOOLS)\n'
         '\tSCR_DOTA, SCR_DOTA_PICK,\t// DDM32 Plus: Tools > DOTA Transfer, its picker (CONFIG_DOTA)\n'),
    ])
    transform('core/ui/ui_menus.c', [
        ('extern const ui_scr_def_t ui_clock_screen;\t// ui_clock.c (DDM32 Plus)',
         'extern const ui_scr_def_t ui_clock_screen;\t// ui_clock.c (DDM32 Plus)\n'
         'extern const ui_scr_def_t ui_dota_screen, ui_dota_pick_screen;\t// ui_dota.c (DDM32 Plus)'),
        ('#if CONFIG_CLOCK_TOOLS\n\t[SCR_CLOCK] = &ui_clock_screen,\n#endif\n',
         '#if CONFIG_CLOCK_TOOLS\n\t[SCR_CLOCK] = &ui_clock_screen,\n#endif\n'
         + ON + '\t[SCR_DOTA] = &ui_dota_screen,\n\t[SCR_DOTA_PICK] = &ui_dota_pick_screen,\n#endif\n'),
        ('T_SELCALL, T_SOS, T_HEARD, T_GPSDASH, T_CLOCK, T_CW, T_BATTERY,',
         'T_SELCALL, T_SOS, T_HEARD, T_GPSDASH, T_CLOCK, T_DOTA, T_CW, T_BATTERY,'),
        ('(i != T_CLOCK || CONFIG_CLOCK_TOOLS) &&', '(i != T_CLOCK || CONFIG_CLOCK_TOOLS) && (i != T_DOTA || CONFIG_DOTA) &&'),
        (': TRN("None"), TRN("CW decoder"),', ': TRN("None"), TRN("DOTA Transfer"), TRN("CW decoder"),'),
        ('#if CONFIG_CLOCK_TOOLS\n\tcase T_CLOCK: ui_push(SCR_CLOCK, 0); break;\n#endif\n',
         '#if CONFIG_CLOCK_TOOLS\n\tcase T_CLOCK: ui_push(SCR_CLOCK, 0); break;\n#endif\n'
         + ON + '\tcase T_DOTA: ui_push(SCR_DOTA, 0); break;\n#endif\n'),
        ('[T_CLOCK] = ART_ICON_ALMANAC + 1,', '[T_CLOCK] = ART_ICON_ALMANAC + 1, [T_DOTA] = ART_ICON_DOTA + 1,'),
    ])
    transform('core/ui/ui_app.c', [
        ('#if CONFIG_CLOCK_TOOLS\n#include "ui/ui_clock.h"\n#endif\n',
         '#if CONFIG_CLOCK_TOOLS\n#include "ui/ui_clock.h"\n#endif\n' + ON + '#include "ui/ui_dota.h"\n#endif\n'),
        ('\tif (ui_clock_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n',
         '\tif (ui_clock_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n'
         + ON + '\t// DDM32 Plus DOTA Transfer: the receiver\'s storage jobs, the transfer\'s redraws (ui_dota.c)\n'
         '\tif (ui_dota_tick(stack[sp].scr, now))\n\t\tdirty = true;\n#endif\n'),
    ])
