#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus user data pack builder (docs/r14.md 7, docs/plus.md "User data packs"). Runs on the USER's PC, on
files the user downloaded themselves: nothing it reads or writes is ever part of this repository or a release.

    datapack.py radioid --csv user.csv [--country Turkey ...] [--date YYYYMMDD] --out radioid.dpk
        RadioID.net's user list (https://radioid.net, "user.csv") -> a RadioID pack (format 1)
    datapack.py air --pdf LT_AD_2_LTAC_en.pdf [...] | --text CODE=file.txt [...] [--date YYYYMMDD] --out air.dpk
        the aerodromes' own AIP pages (AD 2.18 "ATS communication facilities", as published by the State's AIS; the
        user downloads them) -> an air band pack (format 1). PDF reading needs pymupdf; --text takes the page text
    datapack.py info pack.dpk
        the header, the payload's CRC, what it holds

A .dpk file is the slot image the radio keeps: the 64-byte header, then the payload. How it reaches the radio:
docs/plus.md "User data packs" (the CPS protocol: payload sectors first, the header's sector last) or DOTA Transfer
from another radio. The firmware side is core/storage/datapack.c (integration/radiofoundry/datapack/)."""
import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import airfields, iddb  # noqa: E402

MAGIC = b'DPK1'
RADIOID, AIR, REF = 1, 2, 3			# REF: the map pack (tools/refpack.py, docs/mappack-design.md)
HDR = struct.Struct('<4sBBHIII8s16s16s')		# then the header's own CRC-32: 64 bytes
# Layout 2 (firmware 1.1, docs/mappack-design.md A.1): datapack.c slots[]. RadioID keeps 1.0.x's slot starts with a
# 192 KB limit (also safe for a 1.0.x radio: same starts); the map pack's slots were the tails of 1.0.x's 256 KB ones
SLOT = {RADIOID: 0x30000, AIR: 0x4000, REF: 0x10000}
SLOT_ADDR = {RADIOID: (0xE70000, 0xEB0000), AIR: (0xEF0000, 0xEF4000), REF: (0xEA0000, 0xEE0000)}
SECTOR = 4096
ID_CHARS = 16					# 6-bit characters a record's text, a multiple of 4
AIR_FREQS = 64					# air_pack.h AIR_PACK_FREQS
TAG = b'datapack.py 1'


class PackError(Exception):
    pass


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF		# = storage_crc32(0, ...)


def header(ptype, payload, date, name, seq=0, tag=TAG, flags=0):
    """flags: 0 for RadioID and air band; a map pack's version hint (refpack.py), 0 = no hint"""
    if not re.fullmatch(r'\d{8}', date):
        raise PackError(f'date {date!r}: YYYYMMDD')
    if len(payload) > SLOT[ptype] - 64:
        raise PackError(f'{len(payload)} bytes: the slot holds {SLOT[ptype] - 64}')
    h = HDR.pack(MAGIC, ptype, 1, flags & 0xFFFF, seq, len(payload), crc32(payload), date.encode('ascii'),
                 name.encode('ascii', 'replace')[:16], tag[:16])
    return h + struct.pack('<I', crc32(h))


def reseq(image, seq):
    """The same pack with another sequence (what a writer programs into the radio's slot): every other header field,
    the map pack's flags too, carried from the .dpk"""
    if len(image) < 64 or image[:4] != MAGIC:
        raise PackError('not a data pack')
    h = bytearray(image[:60])
    struct.pack_into('<I', h, 8, seq)
    return bytes(h) + struct.pack('<I', crc32(bytes(h))) + image[64:]


def parse(image):
    """header fields of a slot image (or None when it is no pack), and the payload"""
    if len(image) < 64 or image[:4] != MAGIC or struct.unpack_from('<I', image, 60)[0] != crc32(image[:60]):
        return None, b''
    magic, ptype, fmt, flags, seq, length, crc, date, name, tag = HDR.unpack_from(image)
    payload = image[64:64 + length]
    return dict(type=ptype, fmt=fmt, flags=flags, seq=seq, len=length, crc=crc, date=date.decode('ascii', 'replace'),
                name=name.rstrip(b'\0').decode('ascii', 'replace'), tag=tag.rstrip(b'\0').decode('ascii', 'replace'),
                crc_ok=len(payload) == length and crc32(payload) == crc), payload


def next_seq(images):
    """The sequence a new pack gets: one more than any the radio's two slots hold under a header that checks (valid or
    rejected), so the new pack wins over both"""
    seqs = [h['seq'] for h, _ in map(parse, images) if h]
    return max(seqs, default=0) + 1


def slot_valid(ptype, image):
    """Whether the firmware (1.1, layout 2) takes this slot image as a whole pack of the type: header, length, payload
    CRC and, for a map pack, its structure and version (refpack.validate: the firmware's ref_validate rules)"""
    h, payload = parse(image)
    if not h or not h['crc_ok'] or h['type'] != ptype or h['fmt'] != 1 or h['len'] > SLOT[ptype] - 64:
        return False
    if ptype == REF:
        from tools import refpack
        return refpack.validate(payload, h['flags'])[0] == 'ok'
    return True


def inactive_slot(ptype, images):
    """Which slot a new pack goes into: the one not in force (slot 0 when neither holds a valid pack). In force is the
    valid slot with the higher sequence, by the firmware's own rules (slot_valid): a newer slot it rejects next to a
    valid older one is the one overwritten"""
    best = None
    for i, img in enumerate(images):
        h, _ = parse(img)
        if h and slot_valid(ptype, img) and (best is None or h['seq'] > best[1]):
            best = (i, h['seq'])
    return 0 if best is None else 1 - best[0]


def ref_write_slot(images, running_valid=None):
    """Which map pack slot a writer overwrites (docs/mappack-design.md E.3), None: write nothing. images: both slots'
    full images (None for one that could not be read in full: nothing is written). The slot in force for the RUNNING
    firmware (running_valid: its slot check; default, when its rules are not known, header + payload CRC) is never
    overwritten, so a failed firmware install that follows a pack-first update keeps its map; otherwise the target
    firmware's own rules (slot_valid) decide, as inactive_slot: the rejected or empty slot, else the older valid one"""
    if len(images) != 2 or any(img is None for img in images):
        return None

    def crc_ok(img):
        h, _ = parse(img)
        return bool(h and h['crc_ok'] and h['type'] == REF)
    check = running_valid or crc_ok
    run = [i for i, img in enumerate(images) if check(img)]
    if run:
        keep = max(run, key=lambda i: parse(images[i])[0]['seq'])
        return 1 - keep
    return inactive_slot(REF, images)


def cps_plan(slot_addr, image):
    """The CPS sector writes for a slot image, in order: the payload's sectors, then the header's sector last (its
    rewrite is the commit: until then the slot does not count). [(address, 4096 bytes)]"""
    padded = image + b'\xff' * (-len(image) % SECTOR)
    secs = [(slot_addr + o, padded[o:o + SECTOR]) for o in range(0, len(padded), SECTOR)]
    first = secs[0]
    return secs[1:] + [first]


# --- RadioID (format 1) ---

def _code6(c):
    if c == ' ':
        return 0
    if '0' <= c <= '9':
        return 1 + ord(c) - 48
    if 'A' <= c <= 'Z':
        return 11 + ord(c) - 65
    if 'a' <= c <= 'z':
        return 37 + ord(c) - 97
    return 63 if c == '.' else 0


def radioid_payload(entries, chars=ID_CHARS):
    """entries [(id, callsign, name)] -> payload: 8-byte header, records sorted by ID (3 bytes LE, text 6 bits a
    character)"""
    if chars % 4 or not 4 <= chars <= 32:
        raise PackError('text characters: 4..32, a multiple of 4')
    rec = 3 + chars * 3 // 4
    out = bytearray(struct.pack('<IBBH', len(entries), rec, chars, 0))
    last = 0
    for rid, call, name in sorted(entries):
        if not 0 < rid <= 0xFFFFFF or rid <= last:
            raise PackError(f'ID {rid}: out of range or repeated')
        last = rid
        text = iddb.fold(f'{call} {name}'.strip())[:chars].ljust(chars)
        out += rid.to_bytes(3, 'little')
        for k in range(0, chars, 4):
            v = 0
            for c in text[k:k + 4]:
                v = v << 6 | _code6(c)
            out += v.to_bytes(3, 'big')
    return bytes(out)


def radioid_from_csv(path, countries=()):
    rows, skipped = iddb.read(path, countries)
    return radioid_payload([(rid, call, name) for rid, call, _packed, name in rows]), len(rows), skipped


# --- Air band (format 1) ---

FREQ = re.compile(r'(?<![\d.])(1[0-3]\d[.,]\d{1,3})\s*(?:MHZ|MHz|Mhz)?(?![\d.])')
SECTION = re.compile(r'AD\s*[23]\s*\.\s*18')
END = re.compile(r'AD\s*[23]\s*\.\s*19')


def parse_ad218(text):
    """The AD 2.18 (AD 3.18) table's lines as page text -> [(type index, kHz)] in the table's order: each line with an
    air band frequency, its service named by the line's own words (or the last named one above it)"""
    m = SECTION.search(text)
    if m:
        text = text[m.end():]
        e = END.search(text)
        text = text[:e.start()] if e else text
    out, service = [], None
    for line in text.splitlines():
        words = re.sub(r'[^A-Za-z/ -]', ' ', line).upper().split()
        named = [w for w in words if w in airfields.TYPE_WORDS]
        if named:
            service = airfields.TYPE_WORDS[named[0]]
        for f in FREQ.findall(line):
            khz, why = airfields.channel_khz(f.replace(',', '.'))
            if why or khz is None:
                continue
            t = airfields.TYPES.index(service) if service in airfields.TYPES else airfields.TYPES.index('OTH')
            if (t, khz) not in out:
                out.append((t, khz))
    return out


def air_payload(fields):
    """{code: [(type index, kHz)]} -> payload: 4-byte header, per airfield code (8), count (1), (channel 2, type 1)"""
    out = bytearray(struct.pack('<HH', len(fields), 0))
    for code in sorted(fields):
        freqs = fields[code][:AIR_FREQS]
        c = code.encode('ascii')
        if not 0 < len(c) <= 8:
            raise PackError(f'code {code!r}')
        out += c.ljust(8, b'\0') + bytes([len(freqs)])
        for t, khz in freqs:
            out += struct.pack('<HB', (khz - airfields.CH_BASE_KHZ) // 5, t)
    return bytes(out)


def pdf_text(path):
    try:
        import pymupdf
    except ImportError as e:
        raise PackError('reading PDF needs pymupdf (pip install pymupdf), or give the text with --text') from e
    with pymupdf.open(str(path)) as doc:
        return '\n'.join(page.get_text() for page in doc)


CODE = re.compile(r'(?<![A-Za-z0-9])(LT[A-Z]{2})(?![A-Za-z0-9])')


def code_of(path, text=None):
    """The aerodrome code (LTxx) in a file name, its last one (LT_AD_2_LTAC_en.pdf -> LTAC: an underscore counts as a
    separator here, unlike for \\b), else the last one in the page text before its AD 2.18 heading (as AirText.CodeOf
    in win/ does)."""
    found = CODE.findall(Path(path).stem)
    if not found and text is not None:
        sec = SECTION.search(text)
        found = CODE.findall(text[:sec.start()] if sec else text)
    if not found:
        raise PackError(f'{path}: no aerodrome code (LTxx) in the file name or the page text')
    return found[-1]


def info(image):
    h, payload = parse(image)
    if h is None:
        return 'not a data pack'
    lines = [f"type {h['type']} format {h['fmt']} sequence {h['seq']} \"{h['name']}\" date {h['date']} "
             f"{h['len']} bytes, payload CRC {'ok' if h['crc_ok'] else 'WRONG'}, built by {h['tag']}"]
    if h['type'] == RADIOID and len(payload) >= 8:
        n, rec, chars, _ = struct.unpack_from('<IBBH', payload)
        lines.append(f'{n} IDs, {chars} characters a record ({rec} bytes)')
    if h['type'] == AIR and len(payload) >= 4:
        lines.append(f"{struct.unpack_from('<H', payload)[0]} airfields")
    if h['type'] == REF:
        from tools import refpack
        lines.append(refpack.describe(payload, h['flags']))
    return '\n'.join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('radioid')
    r.add_argument('--csv', required=True)
    r.add_argument('--country', action='append', default=[])
    r.add_argument('--date', required=True)
    r.add_argument('--name', default='RadioID')
    r.add_argument('--out', required=True)
    a = sub.add_parser('air')
    a.add_argument('--pdf', action='append', default=[])
    a.add_argument('--text', action='append', default=[], help='CODE=page text file')
    a.add_argument('--date', required=True)
    a.add_argument('--name', default='Aerodromes')
    a.add_argument('--out', required=True)
    i = sub.add_parser('info')
    i.add_argument('pack')
    args = ap.parse_args(argv)
    try:
        if args.cmd == 'info':
            print(info(Path(args.pack).read_bytes()))
            return 0
        if args.cmd == 'radioid':
            payload, n, skipped = radioid_from_csv(args.csv, args.country)
            image = header(RADIOID, payload, args.date, args.name) + payload
            print(f'{n} IDs, {len(payload)} bytes; skipped {skipped}')
        else:
            fields = {}
            for p in args.pdf:
                text = pdf_text(p)
                fields[code_of(p, text)] = parse_ad218(text)
            for t in args.text:
                code, _, f = t.partition('=')
                fields[code.strip().upper()] = parse_ad218(Path(f).read_text(encoding='utf-8', errors='replace'))
            payload = air_payload(fields)
            image = header(AIR, payload, args.date, args.name) + payload
            print(f'{len(fields)} airfields, {sum(len(v) for v in fields.values())} frequencies, {len(payload)} bytes')
        Path(args.out).write_bytes(image)
        return 0
    except (PackError, iddb.IddbError, OSError) as e:
        print(f'datapack: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
