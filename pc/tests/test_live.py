"""Canlı panel: radiotools/plus_live.py against a fake debug link, its parser, and the tab with a fake process runner.
No port, tool process or radio is used."""
import inspect
import io
import os
from pathlib import Path
import struct
import sys
import unittest

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

import fixtures as fx  # noqa: E402

PC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PC))
from radiotools import native_modem, plus_live, plus_txdiag  # noqa: E402

from ddm32_app import parsers  # noqa: E402

CORE = ('section=core\nview=5\nstate=rx\ntx_deny=none\nmode=fm\nvfo=0\nzone_index=0\nzone_pos=2\nzone_name=Zone A\n'
        'channel_number=4\nchannel_name=P25 659\nrx_hz=434250000\ntx_hz=434250000\nsquelch=3\nmonitor=0\n'
        'tone_ok=1\nrssi_dbm=-87\nnoise=12\n')
TASKS = fx.HEALTH.split('--- view tasks (20) status 0\nuptime_ms=82000\n')[1].split('--- view crash')[0]
CRASH = 'crash.built=1\ncrash.valid=0\n'
RESET = 'reset.built=1\nreset.cause=power\n'


class LinkError(Exception):
    def __init__(self, msg, status=None):
        super().__init__(msg)
        self.status = status


def view(text, ms=82000):
    return struct.pack('<IB', ms, 0xFF) + text.encode()


def native_status(state=2, sync=1):
    r = dict.fromkeys(native_modem.FIELDS, 0)
    r.update(version=1, state=state, reason=0, frequency_hz=434_250_000, rx_count=40, p25_sync=sync)
    return struct.pack('<%dI' % len(native_modem.FIELDS), *[r[k] for k in native_modem.FIELDS])


class FakeLink:
    """Answers the six read-only queries; scan=None answers 'unavailable' like a build without the counters"""

    def __init__(self, scan=(1, 7, 5, 1, 4, 4, 3, 2, 1, 0, 1, 180, 420), silent=False):
        self.sent = []
        self.scan = scan
        self.silent = silent

    def request(self, cmd, payload=b'', **kw):
        self.sent.append((cmd, bytes(payload)))
        if self.silent:
            raise LinkError('no answer to command within 1 s')
        answers = {(0x09, b'\x00'): view(CORE), (0x09, b'\x14'): view(TASKS), (0x09, b'\x15'): view(CRASH),
                   (0x09, b'\x17'): view(RESET), (0x21, b'\x00'): native_status()}
        if (cmd, bytes(payload)) == (0x21, b'\x0c'):
            if self.scan is None:
                raise LinkError('command 0x21: unavailable', 4)
            return 0, struct.pack('<13I', *self.scan)
        return 0, answers[(cmd, bytes(payload))]


class PlusLiveToolTest(unittest.TestCase):
    def run_tool(self, link, count=2):
        out = io.StringIO()
        code = plus_live.run(link, 1, count, out=out, sleep=lambda s: None)
        return code, out.getvalue()

    def test_only_allowlisted_reads(self):
        link = FakeLink()
        code, text = self.run_tool(link)
        self.assertEqual(code, 0)
        self.assertTrue(set(link.sent) <= plus_live.ALLOWED)
        self.assertEqual(link.sent[:2], [(0x09, b'\x15'), (0x09, b'\x17')])      # crash, reset once
        self.assertEqual(len(link.sent), 2 + 2 * 4)
        self.assertIn('=== poll 2', text)
        self.assertIn('--- view core (0) status 0', text)
        self.assertIn('--- scan (0x21/12) status 0', text)

    def test_allowlist_is_read_only(self):
        # every query is a read plus_txdiag already makes, plus GET_VIEW core (read only)
        self.assertEqual(plus_live.ALLOWED - plus_txdiag.ALLOWED, {(0x09, b'\x00')})
        self.assertIn('refused a command before transmission', inspect.getsource(plus_txdiag.open_link))
        for cmd, payload in plus_live.ALLOWED:
            self.assertIn(cmd, (0x09, 0x21))
            self.assertEqual(len(payload), 1)
        g = plus_live.GuardedLink(FakeLink())
        for cmd, payload in ((0x06, b''), (0x07, b'\x01'), (0x0a, b'\x01'), (0x21, b'\x01'), (0x21, b'\x0a\x01'),
                             (0x09, b'\x09'), (0x0f, b'\x02'), (0x0d, b'')):
            with self.assertRaises(plus_live.Refused):
                g.request(cmd, payload)
        self.assertEqual(g.link.sent, [])

    def test_scan_stats_only_when_asked(self):
        scanst = ('scanst.built=1\nscanst.on=1\nscanst.st=fast,30000,560,40,750,14,0,0,560,0,3\n'
                  'scanst.slow=2140,6,9,FM,7\nscanst.fb=9,FM,40,60,-105\nscanst.cost=13,2,0,0\n')

        class ScanLink(FakeLink):
            def request(self, cmd, payload=b'', **kw):
                if (cmd, bytes(payload)) == (0x09, b'\x1f'):
                    self.sent.append((cmd, bytes(payload)))
                    return 0, view(scanst)
                return super().request(cmd, payload, **kw)

        link = ScanLink()
        code, text = self.run_tool(link, count=1)      # without --scan-stats: never asked
        self.assertEqual(code, 0)
        self.assertNotIn((0x09, b'\x1f'), link.sent)
        with self.assertRaises(plus_live.Refused):
            plus_live.GuardedLink(ScanLink()).request(0x09, b'\x1f')
        link = ScanLink()
        out = io.StringIO()
        code = plus_live.run(link, 1, 2, out=out, sleep=lambda s: None, scan_stats=True)
        self.assertEqual(code, 0)
        self.assertEqual(link.sent.count((0x09, b'\x1f')), 2)
        self.assertTrue(set(link.sent) <= plus_live.ALLOWED_SCANST)
        self.assertEqual(plus_live.ALLOWED_SCANST - plus_live.ALLOWED, {(0x09, b'\x1f')})
        self.assertIn('--- view scanst (31) status 0', out.getvalue())
        self.assertIn('scanst.slow=2140,6,9,FM,7', out.getvalue())

    def test_unavailable_counters_and_parse(self):
        code, text = self.run_tool(FakeLink(scan=None), count=1)
        self.assertEqual(code, 0)
        self.assertIn('--- scan (0x21/12) status 4', text)
        lines = text.splitlines()
        first = lines.index('=== poll 1')
        once = parsers.parse_blocks(lines[:first])
        blocks = parsers.parse_blocks(lines[first + 1:lines.index('=== end 1')])
        s = parsers.live_summary(blocks, once)
        self.assertIsNone(s['scan'])
        self.assertEqual(s['scan_status'], 4)
        self.assertEqual(s['signal']['rssi_dbm'], -87)
        self.assertEqual(s['signal']['channel'], 'P25 659')
        self.assertEqual(s['signal']['rx_hz'], 434_250_000)
        self.assertEqual(s['native']['state'], 'receive')
        self.assertEqual(s['native']['p25_sync'], 1)
        self.assertEqual(s['health']['reset_cause'], 'power')
        self.assertEqual(s['health']['heap_min_free'], 4048)
        self.assertFalse(s['health']['crash_valid'])

    def test_silent_radio_stops(self):
        code, text = self.run_tool(FakeLink(silent=True), count=0)
        self.assertEqual(code, 3)
        self.assertIn('No answer from the radio for 3 polls', text)
        self.assertIn('--- view core (0) status -1', text)


class LiveParserTest(unittest.TestCase):
    def test_s_meter(self):
        self.assertEqual(parsers.s_meter(-93), 'S9')
        self.assertEqual(parsers.s_meter(-73), 'S9+20')
        self.assertEqual(parsers.s_meter(-99), 'S8')
        self.assertEqual(parsers.s_meter(-141), 'S1')
        self.assertEqual(parsers.s_meter(None), '')
        self.assertEqual(parsers.format_mhz(434_250_000), '434,25000 MHz')

    def test_no_signal_reading(self):
        s = parsers.live_summary(parsers.parse_blocks(['--- view core (0) status 0', 'rssi_dbm=0', 'state=idle']))
        self.assertIsNone(s['signal']['rssi_dbm'])
        self.assertEqual(s['signal']['state'], 'idle')
        self.assertIsNone(s['health'])


try:
    from PyQt5.QtWidgets import QApplication
    from test_ui import UiBase
    APP = QApplication.instance() or QApplication([])
except ImportError:  # pragma: no cover
    UiBase = unittest.TestCase


class LiveTabTest(UiBase):
    def poll_text(self, link=None):
        out = io.StringIO()
        plus_live.run(link or FakeLink(), 1, 1, out=out, sleep=lambda s: None)
        return out.getvalue()

    def test_connect_poll_stop(self):
        lv = self.w.live
        self.assertEqual(self.w.tabs.tabText(self.w.tabs.indexOf(lv)), 'Canlı panel')
        self.assertTrue(lv.connect_btn.isEnabled())
        self.assertFalse(lv.stop_btn.isEnabled())
        self.assertEqual(fx.FakeRunner.instances, [])          # nothing opens before the click
        lv.interval.setCurrentIndex(lv.interval.findData(3))
        lv.connect_btn.click()
        r = self.runner()
        program, args, cwd = r.calls[0]
        self.assertEqual(args, ['-u', 'radiotools/plus_live.py', '--port', 'COM7', '--interval', '3'])
        self.assertEqual(cwd, self.root)
        self.assertTrue(self.w.port_state.locked)
        self.assertFalse(lv.interval.isEnabled())
        self.w._guard()
        self.assertFalse(self.w.health.read_btn.isEnabled())   # one radio step at a time
        self.assertFalse(self.u.backup_btn.isEnabled())
        r.line.emit('LIVE: COM7 open, read only, every 3 s')
        for line in self.poll_text().splitlines():
            r.line.emit(line)
        self.assertEqual(lv.polls, 1)
        self.assertIn('-87 dBm', lv.rssi_bar.format())
        self.assertIn('S9+6', lv.rssi_bar.format())
        self.assertEqual(lv.v_state.text(), 'Alıyor')
        self.assertIn('P25 659', lv.v_channel.text())
        self.assertIn('Zone A', lv.v_channel.text())
        self.assertEqual(lv.v_freq.text(), '434,25000 MHz')
        self.assertEqual(lv.v_native.text(), 'alıyor')
        self.assertEqual(lv.scan_labels['stops'].text(), '7')
        self.assertEqual(lv.scan_labels['voice_ms'].text(), '420')
        self.assertEqual(lv.v_reset.text(), 'normal açılış (güç)')
        self.assertEqual(lv.v_crash.text(), 'yok')
        self.assertEqual(lv.history.values, [-87])
        lv.stop_btn.click()
        self.assertTrue(r.killed)
        r.finished.emit(-1)
        self.assertIn('Durduruldu', lv.status.text())
        self.assertFalse(self.w.port_state.locked)
        self.assertIsNone(self.w.lock.owner)
        self.assertTrue(lv.connect_btn.isEnabled())

    def test_lost_connection(self):
        lv = self.w.live
        lv.connect_btn.click()
        r = self.runner()
        r.feed(self.poll_text(FakeLink(silent=True)) + 'No answer from the radio for 3 polls; stopping.', 3)
        self.assertIn('Bağlantı kesildi', lv.status.text())
        self.assertIsNone(self.w.lock.owner)

    def test_busy_flash_blocks_connect(self):
        self.u.prep_btn.click()
        self.u.backup_btn.click()
        self.assertEqual(self.w.lock.owner, 'flash')
        self.w._guard()
        self.assertFalse(self.w.live.connect_btn.isEnabled())
        self.w.live.start()
        self.assertEqual(len(fx.FakeRunner.instances), 1)        # only the backup


if __name__ == '__main__':
    unittest.main()
