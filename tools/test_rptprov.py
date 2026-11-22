# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests of Tools > Repeaters by province (CONFIG_RPT_PROV, docs/plus.md "Repeater list and nearest repeater"),
run by tools/test_classic.py after its own repeater tests (the build tree prepared, ASan / UBSan flags given):

- tools/rpt_list.py's province of a repeater: positions inside the map's outlines (one per province: each outline's
  own label point lands in it), just off a simplified coast (a district centre near), far out at sea with a city
  text, old names, a word's start, no position, abroad; the generated table's province bytes and comment; the real
  research-data lists when present (the distribution printed: by province, unknown, how each was placed, and the
  entries whose position and city text name different provinces, for the owner to check);
- the screen's flows in upstream's simulator (tests/plus/test_ui_rptprov.c, wrapped by tests/plus/ui_sim_mapnav.c)."""
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def fail(what):
    raise SystemExit('rpt_list provinces: ' + what)


def e5(deg):
    return round(deg * 100000)


def province_tests(ROOT):
    sys.path.insert(0, str(ROOT))
    from tools import rpt_list
    from tools.turkey_map import NAMES
    P = rpt_list.Provinces()
    if len(P.outlines) != 81 or len(P.districts) != 973:
        fail('map tables: %d outlines, %d districts' % (len(P.outlines), len(P.districts)))
    # Each province's label point (inside it, turkey_map.py) is placed in that province by the outline test
    text = (ROOT / 'integration/radiofoundry/map/turkey_map_data.c').read_text(encoding='latin-1')
    labels = re.findall(r'\{ \d+, \d+, (\d+), \d+, \{ (-?\d+), (-?\d+) \}', text)
    if len(labels) != 81:
        fail('label points')
    for plate, x, y in labels:
        lat5, lon5 = 3550000 + int(y) * 100 + 50, 2500000 + int(x) * 100 + 50
        if P.of(lat5, lon5, '') != (int(plate), 'outline'):
            fail('the label point of plate %s gives %s' % (plate, P.of(lat5, lon5, '')))
    cases = [
        # (lat, lon, city) -> (plate, how)
        ((39.9334, 32.8597, ''), (6, 'outline')),                   # Ankara, Kizilay
        ((41.0082, 28.9784, 'Ankara'), (34, 'outline')),            # in Istanbul: the position wins over the text
        ((37.0000, 35.3213, 'Adana'), (1, 'outline')),
        ((38.4192, 27.1287, ''), (35, 'outline')),                  # Izmir, Konak
        ((40.9900, 29.0300, 'Kadikoy'), (34, 'district')),          # Kadikoy's shore: off the simplified coast
        ((36.5444, 31.9954, 'Alanya'), (7, 'district')),            # Alanya: the same
        ((41.4333, 38.0767, 'Giresun'), (28, 'city')),              # 50 km out at sea: the text
        ((40.6797, 25.9397, 'Kocaeli'), (41, 'city')),              # a typo's position in the Aegean: the text
        ((None, None, 'Afyon Merkez'), (3, 'city')),                # old names
        ((None, None, 'K.Maras'), (46, 'city')),
        ((None, None, 'Icel / Tarsus'), (33, 'city')),
        ((None, None, 'ŞANLIURFA'), (63, 'city')),                  # Turkish capitals folded
        ((None, None, 'Ordualtinordu'), (52, 'city')),              # a word starting with a name of 4+
        ((None, None, 'Vanadium'), (0, 'unknown')),                 # but not Van (3 letters) inside a word
        ((None, None, 'Nowhere'), (0, 'unknown')),
        ((52.5200, 13.4050, 'Berlin'), (0, 'unknown')),             # abroad
        ((35.1856, 33.3823, 'Lefkosa'), (0, 'unknown')),            # Cyprus: no district within 30 km
    ]
    for (la, lo, city), want in cases:
        got = P.of(rpt_list.NOPOS if la is None else e5(la), rpt_list.NOPOS if lo is None else e5(lo), city)
        if got != want:
            fail('%s %s %r: %s, want %s' % (la, lo, city, got, want))
    # The table: the province byte last in each row, the distribution in a comment, the sample's own
    with tempfile.TemporaryDirectory() as tmp:
        csv_path = Path(tmp) / 'own.csv'
        csv_path.write_text('callsign,city,lat,lon,rx_mhz,offset_mhz,mode\n'
                            'TA2AAA,Ankara,39.93,32.86,145.600,-0.6,FM\n'
                            'TA2BBB,Kadikoy,40.99,29.03,145.625,-0.6,FM\n'
                            'TA2CCC,Atlantis,,,145.650,-0.6,FM\n', encoding='utf-8')
        c_text, stats = rpt_list.generate(str(csv_path), date='2026-01-02')
        rows = re.findall(r'"(TA2\w+)", "[^"]*", (\d+) \},', c_text)
        if rows != [('TA2AAA', '6'), ('TA2BBB', '34'), ('TA2CCC', '0')]:
            fail('table rows %s' % rows)
        if stats['provinces'] != {6: 1, 34: 1, 0: 1} or stats['how'] != {'outline': 1, 'district': 1, 'unknown': 1}:
            fail('stats %s %s' % (stats['provinces'], stats['how']))
        if '/* 2 provinces (06 1, 34 1), unknown 1; by district 1, outline 1, unknown 1 */' not in c_text:
            fail('the comment')
        if 'unknown 1' not in rpt_list.describe(stats, False):
            fail('the build line')
    _, stats = rpt_list.generate()
    if stats['provinces'].get(0, 0) != 1 or sum(stats['provinces'].values()) != stats['count']:
        fail('the sample: %s' % stats['provinces'])
    # The real lists (git-ignored; the owner's machine): the distribution, for the report
    data = ROOT / 'research-data'
    csv_real, json_real = data / 'repeaters.csv', data / 'rptrs.json'
    if csv_real.is_file() or json_real.is_file():
        _, stats = rpt_list.generate(str(csv_real) if csv_real.is_file() else None,
                                     str(json_real) if json_real.is_file() else None, ['Turkey'])
        dist = stats['provinces']
        print('repeaters by province (research-data): %d repeaters, %d provinces, unknown %d; by %s' % (
            stats['count'], sum(1 for p in dist if p), dist.get(0, 0),
            ', '.join('%s %d' % kv for kv in sorted(stats['how'].items()))))
        print('  ' + ', '.join('%02d %s %d' % (p, rpt_list.fold(NAMES[p - 1]), n) for p, n in sorted(dist.items()) if p))
        if sum(dist.values()) != stats['count'] or any(not 0 <= p <= 81 for p in dist):
            fail('real data distribution %s' % dist)
        if stats['differ']:
            print('  placed by position, the city text names another province (%d): %s' % (len(stats['differ']), '; '.join(
                '%s "%s" %s, not %s' % (n, c, rpt_list.fold(NAMES[p - 1]) if p else '-', rpt_list.fold(NAMES[t - 1]))
                for n, c, p, t in stats['differ'])))
    else:
        print('repeaters by province: no research-data list here (the synthetic cases above only)')


def ui_tests(ROOT, APP, OUT, host, test='test_ui_rptprov', extra=()):
    tp = ROOT / 'tests/plus'
    command = run(['make', '-Bn', 'uiFlowTest', 'SANITIZE=1'], cwd=host, capture_output=True, text=True).stdout
    argv = shlex.split(next(line for line in command.splitlines() if ' -o uiFlowTest ' in line))
    argv[argv.index('uiFlowTest.c')] = str(tp / (test + '.c'))
    argv[argv.index('sim/ui_sim.c')] = str(tp / 'ui_sim_mapnav.c')
    argv[argv.index('-o') + 1] = str(OUT / test)
    run([*argv, '-I' + str(host), '-I' + str(tp), '-DCONFIG_P25_CHANNELS=1', '-DCONFIG_MAP=1', '-DCONFIG_NAV=1',
         '-DCONFIG_REPEATERS=1', '-DCONFIG_AUTO_RPT=1', '-DCONFIG_MAP_TRACK=1', '-DCONFIG_APRS_MSG=1',
         '-DCONFIG_RPT_PROV=1', *extra,
         *(str(APP / 'core/ui' / f) for f in ('ui_map.c', 'map.c', 'turkey_map_data.c', 'turkey_district_data.c',
                                             'mapnav.c', 'ui_mapnav.c', 'nav.c', 'ui_goto.c', 'rpt.c', 'ui_rpt.c',
                                             'ui_aprsmsg.c')),
         *(str(APP / 'core/aprs' / f) for f in ('aprs_plus.c', 'aprs_fmt.c', 'ax25.c')), '-lm'], cwd=host)
    run([OUT / test], cwd=host)


def main(ROOT, APP, OUT, flags, host):
    province_tests(Path(ROOT))
    ui_tests(Path(ROOT), Path(APP), Path(OUT), Path(host))
    # * adds the picked repeater as a channel (CONFIG_CHAN_EXTRAS): the real codeplug and zone editor
    ui_tests(Path(ROOT), Path(APP), Path(OUT), Path(host), 'test_ui_rptadd',
             ('-DCONFIG_CHAN_EXTRAS=1', '-DCONFIG_DMR_ANY=1', '-DCONFIG_LANG_TR=1'))


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1]
    province_tests(root)
