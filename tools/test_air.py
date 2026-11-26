# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the round 14 air items (docs/plus.md "Round 14: air band and spectrum", branch r14-air), run by
tools/test_classic.py after its own (the build tree prepared, ASan / UBSan flags given):

- item 44a, tools/airfields.py's exclusion list: its entries well formed, applied to a synthetic OurAirports pair
  (removed by code and channel name whatever the type, counted, a small field left without a frequency not listed,
  one that matches nothing reported), and none of them in the committed table;
- item 44b, the air watch in the radio core (tests/plus/test_airwatch_radio.inc), after upstream's radio suite, the
  Classic / Plus P25 suites, the multi-watch ring and every mode in the VFO run again with it built in (and the
  priority channel: never a stop of the air watch's ring);
- item 45, the 3D waterfall's drawing into a test frame buffer (tests/plus/test_wf3d.c);
- the screens' UI flows in upstream's simulator, English and Turkish (tests/plus/test_ui_air.c; the spectrum's views
  with the toolbox job's simulator, tests/plus/test_ui_wf3d.c)."""
import re
import shlex
import subprocess
import sys


def run(args, **kw):
    import refpack_host	# the map pack's reader, its fixture and the tables (tools/refpack_host.py)
    return subprocess.run([str(a) for a in refpack_host.patch([str(a) for a in args])], check=True, **kw)


AIRPORTS = '''"id","ident","type","name","latitude_deg","longitude_deg","elevation_ft","continent","iso_country","iso_region","municipality","scheduled_service","icao_code","iata_code","gps_code","local_code","home_link","wikipedia_link","keywords"
1,"LTAD","medium_airport","Etimesgut Air Base",39.949798,32.688599,2653,"AS","TR","TR-06","Ankara","no","LTAD",,"LTAD",,,,
2,"TR-0018","small_airport","Bolu Airfield",40.748611,31.651667,2441,"AS","TR","TR-14","Bolu","no",,,,,,,
3,"LTAH","medium_airport","Afyon Air Base",38.726398,30.6011,3310,"AS","TR","TR-03","Afyon","no","LTAH",,"LTAH",,,,
'''
FREQS = '''"id","airport_ref","airport_ident","type","description","frequency_mhz"
1,1,"LTAD","TWR","TOWER",122.1
2,1,"LTAD","APP","APPROACH",119.1
3,1,"LTAD","DEP","DEPARTURE",119.1
4,2,"TR-0018","TWR","TOWER",122.1
5,2,"TR-0018","PAR","PAR",123.3
6,3,"LTAH","TWR","TOWER",122.1
7,3,"LTAH","APP","APP",122.1
'''


def exclusion_tests(ROOT):
    sys.path.insert(0, str(ROOT))
    from tools import airfields
    # The list: codes as the table has them, channel names of the band, once each, every one with a reason
    seen = set()
    for code, name, reason in airfields.EXCLUDE:
        khz, why = airfields.channel_khz(name)
        assert why is None and re.match(r'^(LT[A-Z]{2}|TR-\d{4})$', code) and reason.startswith('not in '), (code, name)
        assert (code, khz) not in seen, (code, name)
        seen.add((code, khz))
    assert len(seen) == 61, len(seen)
    # Applied: by code and channel name, every type (LTAD's APP and DEP 119.100); a small field left without a
    # frequency is not listed (Bolu); LTAH's two 122.100 rows stay (not excluded)
    ex = [('LTAD', '119.100', 'r'), ('TR-0018', '122.100', 'r'), ('TR-0018', '123.300', 'r'), ('LTAZ', '119.000', 'r')]
    fields, stats = airfields.build(AIRPORTS, FREQS, ex)
    by = {f['code']: f for f in fields}
    assert sorted(by) == ['LTAD', 'LTAH'], sorted(by)
    assert by['LTAD']['freqs'] == [('TWR', 122100)] and by['LTAH']['freqs'] == [('TWR', 122100), ('APP', 122100)]
    assert stats['excluded'] == 4 and stats['unused_exclusions'] == ['LTAZ 119.000'], stats
    # Without the list: all of them back
    fields, stats = airfields.build(AIRPORTS, FREQS, ())
    assert len(fields) == 3 and stats['excluded'] == 0 and stats['frequencies'] == 7, stats
    # A bad entry is refused: off the grid, twice, without a reason
    for bad in ([('LTAD', '119.12', 'r')], [('LTAD', '119.100', 'r'), ('LTAD', '119.1', 'r')], [('LTAD', '119.1', '')]):
        try:
            airfields.build(AIRPORTS, FREQS, bad)
            raise SystemExit('airfields.py took a bad exclusion: %r' % bad)
        except airfields.AirError:
            pass
    # The committed table holds none of them (each airfield's frequency line ends with its code)
    c = (ROOT / 'integration/radiofoundry/air/airfield_data.c').read_text(encoding='utf-8')
    table = {}
    for line in c.split('const air_freq_t air_freqs[AIR_FREQS] = {')[1].splitlines():
        m = re.match(r'\t(.*),\t/\* (\S+) \*/$', line)
        if m:
            table[m.group(2)] = {airfields.CH_BASE_KHZ + 5 * int(ch) for ch in re.findall(r'\{ (\d+), AIR_', m.group(1))}
    assert table and len(table) >= 50, len(table)
    left = sorted('%s %d' % (code, khz) for code, khz in seen if khz in table.get(code, ()))
    assert not left, left
    assert 'TR-0018' not in c and 'TR-0040' not in c, 'a small field without a frequency is listed'
    assert max(len(v) for v in table.values()) <= airfields.FREQ_MAX
    print('airfields: %d exclusions well formed, applied, none in the committed table' % len(seen))


def radio_tests(ROOT, APP, OUT, host):
    # The fixture as tools/test_classic.py builds its own (the native receiver, an AM receiver; its radio task's
    # dispatch is in OUT/classic_dispatch.inc, written there before this runs)
    fixture = (host / 'radioCoreTest.c').read_text().replace('int main(void)', 'int upstream_radio_main(void)')
    fixture = fixture.replace('const board_t board = {',
                              'const board_t board = {\n\t.native_modem=&classic_ops,\n\t.caps=CAP_AM_RX,')
    wrapper = OUT / 'radioCoreAir.c'
    wrapper.write_text('#include "native_modem/native_lab.h"\nextern const nt_ops classic_ops;\n' + fixture +
                       '\n#define main classic_radio_main\n#include "' + str(ROOT / 'tests/classic/test_radio.inc') +
                       '"\n#undef main\n#define main mwatch_radio_main\n#include "' +
                       str(ROOT / 'tests/plus/test_mwatch_radio.inc') + '"\n#undef main\n'
                       '#define main vfox_radio_main\n#include "' + str(ROOT / 'tests/plus/test_vfox_radio.inc') +
                       '"\n#undef main\n#include "' + str(ROOT / 'tests/plus/test_airwatch_radio.inc') + '"\n')
    command = run(['make', '-Bn', 'radioCoreTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o radioCoreTest ' in line))
    argv[argv.index('radioCoreTest.c')] = str(wrapper)
    argv[argv.index('-o') + 1] = str(OUT / 'test_radio_air')
    run([*argv, '-I' + str(OUT), '-I' + str(host), '-I' + str(ROOT / 'tests/native/stubs'), '-I' + str(APP / 'core'),
         '-I' + str(ROOT / 'src/modem'), '-I' + str(ROOT / 'integration/radiofoundry'), '-I' + str(ROOT / 'tests/classic'),
         '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_P25_NATIVE=1', '-DCONFIG_P25=1', '-DCONFIG_DEV_TX=1',
         '-DCONFIG_MULTI_WATCH=1', '-DCONFIG_P25_SCAN=1', '-DCONFIG_SPECTRUM=1', '-DCONFIG_FOX=1', '-DCONFIG_CW_BEACON=1',
         '-DCONFIG_P25_SEARCH=1', '-DCONFIG_P25_CALLER=1', '-DCONFIG_P25_ANY=1', '-DCONFIG_VFO_MODES=1', '-DCONFIG_YSF=1',
         '-DCONFIG_NXDN=1', '-DCONFIG_DPMR=1', '-DCONFIG_DMR_ANY=1', '-DCONFIG_AIR_WATCH=1',
         '-DCONFIG_SCAN_PRIO=1', str(ROOT / 'src/modem/native_test.c'), str(ROOT / 'integration/radiofoundry/native/native_lab.c'),
         *(str(APP / 'core/radio' / f) for f in ('spectrum.c', 'fox.c', 'cw_beacon.c', 'p25search.c', 'vfox.c'))],
        cwd=host)
    run([OUT / 'test_radio_air'], cwd=host)


def main(ROOT, APP, OUT, flags, host):
    exclusion_tests(ROOT)
    radio_tests(ROOT, APP, OUT, host)
    tp = ROOT / 'tests/plus'
    # The screens through upstream's UI simulator (tests/plus/ui_sim_air.c wraps it), English and Turkish
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_air.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_air.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_air')
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_AM_RX=1', '-DCONFIG_APRS=1',
         '-DCONFIG_VFO=1', '-DCONFIG_AIRFIELDS=1', '-DCONFIG_LANG_TR=1', '-DCONFIG_LARGE_SCALE=1',
         '-DCONFIG_HOME_FREQ=1', '-DCONFIG_MULTI_WATCH=1', '-DCONFIG_VFO_MODES=1', '-DCONFIG_AIR_WATCH=1',
         # DDM32 Plus auto scan (tools/test_autoscan.py has its selection; these are its screens and the ring)
         '-DCONFIG_AUTO_SCAN=1', '-DCONFIG_REPEATERS=1', '-DCONFIG_VFO_FM=1', '-DCONFIG_VFO_DMR=1',
         *(str(APP / 'core/ui' / f) for f in ('airfield.c', 'airfield_data.c', 'ui_airfield.c', 'air_pack.c',
                                              'rpt.c', 'rpt_data.c', 'ui_rpt.c', 'autoscan.c', 'ui_autoscan.c')),
         str(APP / 'core/aprs/aprs_fmt.c'), str(APP / 'core/radio/vfox.c'), '-lm'], cwd=host)
    run([OUT / 'test_ui_air'], cwd=host)
    # Item 45: the 3D waterfall into a test frame buffer, then Tools > Spectrum's views (the toolbox job's simulator:
    # the real sweep engine, the heap counted), English and Turkish
    wf = ['-DCONFIG_SPECTRUM=1', '-DCONFIG_SPECTRUM_WF=1', '-DCONFIG_SPECTRUM_WF3D=1']
    run(['clang', *flags, *wf, tp / 'test_wf3d.c', APP / 'core/ui/wf3d.c', APP / 'core/ui/wfall.c',
         '-o', OUT / 'test_wf3d'], cwd=ROOT)
    run([OUT / 'test_wf3d'], cwd=ROOT)
    ui = APP / 'core/ui'
    argv[argv.index(str(tp / 'test_ui_air.c'))] = str(tp / 'test_ui_wf3d.c')
    argv[argv.index(str(tp / 'ui_sim_air.c'))] = str(tp / 'ui_sim_toolbox.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_wf3d')
    # (as tools/test_toolbox.py builds its screens, with the switch and Turkish)
    run([*argv, '-I' + str(host), '-I' + str(tp), '-Wno-unused-function', '-DUI_HOST_HEAP', '-DCONFIG_P25_CHANNELS=1',
         '-DCONFIG_MAP=1', '-DCONFIG_NAV=1', '-DCONFIG_REPEATERS=1', '-DCONFIG_MAP_TRACK=1', '-DCONFIG_MAP_COVERAGE=1',
         *wf, '-DCONFIG_P25_SEARCH=1', '-DCONFIG_DMR_SEARCH=1', '-DCONFIG_DMR_ANY=1', '-DCONFIG_FIND_SAVE=1',
         '-DCONFIG_P25_TRUNK=1', '-DCONFIG_DMR_TRUNK=1', '-DCONFIG_MSG_RX=1', '-DCONFIG_CALL_LOG=1', '-DCONFIG_HEARD=1',
         '-DCONFIG_VFO_MODES=1', '-DCONFIG_GPS_DASH=1', '-DCONFIG_CLOCK_TOOLS=1', '-DCONFIG_LANG_TR=1',
         *(str(ui / f) for f in ('ui_map.c', 'map.c', 'turkey_map_data.c', 'turkey_district_data.c', 'mapnav.c',
                                 'ui_mapnav.c', 'nav.c', 'ui_goto.c', 'rpt.c', 'ui_rpt.c', 'ui_spectrum.c',
                                 'ui_p25search.c', 'ui_dmrsearch.c', 'ui_p25trunk.c', 'ui_dmrtrunk.c', 'ui_msg.c',
                                 'ui_scratch.c', 'findsave.c', 'ui_findsave.c', 'heard.c', 'ui_heard.c', 'wfall.c',
                                 'wf3d.c', 'tripm.c', 'ui_gpsdash.c', 'clockx.c', 'ui_clock.c')),
         str(APP / 'core/radio/spectrum.c'), str(APP / 'core/radio/vfox.c'), str(APP / 'core/aprs/aprs_fmt.c'), '-lm'],
        cwd=host)
    run([OUT / 'test_ui_wf3d'], cwd=host)
