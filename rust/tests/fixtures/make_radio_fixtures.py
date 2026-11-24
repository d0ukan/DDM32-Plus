#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Records what the Python radio tools in pc/radiotools do, so the C# ports can be held to it byte for byte
(win/docs/porting-plan.md). Nothing here touches a radio or opens a serial port: every run talks to the pinned RAM
monitor's own FakeRadio (pc/tests/rtfixtures.fake_radio) behind the tools' real guards, through a recording port.

    DDM32_UPSTREAM=<RadioFoundry tree> python rust/tests/fixtures/make_radio_fixtures.py

Writes Fixtures/radio/:
  index.json        the input images' SHA-256 (the C# generators must make the same bytes), radio_detect answers,
                    the guards' answers to a table of frames and step sequences
  <scenario>.json.gz one tool run: its arguments, the radio's setup, every event on the serial port in order (open,
                    reset, close, and each write with the bytes the radio answered), the printed lines, the error, and
                    every file it wrote (JSON as text, images as SHA-256)

Time is virtual (each clock read advances 1 ms, sleep advances by its argument) and every UTC stamp is
2026-11-20T00:00:00Z, so a run is the same every time and the C# port, given the same virtual clock, must write the
same bytes, print the same lines and leave the same files. All data is synthetic: no backup, stock firmware or codec.
"""
import base64
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]  # firmware/ (this copy lives in rust/tests/fixtures)
PC = REPO / 'pc'
sys.path.insert(0, str(PC))
sys.path.insert(0, str(PC / 'tests'))

import rtfixtures as rt  # noqa: E402
from rtfixtures import SECTOR, SIZE, SLOT, START  # noqa: E402
from radiotools import dm32_readonly_backup as bk  # noqa: E402
from radiotools import install_firstboot as fb  # noqa: E402
from radiotools import install_plus as ip  # noqa: E402
from radiotools import prepare_unit_image as unit  # noqa: E402
from radiotools import radio_detect  # noqa: E402

OUT = HERE / 'radio'
STAMP = '2026-11-20T00:00:00Z'
M = None        # one pinned monitor module, shared by the tools and the FakeRadio (same virtual clock)


def monitor():
    global M
    if M is None:
        M = bk.load_upstream()
    return M


def no_port(*a, **k):
    raise AssertionError('serial port opened')


def sha(b):
    return hashlib.sha256(b).hexdigest()


def b64(b):
    return base64.b64encode(bytes(b)).decode('ascii')


# --- the synthetic inputs (Generators.cs makes the same bytes) ---------------------------------------------------
def rep(n):
    """0, 1, ... 255, 0, 1 ...: compresses well, so the transcripts stay small and the LZ4 path is used"""
    return bytes(i & 255 for i in range(n))


BOOT = rt.pattern(0x1000, 7) + rep(0xB000)
APP_CLASSIC = rt.pattern(0x800, 3) + rep(0x7000) + b'DDM32 Plus'
PLUS_APP = rt.pattern(0x800, 9) + rep(0x5000) + b'DDM32 Plus test build'


def program(app, slot=None):
    slot = rt.make_slot() if slot is None else slot
    p = BOOT + app + b'\xff' * (SLOT - START - len(app)) + slot
    return p + b'\xff' * (SIZE - len(p))


def stock_program(code, tables):
    """Boot, a small made-up stock application, the codec and tables where .047 keeps them, no slot"""
    p = bytearray(BOOT + rt.pattern(0x800, 19) + rep(0x3000) + b'\xff' * (SIZE - START - 0x3800))
    c, t = rt.STOCK_CODE_AT - 0x03000000, rt.STOCK_TABLES_AT - 0x03000000
    p[c:c + len(code)] = code
    p[t:t + len(tables)] = tables
    return bytes(p)


class Inputs:
    def __init__(self, root):
        self.root = Path(root)
        self.program = program(APP_CLASSIC)
        self.data = rt.make_data()
        self.code, self.tables, _ = rt.synthetic_codec()
        self.stock = stock_program(self.code, self.tables)
        self.target = self.program[:START] + PLUS_APP + b'\xff' * (SLOT - START - len(PLUS_APP)) + self.program[SLOT:]
        self.backup = self.root / 'backup'
        rt.write_backup(self.backup, self.program, self.data)
        self.stock_backup = self.root / 'stock-backup'
        rt.write_backup(self.stock_backup, self.stock, self.data)
        self.plus = rt.write_plus(self.root / 'plus', PLUS_APP)

    def hashes(self):
        return {'program': sha(self.program), 'data': sha(self.data), 'stock': sha(self.stock),
                'target': sha(self.target), 'plus_app': sha(PLUS_APP), 'slot': sha(rt.make_slot()),
                'code': sha(self.code), 'tables': sha(self.tables),
                'plus_manifest': b64((self.plus / 'ddm32-manifest.json').read_bytes()),
                'plus_sig': (self.plus / 'ddm32-manifest.json.sig').read_text()}


# --- virtual time ----------------------------------------------------------------------------------------------
class Clock:
    def __init__(self):
        self.t = 1000.0

    def monotonic(self):
        self.t += 0.001
        return self.t

    def time(self):
        return self.monotonic()

    def sleep(self, s):
        self.t += max(0.0, s)

    def gmtime(self, *a):
        return None

    def strftime(self, fmt, t=None):
        if fmt == '%Y-%m-%dT%H:%M:%SZ':
            return STAMP
        raise AssertionError('unexpected strftime ' + fmt)


# --- the recording port ----------------------------------------------------------------------------------------
class Recorder:
    """Sits where serial.Serial would: passes everything to the FakeRadio and records the port's events"""

    def __init__(self, radio):
        object.__setattr__(self, 'radio', radio)
        object.__setattr__(self, 'events', [['init', b64(radio.out)]])

    def __getattr__(self, name):
        return getattr(self.radio, name)

    def __setattr__(self, name, value):
        if name == 'baudrate' and value != 115200:
            self.events.append(['baud', value])
        setattr(self.radio, name, value)

    def open(self):
        self.events.append(['open'])
        self.radio.open()

    def close(self):
        self.events.append(['close'])
        self.radio.close()

    def write(self, data):
        before = len(self.radio.out)
        result = self.radio.write(bytes(data))
        self.events.append(['w', b64(data), b64(self.radio.out[before:])])
        return result

    def read(self, n):
        return self.radio.read(n)

    def reset_input_buffer(self):
        self.events.append(['reset'])
        self.radio.reset_input_buffer()

    def flush(self):
        self.radio.flush()


def run_tool(module, argv0, args, radio=None):
    """(printed lines, error, port events) of module.main() under virtual time, the radio behind a recorder"""
    clock = Clock()
    rec = Recorder(radio) if radio is not None else None
    m = monitor()
    out = io.StringIO()
    error = None
    mods = [module, bk, fb, ip, m]
    saved = [mod.time for mod in mods]
    try:
        for mod in mods:
            mod.time = clock
        with patch.object(bk, 'load_upstream', lambda: m), \
                patch.object(sys, 'argv', [argv0, '--port', 'SIMULATED', *map(str, args)]), \
                patch('serial.Serial', (lambda *a, **k: rec) if rec else no_port), \
                patch('sys.stdout', out), patch('sys.stderr', io.StringIO()):
            try:
                module.main()
            except SystemExit as e:
                error = ['SystemExit', str(e.code)]
            except BaseException as e:      # recorded as the C# port must report it
                error = [type(e).__name__, str(e)]
    finally:
        for mod, t in zip(mods, saved):
            mod.time = t
    return out.getvalue().splitlines(), error, (rec.events if rec else None)


def files_of(folder):
    out = {}
    if folder.is_dir():
        for p in sorted(folder.iterdir()):
            body = p.read_bytes()
            if p.suffix == '.json':
                out[p.name] = {'text': body.decode('utf-8')}
            else:
                out[p.name] = {'sha256': sha(body), 'bytes': len(body)}
    return out


def save(name, tool, args, radio_setup, lines, error, events, files, stock):
    OUT.mkdir(parents=True, exist_ok=True)
    doc = {'name': name, 'tool': tool, 'args': args, 'stock_codec': stock, 'radio': radio_setup, 'lines': lines,
           'error': error,
           'events': events, 'files': files}
    raw = json.dumps(doc, indent=0).encode('utf-8')
    with gzip.GzipFile(OUT / f'{name}.json.gz', 'wb', mtime=0) as f:
        f.write(raw)
    print(f'{name}: {len(events or [])} events, {len(lines)} lines, error={error}')


def scenario(inp, name, tool, args, image=None, data=None, *, radio_kw=None, setup=None, stock=False):
    """Runs one tool; args use {backup}, {stock_backup}, {plus}, {out} placeholders (the C# test fills its own)"""
    module, argv0 = {'backup': (bk, 'dm32_readonly_backup.py'), 'install_plus': (ip, 'install_plus.py'),
                     'restore_stock': (fb, 'install_firstboot.py')}[tool]
    out = inp.root / f'out-{name}'
    real = [str(a).format(backup=inp.backup, stock_backup=inp.stock_backup, plus=inp.plus, out=out) for a in args]
    m = monitor()
    radio = None
    if image is not None:
        kw = dict(radio_kw or {})
        known = {k: kw.pop(k) for k in ('corrupt_after_writes', 'data_jedec') if k in kw}
        radio = rt.fake_radio(m, image, inp.data if data is None else data, **known)
        for k, v in kw.items():                 # FakeRadio's own fault switches
            setattr(radio, k, set(v) if isinstance(v, list) else v)
        if setup:
            setup(radio)
    with patch.object(rt.sm, 'PUBLIC_KEY_HEX', rt.TEST_PUBLIC_HEX):
        if stock:
            with rt.synthetic_stock_codec(unit):
                lines, error, events = run_tool(module, argv0, real, radio)
        else:
            lines, error, events = run_tool(module, argv0, real, radio)
    # paths in printed lines and errors are the run's own: written as placeholders
    def generic(s):
        for key, p in (('out', out), ('backup', inp.backup), ('stock_backup', inp.stock_backup), ('plus', inp.plus)):
            s = s.replace(str(p), '{%s}' % key)
        return s
    lines = [generic(l) for l in lines]
    error = [error[0], generic(error[1])] if error else None
    setup_doc = dict(radio_kw or {}, image=sha(image) if image is not None else None,
                     data=sha(inp.data if data is None else data) if image is not None else None,
                     setup=setup.__name__ if setup else None)
    save(name, tool, [str(a) for a in args], setup_doc, lines, error, events, files_of(out), stock)


# --- radio setups by name (Scenarios.cs applies the same to its replay) ----------------------------------------
def silent_rom(radio):
    """The radio was not switched on: no boot ROM greeting comes"""
    radio.out.clear()


def scenarios(inp):
    ok_install = ['--backup', '{backup}', '--plus', '{plus}', '--out', '{out}', '--wait', 5]
    scenario(inp, 'backup_ok', 'backup', ['--out', '{out}', '--installed-firmware', 'DDM32 Plus 1.0.0', '--wait', 5],
             inp.program)
    scenario(inp, 'backup_radio_off', 'backup', ['--out', '{out}', '--installed-firmware', 'x', '--wait', 3],
             inp.program, setup=silent_rom)
    scenario(inp, 'backup_rom_nak', 'backup', ['--out', '{out}', '--installed-firmware', 'x', '--wait', 5],
             inp.program, radio_kw={'rom_nak': True})
    scenario(inp, 'backup_bad_density', 'backup', ['--out', '{out}', '--installed-firmware', 'x', '--wait', 5],
             inp.program, radio_kw={'data_jedec': 'ef4017'})
    scenario(inp, 'backup_damaged_frames', 'backup', ['--out', '{out}', '--installed-firmware', 'x', '--wait', 5],
             inp.program, radio_kw={'flip': [9, 30], 'silent': [12], 'swap': [40]})
    scenario(inp, 'backup_wait_refused', 'backup', ['--out', '{out}', '--installed-firmware', 'x', '--wait', 121])

    scenario(inp, 'install_plan_only', 'install_plus', ok_install + ['--plan-only'])
    scenario(inp, 'install_ok', 'install_plus', ok_install, inp.program)
    changed = bytearray(inp.program)
    changed[START + 0x20000] ^= 0xff
    scenario(inp, 'install_live_program_changed', 'install_plus', ok_install, bytes(changed))
    data = bytearray(inp.data)
    data[0x800010] ^= 1
    scenario(inp, 'install_live_data_changed', 'install_plus', ok_install, inp.program, bytes(data))
    scenario(inp, 'install_readback_corrupt', 'install_plus', ok_install, inp.program,
             radio_kw={'corrupt_after_writes': START + 0x10})
    scenario(inp, 'install_slot_changed', 'install_plus', ok_install, inp.program,
             radio_kw={'corrupt_after_writes': SLOT + 0x90})
    scenario(inp, 'install_wrong_flash_id', 'install_plus', ok_install, inp.program, radio_kw={'data_jedec': 'ef4017'})
    scenario(inp, 'install_stock_refused', 'install_plus',
             ['--backup', '{stock_backup}', '--plus', '{plus}', '--out', '{out}', '--plan-only'], stock=True)
    scenario(inp, 'install_args_refused', 'install_plus', ['--backup', '{backup}', '--out', '{out}'])

    stock_args = ['--backup', '{stock_backup}', '--plus', '{plus}', '--from-stock', '--out', '{out}', '--wait', 5]
    scenario(inp, 'from_stock_plan_only', 'install_plus', stock_args + ['--plan-only'], stock=True)
    scenario(inp, 'from_stock_ok', 'install_plus', stock_args, inp.stock, stock=True)
    scenario(inp, 'from_stock_slot_corrupt', 'install_plus', stock_args, inp.stock,
             radio_kw={'corrupt_after_writes': SLOT + 0x100}, stock=True)
    scenario(inp, 'from_stock_classic_refused', 'install_plus',
             ['--backup', '{backup}', '--plus', '{plus}', '--from-stock', '--out', '{out}', '--plan-only'], stock=True)

    restore = ['--backup', '{backup}', '--restore', '--out', '{out}', '--wait', 5]
    broken = bytearray(inp.target)
    broken[START + 3 * SECTOR:START + 5 * SECTOR] = b'\xff' * (2 * SECTOR)
    broken[SLOT + 0x200:SLOT + 0x210] = bytes(16)
    scenario(inp, 'restore_plan_only', 'install_plus', restore + ['--plan-only'])
    scenario(inp, 'restore_ok', 'install_plus', restore, bytes(broken))
    scenario(inp, 'restore_readback_corrupt', 'install_plus', restore, inp.target,
             radio_kw={'corrupt_after_writes': SLOT + 0x90})

    stock_restore = ['--backup', '{stock_backup}', '--out', '{out}', '--restore-stock', '--use-saved-backup',
                     '--wait', 5]
    installed = inp.stock[:START] + inp.program[START:]
    scenario(inp, 'restore_stock_plan_only', 'restore_stock', stock_restore + ['--plan-only'])
    scenario(inp, 'restore_stock_ok', 'restore_stock', stock_restore, installed)
    scenario(inp, 'restore_stock_other_chip', 'restore_stock', stock_restore, installed,
             radio_kw={'data_jedec': 'ef4019'})
    scenario(inp, 'restore_stock_args_refused', 'restore_stock', ['--backup', '{stock_backup}', '--out', '{out}'])


# --- radio_detect --------------------------------------------------------------------------------------------------
def detect_cases(inp):
    out = []
    progs = [('classic_ddm32', program(APP_CLASSIC)),
             ('classic_ddm25', program(rt.pattern(0x900, 3) + b'DDM25 Plus')),
             ('classic_open25', program(b'open25-classic-r13' + rt.pattern(0x900, 3))),
             ('classic_unknown', program(rt.pattern(0x900, 3))),
             ('no_slot_classic_app', program(APP_CLASSIC, slot=b'\xff' * 4096)),
             ('no_slot_unknown', program(rt.pattern(0x900, 5), slot=b'\xff' * 4096)),
             ('bad_slot_crc', program(APP_CLASSIC, slot=rt.make_slot()[:0x40] + b'\0' + rt.make_slot()[0x41:]))]
    for name, p in progs:
        out.append({'name': name, 'program': sha(p), 'stock': False, 'result': radio_detect.detect(p)})
    with rt.synthetic_stock_codec(unit):
        moved = bytearray(inp.stock)
        c = rt.STOCK_CODE_AT - 0x03000000
        moved[c:c + 8] = bytes(8)
        for name, p in (('stock_047', inp.stock), ('stock_moved', bytes(moved)), ('classic_with_stock_codec',
                                                                                 inp.program)):
            out.append({'name': name, 'program': sha(p), 'stock': True, 'result': radio_detect.detect(p)})
        target, prof = unit.compose_from_stock(inp.stock, PLUS_APP)
        out.append({'name': 'compose_from_stock', 'program': sha(inp.stock), 'stock': True,
                    'result': {'target_sha256': sha(target), 'profile': prof['name'],
                               'slot': {k: v for k, v in unit.load_codec().parse_slot(target[SLOT:]).items()}}})
    # the printed JSON of radio_detect.py --backup
    o = io.StringIO()
    with patch.object(sys, 'argv', ['radio_detect.py', '--backup', str(inp.backup)]), patch('sys.stdout', o):
        radio_detect.main()
    out.append({'name': 'tool_output', 'printed': o.getvalue()})
    return out


# --- guard tables ---------------------------------------------------------------------------------------------------
def guard_cases():
    m = monitor()
    pk, sp = fb.packet, fb.spi_packet
    q128 = bytes.fromhex('140a00000312')
    prog = bytes.fromhex('040000000100')

    def op5(sel, tx, rx):
        return pk(5, sel + len(tx).to_bytes(2, 'big') + rx.to_bytes(2, 'big') + tx)

    def op7(sel, a, n):
        return pk(7, sel + a.to_bytes(4, 'big') + n.to_bytes(2, 'big'))

    def op9(sel, a, n, mode=1):
        return pk(9, sel + a.to_bytes(4, 'big') + bytes([n, mode]))
    cfg = (24000000).to_bytes(4, 'big') * 2 + bytes([255, 255, 33, 32, 1, 0, 3, 0])
    frames = [
        ('hello', pk(1)), ('hello with payload', pk(1, b'x')), ('timer', pk(2, bytes.fromhex('140000180004'))),
        ('peek other', pk(2, bytes.fromhex('140000200004'))), ('config', pk(0x0d, cfg)),
        ('config other clock', pk(0x0d, (5000000).to_bytes(4, 'big') + (5000001).to_bytes(4, 'big') + cfg[8:])),
        ('config other board', pk(0x0d, cfg[:12] + bytes([2, 0, 3, 0]))),
        ('jedec prog', op5(prog, b'\x9f', 3)), ('jedec data', op5(q128, b'\x9f', 3)), ('jedec rx 2', op5(prog, b'\x9f', 2)),
        ('read prog', op5(prog, b'\x03\x00\x10\x00', 1024)), ('read last', op5(prog, b'\x03\x0f\xfc\x00', 1024)),
        ('read past end', op5(prog, b'\x03\x0f\xfc\x01', 1024)), ('read too big', op5(prog, b'\x03\x00\x00\x00', 1025)),
        ('read data', op5(q128, b'\x03\xff\xf0\x00', 4096 // 4)), ('read data past end', op5(q128, b'\x03\xff\xff\xff', 2)),
        ('status read', op5(prog, b'\x05', 1)), ('write enable', op5(prog, b'\x06', 0)),
        ('erase', op5(prog, b'\x20\x00\xc0\x00', 0)), ('page program', op5(prog, b'\x02\x00\xc0\x00' + b'\0' * 256, 0)),
        ('chip erase', op5(prog, b'\xc7', 0)), ('data erase', op5(q128, b'\x20\x00\x10\x00', 0)),
        ('crc prog', op7(prog, 0, 16)), ('crc 17', op7(prog, 0, 17)), ('crc misaligned', op7(prog, 0x800, 1)),
        ('crc data end', op7(q128, 0xFF0000, 16)), ('crc data past', op7(q128, 0xFF1000, 16)),
        ('sectors', op9(prog, 0xC000, 16)), ('sectors raw', op9(prog, 0xC000, 16, 0)), ('sectors mode 2', op9(prog, 0, 1, 2)),
        ('sectors 0', op9(prog, 0, 0)), ('sectors past', op9(prog, 0xFF000, 2)),
        ('poke', pk(3, (0x11000000).to_bytes(4, 'big') + bytes(4))), ('call', pk(4, bytes(20))),
        ('baud', pk(0x0b, bytes(8))), ('sector write', pk(0x0e, prog + (0xC000).to_bytes(4, 'big'))),
        ('led', pk(8, bytes(15))), ('bad checksum', pk(1)[:4] + b'\x01' + pk(1)[5:]),
        ('bad length', bytes([2, 1, 0, 1, 0, 3])), ('not a frame', b'\x01\x02\x03\x04\x05\x06'), ('short', b'\x02\x01'),
        ('wrong controller', op5(bytes.fromhex('140b00000312'), b'\x9f', 3)),
        ('wrong chip select', op5(bytes.fromhex('140a00000311'), b'\x9f', 3)),
    ]

    def verdict(guard, data):
        try:
            return guard.validate(bytes(data))
        except Exception as e:
            return 'refused: ' + str(e)

    class Raw:
        timeout = 0.02
        baudrate = 115200

        def write(self, b):
            return len(b)

    out = {'readonly_before_monitor': [], 'readonly': [], 'install_sequences': []}
    g = bk.ReadOnlyPort(Raw(), m)
    image = m.stub_univ_image() + b'\xff' * (-len(m.stub_univ_image()) % 256)
    blocks = [m.rom_cmd(0x18, (0x10000 + i).to_bytes(4, 'big') + (64).to_bytes(4, 'big') + image[i:i + 256])
              for i in range(0, len(image), 256)]
    seq = [('rom reply', m.ROM_REPLY), ('go too early', m.rom_cmd(0x1b, (0x10000).to_bytes(4, 'big'))),
           ('other block', m.rom_cmd(0x18, (0x20000).to_bytes(4, 'big') + bytes(260)))]
    seq += [(f'block {i}', b) for i, b in enumerate(blocks)]
    seq += [('go other address', m.rom_cmd(0x1b, (0x20000).to_bytes(4, 'big'))),
            ('go', m.rom_cmd(0x1b, (0x10000).to_bytes(4, 'big'))), ('hello after go', pk(1))]
    for name, f in seq:
        out['readonly_before_monitor'].append({'name': name, 'frame': f.hex(), 'verdict': verdict(g, f)})
    for limits in ({'progmem': 0x100000, 'q128': 0x1000000}, {'progmem': 0, 'q128': 0}):
        g = bk.ReadOnlyPort(Raw(), m)
        g.monitor = True
        g.limits = dict(limits)
        for name, f in frames:
            out['readonly'].append({'name': name, 'limits': limits, 'frame': f.hex(), 'verdict': verdict(g, f)})

    # InstallPort: an original and a target differing in a few application sectors and the slot
    original = rt.make_program()
    tgt = bytearray(original)
    tgt[START:START + 0x300] = rt.pattern(0x300, 11)              # sector 0xC000: three pages
    tgt[START + 0x5000:START + 0x5001] = b'\x00'                  # sector 0x11000: one page
    tgt[SLOT + 0x20] ^= 1                                         # a slot sector
    target = bytes(tgt)
    crcs = fb.crc_list(rt.make_data())
    other_crcs = list(crcs)
    other_crcs[5] ^= 1
    named = {'original': original, 'target': target, 'crcs': crcs, 'other_crcs': other_crcs,
             'original_app': original[START:], 'target_app': target[START:]}

    def install_seq(name, skip, steps):
        g = fb.InstallPort(Raw(), m, original, target, skip_original=skip)
        g.monitor = True
        g.limits = {'progmem': 0x100000, 'q128': 0x1000000}
        rec = {'name': name, 'skip_original': skip, 'todo': g.todo, 'plan': [f.hex() for f in g.plan], 'steps': []}
        for step in steps:
            kind = step[0]
            try:
                if kind == 'frame':
                    result = g.validate(step[1])
                elif kind == 'plan':                       # the next planned frame
                    result = g.validate(g.plan[g.index])
                elif kind == 'plan_all':
                    while g.index < len(g.plan):
                        g.validate(g.plan[g.index])
                    result = 'ok'
                elif kind == 'authorize':
                    g.authorize(*[named[x] for x in step[1:]])
                    result = 'ok'
                elif kind == 'authorize_saved':
                    g.authorize_saved_backup()
                    result = 'ok'
                elif kind == 'finish':
                    g.finish_writes()
                    result = 'ok'
                elif kind == 'reset_ok':
                    g.authorize_reset(*[named[x] for x in step[1:]])
                    result = 'ok'
                elif kind == 'reset_app':
                    g.authorize_reset_application(named[step[1]])
                    result = 'ok'
                elif kind == 'reset_frames':
                    result = [g.validate(f) for f in g.reset_plan]
                else:
                    raise AssertionError(kind)
            except Exception as e:
                result = 'refused: ' + str(e)
            rec['steps'].append({'step': kind, 'arg': step[1].hex() if kind == 'frame' else list(step[1:]),
                                 'result': result, 'index': g.index, 'enabled': g.enabled,
                                 'closed': g.closed_writes})
        out['install_sequences'].append(rec)

    status = sp(b'\x05', 1)
    poke = pk(3, (0x18007EF4).to_bytes(4, 'big') + (0x54464F53).to_bytes(4, 'little'))
    install_seq('normal install', False, [
        ('frame', sp(b'\x06')), ('plan',), ('authorize', 'original', 'crcs', 'crcs'), ('authorize', 'original', 'crcs', 'crcs'),
        ('frame', status), ('frame', sp(b'\x20\x00\xd0\x00')), ('plan',), ('plan',), ('frame', sp(b'\x06')),
        ('finish',), ('plan_all',), ('frame', sp(b'\x06')), ('frame', poke), ('reset_ok', 'original', 'crcs', 'crcs'),
        ('finish',), ('reset_ok', 'target', 'crcs', 'other_crcs'), ('reset_ok', 'target', 'crcs', 'crcs'),
        ('reset_ok', 'target', 'crcs', 'crcs'), ('frame', poke), ('reset_frames',), ('frame', poke)])
    install_seq('changed live program', False, [('authorize', 'target', 'crcs', 'crcs'), ('plan',)])
    install_seq('changed data chip', False, [('authorize', 'original', 'other_crcs', 'crcs'), ('plan',)])
    install_seq('saved backup', True, [
        ('frame', pk(7, bytes.fromhex('040000000100') + (0).to_bytes(4, 'big') + (16).to_bytes(2, 'big'))),
        ('frame', pk(5, bytes.fromhex('040000000100') + (4).to_bytes(2, 'big') + (1024).to_bytes(2, 'big') +
                     b'\x03\x00\xc0\x00')),
        ('authorize', 'original', 'crcs', 'crcs'), ('authorize_saved',), ('authorize_saved',), ('plan',), ('plan',),
        ('frame', pk(5, bytes.fromhex('040000000100') + (4).to_bytes(2, 'big') + (1024).to_bytes(2, 'big') +
                     b'\x03\x00\xc0\x00')),
        ('plan_all',),
        ('frame', pk(5, bytes.fromhex('040000000100') + (4).to_bytes(2, 'big') + (1024).to_bytes(2, 'big') +
                     b'\x03\x01\x10\x00')),
        ('frame', pk(5, bytes.fromhex('040000000100') + (4).to_bytes(2, 'big') + (1024).to_bytes(2, 'big') +
                     b'\x03\x00\xb0\x00')),
        ('frame', pk(5, bytes.fromhex('140a00000312') + (4).to_bytes(2, 'big') + (1024).to_bytes(2, 'big') +
                     b'\x03\x00\xc0\x00')),
        ('finish',),
        ('frame', pk(9, bytes.fromhex('040000000100') + (0xC000).to_bytes(4, 'big') + bytes([16, 1]))),
        ('frame', pk(9, bytes.fromhex('040000000100') + (0x8000).to_bytes(4, 'big') + bytes([16, 1]))),
        ('frame', pk(7, bytes.fromhex('140a00000312') + (0).to_bytes(4, 'big') + (16).to_bytes(2, 'big'))),
        ('reset_app', 'original_app'), ('reset_app', 'target_app'), ('reset_frames',)])
    install_seq('not saved-backup mode', False, [('authorize_saved',)])
    return out


def main():
    if not (rt.upstream.available()):
        raise SystemExit('Set DDM32_UPSTREAM to the pinned RadioFoundry tree')
    if OUT.exists():
        shutil.rmtree(OUT)
    with tempfile.TemporaryDirectory() as td:
        inp = Inputs(td)
        scenarios(inp)
        with patch.object(rt.sm, 'PUBLIC_KEY_HEX', rt.TEST_PUBLIC_HEX):
            index = {'note': 'Generated by make_radio_fixtures.py; do not edit by hand.', 'stamp': STAMP,
                     'inputs': inp.hashes(), 'detect': detect_cases(inp), 'guards': guard_cases(),
                     'ram_monitor_sha256': bk.RAM_SHA, 'codec': codec_tables()}
    (OUT / 'index.json').write_bytes((json.dumps(index, indent=1, ensure_ascii=True) + '\n').encode('ascii'))
    # What the C# port embeds from the pinned RadioFoundry files (MIT, LICENSES/RadioFoundry-MIT.txt): the RAM monitor
    # image the backup and install tools load into the radio, and the DM32 codec profiles and maps
    data = REPO / 'win' / 'src' / 'DDM32Plus.Core' / 'Radio' / 'Data'
    data.mkdir(parents=True, exist_ok=True)
    (data / 'ram-monitor.bin').write_bytes(monitor().stub_univ_image())
    (data / 'codec-dm32.json').write_bytes((json.dumps(index['codec'], indent=1) + '\n').encode('ascii'))
    total = sum(p.stat().st_size for p in OUT.iterdir())
    print(f'wrote {OUT} ({total} bytes)')


def codec_tables():
    """The pinned codec tool's DM32 profiles and codec maps as the C# port embeds them (Codec.cs): checked equal"""
    codec = unit.load_codec()
    profs = {h: p for h, p in codec.PROFILES.items() if p['board'] == 'dm32'}
    maps = {k: {'code': v['code'], 'entries': v['entries'], 'self_refs': v['self_refs'], 'islands': v['islands'],
                'island_ptrs': v['island_ptrs']}
            for k, v in codec.codecmap.load_maps().items() if v['board'] == 'dm32'}
    return {'profiles': profs, 'maps': maps, 'board': {k: (list(v) if isinstance(v, tuple) else v)
                                                       for k, v in codec.BOARDS['dm32'].items()}}


if __name__ == '__main__':
    main()
