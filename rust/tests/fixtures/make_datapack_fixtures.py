#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes datapack-results.json: what tools/datapack.py (with tools/iddb.py and tools/airfields.py) builds from a set
of SYNTHETIC inputs, so the C# pack builder (src/DDM32Plus.Core/DataPacks) can be held to it byte for byte
(DataPackTests.cs). Run from anywhere:

    python rust/tests/fixtures/make_datapack_fixtures.py

Every input here is made up: fictional IDs, callsigns and names, invented AD 2.18 lines. Never put a real RadioID.net
list or real AIP pages into this script or its output.
"""
import base64
import json
import struct
from pathlib import Path
import sys
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]  # firmware/ (this copy lives in rust/tests/fixtures)
sys.path.insert(0, str(REPO))

from tools import datapack as dp  # noqa: E402
from tools import iddb  # noqa: E402

CSHARP_TAG = b'DDM32Plus-win 1'


def b64(data):
    return base64.b64encode(data).decode('ascii')


# --- RadioID lists (fictional) --------------------------------------------------------------------------------------
CSV_BASIC = '''RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY
2860105,TA2XYZ,Ayşe,Kaya,Izmir,,Turkey
2860001,ta1abc,Ahmet,Yilmaz,Ankara,,Turkey
3101234,TA3AA,Mehmet Ali,Demir,Bursa,,Turkey
2860001,TA9DUP,Dup,Dup,X,,Turkey
3109999,W1AW,Hiram,Maxim,Newington,CT,United States
'''.encode('utf-8')

CSV_EDGE = (
    '﻿RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY,EXTRA\r\n'
    '2869001,TA1AAA,MEHMET,ÖZTÜRK,Ankara,,TURKEY\r\n'
    '2869002, ta1aab ,ayşe nur,kaya,,, Türkiye \r\n'
    '\r\n'
    '2869003,TA1AAC,"Çağrı, Jr.","Yıl""maz",x,,Turkiye\r\n'
    '2869004,TA1AAD,"multi\nline name",Demir,,,Turkey\r\n'
    '+2869005,TA1AAE,O\'NEIL,Smith,,,Turkey\r\n'
    '2_869_006,TA1AAF,jean-luc,Picard,,,Turkey\r\n'
    '0002869007,TA1AAG,Ødegård,Łukasz,,,Turkey\r\n'
    'abc,TA1AAH,Bad,Id,,,Turkey\r\n'
    '0,TA1AAI,Zero,Id,,,Turkey\r\n'
    '-5,TA1AAJ,Neg,Id,,,Turkey\r\n'
    '16777216,TA1AAK,Big,Id,,,Turkey\r\n'
    '16777215,TA1AAL,Max,Id,,,Turkey\r\n'
    '2869008,TA1ABCD,Long,Call,,,Turkey\r\n'
    '2869009,TA-1,Dash,Call,,,Turkey\r\n'
    '2869010,,No,Call,,,Turkey\r\n'
    '2869011,TAß1,Eszett,Call,,,Turkey\r\n'
    '2869012,tı2a,Dotless,Call,,,Turkey\r\n'
    '2869013,ﬁve,Ligature,Call,,,Turkey\r\n'
    '2869014,TA1AAM,123,,,,Turkey\r\n'
    '2869015,TA1AAN,,Yılmaz,,,Turkey\r\n'
    '2869016,TA1AAO,,,,,Turkey\r\n'
    '2869017,TA1AAP,Abdurrahmangazi,X,,,Turkey\r\n'
    '2869018,TA1AAQ,émile,X,,,Turkey\r\n'
    '2869019,TA1AAR,Short\r\n'
    '2869020,5B4AAA,Andreas,Georgiou,Lefkosa,,Cyprus\r\n'
    '2869021,TA1AAS,Too,Many,a,b,Turkey,c,d,e\r\n'
    '2869001,TA1DUP,Dup,Again,,,Turkey\r\n'
    '2869022,TA1AAT,"Quote "inside,X,,,Turkey\r\n'
    ' 2869023 ,TA1AAU,Space,Id,,,Turkey\r\n'
    '2869024,TA1AAV, sep,X,,,Turkey\r\n'
    '2869025,TA1AAW,Mx Nbsp,X,,,Turkey\r\n'
    '3109998,K1ABC,Hiram,Maxim,X,CT,United States\r\n'
    '2869026,TA1AAX,Last,Row,,,Turkey'
).encode('utf-8') + b'\r\n2869027,TA1AAY,Bad\xffByte,X,,,Turkey\r\n'

CSV_MISSING = b'RADIO_ID,CALLSIGN,LAST_NAME,COUNTRY\n1,TA1A,X,Turkey\n'


def radioid_cases():
    out = []
    for name, data, countries in (('basic', CSV_BASIC, ['Turkey']), ('basic-default', CSV_BASIC, []),
                                  ('edge-turkey', CSV_EDGE, []), ('edge-two', CSV_EDGE, ['Turkey', 'Cyprus']),
                                  ('edge-us', CSV_EDGE, ['United States']), ('edge-none', CSV_EDGE, ['Atlantis']),
                                  ('missing', CSV_MISSING, ['Turkey'])):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'user.csv'
            p.write_bytes(data)
            case = {'name': name, 'csv': b64(data), 'countries': countries}
            try:
                rows, skipped = iddb.read(str(p), countries)
                payload, n, _ = dp.radioid_from_csv(str(p), countries)
                case.update(rows=[[rid, call, packed, nm] for rid, call, packed, nm in rows], skipped=skipped,
                            count=n, payload=b64(payload),
                            image=b64(dp.header(dp.RADIOID, payload, '20261121', 'RadioID TR') + payload),
                            image_cs=b64(dp.header(dp.RADIOID, payload, '20261121', 'RadioID TR', seq=7,
                                                   tag=CSHARP_TAG) + payload))
            except (dp.PackError, iddb.IddbError) as e:
                case['error'] = str(e).replace(str(p), 'user.csv')
        out.append(case)
    return out


def fold_cases():
    words = ['Ayşe', 'ÇĞİÖŞÜçğıöşü', 'Ødegård', 'Łukasz', 'straße', 'Æsir', 'þorn', 'œuvre', 'Đorđe', 'naïve café',
             'ﬁne', 'Ｆｕｌｌ', '①②', 'x́y', '日本', '']
    names = [['MEHMET', ''], ['ayşe nur', 'x'], ["O'NEIL", ''], ['jean-luc', ''], ['', 'Yılmaz'], ['123', ''],
             ['Abdurrahmangazi', ''], ['a.b.', ''], ['--', 'Z'], ['', ''], ['mIxEd', ''], ['ÇAĞRI', '']]
    calls = ['TA1ABC', 'ta1abc', ' TA1ABC ', 'TA1ABCD', 'TA-1', '', 'TAß1', 'tı2a', 'ﬁve', 'Ta1', '123456', 'A']
    return {'fold': [[w, iddb.fold(w)] for w in words],
            'country': [[w, iddb.country_key(w)] for w in ['Turkey', ' TÜRKİYE ', 'Türkiye', 'Cyprus', 'turkey']],
            'short_name': [[f, l, iddb.short_name(f, l)] for f, l in names],
            'pack_call': [[c, iddb.pack_call(c)] for c in calls]}


def payload_cases():
    """radioid_payload with direct entries (other text widths, refusals)"""
    entries = [(5, 'TA1A', 'Zed'), (3, 'TA1B', 'Ayşe Nur'), (16777215, 'TA9ZZZ', 'Max.Name-x'), (1, '', '')]
    out = []
    for chars in (4, 8, 16, 20, 32, 6, 36):
        try:
            out.append({'chars': chars, 'payload': b64(dp.radioid_payload(entries, chars))})
        except dp.PackError as e:
            out.append({'chars': chars, 'error': str(e)})
    bad = []
    for es in ([(1, 'A', ''), (1, 'B', '')], [(0, 'A', '')], [(16777216, 'A', '')]):
        try:
            dp.radioid_payload(es)
            bad.append(None)
        except dp.PackError as e:
            bad.append(str(e))
    return {'entries': [list(e) for e in entries], 'widths': out, 'refused': bad}


# --- AD 2.18 page text (invented) ----------------------------------------------------------------------------------
AD218 = '''LTAC AD 2.17 something
LTAC AD 2.18 ATS COMMUNICATION FACILITIES
Service designation  Call sign  Frequency  Hours  Remarks
ATIS  ESENBOGA INFORMATION  123.600 MHZ  H24
TWR   ESENBOGA TOWER  118.100 MHZ  H24
      121.500 MHZ emergency
APP   ANKARA APPROACH 119.100 MHZ H24
      UHF 257.800 MHZ (military, not air band)
LTAC AD 2.19 RADIO NAVIGATION
VOR 112.500 MHZ
'''

AD218_EDGE = (
    'XXXX AD 3 . 18 Heliport communication\n'
    'D-ATIS 128,525 Mhz\n'
    'GND ground 121.58 / 121.580\n'
    'tower 118.02 136.995 137.000 117.975\n'
    'APP/DEP 120.1MHz 120.10 MHz 120.100\n'
    'TWR/GND 118.0125 and 118.012\n'
    'clearance\x0b124.000\x0c125.000\x85126.000 127.000\r\n'
    'nothing here 1180.100 2118.100 118.1005\n'
    '  132.005 MHz  (after the delivery line)\n'
    'RAMP 131.5 Apron 131.55\n'
    'UNICOM 122.800, FIS 126.900\n'
    'HAVAS ops 129.900\n'
    'unknown service 130.000\n'
    'XXXX AD 3.19 NAV\n'
    'TWR 119.900\n'
)

AD218_NOSECTION = 'TWR 118.300\nATIS 127.200 MHZ\nGND 121.700 TWR 118.300\n'


def air_cases():
    texts = {'basic': AD218, 'edge': AD218_EDGE, 'nosection': AD218_NOSECTION, 'empty': '',
             'freqonly': '118.100\n118.125\n'}
    parsed = {k: [list(x) for x in dp.parse_ad218(v)] for k, v in texts.items()}
    channel = [[t, *dp.airfields.channel_khz(t)] for t in
               ['118.100', '121.58', '118.02', '136.995', '137.000', '117.975', '132', '118.675', ' 120.1 ', 'abc',
                '118.0001', '0118.100', '120.10']]
    fields = {'LTAC': dp.parse_ad218(AD218), 'LTBA': dp.parse_ad218(AD218_EDGE), 'LTAB': dp.parse_ad218(AD218_NOSECTION),
              'LTXYZABC': [(4, 118000 + 5 * i) for i in range(70)], 'LTAA': []}
    payload = dp.air_payload(fields)
    out = {'texts': texts, 'parsed': parsed, 'channel': channel,
           'fields': {k: [list(x) for x in v] for k, v in fields.items()}, 'payload': b64(payload),
           'image': b64(dp.header(dp.AIR, payload, '20261121', 'TR aerodromes') + payload),
           'image_cs': b64(dp.header(dp.AIR, payload, '20261121', 'TR aerodromes', seq=3, tag=CSHARP_TAG) + payload)}
    bad = []
    for f in ({'': [(0, 118000)]}, {'TOOLONGXX': [(0, 118000)]}):
        try:
            dp.air_payload(f)
            bad.append(None)
        except dp.PackError as e:
            bad.append(str(e))
    out['refused'] = bad
    return out


# --- headers, parse, slots, the CPS order --------------------------------------------------------------------------
def header_cases():
    pl = bytes(range(200)) * 3
    out = []
    for ptype, payload, date, name, seq, tag in (
            (dp.RADIOID, pl, '20261121', 'RadioID TR', 0, dp.TAG),
            (dp.AIR, pl[:50], '20991231', 'Türkiye ✈ uçuş', 9, dp.TAG),
            (dp.RADIOID, b'', '20000101', 'A name longer than sixteen', 4294967295, CSHARP_TAG),
            (dp.AIR, pl[:10], '20261121', 'astral \U0001F6E9 x', 1, CSHARP_TAG),
            (dp.AIR, bytes(0x4000 - 64), '20261121', 'full', 2, dp.TAG),
            (dp.AIR, bytes(0x4000 - 63), '20261121', 'too big', 2, dp.TAG),
            (dp.RADIOID, pl, '2026112', 'bad date', 0, dp.TAG),
            (dp.RADIOID, pl, '2026-11-2', 'bad date', 0, dp.TAG)):
        case = {'type': ptype, 'payload': b64(payload), 'date': date, 'name': name, 'seq': seq,
                'tag': tag.decode('ascii')}
        try:
            case['header'] = b64(dp.header(ptype, payload, date, name, seq, tag))
        except dp.PackError as e:
            case['error'] = str(e)
        out.append(case)
    return out


def parse_cases():
    pl = bytes(range(256)) * 2
    good = dp.header(dp.RADIOID, pl, '20261121', 'RadioID TR', 5) + pl
    bad_payload = bytearray(good)
    bad_payload[100] ^= 1
    bad_header = bytearray(good)
    bad_header[30] ^= 1
    images = {'good': good, 'bad_payload': bytes(bad_payload), 'bad_header': bytes(bad_header),
              'short': good[:200], 'erased': b'\xff' * 64, 'tiny': good[:63], 'extra': good + b'\xff' * 100,
              'air': dp.header(dp.AIR, pl[:30], '20261121', 'Air', 2) + pl[:30],
              'latin': dp.header(dp.AIR, b'x', '20261121', 'x') + b'x'}
    lat = bytearray(images['latin'])
    lat[28:32] = b'\xe7\xfe\x00\x00'
    lat[60:64] = struct.pack('<I', dp.crc32(bytes(lat[:60])))
    images['latin'] = bytes(lat)
    out = {}
    for k, img in images.items():
        h, payload = dp.parse(img)
        out[k] = {'image': b64(img), 'header': h, 'payload': b64(payload), 'info': dp.info(img)}
    return out


def slot_cases():
    pl = b'payload' * 10
    e = b'\xff' * 64
    r4 = dp.header(dp.RADIOID, pl, '20261121', 'a', 4) + pl
    r9 = dp.header(dp.RADIOID, pl, '20261121', 'b', 9) + pl
    a7 = dp.header(dp.AIR, pl, '20261121', 'c', 7) + pl
    broken = bytearray(r9)
    broken[70] ^= 1
    sets = {'empty': [e, e], 'r4_e': [r4, e], 'e_r4': [e, r4], 'r4_r9': [r4, r9], 'r9_r4': [r9, r4],
            'broken9_r4': [bytes(broken), r4], 'air_in_radioid': [a7, e], 'r4_air': [r4, a7]}
    out = []
    for k, imgs in sets.items():
        out.append({'name': k, 'images': [b64(i) for i in imgs], 'inactive_radioid': dp.inactive_slot(dp.RADIOID, imgs),
                    'inactive_air': dp.inactive_slot(dp.AIR, imgs), 'next_seq': dp.next_seq(imgs)})
    return out


def plan_cases():
    out = []
    for size, addr in ((64, 0xE70000), (4096, 0xEB0000), (4097, 0xEF4000), (9064, 0xEB0000), (0x4000, 0xEF0000)):
        img = bytes((i * 7 + 3) & 255 for i in range(size))
        plan = dp.cps_plan(addr, img)
        out.append({'size': size, 'address': addr, 'image': b64(img),
                    'plan': [[a, b64(d)] for a, d in plan]})
    return out


def main():
    results = {'radioid': radioid_cases(), 'text': fold_cases(), 'payloads': payload_cases(), 'air': air_cases(),
               'headers': header_cases(), 'parse': parse_cases(), 'slots': slot_cases(), 'plans': plan_cases(),
               'python_tag': dp.TAG.decode('ascii')}
    path = HERE / 'datapack-results.json'
    path.write_text(json.dumps(results, indent=1, ensure_ascii=True, sort_keys=True) + '\n', encoding='utf-8',
                    newline='\n')
    print(f'wrote {path} ({path.stat().st_size} bytes)')


if __name__ == '__main__':
    main()
