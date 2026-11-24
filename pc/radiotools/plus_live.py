#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live read-only view of DDM32 Plus over the debug link, for the PC app's "Canlı panel".

Polls every --interval seconds (1..5) until stopped (the app ends the process) or --count polls. Sends only these
read-only queries, refused locally before transmission otherwise:
  0x09 [0]         GET_VIEW core: state, mode, zone, channel, rx_hz, rssi_dbm, noise, squelch
                   (GET_VIEW is read only; the lab's health check reads the same core view)
  0x09 [20|21|23]  GET_VIEW tasks, crash, reset (as plus_txdiag.py --health)
  0x21 [0]         native modem status (as plus_txdiag.py)
  0x21 [12]        P25 zone scan counters (as plus_txdiag.py --scan)
  0x09 [31]        GET_VIEW scanst, only with --scan-stats: the zone / VFO scan's counters (firmware 1.0.1 and later;
                   the slow stop, fall-back reading and radio task timing lines from 1.0.2)
No TX, key, radio operation, flash, storage or register command is ever sent.

Output: one block per poll, '=== poll N' ... '=== end N', each query as '--- NAME (ID) status CODE' followed by
key=value lines (crash and reset once, before the first poll).
"""
import argparse
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from radiotools import native_modem as native  # noqa: E402
from radiotools import plus_txdiag as diag  # noqa: E402

ALLOWED = {(0x09, b'\x00'), (0x09, b'\x14'), (0x09, b'\x15'), (0x09, b'\x17'), (0x21, b'\x00'), (0x21, b'\x0c')}
ONCE = (('view crash', 0x15), ('view reset', 0x17))
POLL_VIEWS = (('view core', 0x00), ('view tasks', 0x14))
SCANST = ('view scanst', 0x1F)      # --scan-stats: GET_VIEW 31, read only
ALLOWED_SCANST = ALLOWED | {(0x09, bytes([SCANST[1]]))}
MAX_SILENT_POLLS = 3


class Refused(Exception):
    pass


class GuardedLink:
    """Any link with request(cmd, payload, **kw); refuses everything outside allowed (ALLOWED) before it is sent"""

    def __init__(self, link, allowed=ALLOWED):
        self.link = link
        self.allowed = allowed

    def request(self, cmd, payload=b'', **kw):
        if (cmd, bytes(payload)) not in self.allowed:
            raise Refused(f'plus_live refused command 0x{cmd:02x} {bytes(payload).hex()} before transmission')
        return self.link.request(cmd, payload, **kw)


def _status(exc):
    """The debug link's status from a DebugLinkError (its .status), -1 for no answer"""
    status = getattr(exc, 'status', None)
    return status if isinstance(status, int) else -1


def query(link, name, cmd, payload, out):
    """One query, printed. Returns (status, data or None); never raises for a radio error"""
    label = f'{cmd:#04x}/{payload[0]}' if cmd != 0x09 else str(payload[0])
    try:
        code, data = link.request(cmd, payload, busy_wait=0)
    except Refused:
        raise
    except Exception as exc:        # the radio's error status or no answer: reported, the poll goes on
        code = _status(exc)
        print(f'--- {name} ({label}) status {code}', file=out)
        print(f'error={type(exc).__name__}: {exc}', file=out)
        return code, None
    print(f'--- {name} ({label}) status {code}', file=out)
    return code, bytes(data)


def print_view(link, name, section, out):
    code, data = query(link, name, 0x09, bytes([section]), out)
    if not code and data is not None and len(data) >= 5:
        print(f'uptime_ms={struct.unpack("<I", data[:4])[0]}', file=out)
        text = data[5:].decode('ascii', 'replace').rstrip()
        if text:
            print(text, file=out)
    return code


def print_native(link, out):
    code, data = query(link, 'native', 0x21, b'\x00', out)
    if code or data is None:
        return code
    if len(data) == 4 * len(native.FIELDS):
        r = dict(zip(native.FIELDS, struct.unpack('<' + 'I' * len(native.FIELDS), data)))
        for k, v in r.items():
            print(f'{k}={v}', file=out)
        if r['state'] < len(native.STATES):
            print('state_name=' + native.STATES[r['state']], file=out)
        if r['reason'] < len(native.REASONS):
            print('reason_name=' + native.REASONS[r['reason']], file=out)
    else:
        print('raw=' + data.hex(), file=out)
    return code


def print_scan(link, out):
    code, data = query(link, 'scan', 0x21, b'\x0c', out)
    if code or data is None:
        return code
    if len(data) == 4 * len(diag.SCAN_FIELDS):
        for k, v in zip(diag.SCAN_FIELDS, struct.unpack('<%dI' % len(diag.SCAN_FIELDS), data)):
            print(f'{k}={v}', file=out)
    else:
        print('raw=' + data.hex(), file=out)
    return code


def run(link, interval, count=0, out=sys.stdout, sleep=time.sleep, clock=time.monotonic, scan_stats=False):
    """The polling loop on an open (guarded) link. Returns the exit code: 0 after count polls, 3 when the radio
    stopped answering. scan_stats: GET_VIEW scanst too, each poll"""
    link = link if isinstance(link, GuardedLink) else GuardedLink(link, ALLOWED_SCANST if scan_stats else ALLOWED)
    for name, section in ONCE:
        print_view(link, name, section, out)
    out.flush()
    n = silent = 0
    while not count or n < count:
        n += 1
        started = clock()
        print(f'=== poll {n}', file=out)
        codes = [print_view(link, name, section, out) for name, section in POLL_VIEWS]
        codes += [print_native(link, out), print_scan(link, out)]
        if scan_stats:
            codes.append(print_view(link, SCANST[0], SCANST[1], out))
        print(f'=== end {n}', file=out)
        out.flush()
        silent = silent + 1 if all(c == -1 for c in codes) else 0
        if silent >= MAX_SILENT_POLLS:
            print(f'No answer from the radio for {silent} polls; stopping.', file=out)
            out.flush()
            return 3
        if not count or n < count:
            sleep(max(0.0, interval - (clock() - started)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', required=True)
    ap.add_argument('--interval', type=float, default=2.0, help='seconds between polls, 1..5')
    ap.add_argument('--count', type=int, default=0, help='stop after this many polls (0: until stopped)')
    ap.add_argument('--scan-stats', action='store_true',
                    help='also read the zone / VFO scan counters (GET_VIEW scanst, read only) each poll')
    args = ap.parse_args()
    if not 1 <= args.interval <= 5:
        ap.error('--interval must be 1..5 seconds')
    if args.count < 0:
        ap.error('--count must be 0 or more')
    allowed = ALLOWED_SCANST if args.scan_stats else ALLOWED
    link = diag.open_link(args.port, allowed=allowed)
    print(f'LIVE: {args.port} open, read only, every {args.interval:g} s', flush=True)
    try:
        code = run(GuardedLink(link, allowed), args.interval, args.count, scan_stats=args.scan_stats)
    finally:
        link.s.close()
    sys.exit(code)


if __name__ == '__main__':
    main()
