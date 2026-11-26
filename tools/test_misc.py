# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of the round 3 "misc" job (docs/plus.md: Airfields, Marine VHF, QSO log, Battery graph, PC live panel),
run by tools/test_classic.py after its own (the build tree prepared, ASan / UBSan flags given):

- tools/airfields.py on a synthetic OurAirports pair (selection, refusals by reason, types, names, the 8.33 kHz
  grid, the order, the byte cap, the pins), then --check of the committed table when the pinned download is in
  research-data/ourairports (else said and skipped: the committed table is still tested below);
- the shipped airfield table and math, the marine table, the QSO log with its export bytes, the battery estimator on
  synthetic curves, the spectrum query (tests/plus/test_*.c);
- the marine receive only rule in the real radio state machine (upstream's radioCoreTest suite first, built with it);
- the new screens' UI flows in upstream's simulator (tests/plus/test_ui_misc.c)."""
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


def run(args, **kw):
    import refpack_host	# the map pack's reader, its fixture and the tables (tools/refpack_host.py)
    return subprocess.run([str(a) for a in refpack_host.patch([str(a) for a in args])], check=True, **kw)


AIRPORTS = '''"id","ident","type","name","latitude_deg","longitude_deg","elevation_ft","continent","iso_country","iso_region","municipality","scheduled_service","icao_code","iata_code","gps_code","local_code","home_link","wikipedia_link","keywords"
1,"LTAC","large_airport","Esenboğa International Airport",40.128101,32.995098,3125,"AS","TR","TR-06","Ankara","yes","LTAC","ESB","LTAC",,,,
2,"LTAD","medium_airport","Etimesgut Air Base",39.949798,32.688599,2653,"AS","TR","TR-06","Ankara","no","LTAD",,"LTAD",,,,
3,"LTXX","medium_airport","Nowhere Airport",38.0,30.0,100,"AS","TR","TR-03","X","no","LTXX",,"LTXX",,,,
4,"TR-0099","small_airport","Tiny Sport Airfield",39.5,31.5,100,"AS","TR","TR-26","Y","no",,,,,,,
5,"TR-0098","small_airport","Silent Airfield",39.6,31.6,100,"AS","TR","TR-26","Y","no",,,,,,,
6,"LTH1","heliport","Some Heliport",39.7,31.7,100,"AS","TR","TR-26","Y","no","LTH1",,,,,,
7,"LGAV","large_airport","Athens International Airport",37.93,23.94,300,"EU","GR","GR-I","Athens","yes","LGAV",,,,,,
8,"LTZZ","closed","Old Field",39.8,31.8,100,"AS","TR","TR-26","Y","no",,,,,,,
9,"LTBR","medium_airport","Akhisar Airport / Akhisar Air Base",38.80,27.83,300,"AS","TR","TR-45","Akhisar","no","LTBR",,,,,,
10,"LTFG","medium_airport","Ordu–Giresun Airport",40.96,38.08,10,"AS","TR","TR-52","Ordu","no",,,"LTFG",,,,
'''
FREQS = '''"id","airport_ref","airport_ident","type","description","frequency_mhz"
1,1,"LTAC","TWR","TOWER",118.1
2,1,"LTAC","Gokcen GND","GROUND",121.58
3,1,"LTAC","ATIS","ATIS",123.6
4,1,"LTAC","APP","APP",119.1
5,1,"LTAC","APP/DEP","APP",119.1
6,1,"LTAC","APP/DEP","APP",119.1
7,1,"LTAC","TWR","MIL",257.8
8,2,"LTAD","Tower","TWR",122.1
9,2,"LTAD","Clearance","CLD",121.62
10,2,"LTAD","GND","GND",25.125
11,4,"TR-0099","PMR","PMR",433
12,4,"TR-0099","CTAF","CTAF",123.5
13,6,"LTH1","TWR","TWR",130.0
14,7,"LGAV","TWR","TWR",118.1
15,2,"LTAD","HAVAS","OPS",131.45
16,2,"LTAD","XYZ","?",136.99
17,9,"LTBR","GND","GND",abc
'''


def airfields_tests(ROOT):
    sys.path.insert(0, str(ROOT))
    from tools import airfields
    fields, stats = airfields.build(AIRPORTS, FREQS)
    codes = [f['code'] for f in fields]
    # heliport, closed, foreign, a small field without a usable frequency: out; a medium one without any: in
    assert codes == ['LTAC', 'LTAD', 'LTBR', 'LTFG', 'LTXX', 'TR-0099'], codes
    by = {f['code']: f for f in fields}
    assert by['LTAC']['freqs'] == [('ATIS', 123600), ('GND', 121580), ('TWR', 118100), ('APP', 119100)], by['LTAC']
    assert by['LTAD']['freqs'] == [('TWR', 122100), ('OPS', 131450), ('OTH', 136990)], by['LTAD']
    assert by['TR-0099']['freqs'] == [('CTAF', 123500)] and by['LTXX']['freqs'] == []
    assert stats['refused'] == {'band': 3, 'duplicate': 2, 'grid': 1, 'not a number': 1}, stats['refused']
    assert stats['without_frequency'] == 3 and stats['airfields'] == 6 and stats['frequencies'] == 8, stats
    assert by['LTAC']['name'] == 'Esenboğa' and by['LTAD']['name'] == 'Etimesgut AB' and by['LTBR']['name'] == 'Akhisar'
    assert by['LTFG']['name'] == 'Ordu-Giresun' and by['TR-0099']['name'] == 'Tiny' and by['LTAC']['kind'] == 0
    # 121.62 is not a channel name (x.x20 does not exist on the 8.33 kHz raster): refused as off the grid, and the
    # one that was: 121.580 -> its grid point 121.575; x.010 and x.015 one and two points on
    assert airfields.channel_khz('121.62') == (None, 'grid') and airfields.channel_khz('121.625') == (121625, None)
    assert [airfields.tune_hz(k) for k in (121580, 121585, 121590, 121600, 136990)] == \
        [121575000, 121583333, 121591667, 121600000, 136991667]
    assert airfields.channel_khz('118.0001') == (None, 'grid') and airfields.channel_khz('137') == (None, 'band')
    assert airfields.channel_khz('117.975') == (None, 'band') and airfields.channel_khz('118') == (118000, None)
    assert airfields.norm_type('Approach') == 'APP' and airfields.norm_type('Antalya APP') == 'APP'
    assert airfields.short_name('Istanbul Sabiha Gökçen International Airport') == 'Istanbul Sabiha Gökçen'
    assert airfields.short_name('Kâhta Airport') == 'Kahta'  # the fonts have no a, i, u with a circumflex
    with tempfile.TemporaryDirectory() as tmp:
        a, f = Path(tmp) / 'airports.csv', Path(tmp) / 'freqs.csv'
        a.write_text(AIRPORTS, encoding='utf-8')
        f.write_text(FREQS, encoding='utf-8')
        out = Path(tmp) / 'out'
        # The pins: a file that is not the pinned download is refused
        r = subprocess.run(['python3', '-I', ROOT / 'tools/airfields.py', '--airports', a, '--freqs', f,
                            '--out-dir', out], capture_output=True, text=True)
        if r.returncode == 0 or 'SHA-256' not in r.stderr:
            raise SystemExit('airfields.py took an unpinned input')
        run(['python3', '-I', ROOT / 'tools/airfields.py', '--unpinned', '--airports', a, '--freqs', f, '--out-dir', out])
        c = (out / 'airfield_data.c').read_text(encoding='utf-8')
        assert '"LTAC", "Esenbo\\360a"' in c and '{ 2716, AIR_GND }' in c and 'air_field_count = AIR_FIELDS' in c, c
        run(['python3', '-I', ROOT / 'tools/airfields.py', '--unpinned', '--airports', a, '--freqs', f, '--out-dir', out,
             '--check'])
        # The byte cap
        old = airfields.MAX_BYTES
        airfields.MAX_BYTES = 100
        try:
            airfields.build(AIRPORTS, FREQS)
            raise SystemExit('airfields.py made a table over its byte cap')
        except airfields.AirError:
            pass
        finally:
            airfields.MAX_BYTES = old
    data = ROOT / 'research-data/ourairports'
    if (data / 'airports.csv').exists() and (data / 'airport-frequencies.csv').exists():
        run(['python3', '-I', ROOT / 'tools/airfields.py', '--check'], cwd=ROOT)
        print('airfields.py --check: the committed table is the pinned download\'s')
    else:
        print('airfields.py --check skipped: research-data/ourairports/ has no download (the committed table is tested)')


def main(ROOT, APP, OUT, flags, host):
    airfields_tests(ROOT)
    tp = ROOT / 'tests/plus'
    units = [
        ('test_airfield', ['-DCONFIG_AIRFIELDS=1', '-DCONFIG_APRS=1'],
         [APP / 'core/ui/airfield.c', APP / 'core/ui/airfield_data.c', APP / 'core/aprs/aprs_fmt.c', '-lm']),
        ('test_marine', ['-DCONFIG_MARINE=1'], [APP / 'core/radio/marine.c']),
        ('test_qso', ['-DCONFIG_QSO_LOG=1', '-DCONFIG_CALL_LOG=1', '-DCONFIG_P25_EXTRAS=1'], [APP / 'core/ui/qso_log.c']),
        ('test_battgraph', ['-DCONFIG_BATT_GRAPH=1'], [APP / 'core/ui/battgraph.c']),
        ('test_spectrum_export', ['-DCONFIG_SPECTRUM=1', '-DCONFIG_PC_LIVE=1'], [APP / 'core/radio/spectrum.c']),
    ]
    for name, defs, sources in units:
        run(['clang', *flags, *defs, tp / (name + '.c'), *sources, '-o', OUT / name], cwd=ROOT)
        run([OUT / name], cwd=ROOT)
    # The marine rule in the real radio state machine: upstream's radio suite first, with it built in
    fixture = (host / 'radioCoreTest.c').read_text().replace('int main(void)', 'int upstream_radio_main(void)')
    wrapper = OUT / 'radioCoreMarine.c'
    wrapper.write_text(fixture + '\n#include "' + str(tp / 'test_marine_sm.inc') + '"\n')
    command = run(['make', '-Bn', 'radioCoreTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o radioCoreTest ' in line))
    argv[argv.index('radioCoreTest.c')] = str(wrapper)
    argv[argv.index('-o') + 1] = str(OUT / 'test_radio_marine')
    run([*argv, '-I' + str(host), '-DCONFIG_MARINE=1', str(APP / 'core/radio/marine.c')], cwd=host)
    run([OUT / 'test_radio_marine'], cwd=host)
    # The screens in upstream's simulator (wrapped by tests/plus/ui_sim_misc.c)
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / 'test_ui_misc.c')
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_misc.c')
    argv[argv.index('-o') + 1] = str(OUT / 'test_ui_misc')
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_AM_RX=1', '-DCONFIG_APRS=1',
         '-DCONFIG_VFO=1', '-DCONFIG_AIRFIELDS=1', '-DCONFIG_MARINE=1', '-DCONFIG_QSO_LOG=1', '-DCONFIG_BATT_GRAPH=1',
         '-DCONFIG_LARGE_SCALE=1', '-DCONFIG_HOME_FREQ=1',
         *(str(APP / 'core/ui' / f) for f in ('airfield.c', 'airfield_data.c', 'ui_airfield.c', 'air_pack.c', 'ui_marine.c',
                                             'qso_log.c', 'ui_qso.c', 'battgraph.c', 'ui_battgraph.c')),
         str(APP / 'core/radio/marine.c'), str(APP / 'core/aprs/aprs_fmt.c'), '-lm'], cwd=host)
    run([OUT / 'test_ui_misc'], cwd=host)
