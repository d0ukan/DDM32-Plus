#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes codeplug-results.json.gz: what the Python codeplug viewer (pc/ddm32_app/codeplug.py) reads from a set of
SYNTHETIC data-flash images, so the C# viewer (src/DDM32Plus.Core/Codeplug/Codeplug.cs) can be held to it
(CodeplugTests.cs). Run from anywhere:

    python rust/tests/fixtures/make_codeplug_fixtures.py

Each image is built with the Python tests' own builder (pc/tests/fixtures.py ImageBuilder) and stored sparse: only
the runs that differ from erased flash (0xFF), so the C# test rebuilds the same 16 MiB. Expected: every channel row,
zone, contact and RX group list, the counts, and each table's CSV, as the Python code makes them.

One difference on purpose: the firmware now shows a P25 NAC decimal first, "659 ($293)", "Any 3966 ($F7E)"
(CONFIG_NAC_DEC, docs/plus.md "P25 NAC in decimal"); the Python viewer still writes "$293 = 659". The expected texts are
the Python texts with that one form rewritten (nac_dec below), nothing else.

Every channel, name and ID here is made up. Never put a real backup or anything read from one into this script.
"""
import base64
import gzip
import json
from pathlib import Path
import re
import sys
import zlib

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]  # firmware/ (this copy lives in rust/tests/fixtures)
sys.path.insert(0, str(REPO / 'pc'))
sys.path.insert(0, str(REPO / 'pc' / 'tests'))

import fixtures as fx  # noqa: E402
from ddm32_app import codeplug as cp  # noqa: E402

OUT = HERE / 'codeplug-results.json.gz'

RE_NAC = re.compile(r'\$([0-9A-F]{3}) = (\d+)')


def nac_dec(text):
    """'$293 = 659' -> '659 ($293)', 'Herhangi ($F7E)' -> 'Herhangi 3966 ($F7E)' (the firmware's form)"""
    if not isinstance(text, str):
        return text
    text = RE_NAC.sub(lambda m: f'{m.group(2)} (${m.group(1)})', text)
    return text.replace('Herhangi ($F7E)', 'Herhangi 3966 ($F7E)')


def sparse(image):
    """[[address, base64 bytes], ...]: the runs that are not 0xFF"""
    runs = [[m.start(), m.end()] for m in re.finditer(rb'[^\xff]+', image)]
    merged = []
    for s, e in runs:               # runs closer than 16 bytes become one
        if merged and s - merged[-1][1] < 16:
            merged[-1][1] = e
        else:
            merged.append([s, e])
    return [[s, base64.b64encode(image[s:e]).decode('ascii')] for s, e in merged]


def kv_area(b, entries, crc_ok=True):
    data = b''.join(k.to_bytes(2, 'little') + len(v).to_bytes(2, 'little') + v for k, v in entries.items())
    crc = zlib.crc32(data) ^ (0 if crc_ok else 1)
    h = cp.KV_MAGIC.to_bytes(4, 'little') + bytes([1, 16]) + len(data).to_bytes(2, 'little') + \
        crc.to_bytes(4, 'little') + bytes(4)
    b.img[cp.KV_ROOT:cp.KV_ROOT + 16 + len(data)] = h + data


def kv_pages(b, pages):
    """A chain of RFKV pages: the root at EE+0xE000, then the given bank offsets"""
    offs = [0xE000] + [p for p, _ in pages[1:]]
    for i, (off, entries) in enumerate(pages):
        data = b''.join(k.to_bytes(2, 'little') + len(v).to_bytes(2, 'little') + v for k, v in entries.items())
        nxt = offs[i + 1] if i + 1 < len(offs) else 0
        h = cp.KV_MAGIC.to_bytes(4, 'little') + bytes([1, 16]) + len(data).to_bytes(2, 'little') + \
            zlib.crc32(data).to_bytes(4, 'little') + nxt.to_bytes(4, 'little')
        a = cp.kv_page_addr(offs[i])
        b.img[a:a + 16 + len(data)] = h + data


def p25_tag(name, flags=1, squelch=1, rx=0x8293, tx=0, tg=None, fallback=False):
    raw = name.encode('latin-1')[:16].ljust(16, b'\xff')
    block = bytes([squelch]) + rx.to_bytes(2, 'little') + tx.to_bytes(2, 'little')
    block += bytes([1]) + tg.to_bytes(2, 'little') + b'\x00' if tg is not None else bytes(4)
    head = bytes([2, 0xFF]) + cp.name_check(raw).to_bytes(2, 'little') + bytes([flags | (4 if fallback else 0)])
    return head + (bytes(4) if fallback else b'') + block


MODE2_18 = bytes.fromhex('00 25 31 43 00 25 31 43 02 00 00 00 00 00 00 00 93 02 93 02 00 00 80 00 00 01 00 93'
                         ' 00 00 01 00 00')


def mode2_record(name, tail=MODE2_18, **at):
    r = bytearray(cp.CH_SIZE)
    fx.put_name(r, cp.CH_NAME, name)
    r[0x10:0x10 + len(tail)] = tail
    for off, v in at.items():
        r[int(off[1:], 16)] = v
    return bytes(r)


def image_p25():
    """Classic r13 P25 channels (DMR base + RFKV tag) in every form, the old forms, a two-page RFKV chain"""
    b = fx.ImageBuilder()
    b.general('TA0TST', 2869999)
    rec = lambda name, **kw: fx.channel_record(name, 434_250_000, mode=1, cc=1, flag4=cp.FLAG4_RX_ONLY, **kw)
    names = ['Rpt TX659', 'Both def', 'RX Any', 'Normal sq', 'No TX', 'Contact TG', 'Radio ID', 'Other name',
             'Kind 3', 'Fallback', 'TG Any']
    for i, n in enumerate(names):
        kw = {'unit_id': 1234567}
        if n == 'Contact TG':
            kw['contact'] = 3
        if n == 'Radio ID':
            kw = {}
        b.channel(10 + i, rec(n, **kw))
    tags = {
        10: p25_tag('Rpt TX659', tx=0x8659, tg=101),
        11: p25_tag('Both def', rx=0, tx=0, tg=101),
        12: p25_tag('RX Any', rx=0x8F7E, tx=0x8293, tg=101),
        13: p25_tag('Normal sq', squelch=0, tg=101),
        14: p25_tag('No TX', flags=0, tg=101),
        15: p25_tag('Contact TG'),
        16: p25_tag('Radio ID', tg=7),
        17: p25_tag('Not this one', tg=101),
        18: bytes([3, 0xFF]) + p25_tag('Kind 3')[2:],
        19: p25_tag('Fallback', tg=55, fallback=True),
        20: p25_tag('TG Any', tg=0, rx=0x8F7F),
    }
    # the RFKV area over two pages; the radio-wide P25 unit ID on the second
    page1 = {cp.KEY_CH_TAG + n - 1: v for n, v in tags.items() if n < 16}
    page2 = {cp.KEY_CH_TAG + n - 1: v for n, v in tags.items() if n >= 16}
    page2[cp.KEY_MODE_SET + cp.KIND_P25] = (7654321).to_bytes(4, 'little')
    kv_pages(b, [(0xE000, page1), (0xAF000, page2)])
    # the older forms
    b.channel(30, fx.channel_record('P25 r7', 434_250_000, mode=0xA5, cc=0xD2, rx_group=0x93, contact=101,
                                    unit_id=1234567))
    b.channel(31, fx.channel_record('P25 bad', 433_000_000, mode=0xA5, cc=0x02, rx_group=0x93, contact=1))
    b.channel(32, mode2_record('433.125'))
    b.channel(33, mode2_record('m2 tx', x22=0x25, x23=0x01))
    b.channel(34, mode2_record('m2 all', x31=0x40))
    b.channel(35, mode2_record('m2 noid', x26=0x00))
    b.channel(36, mode2_record('m2 notg', x2E=0x00))
    b.channel(37, mode2_record('m2 any', x20=0x7E, x21=0x0F))
    b.channel(38, fx.channel_record('Unknown', 433_000_000, mode=0x03))
    b.zone(0, 'P25', [10, 11, 12, 30, 32])
    return b.bytes()


def image_full():
    """1024 channels in every mode, 68 zones of 80 channels each (the most the firmware keeps)"""
    b = fx.ImageBuilder()
    b.general('TA0BIG', 2860123)
    modes = [
        dict(mode=0, rx_tone='88.5', tx_tone='88.5', flag4=cp.FLAG4_25K, power=9),
        dict(mode=1, cc=1, rx_group=1, contact=1, ts2=True, power=5),
        dict(mode=0xA6, rx_group=0, flag4=cp.FLAG4_RX_ONLY),
        dict(mode=0xA7, rx_group=9, flag4=cp.FLAG4_RX_ONLY),
        dict(mode=0xA8, rx_group=2, flag4=cp.FLAG4_RX_ONLY),
        dict(mode=0, tot=8, flag4=cp.FLAG4_ZONE_SKIP, power=3),
        dict(mode=0xA5, cc=0xD6, rx_group=0x59, contact=101, unit_id=2860123),
    ]
    for n in range(1, 1025):
        kw = dict(modes[n % len(modes)])
        rx = 430_000_000 + n * 12_500
        tx = rx - 7_600_000 if n % 5 == 0 else None
        b.channel(n, fx.channel_record(f'Ch {n:04d}', rx, tx, **kw))
    for z in range(cp.ZONE_SLOTS):
        b.zone(z, f'Zone {z + 1:02d}', [((z * 80 + i) % 1024) + 1 for i in range(80)])
    for i in range(1, 41):
        b.contact(i, f'TG {i * 10}', i * 10, kind=0)
    b.contact(41, 'Friend', 2860001, kind=1, ts=1)
    b.contact(42, 'All call', 0, kind=2)
    b.rx_list(1, 'Local', [1, 2, 3, 41])
    b.rx_list(76, 'Last', list(range(1, 33)))
    return b.bytes()


def image_zone16():
    b = fx.ImageBuilder()
    for n in (1, 2, 3):
        b.channel(n, fx.channel_record(f'Old {n}', 145_000_000 + n * 25_000))
    b.img[cp.ZONE_LAYOUT] = 0x05
    b.zone(2, 'Old layout', [1, 2, 3], cap=16)
    b.zone(5, '', [1], cap=16)        # a zone without a name: not shown
    return b.bytes()


def image_empty():
    return fx.ImageBuilder().bytes()


def result(image):
    d = cp.decode(image)
    tables = {
        'channels': (cp.CHANNEL_COLUMNS, d.channels), 'zones': (cp.ZONE_COLUMNS, d.zones),
        'contacts': (cp.CONTACT_COLUMNS, d.contacts), 'lists': (cp.LIST_COLUMNS, d.lists),
    }
    fix = lambda rows: [{k: nac_dec(v) for k, v in r.items()} for r in rows]
    return {
        'counts': d.counts(), 'mode_counts': d.mode_counts(), 'bad_records': d.bad_records,
        'rfkv_entries': d.rfkv_entries, 'zone_capacity': d.zone_capacity, 'radio_name': d.radio_name,
        'dmr_id': d.dmr_id,
        'channels': fix(d.channels), 'zones': fix(d.zones), 'contacts': fix(d.contacts), 'lists': fix(d.lists),
        'csv': {k: nac_dec(cp.to_csv(cols, rows)) for k, (cols, rows) in tables.items()},
    }


def main():
    images = {'sample': fx.sample_image(), 'p25': image_p25(), 'full': image_full(), 'zone16': image_zone16(),
              'empty': image_empty()}
    out = {
        'source': 'pc/ddm32_app/codeplug.py decode / to_csv on pc/tests/fixtures.py images; NAC texts as nac_dec',
        'columns': {k: [list(c) for c in cols] for k, cols in (('channels', cp.CHANNEL_COLUMNS),
                                                                ('zones', cp.ZONE_COLUMNS),
                                                                ('contacts', cp.CONTACT_COLUMNS),
                                                                ('lists', cp.LIST_COLUMNS))},
        'name_check': [[n, cp.name_check(n.encode('latin-1')[:16].ljust(16, b'\xff'))]
                       for n in ('Rpt', 'P25 659', '', 'ABCDEFGHIJKLMNOP')],
        'tones': [[w, cp.tone(w)] for w in (0, 0xFFFF, 0x0670, 0x0885, 0x2541, 0x8023, 0xC023, 0x8754, 0x8780,
                                            0x0299, 0x3001, 0x4123)],
        'images': {},
    }
    for name, img in images.items():
        assert len(img) == cp.IMAGE_SIZE
        out['images'][name] = {'sparse': sparse(img), 'expected': result(img)}
        assert bytes(rebuild(out['images'][name]['sparse'])) == img, name
    with gzip.open(OUT, 'wt', encoding='utf-8', newline='\n') as f:
        json.dump(out, f, ensure_ascii=False, separators=(',', ':'))
    counts = ', '.join('%s %d channels' % (k, len(v['expected']['channels'])) for k, v in out['images'].items())
    print(f'{OUT}: {counts}')


def rebuild(runs):
    img = bytearray(b'\xff' * cp.IMAGE_SIZE)
    for addr, data in runs:
        raw = base64.b64decode(data)
        img[addr:addr + len(raw)] = raw
    return img


if __name__ == '__main__':
    main()
