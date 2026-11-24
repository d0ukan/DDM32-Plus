#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes live-results.json: what the Python health and live readers (pc/radiotools/plus_txdiag.py --health,
pc/radiotools/plus_live.py) send and print over the pinned RadioFoundry debug-link protocol (tools/rfoundry_debug.py,
checked against its SHA-256 pin by pc/radiotools/upstream.py), and what the program's parsers (pc/ddm32_app/parsers.py)
make of it. The C# ports (src/DDM32Plus.Core/Radio/DebugLink.cs, LiveData.cs) are held to it byte for byte
(LiveTests.cs).

    set DDM32_UPSTREAM=E:\\GIT-REPO\\open25-dm32\\research\\upstream\\radiofoundry
    python rust/tests/fixtures/make_live_fixtures.py

Nothing here opens a serial port or touches a radio: the pinned DebugLink class talks to an in-memory fake radio that
answers each request frame, and every written frame and every answer is recorded. Time is virtual (each clock read
advances 1 ms), so a radio that does not answer costs no real time. All answers are made up.
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]  # firmware/ (this copy lives in rust/tests/fixtures)
PC = REPO / 'pc'
sys.path.insert(0, str(PC))

from radiotools import native_modem as native  # noqa: E402
from radiotools import plus_live  # noqa: E402
from radiotools import plus_txdiag as diag  # noqa: E402
from radiotools import upstream  # noqa: E402
from ddm32_app import parsers  # noqa: E402

OUT = HERE / 'live-results.json'


class VirtualTime:
    def __init__(self):
        self.t = 1000.0

    def monotonic(self):
        self.t += 0.001
        return self.t

    def sleep(self, s):
        self.t += max(0.0, s)


def load_protocol(clock):
    p = upstream.path(upstream.DEBUG)
    spec = importlib.util.spec_from_file_location('live_fixture_protocol', p)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    m.time = clock          # the module's deadlines run on the virtual clock
    return m


# --- the fake radio's answers (made up) ---------------------------------------------------------------------------
CORE = ('section=core\nview=5\nstate=rx\ntx_deny=none\nmode=p25\nvfo=0\nzone_index=0\nzone_pos=2\nzone_name=Zone A\n'
        'channel_number=4\nchannel_name=P25 659\nrx_hz=434250000\ntx_hz=434250000\nsquelch=3\nmonitor=0\n'
        'tone_ok=1\nrssi_dbm=-87\nnoise=12\n')
CORE_IDLE = ('section=core\nview=5\nstate=idle\ntx_deny=none\nmode=fm\nvfo=1\nzone_index=0\nzone_pos=0\n'
             'zone_name=\nchannel_number=0\nchannel_name=\nrx_hz=145500000\ntx_hz=145500000\nsquelch=3\nmonitor=1\n'
             'tone_ok=1\nrssi_dbm=0\nnoise=3\n')
TASKS = ('tasks.count=4\ntasks.untracked=0\ntasks.min_free=150\ntasks.min_task=p25_lab\ntasks.free.IDLE=452\n'
         'tasks.free.host=1200\ntasks.free.ui=884\ntasks.free.p25_lab=150\ntasks.heap_free=6104\n'
         'tasks.heap_min_free=4048\n')
TASKS_OK = TASKS.replace('min_free=150', 'min_free=452').replace('min_task=p25_lab', 'min_task=IDLE') \
    .replace('free.p25_lab=150', 'free.p25_lab=640')
CRASH = 'crash.built=1\ncrash.valid=0\n'
CRASH_VALID = 'crash.built=1\ncrash.valid=1\ncrash.task=ui\ncrash.pc=0x0001a2b4\ncrash.uptime_ms=3605000\n'
RESET = 'reset.built=1\nreset.cause=power\nreset.flags=0x00000001\nreset.uptime_ms=0\nreset.warm=0\n'
RESET_WDT = 'reset.built=1\nreset.cause=watchdog\nreset.flags=0x00000004\nreset.uptime_ms=0\nreset.warm=1\n'
SCAN = (1, 7, 5, 1, 4, 4, 3, 2, 1, 0, 1, 180, 420)


def view(text, ms=82000):
    return struct.pack('<IB', ms, 5) + text.encode('latin-1')


def native_status(state=2, sync=1, reason=0):
    r = dict.fromkeys(native.FIELDS, 0)
    r.update(version=1, state=state, reason=reason, frequency_hz=434_250_000, rx_count=40, rx_seen=41, p25_sync=sync)
    return struct.pack('<%dI' % len(native.FIELDS), *[r[k] for k in native.FIELDS])


def ok(data):
    return ('ok', bytes([0]) + data)


class FakeSerial:
    """Answers each request frame from a table {(cmd, payload): answer}; records frames and answers"""

    def __init__(self, m, answers, noise=False, late=False):
        self.m = m
        self.answers = answers
        self.pending = bytearray()
        self.steps = []
        self.noise = noise
        self.late = late

    @property
    def in_waiting(self):
        return len(self.pending)

    def read(self, n=1):
        out = bytes(self.pending[:n])
        del self.pending[:n]
        return out

    def write(self, data):
        data = bytes(data)
        cmd, seq, n = data[2], data[3], data[4] | data[5] << 8
        payload = data[6:6 + n]
        a = self.answers.get((cmd, payload), ('silent', b''))
        if callable(a):
            a = a()
        kind, body = a
        reply = b''
        if self.noise:          # bytes of other protocols, a damaged frame of ours, then the answer
            reply += b'\x00\x11\xa5\x22' + self.m.frame(cmd | 0x80, seq, body)[:-1] + b'\x00'
        if self.late and kind != 'silent':      # a late answer to an earlier request first
            reply += self.m.frame(cmd | 0x80, (seq - 1) & 0xFF, b'\x00late')
        if kind == 'ok' or kind == 'status':
            reply += self.m.frame(cmd | 0x80, seq, body)
        elif kind == 'error':
            reply += self.m.frame(self.m.CMD_ERROR, seq, body)
        self.pending += reply
        self.steps.append({'write': data.hex(), 'reply': reply.hex()})
        return len(data)

    def close(self):
        pass


def make_link(m, ser, port='COM9'):
    class FixtureLink(m.DebugLink):
        def __init__(self):
            self.port = port
            self.timeout = 1
            self.seq = 0
            self.packed_shots = None
            self.buf = bytearray()
            self.s = ser
    return FixtureLink()


def answers(core=CORE, tasks=TASKS, crash=CRASH, reset=RESET, native_answer=None, scan=SCAN):
    a = {(0x09, b'\x00'): ok(view(core)), (0x09, b'\x14'): ok(view(tasks)), (0x09, b'\x15'): ok(view(crash, 82001)),
         (0x09, b'\x17'): ok(view(reset, 82002)),
         (0x21, b'\x00'): native_answer or ok(native_status())}
    if scan is not None:
        a[(0x21, b'\x0c')] = ok(struct.pack('<13I', *scan))
    else:
        a[(0x21, b'\x0c')] = ('status', bytes([4]))         # a build without the counters: unavailable
    return a


def run_live(name, table, count, **kw):
    clock = VirtualTime()
    m = load_protocol(clock)
    ser = FakeSerial(m, table, **kw)
    link = make_link(m, ser)
    out = io.StringIO()
    code = plus_live.run(plus_live.GuardedLink(link), 2, count, out=out, sleep=clock.sleep, clock=clock.monotonic)
    text = out.getvalue()
    lines = text.splitlines()
    polls = []
    once = []
    block = None
    for line in lines:
        s = line.strip()
        mm = parsers.RE_POLL.match(s)
        if mm and mm.group(1) == 'poll':
            block = []
            continue
        if mm and mm.group(1) == 'end':
            polls.append(parsers.live_summary(parsers.parse_blocks(block), parsers.parse_blocks(once)))
            block = None
            continue
        (once if block is None else block).append(s)
    return {'name': name, 'tool': 'live', 'count': count, 'interval': 2, 'steps': ser.steps, 'output': text,
            'exit': code, 'summaries': polls}


def run_health(name, table):
    clock = VirtualTime()
    m = load_protocol(clock)
    ser = FakeSerial(m, table)
    link = make_link(m, ser)
    out = io.StringIO()
    error = None
    real_open = diag.open_link
    diag.open_link = lambda port, allowed=diag.ALLOWED: link
    argv = sys.argv
    sys.argv = ['plus_txdiag.py', '--port', 'COM9', '--health']
    try:
        with contextlib.redirect_stdout(out):
            diag.main()
    except Exception as exc:        # the tool's own error ends the run (a traceback in the app's log)
        error = f'{type(exc).__name__}: {exc}'
    finally:
        diag.open_link = real_open
        sys.argv = argv
    text = out.getvalue()
    h = parsers.parse_health(text)
    s = parsers.health_summary(h)
    verdict = None
    if s['ok']:
        verdict = health_verdict(s)
    return {'name': name, 'tool': 'health', 'steps': ser.steps, 'output': text, 'error': error, 'summary': s,
            'verdict': verdict}


def health_verdict(s):
    # pc/ddm32_app/health_tab.verdict (that module imports PyQt5; the rule is copied here word for word)
    if s['crash_valid']:
        return 'red'
    low_task = any(v is not None and v < 200 for v in s['tasks'].values())
    if (s['heap_min_free'] is not None and s['heap_min_free'] < 1024) or low_task:
        return 'amber'
    return 'green'


def main():
    clock = VirtualTime()
    m = load_protocol(clock)
    frames = []
    for cmd, seq, payload in ((0x09, 1, b'\x00'), (0x09, 2, b'\x14'), (0x21, 255, b'\x0c'), (0x89, 7, b'\x00abc'),
                              (0xFF, 3, b'\x04'), (0x01, 0, b'')):
        frames.append({'cmd': cmd, 'seq': seq, 'payload': payload.hex(), 'frame': m.frame(cmd, seq, payload).hex()})
    crc = [{'data': d.hex(), 'crc': m.crc16(d)} for d in (b'', b'123456789', bytes(range(64)))]
    flaky = iter([ok(native_status()), ('silent', b''), ('error', bytes([3])), ok(native_status(state=6, reason=7))]
                 + [ok(native_status())] * 10)
    cases = [
        run_live('live_ok', answers(), 2),
        run_live('live_scan_unavailable', answers(scan=None), 1),
        run_live('live_idle_vfo_crash', answers(core=CORE_IDLE, crash=CRASH_VALID, reset=RESET_WDT), 1),
        run_live('live_silent', {}, 0),
        run_live('live_noise_and_late', answers(), 2, noise=True, late=True),
        run_live('live_native_flaky', {**answers(), (0x21, b'\x00'): lambda: next(flaky)}, 4),
        run_health('health_ok', answers(tasks=TASKS_OK)),
        run_health('health_low_stack', answers()),
        run_health('health_crash', answers(crash=CRASH_VALID, reset=RESET_WDT)),
        run_health('health_view_error', {**answers(), (0x09, b'\x15'): ('error', bytes([1]))}),
        run_health('health_silent', {}),
    ]
    data = {
        'source': 'pc/radiotools/plus_live.py, plus_txdiag.py --health, pinned tools/rfoundry_debug.py, '
                  'pc/ddm32_app/parsers.py',
        'native_fields': list(native.FIELDS), 'native_states': list(native.STATES),
        'native_reasons': list(native.REASONS), 'scan_fields': list(diag.SCAN_FIELDS),
        'live_allowed': sorted([c, p.hex()] for c, p in plus_live.ALLOWED),
        'frames': frames, 'crc16': crc, 'cases': cases,
        's_meter': [[d, parsers.s_meter(d)] for d in (None, 0, -40, -73, -92, -93, -94, -96, -99, -105, -120, -141, -160)],
        'format_mhz': [[hz, parsers.format_mhz(hz)] for hz in (None, 0, 434250000, 145500000, 446106250)],
        'format_uptime': [[ms, parsers.format_uptime(ms)] for ms in (None, 0, 999, 82000, 3605000, 90061000)],
    }
    OUT.write_text(json.dumps(data, indent=1, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    print(f'{OUT}: {len(cases)} cases')


if __name__ == '__main__':
    main()
