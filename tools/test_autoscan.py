# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the auto airfield scan and auto repeater scan (docs/autoscan-design.md, branch autoscan), run by
tools/test_classic.py after its own (the build tree prepared, ASan / UBSan flags given):

- the selection (tests/plus/test_autoscan.c, integration/radiofoundry/map/autoscan.c) on the map pack made from the
  build's tables: nearest repeaters and airfield frequencies against brute force all over the country, the refresh's
  merge and order, when a refresh is due;
- the prepared radio core: PTT asks air_talk_back before dual_talk, a scan request ends the air state first, DOTA's
  and cross-band's take end it before they tune (the radio core suites build without CONFIG_DOTA / CONFIG_XBAND
  beside CONFIG_AIR_WATCH, so the calls are checked in the text; air_session_end itself runs in
  tests/plus/test_airwatch_radio.inc);
- the ring, PTT on the letter heard and the replacement refusals in the radio core run in tools/test_air.py
  (tests/plus/test_airwatch_radio.inc), the screens there too (tests/plus/test_ui_air.c, CONFIG_AUTO_SCAN)."""
import re


def run(args, **kw):
    import test_air
    return test_air.run(args, **kw)


def body(text, signature):
    """The body of the C function that starts with signature (braces counted)"""
    start = text.index(signature)
    begin = text.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


def text_checks(APP):
    sm = (APP / 'core/radio/radio_sm.c').read_text(encoding='utf-8')
    ptt = body(sm, 'static void ptt_down(radio_sm_t *sm, uint32_t now_ms)\n{')
    assert re.search(r'air_talk_back\(sm\);[^\n]*\n#endif\n\t\tdual_talk\(sm, now_ms\);', ptt), 'ptt_down: air_talk_back'
    scan = body(sm, 'static void scan_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)\n{')
    assert scan.index('air_scan_first(sm, ') < scan.index('switch (req->arg)'), 'scan_request: air_scan_first'
    dota = (APP / 'core/radio/radio_dota.inc').read_text(encoding='utf-8')
    xb = (APP / 'core/radio/radio_xband.inc').read_text(encoding='utf-8')
    for name, text, sig in (('dota_take', dota, 'static void dota_take(radio_sm_t *sm, uint32_t now_ms)\n{'),
                            ('xb_take', xb, 'static void xb_take(radio_sm_t *sm, uint32_t now_ms)\n{')):
        b = body(text, sig)
        assert b.index('air_session_end(sm, now_ms);') < b.index('tune(sm, now_ms)'), name + ': air_session_end'
    menus = (APP / 'core/ui/ui_menus.c').read_text(encoding='utf-8')
    assert 'T_ISS, T_BATTERY, T_AIRSCAN, T_RPTSCAN, T_ZONE_OFFER, T_COUNT' in menus and 'ui_autoscan_toggle(AS_RPT)' in menus, 'Tools entries'
    print('auto scan: PTT, scan requests, DOTA and cross-band call the air watch\'s hooks; the Tools entries')


def main(ROOT, APP, OUT, flags, host):
    text_checks(APP)
    tp = ROOT / 'tests/plus'
    ui = APP / 'core/ui'
    run(['clang', *flags, '-DCONFIG_AUTO_SCAN=1', '-DCONFIG_REPEATERS=1', '-DCONFIG_AIRFIELDS=1', '-DCONFIG_APRS=1',
         tp / 'test_autoscan.c', ui / 'autoscan.c', ui / 'rpt.c', ui / 'rpt_data.c', ui / 'airfield.c',
         ui / 'airfield_data.c', ui / 'air_pack.c', APP / 'core/aprs/aprs_fmt.c', '-lm', '-o', OUT / 'test_autoscan'],
        cwd=ROOT)
    run([OUT / 'test_autoscan'], cwd=ROOT)
