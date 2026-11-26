#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus map pack builder (data pack type 3, docs/mappack-design.md C.2, E.1): the Turkey map, the districts, the
repeater list and the airfields as a .dpk the PC programmer (or DOTA) writes into the radio's map pack slot
(0xEA0000 / 0xEE0000). The firmware build makes the release pack with it from the same generator outputs the firmware
linked before 1.1 (so the pack is exactly what the firmware had in program flash); a user builds their own pack with
their own repeater list the same way.

    refpack.py build --out ddm32-ref.dpk [--rpt-c build/rpt/rpt_data.c | --csv my.csv --radioid rptrs.json
                     --rpt-sources own,radioid | --no-rpt] [--version 1.1.0] [--date YYYYMMDD]
    refpack.py info pack.dpk        the header, the directory, whether the firmware takes it

Payload format 1 (all little-endian, every section 4-byte aligned): a 16-byte directory head ("REF1", directory
version 1, the section count, 2 reserved, the map coordinate id = CRC-32 of the 7 coordinate constants as LE int32 and
the charset text, 4 reserved), then 12-byte entries (id u16, record size u16, offset u32, count u32), then the
sections (ids 1-15, ref_pack.h). The header's flags carry the version hint: directory version | coordinate id's low
byte << 8. validate() applies the firmware's own rules (ref_pack.c ref_validate). Python stdlib only."""
import argparse
import datetime
import os
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import datapack  # noqa: E402

MAP_C = ROOT / 'integration/radiofoundry/map/turkey_map_data.c'
MAP_H = ROOT / 'integration/radiofoundry/map/turkey_map_data.h'
DIST_C = ROOT / 'integration/radiofoundry/map/turkey_district_data.c'
AIR_C = ROOT / 'integration/radiofoundry/air/airfield_data.c'
AIR_H = ROOT / 'integration/radiofoundry/air/airfield_data.h'
RPT_C = ROOT / 'build/rpt/rpt_data.c'

DIR_VERSION = 1
HEAD, ENTRY, MAX_SECTIONS = 16, 12, 16
(META, PROV, RING, STEP, PNAME, DPOS, DFIRST, DNFIRST, DNAME, RINFO, RPT, RKEY, AIR, AFREQ, AKEY) = range(1, 16)
REC = {META: 32, PROV: 18, RING: 8, STEP: 2, PNAME: 1, DPOS: 4, DFIRST: 2, DNFIRST: 2, DNAME: 1, RINFO: 32, RPT: 44,
       RKEY: 10, AIR: 44, AFREQ: 4, AKEY: 10}
NAMES = {META: 'map meta', PROV: 'provinces', RING: 'rings', STEP: 'steps', PNAME: 'province names',
         DPOS: 'district positions', DFIRST: 'district first', DNFIRST: 'district name first', DNAME: 'district names',
         RINFO: 'repeater info', RPT: 'repeaters', RKEY: 'repeater key', AIR: 'airfields', AFREQ: 'air frequencies',
         AKEY: 'airfield key'}
GROUPS = {'map': list(range(META, DNAME + 1)), 'rpt': [RINFO, RPT, RKEY], 'air': [AIR, AFREQ, AKEY]}
PROVINCES = 81
RPT_NOPOS = -2 ** 31
RPT_MODE, RPT_PROV = 16, 42             # ref_pack.h REF_RPT_MODE / REF_RPT_PROV
AIR_FIRST, AIR_PROV = 8, 43
PAYLOAD_MAX = datapack.SLOT[datapack.REF] - 64     # 65,472
SOURCES = ('own', 'radioid')            # rpt_list.py inputs: the user's / owner's CSV (TRAC), RadioID.net


class RefError(Exception):
    pass


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


# ---- the generated C tables ----

def c_array(text, name):
    start = text.index(name)
    start = text.index('{', text.index('=', start))
    return text[start + 1:text.index('\n};', start)]


def c_strings(body):
    """The bytes of every string literal in body, C escapes decoded (octal, \\0, \\\\, \\\", \\n)"""
    out = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', body):
        s, b, i = m.group(1), bytearray(), 0
        while i < len(s):
            c = s[i]
            if c != '\\':
                b += c.encode('latin-1')
                i += 1
                continue
            j = i + 1
            if s[j] in '01234567':
                k = j
                while k < len(s) and k < j + 3 and s[k] in '01234567':
                    k += 1
                b.append(int(s[j:k], 8))
                i = k
            else:
                b.append({'n': 10, 't': 9, '\\': 92, '"': 34, "'": 39}[s[j]])
                i = j + 1
        out.append(bytes(b))
    return out


def defines(path):
    out = {}
    for m in re.finditer(r'#define\s+(\w+)\s+(-?\d+)u?\b', Path(path).read_text(encoding='latin-1')):
        out[m.group(1)] = int(m.group(2))
    m = re.search(r'#define\s+TURKEY_MAP_CHARSET\s+"([^"]*)"', Path(path).read_text(encoding='latin-1'))
    if m:
        out['CHARSET'] = m.group(1)
    return out


def ints(body):
    return [int(v) for v in re.findall(r'-?\d+', body)]


def load_map(map_c=MAP_C, map_h=MAP_H, dist_c=DIST_C):
    k = defines(map_h)
    t = Path(map_c).read_text(encoding='latin-1')
    provs = [tuple(int(v) for v in g) for g in re.findall(
        r'\{ (\d+), (\d+), (\d+), (\d+), \{ (-?\d+), (-?\d+) \}, \{ (-?\d+), (-?\d+) \}, \{ (-?\d+), (-?\d+) \} \}',
        c_array(t, 'turkey_map_provinces['))]
    rings = [tuple(int(v) for v in g) for g in re.findall(r'\{ \{ (-?\d+), (-?\d+) \}, (\d+), (\d+) \}',
                                                          c_array(t, 'turkey_map_rings['))]
    steps = [(int(a), int(b)) for a, b in re.findall(r'\{ (-?\d+), (-?\d+) \}', c_array(t, 'turkey_map_steps['))]
    nsec = t[t.index('turkey_map_names['):]
    names = b''.join(c_strings(nsec[:nsec.index(';')])) + b'\0'
    d = Path(dist_c).read_text(encoding='latin-1')
    pos = [(int(a), int(b)) for a, b in re.findall(r'\{ (-?\d+), (-?\d+) \}', c_array(d, 'turkey_district_pos['))]
    first = ints(c_array(d, 'turkey_district_first['))
    name_first = ints(c_array(d, 'turkey_district_name_first['))
    dsec = d[d.index('turkey_district_names['):]
    dnames = b''.join(c_strings(dsec[:dsec.index(';')])) + b'\0'
    if (len(provs), len(rings), len(steps)) != (k['TURKEY_MAP_PROVINCES'], k['TURKEY_MAP_RINGS'], k['TURKEY_MAP_STEPS']):
        raise RefError(f'{map_c}: counts do not match its header')
    return dict(k=k, provs=provs, rings=rings, steps=steps, names=names, pos=pos, first=first, name_first=name_first,
                dnames=dnames)


def load_air(air_c=AIR_C, air_h=AIR_H):
    from tools import airfields
    t = Path(air_c).read_text(encoding='latin-1')
    fields = []
    for m in re.finditer(r'\{ (-?\d+), (-?\d+), (\d+), (\d+), (\d+), ("(?:[^"\\]|\\.)*"), ("(?:[^"\\]|\\.)*"), (\d+) \}',
                         c_array(t, 'air_fields[')):
        g = m.groups()
        fields.append((int(g[0]), int(g[1]), int(g[2]), int(g[3]), int(g[4]), c_strings(g[5])[0], c_strings(g[6])[0],
                       int(g[7])))
    freqs = [(int(ch), airfields.TYPES.index(ty[4:])) for ch, ty in re.findall(r'\{ (\d+), (AIR_\w+) \}',
                                                                             c_array(t, 'air_freqs['))]
    k = defines(air_h)
    if (len(fields), len(freqs)) != (k['AIR_FIELDS'], k['AIR_FREQS']):
        raise RefError(f'{air_c}: counts do not match its header')
    return dict(fields=fields, freqs=freqs)


def load_rpt_c(rpt_c=RPT_C):
    """rpt_list.py's generated table: (info (count, sample, date, source), [entries])"""
    t = Path(rpt_c).read_text(encoding='latin-1')
    m = re.search(r'rpt_info = \{ (\d+)u, (\d), ("(?:[^"\\]|\\.)*"), ("(?:[^"\\]|\\.)*") \}', t)
    info = (int(m.group(1)), int(m.group(2)), c_strings(m.group(3))[0], c_strings(m.group(4))[0])
    entries = []
    for g in re.findall(r'\{ (RPT_NOPOS|-?\d+), (RPT_NOPOS|-?\d+), (\d+)u, (-?\d+), (\d+)u, (\d+), 0x([0-9a-f]{2}), '
                        r'("(?:[^"\\]|\\.)*"), ("(?:[^"\\]|\\.)*"), (\d+) \}', c_array(t, 'rpt_table[')):
        lat = RPT_NOPOS if g[0] == 'RPT_NOPOS' else int(g[0])
        lon = RPT_NOPOS if g[1] == 'RPT_NOPOS' else int(g[1])
        entries.append((lat, lon, int(g[2]), int(g[3]), int(g[4]), int(g[5]), int(g[6], 16), c_strings(g[7])[0],
                        c_strings(g[8])[0], int(g[9])))
    return info, entries[:info[0]]


def rpt_from_rows(rows, info):
    """rpt_list.Row objects (generate()'s stats['rows']) as load_rpt_c's entries"""
    out = []
    for r in rows:
        out.append((RPT_NOPOS if r.lat is None else r.lat, RPT_NOPOS if r.lon is None else r.lon, r.rx, r.off,
                    r.ctcss10, r.mode | (0x80 if r.narrow else 0), r.cc | r.ts << 4, r.name.encode('ascii'),
                    r.city.encode('ascii'), r.prov))
    return info, out


# ---- the payload ----

def coord_id(k):
    """ref_pack.c ref_coord_id: CRC-32 of the 7 coordinate constants (LE int32) then the charset text"""
    vals = [k['TURKEY_MAP_LON0_E7'], k['TURKEY_MAP_LAT0_E7'], k['TURKEY_MAP_UNIT_E7'], k['TURKEY_MAP_XMIN'],
            k['TURKEY_MAP_YMIN'], k['TURKEY_MAP_XMAX'], k['TURKEY_MAP_YMAX']]
    return crc32(struct.pack('<7i', *vals) + k['CHARSET'].encode('ascii'))


def hint(k):
    return DIR_VERSION | (coord_id(k) & 0xFF) << 8


def _cstr(b, size):
    """a NUL-padded char[size] (size includes the NUL)"""
    if len(b) >= size:
        raise RefError(f'{b!r} longer than {size - 1}')
    return b + b'\0' * (size - len(b))


def sections(m, air=None, rpt=None):
    """{id: (count, bytes)} for the map (always), the airfields and the repeaters when given"""
    k = m['k']
    s = {}
    s[META] = (1, struct.pack('<3i4h4H4x', k['TURKEY_MAP_LON0_E7'], k['TURKEY_MAP_LAT0_E7'], k['TURKEY_MAP_UNIT_E7'],
                              k['TURKEY_MAP_XMIN'], k['TURKEY_MAP_YMIN'], k['TURKEY_MAP_XMAX'], k['TURKEY_MAP_YMAX'],
                              len(m['provs']), len(m['rings']), len(m['steps']), len(m['pos'])))
    s[PROV] = (len(m['provs']), b''.join(struct.pack('<HBBH6h', *p) for p in m['provs']))
    s[RING] = (len(m['rings']), b''.join(struct.pack('<hhHH', *r) for r in m['rings']))
    s[STEP] = (len(m['steps']), b''.join(struct.pack('<bb', *st) for st in m['steps']))
    s[PNAME] = (len(m['names']), m['names'])
    s[DPOS] = (len(m['pos']), b''.join(struct.pack('<hh', *q) for q in m['pos']))
    s[DFIRST] = (len(m['first']), struct.pack(f'<{len(m["first"])}H', *m['first']))
    s[DNFIRST] = (len(m['name_first']), struct.pack(f'<{len(m["name_first"])}H', *m['name_first']))
    s[DNAME] = (len(m['dnames']), m['dnames'])
    if rpt is not None:
        info, entries = rpt
        s[RINFO] = (1, struct.pack('<IB11s16s', info[0], info[1], _cstr(info[2][:10], 11), _cstr(info[3][:15], 16)))
        s[RPT] = (len(entries), b''.join(struct.pack('<iiIhHBB11s13sBx', e[0], e[1], e[2], e[3], e[4], e[5], e[6],
                                                     _cstr(e[7], 11), _cstr(e[8], 13), e[9]) for e in entries))
        s[RKEY] = (len(entries), b''.join(struct.pack('<iiBB', e[0], e[1], e[9], e[5]) for e in entries))
    if air is not None:
        f, q = air['fields'], air['freqs']
        s[AIR] = (len(f), b''.join(struct.pack('<iiHBB8s23sB', a[0], a[1], a[2], a[3], a[4], _cstr(a[5], 8),
                                               _cstr(a[6], 23), a[7]) for a in f))
        s[AFREQ] = (len(q), b''.join(struct.pack('<HBx', ch, ty) for ch, ty in q))
        s[AKEY] = (len(f), b''.join(struct.pack('<iiBB', a[0], a[1], a[7], a[4]) for a in f))
    return s


def payload(k, secs):
    ids = sorted(secs)
    off = HEAD + ENTRY * len(ids)
    off += -off % 4
    entries, body = b'', b''
    for i in ids:
        count, data = secs[i]
        if len(data) != REC[i] * count:
            raise RefError(f'section {i}: {len(data)} bytes for {count} x {REC[i]}')
        entries += struct.pack('<HHII', i, REC[i], off + len(body), count)
        body += data + b'\0' * (-len(data) % 4)
    head = b'REF1' + struct.pack('<BBHII', DIR_VERSION, len(ids), 0, coord_id(k), 0)
    p = head + entries
    p += b'\0' * (-len(p) % 4) + body
    if len(p) > PAYLOAD_MAX:
        raise RefError(f'{len(p)} bytes: the map pack slot holds {PAYLOAD_MAX}')
    return p


# ---- the firmware's rules (ref_pack.c ref_validate) ----

def validate(p, flags=0, k=None):
    """('ok' | 'bad' | 'other', why) as the firmware decides for a payload whose CRCs check"""
    if k is None:
        k = defines(MAP_H)
    if len(p) < HEAD or len(p) > 0xFFFF or p[:4] != b'REF1':
        return 'bad', 'no directory'
    ver, n = p[4], p[5]
    if ver != DIR_VERSION:
        return 'other', f'directory version {ver}'
    if struct.unpack_from('<I', p, 8)[0] != coord_id(k):
        return 'other', 'map coordinate id'
    if flags and flags != hint(k):
        return 'other', f'flags hint 0x{flags:04x}'
    if n > MAX_SECTIONS or HEAD + ENTRY * n > len(p):
        return 'bad', 'directory size'
    s, bad = {}, None
    for e in range(n):
        i, rec, off, count = struct.unpack_from('<HHII', p, HEAD + ENTRY * e)
        if i not in REC:
            continue
        if rec != REC[i]:
            return 'other', f'section {i} record size {rec}'
        if i in s or off % 4 or off < HEAD + ENTRY * n or off > len(p) or count > (len(p) - off) // rec:
            bad = bad or f'section {i} placement'
            continue
        s[i] = (off, count)
    if bad:
        return 'bad', bad
    spans = sorted((off, off + REC[i] * c, i) for i, (off, c) in s.items() if c)
    for a, b in zip(spans, spans[1:]):
        if b[0] < a[1]:
            return 'bad', f'sections {a[2]} and {b[2]} overlap'
    for g, ids in GROUPS.items():
        if any(i in s for i in ids) and not all(i in s for i in ids):
            return 'bad', f'{g} sections incomplete'

    def sec(i):
        off, c = s[i]
        return p[off:off + REC[i] * c], c
    if META in s:
        meta, c = sec(META)
        if c != 1:
            return 'bad', 'map meta count'
        v = struct.unpack_from('<3i4h4H', meta)
        want = (k['TURKEY_MAP_LON0_E7'], k['TURKEY_MAP_LAT0_E7'], k['TURKEY_MAP_UNIT_E7'], k['TURKEY_MAP_XMIN'],
                k['TURKEY_MAP_YMIN'], k['TURKEY_MAP_XMAX'], k['TURKEY_MAP_YMAX'])
        rings, nr = sec(RING)
        steps, ns = sec(STEP)
        pnames, npn = sec(PNAME)
        dnames, ndn = sec(DNAME)
        nd = s[DPOS][1]
        if v[:7] != want or v[7] != PROVINCES or s[PROV][1] != PROVINCES or v[8] != nr or v[9] != ns or v[10] != nd \
                or s[DFIRST][1] != PROVINCES + 1 or s[DNFIRST][1] != PROVINCES or not npn or not ndn:
            return 'bad', 'map meta'
        nxt = 0     # rings consecutive and disjoint, together exactly the steps (no step walked twice)
        for r in range(nr):
            _, _, first, count = struct.unpack_from('<hhHH', rings, 8 * r)
            if count == 0 or first != nxt:
                return 'bad', f'ring {r}'
            nxt = first + count
        if nxt != ns:
            return 'bad', 'rings do not cover the steps exactly'
        provs, _ = sec(PROV)
        nxt = 0     # provinces' rings consecutive and disjoint in province order, together exactly the rings
        for q in range(PROVINCES):
            fr, rc, _, name, _, _, x0, y0, x1, y1 = struct.unpack_from('<HBBH6h', provs, 18 * q)
            if fr != nxt or name >= npn or x0 > x1 or y0 > y1:
                return 'bad', f'province {q}'
            nxt = fr + rc
        if nxt != nr:
            return 'bad', 'provinces do not cover the rings exactly'
        if pnames[-1] != 0 or dnames[-1] != 0:
            return 'bad', 'name blob without its NUL'
        first = struct.unpack(f'<{PROVINCES + 1}H', sec(DFIRST)[0])
        nfirst = struct.unpack(f'<{PROVINCES}H', sec(DNFIRST)[0])
        if first[0] != 0 or first[-1] != nd or any(b < a for a, b in zip(first, first[1:])):
            return 'bad', 'district first'
        prev = 0
        for q in range(PROVINCES):
            at = nfirst[q]
            if at < prev or at >= ndn:
                return 'bad', f'district names of province {q}'
            want_n, pos = first[q + 1] - first[q], at
            while want_n:
                j = dnames.find(b'\0', pos)
                if j < 0:
                    return 'bad', f'district names of province {q}'
                pos, want_n = j + 1, want_n - 1
            prev = at

    def keys_ok(rec_id, key_id, prov_at, mode_at):
        recs, nrec = sec(rec_id)
        keys, nk = sec(key_id)
        if nrec != nk or nrec > 65534:
            return False
        for j in range(nrec):
            r, kk = recs[44 * j:44 * j + 44], keys[10 * j:10 * j + 10]
            if r[:8] != kk[:8] or r[prov_at] != kk[8] or r[mode_at] != kk[9]:
                return False
        return True
    if RPT in s:
        if s[RINFO][1] != 1 or not keys_ok(RPT, RKEY, RPT_PROV, RPT_MODE):
            return 'bad', 'repeaters'
    if AIR in s:
        if not keys_ok(AIR, AKEY, AIR_PROV, AIR_FIRST + 3):
            return 'bad', 'airfields'
        recs, nrec = sec(AIR)
        for j in range(nrec):
            first, count = struct.unpack_from('<HB', recs, 44 * j + AIR_FIRST)
            if count > 64 or first + count > s[AFREQ][1]:
                return 'bad', f'airfield {j} frequencies'
    return 'ok', ''


def directory(p):
    """[(id, rec, off, count)] of a payload's directory (no checks)"""
    if len(p) < HEAD or p[:4] != b'REF1':
        return []
    n = min(p[5], (len(p) - HEAD) // ENTRY)
    return [struct.unpack_from('<HHII', p, HEAD + ENTRY * e) for e in range(n)]


def rpt_info(p):
    """(count, sample, date, source) of a payload's repeater info section, None without one"""
    for i, rec, off, count in directory(p):
        if i == RINFO and rec == 32 and count == 1 and off + 32 <= len(p):
            n, sample, date, source = struct.unpack_from('<IB11s16s', p, off)
            return n, sample, date.split(b'\0')[0].decode('ascii', 'replace'), \
                source.split(b'\0')[0].decode('ascii', 'replace')
    return None


def describe(p, flags=0):
    verdict, why = validate(p, flags)
    lines = [f'map pack format 1, directory version {p[4] if len(p) > 4 else "?"}, flags 0x{flags:04x}: '
             f'{"valid for this firmware" if verdict == "ok" else verdict + " (" + why + ")"}']
    for i, rec, off, count in directory(p):
        lines.append(f'  section {i:2} {NAMES.get(i, "unknown"):20} {count:6} x {rec:2} B at {off}')
    info = rpt_info(p)
    if info:
        lines.append(f'  repeaters: {info[0]}, {info[2]}, source "{info[3]}"{" (SAMPLE)" if info[1] else ""}')
    return '\n'.join(lines)


# ---- building ----

def rpt_sources_split(text):
    out = [s.strip().lower() for s in text.split(',') if s.strip()]
    if any(s not in SOURCES for s in out):
        raise RefError(f'--rpt-sources takes {", ".join(SOURCES)}')
    return out


def build(version, date=None, rpt=None, air=True, tag=None, map_tables=None, air_tables=None):
    """The release .dpk image (sequence 0): name "DDM32 TR <version>", tag "build <version>"; rpt: load_rpt_c() or
    rpt_from_rows() output, None for a pack without repeater sections"""
    m = map_tables or load_map()
    a = (air_tables or load_air()) if air else None
    secs = sections(m, a, rpt)
    p = payload(m['k'], secs)
    if date is None:
        dates = ['20261119']        # the map's and airfields' pinned sources (GeoNames / OurAirports of 2026-11-19)
        if rpt is not None and re.fullmatch(r'\d{4}-\d{2}-\d{2}', rpt[0][2].decode('ascii', 'replace')):
            dates.append(rpt[0][2].decode('ascii').replace('-', ''))
        date = max(dates)
    verdict, why = validate(p, 0, m['k'])
    if verdict != 'ok':
        raise RefError(f'the pack does not validate: {verdict} ({why})')
    image = datapack.header(datapack.REF, p, date, f'DDM32 TR {version}'[:16], tag=(tag or f'build {version}').encode(
        'ascii')[:16], flags=hint(m['k'])) + p
    return image


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--out', required=True)
    b.add_argument('--version', default=(ROOT / 'VERSION').read_text(encoding='ascii').strip()
                   if (ROOT / 'VERSION').is_file() else '0')
    b.add_argument('--date', help='YYYYMMDD (default: the newest source date)')
    b.add_argument('--rpt-c', help='a table tools/rpt_list.py generated (default build/rpt/rpt_data.c)')
    b.add_argument('--csv', help='your own repeater CSV (tools/rpt_list.py columns)')
    b.add_argument('--radioid', help='RadioID.net rptrs.json')
    b.add_argument('--country', action='append', default=[])
    b.add_argument('--rpt-sources', default='own,radioid', help='which of --csv (own) and --radioid to use')
    b.add_argument('--no-rpt', action='store_true', help='a pack without repeater sections')
    b.add_argument('--tag', help='the builder tag (default "build <version>"; a user pack: e.g. "user <date>")')
    i = sub.add_parser('info')
    i.add_argument('pack')
    args = ap.parse_args(argv)
    try:
        if args.cmd == 'info':
            image = Path(args.pack).read_bytes()
            h, p = datapack.parse(image)
            if not h or h['type'] != datapack.REF:
                print('not a map pack')
                return 1
            print(datapack.info(image))
            return 0
        rpt = None
        if not args.no_rpt:
            if args.csv or args.radioid:
                from tools import rpt_list
                use = rpt_sources_split(args.rpt_sources)
                csv = args.csv if 'own' in use else None
                rid = args.radioid if 'radioid' in use else None
                if csv is None and rid is None:
                    raise RefError('no repeater input left by --rpt-sources')
                _, stats = rpt_list.generate(csv, rid, args.country or ['Turkey'])
                rpt = rpt_from_rows(stats['rows'], (stats['count'], 0, stats['date'][:10].encode('ascii'),
                                                    stats['source'].encode('ascii')))
            else:
                rpt = load_rpt_c(args.rpt_c or RPT_C)
        image = build(args.version, args.date, rpt, tag=args.tag)
        Path(args.out).write_bytes(image)
        print(f'{args.out}: {len(image)} bytes ({(len(image) + 4095) // 4096} sectors)')
        print(describe(image[64:], datapack.parse(image)[0]['flags']))
        return 0
    except (RefError, datapack.PackError, OSError, ValueError) as e:
        print(f'refpack: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
