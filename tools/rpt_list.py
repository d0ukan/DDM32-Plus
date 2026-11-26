#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DM32 Plus repeater list: repeaters to a compact C table in program flash (CONFIG_REPEATERS, docs/plus.md
"Repeater list").

Inputs (any of them; rows of the first ones win a duplicate callsign + RX frequency):
  --csv FILE    a list the owner keeps (UTF-8, '#' lines are comments), columns (header names, any order):
                  callsign  required, up to 10 letters / digits / '-' '/' (shown as the name)
                  city      the place (ASCII folded, cut to 12 for the radio; also finds coordinates, below)
                  lat, lon  decimal degrees (north / east positive), or
                  locator   Maidenhead, 4 or 6 characters (the square's centre)
                  rx_mhz    required, the repeater's output = what the radio receives (a multiple of 1.25 kHz)
                  tx_mhz    the repeater's input = what the radio sends, or
                  offset_mhz  TX - RX ("-0.600", "-7.6"); neither = simplex
                  ctcss     the access tone in Hz ("88.5"), empty = none
                  mode      FM, NFM, DMR, C4FM (YSF, Fusion), D-STAR, P25, NXDN, M17, other ones are refused
                  cc        DMR colour code 0..15 (default 1)
                  ts        DMR slots: 1, 2, 1+2 (or "both"), empty = not known
                  bw        FM bandwidth in kHz: 25 (default) or 12.5; NFM is 12.5
  --radioid FILE  RadioID.net's repeater list as it serves it (https://radioid.net/static/rptrs.json): the DMR
                repeaters of --country (default Turkey; "Turkey" matches its "Turkiye"), status ACTIVE only. It has
                no coordinates: the city is looked up in the gazetteer.
  neither       the committed fictional sample (integration/radiofoundry/rpt/sample.csv), said in the output.

Position: lat/lon, else the locator, else the gazetteer (integration/radiofoundry/rpt/places.csv: province centres
and towns, approximate) by the city's words: a whole word first (a town before a province), then a word starting
with a name of 4+ letters ("Ordualtinordu" -> Ordu). None of them: the repeater is kept without a position (the
radio lists it after the located ones, without a distance).

Province (the plate number 1..81, 0 not known; Tools > Repeaters groups by it, CONFIG_RPT_PROV), from the map's own
tables (integration/radiofoundry/map/turkey_map_data.c and turkey_district_data.c, parsed here): (1) the position
inside a province outline (the map's even-odd test on the same simplified outlines, so the list agrees with where the
map draws the repeater); outside every outline (a simplified coast, or coordinates that are off): (2) a district
centre within 10 km, (3) the city text naming a province (a whole word, a few old names such as Afyon, Icel, Maras,
Urfa, Antep, or a word starting with a name of 4+ letters), (4) a district centre within 30 km; without a position
only (3). Nothing: 0 ("Bilinmeyen" on the radio). The city text of the owner's TRAC list is the club branch that runs
the repeater, not always where it stands: the position wins when it is inside an outline.

Layout (rpt.h rpt_entry_t, 44 bytes): latitude / longitude in 1e-5 degree (RPT_NOPOS: none), RX in Hz, TX - RX
in 1.25 kHz units (int16), CTCSS in 0.1 Hz (0 none), mode (| RPT_NARROW), DMR colour code | slots << 4, name (10)
and city (12), NUL terminated, the province (one byte in what was the struct's tail padding). Sorted by RX
frequency, then name. Output is deterministic. Python stdlib only.

  python3 tools/rpt_list.py --radioid research-data/rptrs.json -o rpt_data.c
  python3 tools/rpt_list.py --csv my.csv --stats
"""
import argparse
import csv
import datetime
import io
import json
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools.iddb import fold, country_key  # noqa: E402  (the same ASCII folding as the ID database)

SAMPLE = os.path.join(ROOT, 'integration', 'radiofoundry', 'rpt', 'sample.csv')
PLACES = os.path.join(ROOT, 'integration', 'radiofoundry', 'rpt', 'places.csv')
NAME_MAX = 10           # rpt.h RPT_NAME_MAX
CITY_MAX = 12           # rpt.h RPT_CITY_MAX
ENTRY_BYTES = 44        # sizeof(rpt_entry_t)
INFO_BYTES = 32         # sizeof(rpt_info_t)
MAX_BYTES = 20 * 1024   # the flash this feature may take (docs/plus.md)
STEP_HZ = 1250
MODES = {'FM': 0, 'NFM': 0, 'DMR': 1, 'C4FM': 2, 'YSF': 2, 'FUSION': 2, 'DSTAR': 3, 'D-STAR': 3, 'P25': 4,
         'NXDN': 5, 'M17': 6}
MODE_NAMES = ['FM', 'DMR', 'C4FM', 'D-STAR', 'P25', 'NXDN', 'M17']
NARROW = 0x80           # rpt.h RPT_NARROW
NOPOS = None
TS = {'': 0, '0': 0, '1': 1, '2': 2, '1+2': 3, '12': 3, 'BOTH': 3, '1/2': 3, '1,2': 3}
MAP_DATA = os.path.join(ROOT, 'integration', 'radiofoundry', 'map', 'turkey_map_data.c')
DISTRICT_DATA = os.path.join(ROOT, 'integration', 'radiofoundry', 'map', 'turkey_district_data.c')
PROVINCES = 81
NEAR_KM, FAR_KM = 10, 30    # a district centre this close places a repeater outside every outline (steps 2 and 4)
PROV_ALIASES = {'afyon': 3, 'antep': 27, 'icel': 33, 'maras': 46, 'urfa': 63}   # old / short names in city texts


class RptError(Exception):
    pass


def _c_array(text, name):
    """The initializer of the C array `name` in a generated file (from its '= {' to the closing '};')"""
    start = text.index(name)
    start = text.index('{', text.index('=', start))
    return text[start + 1:text.index('\n};', start)]


class Provinces:
    """Which province (plate 1..81, 0 not known) a repeater is in: see the module's doc. The outlines and district
    centres are the map's generated tables (turkey_map.py / turkey_districts.py output), in 0.001 degree units from
    (25 E, 35.5 N); the point in polygon test runs in 0.0001 degree, as map.c's map_province_at does."""

    def __init__(self, map_c=MAP_DATA, district_c=DISTRICT_DATA):
        from tools.turkey_map import NAMES
        self.names = [fold(n).lower() for n in NAMES]
        self.outlines = []      # (plate, (x0, y0, x1, y1), [ring points]) in 0.0001 degree
        self.districts = []     # (x, y, plate) in 0.001 degree
        if map_c and os.path.isfile(map_c):
            with open(map_c, encoding='latin-1') as f:
                t = f.read()
            provs = re.findall(r'\{ (\d+), (\d+), (\d+), \d+, \{ -?\d+, -?\d+ \}, \{ (-?\d+), (-?\d+) \}, '
                               r'\{ (-?\d+), (-?\d+) \} \}', _c_array(t, 'turkey_map_provinces['))
            rings = re.findall(r'\{ \{ (-?\d+), (-?\d+) \}, (\d+), (\d+) \}', _c_array(t, 'turkey_map_rings['))
            steps = [(int(a), int(b)) for a, b in re.findall(r'\{ (-?\d+), (-?\d+) \}',
                                                             _c_array(t, 'turkey_map_steps['))]
            if len(provs) != PROVINCES:
                raise RptError(f'{map_c}: {len(provs)} provinces, not {PROVINCES}')
            for p in provs:
                first, count, plate = int(p[0]), int(p[1]), int(p[2])
                outline = []
                for r in rings[first:first + count]:
                    x, y, s, n = (int(v) for v in r)
                    x, y = x * 10, y * 10
                    pts = [(x, y)]
                    for dx, dy in steps[s:s + n]:
                        x, y = x + dx * 10, y + dy * 10
                        pts.append((x, y))
                    outline.append(pts)
                self.outlines.append((plate, tuple(int(v) * 10 for v in p[3:7]), outline))
        if district_c and os.path.isfile(district_c):
            with open(district_c, encoding='latin-1') as f:
                t = f.read()
            pos = [(int(a), int(b)) for a, b in re.findall(r'\{ (-?\d+), (-?\d+) \}',
                                                           _c_array(t, 'turkey_district_pos['))]
            first = [int(v) for v in re.findall(r'\d+', _c_array(t, 'turkey_district_first['))]
            if len(first) != PROVINCES + 1 or first[-1] != len(pos):
                raise RptError(f'{district_c}: district index does not match')
            for p in range(PROVINCES):
                self.districts += [(x, y, p + 1) for x, y in pos[first[p]:first[p + 1]]]

    @staticmethod
    def _inside(box, outline, px, py):
        """map.c inside(): even-odd over all of a province's rings, integer"""
        if px < box[0] or px > box[2] or py < box[1] or py > box[3]:
            return False
        inside = False
        for pts in outline:
            for (ax, ay), (bx, by) in zip(pts, pts[1:]):
                if (ay > py) != (by > py):
                    lhs, rhs = (px - ax) * (by - ay), (py - ay) * (bx - ax)
                    if (lhs < rhs) if by > ay else (lhs > rhs):
                        inside = not inside
        return inside

    def at(self, lat_e5, lon_e5):
        """The province whose outline contains the position (the first, as map_province_at), 0 outside all"""
        px, py = (lon_e5 * 100 - 250000000) // 1000, (lat_e5 * 100 - 355000000) // 1000
        for plate, box, outline in self.outlines:
            if self._inside(box, outline, px, py):
                return plate
        return 0

    def district(self, lat_e5, lon_e5, max_km):
        """The province of the nearest district centre within max_km, 0 without one"""
        x, y = (lon_e5 - 2500000) / 100, (lat_e5 - 3550000) / 100
        c = math.cos(math.radians(lat_e5 / 1e5))
        best, plate = max_km / 0.11132, 0     # 0.001 degree of latitude is 111.32 m
        for dx, dy, p in self.districts:
            d = math.hypot((dx - x) * c, dy - y)
            if d < best:
                best, plate = d, p
        return plate

    def named(self, city):
        """The province the city text names, 0 if none"""
        words = ''.join(c if c.isalnum() else ' ' for c in fold(city).lower()).split()
        for w in words:
            if w in self.names:
                return self.names.index(w) + 1
            if w in PROV_ALIASES:
                return PROV_ALIASES[w]
        for w in words:
            best = max((n for n in self.names if len(n) >= 4 and w.startswith(n)), key=len, default=None)
            if best:
                return self.names.index(best) + 1
        return 0

    def of(self, lat_e5, lon_e5, city):
        """(plate, how): how is 'outline', 'district', 'city', 'far district' or 'unknown'"""
        if lat_e5 is not NOPOS:
            p = self.at(lat_e5, lon_e5)
            if p:
                return p, 'outline'
            p = self.district(lat_e5, lon_e5, NEAR_KM)
            if p:
                return p, 'district'
        p = self.named(city)
        if p:
            return p, 'city'
        if lat_e5 is not NOPOS:
            p = self.district(lat_e5, lon_e5, FAR_KM)
            if p:
                return p, 'far district'
        return 0, 'unknown'


def fold_text(text, limit):
    """ASCII, single spaces, an all-upper or all-lower word in title case ("ISTANBUL" -> "Istanbul"), cut"""
    words = []
    for w in ''.join(c if c.isalnum() or c in "-./'" else ' ' for c in fold(text)).split():
        if w.isupper() or w.islower():
            w = w[:1].upper() + w[1:].lower()
        words.append(w)
    return ' '.join(words)[:limit].rstrip()


def load_places(path=PLACES):
    places = {}
    with open(path, encoding='utf-8') as f:
        lines = [ln for ln in f if not ln.startswith('#')]
    for r in csv.DictReader(lines):
        places[r['name'].strip().lower()] = (float(r['lat']), float(r['lon']), r['kind'].strip())
    return places


def place_of(city, places):
    """(lat, lon) of the city text from the gazetteer, None if nothing matches"""
    words = ''.join(c if c.isalnum() else ' ' for c in fold(city).lower()).split()
    exact = [places[w] for w in words if w in places]
    for kind in ('D', 'P', 'A'):
        for p in exact:
            if p[2] == kind:
                return p[:2]
    for w in words:
        best = max((n for n in places if len(n) >= 4 and w.startswith(n)), key=len, default=None)
        if best:
            return places[best][:2]
    return None


def locator_pos(loc):
    """The centre of a 4 or 6 character Maidenhead locator"""
    loc = loc.strip()
    if len(loc) not in (4, 6):
        raise ValueError(loc)
    a, b = loc[0].upper(), loc[1].upper()
    if not ('A' <= a <= 'R' and 'A' <= b <= 'R' and loc[2].isdigit() and loc[3].isdigit()):
        raise ValueError(loc)
    lon = (ord(a) - 65) * 20 - 180 + int(loc[2]) * 2
    lat = (ord(b) - 65) * 10 - 90 + int(loc[3])
    if len(loc) == 4:
        return lat + 0.5, lon + 1.0
    c, d = loc[4].lower(), loc[5].lower()
    if not ('a' <= c <= 'x' and 'a' <= d <= 'x'):
        raise ValueError(loc)
    return lat + (ord(d) - 97) / 24 + 1 / 48, lon + (ord(c) - 97) / 12 + 1 / 24


def mhz_hz(text):
    """'439.16250' -> 439162500 (exact decimal, no float rounding)"""
    t = text.strip().replace(',', '.')
    if not t:
        raise ValueError('empty')
    neg = t.startswith('-')
    t = t.lstrip('+-')
    whole, _, frac = t.partition('.')
    if not whole.isdigit() and whole != '' or (frac and not frac.isdigit()) or len(frac) > 6 or (not whole and not frac):
        raise ValueError(text)
    hz = int(whole or '0') * 1000000 + int((frac + '000000')[:6])
    return -hz if neg else hz


class Row:
    __slots__ = ('name', 'city', 'lat', 'lon', 'rx', 'off', 'ctcss10', 'mode', 'cc', 'ts', 'narrow', 'place', 'prov')


def make_row(name, city, pos, rx_hz, tx_hz, ctcss, mode, cc, ts, bw):
    """A checked Row, or RptError(reason)"""
    r = Row()
    name = name.strip().upper()
    if not name or len(name) > NAME_MAX or any(not (c.isascii() and (c.isalnum() or c in '-/')) for c in name):
        raise RptError('callsign')
    r.name = name
    r.city = fold_text(city, CITY_MAX)
    r.place, r.prov = city, 0           # the whole city text (the province's step 3); generate sets prov
    m = mode.strip().upper().replace(' ', '')
    if m not in MODES:
        raise RptError('mode')
    r.mode = MODES[m]
    if not 1000000 <= rx_hz <= 1300000000 or rx_hz % STEP_HZ:
        raise RptError('frequency')
    off = (tx_hz if tx_hz is not None else rx_hz) - rx_hz
    if off % STEP_HZ or not -32768 <= off // STEP_HZ <= 32767 or not 1000000 <= rx_hz + off <= 1300000000:
        raise RptError('offset')
    r.rx, r.off = rx_hz, off // STEP_HZ
    r.ctcss10 = 0
    if ctcss.strip():
        try:
            t = round(float(ctcss.strip().replace(',', '.')) * 10)
        except ValueError:
            raise RptError('tone')
        if not 670 <= t <= 2541:
            raise RptError('tone')
        r.ctcss10 = t
    r.cc, r.ts = 0, 0
    if r.mode == 1:
        try:
            r.cc = int(cc.strip() or '1')
        except ValueError:
            raise RptError('dmr')
        tsk = ts.strip().upper().replace('TS', '').replace(' ', '')
        if not 0 <= r.cc <= 15 or tsk not in TS:
            raise RptError('dmr')
        r.ts = TS[tsk]
    bw = bw.strip().replace(',', '.')
    if bw not in ('', '25', '12.5'):
        raise RptError('bandwidth')
    r.narrow = m == 'NFM' or bw == '12.5'
    if pos is None:
        r.lat = r.lon = NOPOS
    else:
        lat, lon = pos
        if not (-90 <= lat <= 90 and -180 <= lon <= 180):
            raise RptError('position')
        r.lat, r.lon = round(lat * 100000), round(lon * 100000)
    return r


def read_text(path):
    with open(path, 'rb') as f:
        return f.read().decode('utf-8-sig', errors='replace')


def read_csv(path, places, skipped):
    lines = [ln for ln in io.StringIO(read_text(path)) if not ln.lstrip().startswith('#')]
    reader = csv.DictReader(lines)
    if reader.fieldnames is None:
        raise RptError(f'{path}: empty')
    names = {f: f.strip().lower() for f in reader.fieldnames}
    need = {'callsign', 'rx_mhz', 'mode'}
    if not need <= set(names.values()):
        raise RptError(f'{path}: needs columns {", ".join(sorted(need))}')
    out = []
    for raw in reader:
        g = {names[k]: (v or '') for k, v in raw.items() if k is not None}
        try:
            try:
                rx = mhz_hz(g.get('rx_mhz', ''))
                tx = None
                if g.get('tx_mhz', '').strip():
                    tx = mhz_hz(g['tx_mhz'])
                elif g.get('offset_mhz', '').strip():
                    tx = rx + mhz_hz(g['offset_mhz'])
            except ValueError:
                raise RptError('frequency')
            pos = None
            if g.get('lat', '').strip() or g.get('lon', '').strip():
                try:
                    pos = (float(g['lat']), float(g['lon']))
                except (ValueError, KeyError):
                    raise RptError('position')
            elif g.get('locator', '').strip():
                try:
                    pos = locator_pos(g['locator'])
                except ValueError:
                    raise RptError('position')
            else:
                pos = place_of(g.get('city', ''), places)
            out.append(make_row(g.get('callsign', ''), g.get('city', ''), pos, rx, tx, g.get('ctcss', ''),
                                g.get('mode', ''), g.get('cc', ''), g.get('ts', ''), g.get('bw', '')))
        except RptError as e:
            skipped[str(e)] = skipped.get(str(e), 0) + 1
    return out


def read_radioid(path, countries, places, skipped):
    try:
        data = json.loads(read_text(path))
    except ValueError as e:
        raise RptError(f'{path}: not JSON ({e})')
    rows = data.get('rptrs') if isinstance(data, dict) else None
    if not isinstance(rows, list):
        raise RptError(f'{path}: not RadioID.net\'s rptrs.json (no "rptrs" list)')
    want = {country_key(c) for c in countries} or {country_key('Turkey')}
    out = []
    for r in rows:
        if not isinstance(r, dict) or country_key(str(r.get('country') or '')) not in want:
            continue
        if str(r.get('status') or '').upper() != 'ACTIVE':
            skipped['inactive'] = skipped.get('inactive', 0) + 1
            continue
        try:
            try:
                rx = mhz_hz(str(r.get('frequency') or ''))
                off = str(r.get('offset') or '').strip()
                tx = rx + mhz_hz(off) if off else None
            except ValueError:
                raise RptError('frequency')
            linked = str(r.get('ts_linked') or '').upper()
            ts = '1+2' if 'TS1' in linked and 'TS2' in linked else '1' if 'TS1' in linked else \
                '2' if 'TS2' in linked else ''
            city = str(r.get('city') or '')
            out.append(make_row(str(r.get('callsign') or ''), city, place_of(city, places), rx, tx, '', 'DMR',
                                str(r.get('color_code') if r.get('color_code') is not None else ''), ts, ''))
        except RptError as e:
            skipped[str(e)] = skipped.get(str(e), 0) + 1
    return out


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def generate(csv_path=None, radioid=None, countries=(), date=None, source=None, max_bytes=MAX_BYTES, places=PLACES,
             provinces=None):
    """(C text, stats); provinces: a Provinces (default: the map's committed tables)"""
    sample = csv_path is None and radioid is None
    if sample:
        csv_path = SAMPLE
    gaz = load_places(places)
    skipped = {}
    rows = []
    inputs = []
    if csv_path:
        rows += read_csv(csv_path, gaz, skipped)
        inputs.append(csv_path)
    if radioid:
        rows += read_radioid(radioid, countries, gaz, skipped)
        inputs.append(radioid)
    seen = set()
    kept = []
    for r in rows:
        if (r.name, r.rx) in seen:
            skipped['duplicate'] = skipped.get('duplicate', 0) + 1
            continue
        seen.add((r.name, r.rx))
        kept.append(r)
    kept.sort(key=lambda r: (r.rx, r.name, r.off))
    size = INFO_BYTES + ENTRY_BYTES * max(len(kept), 1)
    if size > max_bytes:
        raise RptError(f'{len(kept)} repeaters take {size} bytes, more than {max_bytes}: select fewer')
    if date is None:
        date = 'sample' if sample else max(
            datetime.datetime.fromtimestamp(os.path.getmtime(p), datetime.timezone.utc) for p in inputs
        ).strftime('%Y-%m-%d')
    if source is None:
        source = 'sample' if sample else 'own+RadioID.net' if csv_path and radioid else             'own list' if csv_path else 'RadioID.net'
    source = source[:15]
    nopos = sum(1 for r in kept if r.lat is NOPOS)
    modes = {}
    for r in kept:
        modes[MODE_NAMES[r.mode]] = modes.get(MODE_NAMES[r.mode], 0) + 1
    if provinces is None:
        provinces = Provinces()
    by_prov, how, differ = {}, {}, []
    for r in kept:
        r.prov, h = provinces.of(r.lat, r.lon, r.place)
        by_prov[r.prov] = by_prov.get(r.prov, 0) + 1
        how[h] = how.get(h, 0) + 1
        named = provinces.named(r.place)
        if named and named != r.prov:   # the city text names another province (a club branch's, or a wrong position)
            differ.append((r.name, r.place, r.prov, named))
    out = ['/* SPDX-License-Identifier: GPL-3.0-or-later */',
           '/* Generated by tools/rpt_list.py from %s (%s): do not edit.' % (
               'integration/radiofoundry/rpt/sample.csv' if sample else ', '.join(os.path.basename(p) for p in inputs),
               date),
           ' * Repeaters: %s. */' % ('fictional sample' if sample else source),
           '#include "config.h"', '#if CONFIG_REPEATERS', '#include "ui/rpt.h"', '',
           '/* %d repeaters (%s), %d without a position, %d skipped (%s), %d bytes */' % (
               len(kept), ', '.join('%s %d' % kv for kv in sorted(modes.items())) or 'none', nopos,
               sum(skipped.values()), ', '.join('%s %d' % kv for kv in sorted(skipped.items())) or 'none', size),
           '/* %s */' % describe_provinces(by_prov, how),
           'const rpt_info_t rpt_info = { %du, %d, %s, %s };' % (len(kept), 1 if sample else 0, c_str(date[:10]),
                                                               c_str(source)),
           'const rpt_entry_t rpt_table[] = {']
    for r in kept:
        lat = 'RPT_NOPOS' if r.lat is NOPOS else str(r.lat)
        lon = 'RPT_NOPOS' if r.lon is NOPOS else str(r.lon)
        out.append('\t{ %s, %s, %du, %d, %du, %d, 0x%02x, %s, %s, %d },' % (
            lat, lon, r.rx, r.off, r.ctcss10, r.mode | (NARROW if r.narrow else 0), r.cc | r.ts << 4,
            c_str(r.name), c_str(r.city), r.prov))
    if not kept:
        out.append('\t{ RPT_NOPOS, RPT_NOPOS, 0u, 0, 0u, 0, 0x00, "", "", 0 },')
    out += ['};', '#endif', '']
    stats = {'count': len(kept), 'nopos': nopos, 'skipped': skipped, 'bytes': size, 'date': date,
             'modes': modes, 'source': source, 'provinces': by_prov, 'how': how, 'differ': differ, 'rows': kept,
             'sample': sample}
    return '\n'.join(out), stats


SOURCES = ('own', 'radioid')
VHF_BASE, VHF_TOP = 135993750, 174006250     # rpt.h RPT_FQ_VHF_BASE: 136 MHz - 6,250 Hz .. 174 MHz + 6,250 Hz
UHF_BASE, UHF_TOP = 399993750, 480006250


def sources_split(text):
    """'own,radioid' -> ['own', 'radioid'] (the --freq-sources / $RPT_*_SOURCES switches)"""
    out = [s.strip().lower() for s in text.split(',') if s.strip()]
    bad = [s for s in out if s not in SOURCES]
    if bad:
        raise RptError(f'repeater sources take {", ".join(SOURCES)}, not {", ".join(bad)}')
    return out


def freq_set(rows):
    """The TX refusal set of rows (docs/mappack-design.md C.7): every output and, with an offset, input; the ones in
    136-174 / 400-480 MHz (widened by 6,250 Hz: an entry on the rim still matches inside the band) as sorted, unique
    steps of 1.25 kHz above each band's base: (vhf steps, uhf steps)"""
    vhf, uhf = set(), set()
    for r in rows:
        rx, off = (r.rx, r.off) if hasattr(r, 'rx') else (r[2], r[3])
        for hz in ((rx, rx + off * STEP_HZ) if off else (rx,)):
            for base, top, s in ((VHF_BASE, VHF_TOP, vhf), (UHF_BASE, UHF_TOP, uhf)):
                if base <= hz <= top and (hz - base) % STEP_HZ == 0:
                    s.add((hz - base) // STEP_HZ)
    return sorted(vhf), sorted(uhf)


def freqs_c(rows, source):
    """build/rpt/rpt_freqs.c: the refusal set of rows, frequencies only (no name, place, position, mode or tone)"""
    vhf, uhf = freq_set(rows)

    def arr(name, steps):
        lines = ['const uint16_t %s[] = {' % name]
        for i in range(0, len(steps), 12):
            lines.append('\t' + ' '.join('%du,' % s for s in steps[i:i + 12]))
        if not steps:
            lines.append('\t0u,\t/* (none; the count says 0) */')
        lines.append('};')
        lines.append('const uint16_t %s_n = %du;' % (name, len(steps)))
        return lines
    out = ['/* SPDX-License-Identifier: GPL-3.0-or-later */',
           '/* Generated by tools/rpt_list.py: do not edit. The TX refusal set (docs/mappack-design.md C.7): the output',
           ' * and input frequencies of %d repeaters (%s), %d frequencies, as 1.25 kHz steps above 135.99375 /' % (
               len(rows), source, len(vhf) + len(uhf)),
           ' * 399.99375 MHz. Frequencies only. */',
           '#include "config.h"', '#if CONFIG_REPEATERS', '#include "ui/rpt.h"', '']
    out += arr('rpt_fq_vhf', vhf) + arr('rpt_fq_uhf', uhf) + ['#endif', '']
    return '\n'.join(out)


def listed_old(rows, hz):
    """The pre-1.1 known_repeater loop over the rows (ui_xband.c): within +-6,250 Hz of an output or input"""
    for r in rows:
        rx, off = (r.rx, r.off) if hasattr(r, 'rx') else (r[2], r[3])
        inp = rx + off * STEP_HZ
        if (hz + 6250 > rx and hz < rx + 6250) or (off != 0 and hz + 6250 > inp and hz < inp + 6250):
            return True
    return False


def listed_set(sets, hz):
    """rpt.c rpt_freq_listed on freq_set() output (the same search and Hz test)"""
    import bisect
    for (lo, hi, base), steps in zip(((135993751, 174006249, VHF_BASE), (399993751, 480006249, UHF_BASE)), sets):
        if lo <= hz <= hi:
            d = hz - 6249 - base
            want = 0 if d <= 0 else (d + STEP_HZ - 1) // STEP_HZ
            k = bisect.bisect_left(steps, want)
            if k >= len(steps):
                return False
            f = base + steps[k] * STEP_HZ
            return hz + 6250 > f and hz < f + 6250
    return False


def check_freq_set(rows, sets):
    """The set agrees with the old loop over its rows (docs/mappack-design.md T15, run by the build on the real list):
    every frequency of the bands in 625 Hz steps, the 2 m and 70 cm amateur bands in 10 Hz steps, each output's and
    input's boundaries and the band rims. Returns how many frequencies it compared; RptError on a difference"""
    checks = list(range(136000000, 174000001, 625)) + list(range(400000000, 480000001, 625))
    checks += list(range(144000000, 146000001, 10)) + list(range(430000000, 440000001, 10))
    for r in rows:
        rx, off = (r.rx, r.off) if hasattr(r, 'rx') else (r[2], r[3])
        for f in (rx, rx + off * STEP_HZ):
            checks += [f - 6250, f - 6249, f + 6249, f + 6250]
    checks += [135993751, 135999999, 136000000, 136006249, 173993751, 174006249, 399993751, 399999999, 400000000,
               480006249]
    import bisect
    with_off = {r.rx if hasattr(r, 'rx') else r[2] for r in rows}
    with_off |= {(r.rx + r.off * STEP_HZ) if hasattr(r, 'rx') else (r[2] + r[3] * STEP_HZ) for r in rows
                 if (r.off if hasattr(r, 'rx') else r[3]) != 0}
    every = sorted(with_off)

    def old(hz):	# listed_old by search: an output or input f with hz - 6250 < f < hz + 6250
        k = bisect.bisect_right(every, hz - 6250)
        return k < len(every) and every[k] < hz + 6250
    for hz in checks:
        inside = 135993751 <= hz <= 174006249 or 399993751 <= hz <= 480006249
        if inside and old(hz) != listed_set(sets, hz):
            raise RptError(f'the refusal set differs from its rows at {hz} Hz')
        if not inside and listed_set(sets, hz):
            raise RptError(f'the refusal set refuses {hz} Hz outside its bands')
    return len(checks)


def describe_provinces(by_prov, how):
    """'63 provinces (06 14, 34 30, ...), unknown 0; by outline 240, district 8, city 4'"""
    return '%d provinces (%s), unknown %d; by %s' % (
        sum(1 for p in by_prov if p), ', '.join('%02d %d' % (p, n) for p, n in sorted(by_prov.items()) if p) or 'none',
        by_prov.get(0, 0), ', '.join('%s %d' % kv for kv in sorted(how.items())) or 'none')


def describe(stats, sample):
    return 'Repeaters: %s%d (%s), %d without a position, %d skipped%s, %d bytes, %s; %d provinces, unknown %d' % (
        'SAMPLE (no research-data list: fictional) ' if sample else '', stats['count'],
        ', '.join('%s %d' % kv for kv in sorted(stats['modes'].items())) or 'none', stats['nopos'],
        sum(stats['skipped'].values()),
        (' (' + ', '.join('%s %d' % kv for kv in sorted(stats['skipped'].items())) + ')') if stats['skipped'] else '',
        stats['bytes'], stats['date'], sum(1 for p in stats['provinces'] if p), stats['provinces'].get(0, 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--csv', help='the owner\'s repeater CSV (columns above)')
    ap.add_argument('--radioid', help='RadioID.net rptrs.json')
    ap.add_argument('--country', action='append', default=[], help='RadioID country (repeatable; default Turkey)')
    ap.add_argument('--date', help='the date shown on the radio (default: the newest input file\'s date, UTC)')
    ap.add_argument('--max-bytes', type=int, default=MAX_BYTES)
    ap.add_argument('--stats', action='store_true', help='print counts and bytes only')
    ap.add_argument('--provinces', action='store_true', help='also print the repeaters by province (stderr)')
    ap.add_argument('-o', '--out')
    ap.add_argument('--freqs-out', help='also write the TX refusal set (rpt_freqs.c, frequencies only)')
    ap.add_argument('--freq-sources', default='own,radioid',
                    help='which of --csv (own) and --radioid feed the refusal set (default both given)')
    args = ap.parse_args()
    if args.freqs_out:
        try:
            use = sources_split(args.freq_sources)
            fcsv = args.csv if 'own' in use else None
            fjson = args.radioid if 'radioid' in use else None
            if fcsv is None and fjson is None:
                ftext, n = freqs_c([], 'none'), 0
            else:
                _, fst = generate(fcsv, fjson, args.country, args.date, max_bytes=args.max_bytes)
                check_freq_set(fst['rows'], freq_set(fst['rows']))
                ftext, n = freqs_c(fst['rows'], fst['source']), fst['count']
        except (RptError, OSError) as e:
            sys.exit(f'rpt_list: {e}')
        with open(args.freqs_out, 'w', encoding='ascii', newline='\n') as f:
            f.write(ftext)
        print(f'refusal set: {n} repeaters -> {args.freqs_out}', file=sys.stderr)
    sample = args.csv is None and args.radioid is None
    try:
        text, stats = generate(args.csv, args.radioid, args.country, args.date, max_bytes=args.max_bytes)
    except (RptError, OSError) as e:
        sys.exit(f'rpt_list: {e}')
    print(describe(stats, sample), file=sys.stderr)
    if args.provinces:
        print(describe_provinces(stats['provinces'], stats['how']), file=sys.stderr)
    if args.stats:
        return
    if args.out:
        with open(args.out, 'w', encoding='ascii', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
