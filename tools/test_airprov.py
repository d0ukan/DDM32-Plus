# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of DDM32 Plus round 16 "airprov" (docs/plus.md "Round 16: airfields by province, spectrum in colour",
branch r16-airprov), run by tools/test_classic.py after its own (the build tree prepared, ASan / UBSan flags given):

- tools/airfields.py's province of an airfield (CONFIG_AIR_PROV): synthetic OurAirports rows (inside an outline, off a
  simplified coast with a district centre near, at a border where the data's own region wins, a region far from the
  position: kept and reported, no region), the generated rows' province byte and the comment line; the committed
  table: every airfield in a province, known ones where they are, the distribution printed;
- the shipped table's walks by province (tests/plus/test_airprov.c);
- Tools > Airfields by province in upstream's simulator, English and Turkish (tests/plus/test_ui_airprov.c);
- the spectrum in colour (CONFIG_SPECTRUM_COLOR): the ramp of each theme and the 3D view drawn with it into a test
  frame buffer (tests/plus/test_wfcolor.c), then Tools > Spectrum's waterfall and 3D view in the simulator's 16 colour
  frame buffer, dark and light, English and Turkish (tests/plus/test_ui_wfcolor.c)."""
import re
import shlex
import subprocess
import sys
from pathlib import Path


def run(args, **kw):
    import refpack_host	# the map pack's reader, its fixture and the tables (tools/refpack_host.py)
    return subprocess.run([str(a) for a in refpack_host.patch([str(a) for a in args])], check=True, **kw)


def fail(what):
    raise SystemExit('airfields by province: ' + what)


HEAD = ('"id","ident","type","name","latitude_deg","longitude_deg","elevation_ft","continent","iso_country",'
        '"iso_region","municipality","scheduled_service","icao_code","iata_code","gps_code","local_code","home_link",'
        '"wikipedia_link","keywords"\n')
AIRPORTS = HEAD + '''1,"LTAC","large_airport","Esenboğa International Airport",40.128101,32.995098,3125,"AS","TR","TR-06","Ankara","yes","LTAC",,,,,,
2,"LTFJ","large_airport","Istanbul Sabiha Gökçen International Airport",40.898602,29.3092,312,"AS","TR","TR-34","Pendik, Istanbul","yes","LTFJ",,,,,,
3,"LTDB","large_airport","Çukurova International Airport",36.891478,35.071235,35,"AS","TR","TR-33","Tarsus","yes","LTDB",,,,,,
4,"LTCB","medium_airport","Ordu–Giresun Airport",40.96687,38.08599,10,"AS","TR","TR-52","Ordu","yes","LTCB",,,,,,
5,"LTXX","medium_airport","Wrong Region Airport",39.93,32.86,3000,"AS","TR","TR-35","Izmir","no","LTXX",,,,,,
6,"LTYY","medium_airport","No Region Airport",36.8969,30.7133,100,"AS","TR",,"","no","LTYY",,,,,,
'''
FREQS = '''"id","airport_ref","airport_ident","type","description","frequency_mhz"
1,1,"LTAC","TWR","TOWER",118.1
'''


def province_tests(ROOT):
    sys.path.insert(0, str(ROOT))
    from tools import airfields
    from tools.rpt_list import fold
    from tools.turkey_map import NAMES
    assert [airfields.region_plate(t) for t in ('TR-06', 'TR-81', 'TR-00', 'TR-82', 'GR-I', '', None)] == \
        [6, 81, 0, 0, 0, 0, 0]
    fields, stats = airfields.build(AIRPORTS, FREQS, ())
    got = {f['code']: f['prov'] for f in fields}
    want = {'LTAC': 6, 'LTFJ': 34, 'LTDB': 33, 'LTCB': 52, 'LTXX': 6, 'LTYY': 7}
    if got != want:
        fail('synthetic provinces %s, want %s' % (got, want))
    if stats['how'] != {'outline': 3, 'region': 2, 'district': 1} or stats['differ'] != [('LTXX', 6, 35)]:
        fail('how %s, differ %s' % (stats['how'], stats['differ']))
    # Sabiha Gokcen and Cukurova: the outline alone would say Kocaeli and Adana (the reason for the region)
    P = airfields.Provinces()
    if P.at(4089860, 2930920) != 41 or P.at(3689148, 3507124) != 1:
        fail('the outlines at the two borders changed: review REGION_KM')
    h, c = airfields.generate(fields, stats, 'test fixture')
    rows = re.findall(r'"(LT\w\w)", "[^"]*", (\d+) \},', c)
    if rows != [(k, str(v)) for k, v in sorted(want.items())]:
        fail('generated rows %s' % rows)
    if '/* 5 provinces (06 2, 07 1, 33 1, 34 1, 52 1), unknown 0; by district 1, outline 3, region 2 */' not in c:
        fail('the comment line')
    if 'by region 2' not in airfields.describe(stats) or 'LTXX 06 not 35' not in airfields.describe(stats):
        fail('the build line: ' + airfields.describe(stats))
    # The committed table: every airfield in a province, known ones where they are
    text = (ROOT / 'integration/radiofoundry/air/airfield_data.c').read_text(encoding='utf-8')
    table = {code: int(p) for code, p in re.findall(r'\{ -?\d+, -?\d+, \d+, \d+, \d, "([A-Z0-9-]+)", "[^"]*", (\d+) \}',
                                                    text)}
    n = int(re.search(r'#define AIR_FIELDS\t(\d+)u', (ROOT / 'integration/radiofoundry/air/airfield_data.h')
                      .read_text(encoding='utf-8')).group(1))
    if len(table) != n or any(not 1 <= p <= 81 for p in table.values()):
        fail('the committed table: %d rows of %d, provinces %s' % (len(table), n, sorted(set(table.values()))))
    known = {'LTAC': 6, 'LTAB': 6, 'LTFM': 34, 'LTFJ': 34, 'LTBA': 34, 'LTDB': 33, 'LTBS': 48, 'LTFE': 48, 'LTCB': 52,
             'LTAI': 7, 'LTBJ': 35, 'LTAF': 1, 'LTFO': 53, 'LTBQ': 41}
    wrong = {k: table.get(k) for k, v in known.items() if table.get(k) != v}
    if wrong:
        fail('the committed table places %s' % wrong)
    dist = {}
    for p in table.values():
        dist[p] = dist.get(p, 0) + 1
    print('airfields by province (committed table): %d airfields, %d provinces: %s' % (
        n, len(dist), ', '.join('%02d %s %d' % (p, fold(NAMES[p - 1]), k) for p, k in sorted(dist.items()))))


def ui_tests(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    core = APP / 'core'
    # The walks by province on the shipped table
    run(['clang', *flags, '-DCONFIG_AIRFIELDS=1', '-DCONFIG_APRS=1', tp / 'test_airprov.c', core / 'ui/airfield.c',
         core / 'ui/airfield_data.c', core / 'aprs/aprs_fmt.c', '-lm', '-o', OUT / 'test_airprov'], cwd=ROOT)
    run([OUT / 'test_airprov'], cwd=ROOT)
    # The screens (tests/plus/ui_sim_dpk.c: the air items' simulator with a data chip), English and Turkish
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_airprov.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_dpk.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_airprov')
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_AM_RX=1', '-DCONFIG_APRS=1',
         '-DCONFIG_VFO=1', '-DCONFIG_AIRFIELDS=1', '-DCONFIG_LANG_TR=1', '-DCONFIG_LARGE_SCALE=1',
         '-DCONFIG_HOME_FREQ=1', '-DCONFIG_MULTI_WATCH=1', '-DCONFIG_VFO_MODES=1', '-DCONFIG_AIR_WATCH=1',
         '-DCONFIG_DATAPACK=1', '-DCONFIG_MAP=1', '-DCONFIG_AIR_PROV=1',
         *(str(core / 'ui' / f) for f in ('airfield.c', 'airfield_data.c', 'ui_airfield.c', 'air_pack.c', 'map.c',
                                          'turkey_map_data.c', 'turkey_district_data.c')),
         str(core / 'storage/datapack.c'), str(core / 'storage/storage.c'), str(host / 'sim/sim_flash.c'),
         str(core / 'aprs/aprs_fmt.c'), str(core / 'radio/vfox.c'), '-lm'], cwd=host)
    run([OUT / 'test_ui_airprov'], cwd=host)


def colour_tests(ROOT, APP, OUT, flags, host):
    tp = ROOT / 'tests/plus'
    ui = APP / 'core/ui'
    wf = ['-DCONFIG_SPECTRUM=1', '-DCONFIG_SPECTRUM_WF=1', '-DCONFIG_SPECTRUM_WF3D=1', '-DCONFIG_SPECTRUM_COLOR=1']
    run(['clang', *flags, *wf, tp / 'test_wfcolor.c', ui / 'wf3d.c', ui / 'wfall.c', '-o', OUT / 'test_wfcolor'], cwd=ROOT)
    run([OUT / 'test_wfcolor'], cwd=ROOT)
    # Tools > Spectrum as tools/test_air.py builds its 3D view's flows (the toolbox job's simulator), with the switch
    run(['make', 'out/gen/lang_tables.h'], cwd=host, stdout=subprocess.DEVNULL)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_wfcolor.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_toolbox.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_wfcolor')
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
    run([OUT / 'test_ui_wfcolor'], cwd=host)


def main(ROOT, APP, OUT, flags, host):
    province_tests(Path(ROOT))
    ui_tests(Path(ROOT), Path(APP), Path(OUT), flags, Path(host))
    colour_tests(Path(ROOT), Path(APP), Path(OUT), flags, Path(host))


if __name__ == '__main__':
    province_tests(Path(__file__).resolve().parents[1])
