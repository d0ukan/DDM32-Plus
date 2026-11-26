"""pc/release/make_release.py with a test key in a temporary folder: key generation (never run on the real key path
here), the signed release, and the whole chain from release files to the installer's gate. Nothing is uploaded."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import fixtures as fx
import rtfixtures as rt

sys.path.insert(0, str(rt.PC / 'release'))
import make_release as mr  # noqa: E402

from ddm32_app import releases  # noqa: E402
from radiotools import ed25519, install_plus  # noqa: E402


class ReleaseToolTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = Path(self.tmp.name)
        self.key = self.top / 'keys' / 'release-signing-key'
        self.restricted = []
        self.public, ok = mr.keygen(self.key, restrict_fn=self.restricted.append)
        self.assertTrue(ok)
        self.build = rt.write_plus(self.top / 'build', fx.release_app('1.0.0') + b'DDM32 Plus', signed_for=False)

    def tearDown(self):
        self.tmp.cleanup()

    def run_build(self, *extra, out=None):
        out = out or self.top / 'out'
        args = ['build', '--version', '1.0.0-beta.1', '--channel', 'beta', '--build-dir', str(self.build),
                '--settings-format', '1', '--codeplug-format', '2', '--min-compatible', '1.0.0-beta.1',
                '--notes-tr', 'İlk beta', '--notes-en', 'First beta', '--out', str(out), '--key', str(self.key),
                *extra]
        with patch('sys.stdout', io.StringIO()) as o:
            code = mr.main(args)
        return code, out, o.getvalue()

    def test_double_encoded_notes_are_refused(self):
        # v1.0.0-beta.4: "bandı" reached the manifest as "bandÄ±" through a Windows shell
        mangled = 'hava bandı'.encode('utf-8').decode('cp1252', errors='replace')
        for extra in (['--notes-tr', mangled], ['--app-net-notes-tr', 'Ã§ağrı']):
            with self.assertRaisesRegex(SystemExit, 'double-encoded'):
                self.run_build(*extra)
        self.assertFalse((self.top / 'out').exists())
        code, out, _ = self.run_build('--notes-tr', 'Sinyal çubukları, şelale, İlk beta')
        self.assertEqual(code, 0)
        notes = json.loads((out / 'ddm32-manifest.json').read_text(encoding='utf-8'))['firmware'][0]['notes']
        self.assertEqual(notes['tr'], 'Sinyal çubukları, şelale, İlk beta')

    def test_keygen(self):
        text = self.key.read_text().strip()
        self.assertEqual(len(text), 64)
        self.assertEqual(ed25519.secret_to_public(bytes.fromhex(text)).hex(), self.public)
        self.assertEqual(self.restricted, [self.key])
        with self.assertRaises(FileExistsError):
            mr.keygen(self.key, restrict_fn=lambda p: None)          # never overwrites a key
        with self.assertRaisesRegex(SystemExit, 'outside the repository'):
            mr.keygen(rt.PC / 'release-signing-key', restrict_fn=lambda p: None)
        self.assertFalse((rt.PC / 'release-signing-key').exists())
        # The new place, or the key made before the rename when only that one exists (never in the repository)
        self.assertIn(mr.DEFAULT_KEY, (Path.home() / '.ddm32' / 'release-signing-key',
                                       Path.home() / '.ddm25' / 'release-signing-key'))
        if mr.DEFAULT_KEY == mr.LEGACY_KEY:
            self.assertFalse((Path.home() / '.ddm32' / 'release-signing-key').exists())

    def test_build_signs_and_verifies(self):
        code, out, text = self.run_build()
        self.assertEqual(code, 0)
        self.assertIn('Nothing was uploaded', text)
        names = sorted(p.name for p in out.iterdir())
        self.assertEqual(names, ['ddm32-manifest.json', 'ddm32-manifest.json.sig', 'ddm32-plus-1.0.0-beta.1.zip'])
        m = mr.verify(out, self.public)
        e = m['firmware'][0]
        self.assertEqual((e['channel'], e['settings_format'], e['codeplug_format']), ('beta', 1, 2))
        self.assertEqual(e['url'], 'https://github.com/d0ukan/DDM32-Plus/releases/download/v1.0.0-beta.1/'
                                   'ddm32-plus-1.0.0-beta.1.zip')
        self.assertEqual(e['notes'], {'tr': 'İlk beta', 'en': 'First beta'})
        with self.assertRaises(rt.sm.ManifestError):
            mr.verify(out, rt.TEST_PUBLIC_HEX)                       # another key does not verify it
        with zipfile.ZipFile(out / 'ddm32-plus-1.0.0-beta.1.zip') as z:
            self.assertEqual(sorted(z.namelist()), sorted(releases.PACKAGE_FILES))
            build = json.loads(z.read('rfoundry_dm32.json'))
            self.assertTrue(build['public_enabled'])
            self.assertEqual(set(build), {'kind', 'version', 'plus_enabled', 'public_enabled',
                                          'forced_rebuild_identical', 'upstream_commit', 'outputs',
                                          'source_build_manifest_sha256', 'source_map_sha256'})
            self.assertTrue(all(l.strip().startswith(('#', '0x')) for l in
                                z.read('rfoundry_dm32.map').decode().splitlines() if l.strip()))

    def test_release_files_pass_the_app_and_the_installer(self):
        """make_release output -> the app's download and hash checks -> install_plus's signed gate"""
        code, out, _ = self.run_build()
        with patch.object(rt.sm, 'PUBLIC_KEY_HEX', self.public):
            files = {}
            tag = 'v1.0.0-beta.1'
            for p in out.iterdir():
                files[rt.url(tag, p.name)] = p.read_bytes()
            files[releases.API_URL] = json.dumps([{'tag_name': tag, 'assets': [
                {'name': n, 'browser_download_url': rt.url(tag, n)}
                for n in ('ddm32-manifest.json', 'ddm32-manifest.json.sig')]}]).encode()
            root = self.top / 'surumler'
            cat = releases.load_catalog(root, lambda url, limit=None: files[url])
            fv = cat.find('1.0.0-beta.1')

            def download(url, dest, size, progress=None):
                Path(dest).write_bytes(files[url])
            folder = releases.download_firmware(fv, root, download)
            self.assertEqual(install_plus.plus_application(folder), (self.build / 'rfoundry_dm32.bin').read_bytes())

    def test_program_zip(self):
        appzip = self.top / 'DDM32Plus-1.0.0-beta.2.zip'
        with zipfile.ZipFile(appzip, 'w') as z:
            z.writestr('DDM32Plus/DDM32Plus.exe', b'MZ')
            z.writestr('DDM32Plus/radiotools/dm32_readonly_backup.py', b'# the backup tool is public source')
        code, out, _ = self.run_build('--app-zip', str(appzip), '--app-version', '1.0.0-beta.2')
        m = mr.verify(out, self.public)
        self.assertEqual(m['app']['version'], '1.0.0-beta.2')
        self.assertTrue((out / 'DDM32Plus-1.0.0-beta.2.zip').is_file())
        bad = self.top / 'bad.zip'
        with zipfile.ZipFile(bad, 'w') as z:
            z.writestr('DDM32Plus/DDM32Plus.exe', b'MZ')
            z.writestr('DDM32Plus/yedekler/progmem-pass1.bin', b'private')
        with self.assertRaisesRegex(SystemExit, 'must not be published'):
            self.run_build('--app-zip', str(bad), '--app-version', '1.0.0-beta.2', out=self.top / 'out2')
        for i, private in enumerate(('DDM32Plus/Backups/notes.txt', 'DDM32Plus/x/backup-manifest.json')):
            bad = self.top / f'bad{i}.zip'
            with zipfile.ZipFile(bad, 'w') as z:
                z.writestr('DDM32Plus/DDM32Plus.exe', b'MZ')
                z.writestr(private, b'private')
            with self.subTest(private=private), self.assertRaisesRegex(SystemExit, 'must not be published'):
                self.run_build('--app-zip', str(bad), '--app-version', '1.0.0-beta.2', out=self.top / f'o{i}')

    def net_zip(self, name, *members):
        path = self.top / name
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr('DDM32Plus/DDM32Plus.exe', b'MZ net')
            for m in members:
                z.writestr(m, b'x')
        return path

    def test_windows_program_zip(self):
        """app_net: the Windows (.NET) program, beside the Python program's app entry; each its own zip"""
        py = self.net_zip('py.zip', 'DDM32Plus/radiotools/install_plus.py')
        net = self.net_zip('net.zip')
        code, out, text = self.run_build('--app-zip', str(py), '--app-version', '1.0.0-beta.2',
                                         '--app-net-zip', str(net), '--app-net-version', '2.0.0-alpha.2',
                                         '--app-net-notes-tr', 'Windows programı', '--app-net-notes-en', 'Windows program')
        self.assertEqual(code, 0)
        m = mr.verify(out, self.public)
        self.assertEqual(m['app']['version'], '1.0.0-beta.2')
        self.assertEqual(m['app_net']['version'], '2.0.0-alpha.2')
        self.assertEqual(m['app_net']['file'], 'DDM32Plus-win-2.0.0-alpha.2.zip')
        self.assertEqual(m['app_net']['notes'], {'tr': 'Windows programı', 'en': 'Windows program'})
        self.assertEqual((out / 'DDM32Plus-win-2.0.0-alpha.2.zip').read_bytes(), net.read_bytes())
        # the Python program's catalog still takes only "app" as its own update
        cat = releases.Catalog()
        cat.add(m['release'], m, out)
        self.assertEqual([a[0]['version'] for a in cat.apps], ['1.0.0-beta.2'])
        # a changed Windows zip fails verify
        (out / 'DDM32Plus-win-2.0.0-alpha.2.zip').write_bytes(b'changed')
        with self.assertRaisesRegex(SystemExit, 'differs from the manifest'):
            mr.verify(out, self.public)

    def test_windows_program_zip_refusals(self):
        for i, member in enumerate(('DDM32Plus/yedekler/x.txt', 'DDM32Plus/settings.json', 'DDM32Plus/Guncelleme/a',
                                    'DDM32Plus/unins000.exe', 'Other/x.txt', 'DDM32Plus/x/backup-manifest.json')):
            bad = self.net_zip(f'bad-net{i}.zip', member)
            with self.subTest(member=member), self.assertRaises(SystemExit):
                self.run_build('--app-net-zip', str(bad), '--app-net-version', '2.0.0', out=self.top / f'n{i}')
            self.assertFalse((self.top / f'n{i}').exists())
        with self.assertRaisesRegex(SystemExit, '--app-net-version'):
            self.run_build('--app-net-zip', str(self.net_zip('ok.zip')), out=self.top / 'n-noversion')

    def test_app_net_schema_is_checked(self):
        base = rt.manifest([rt.firmware_entry(fx.release_app('1.0.0'))], app=rt.app_entry('1.0.0', b'z'))
        good = dict(base, app_net=dict(rt.app_entry('2.0.0', b'n'), file='DDM32Plus-win-2.0.0.zip',
                                       url=rt.url('v2.0.0', 'DDM32Plus-win-2.0.0.zip')))
        data, sig = rt.signed(good)
        self.assertEqual(rt.sm.verify_bytes(data, sig, rt.TEST_PUBLIC_HEX)['app_net']['version'], '2.0.0')
        for field, value in (('url', 'https://example.com/DDM32Plus-win-2.0.0.zip'), ('sha256', 'x'),
                             ('notes', 'text'), ('version', '2.0')):
            bad = json.loads(json.dumps(good))
            bad['app_net'][field] = value
            data, sig = rt.signed(bad)
            with self.subTest(field=field), self.assertRaises(rt.sm.ManifestError):
                rt.sm.verify_bytes(data, sig, rt.TEST_PUBLIC_HEX)

    def rs_zip(self, name, platform, *members):
        """A Rust program zip as rust/packaging/make_app_zip.py makes it"""
        path = self.top / name
        ext = '.exe' if platform.startswith('windows-') else ''
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr(f'DDM32Plus/DDM32Plus{ext}', f'rust gui {platform}'.encode())
            z.writestr(f'DDM32Plus/ddm32{ext}', f'rust cli {platform}'.encode())
            for m in members:
                info = zipfile.ZipInfo(m)
                info.filename = m          # as given: zipfile would turn a backslash into '/' on Windows
                z.writestr(info, b'x')
        return path

    def test_rust_program_zips(self):
        """app_rs: one zip per platform, keyed as the Rust program's platform_key(); beside app and app_net"""
        net = self.net_zip('net.zip')
        win = self.rs_zip('w.zip', 'windows-x86_64')
        lin = self.rs_zip('l.zip', 'linux-x86_64')
        mac = self.rs_zip('m.zip', 'macos-aarch64')
        code, out, text = self.run_build('--app-net-zip', str(net), '--app-net-version', '2.0.0-beta.1',
                                         '--app-rs-zip', f'windows-x86_64={win}', '--app-rs-zip', f'linux-x86_64={lin}',
                                         '--app-rs-zip', f'macos-aarch64={mac}', '--app-rs-version', '3.0.0-alpha.2',
                                         '--app-rs-notes-tr', 'Rust programı', '--app-rs-notes-en', 'Rust program')
        self.assertEqual(code, 0)
        m = mr.verify(out, self.public)
        self.assertEqual(list(m['app_rs']), ['windows-x86_64', 'linux-x86_64', 'macos-aarch64'])
        tag = 'v1.0.0-beta.1'
        for platform, src in (('windows-x86_64', win), ('linux-x86_64', lin), ('macos-aarch64', mac)):
            e = m['app_rs'][platform]
            name = f'DDM32Plus-3.0.0-alpha.2-{platform}.zip'
            self.assertEqual(set(e), {'version', 'file', 'url', 'sha256', 'size', 'notes'})
            self.assertEqual((e['version'], e['file']), ('3.0.0-alpha.2', name))
            self.assertEqual(e['url'], f'https://github.com/d0ukan/DDM32-Plus/releases/download/{tag}/{name}')
            self.assertEqual(e['notes'], {'tr': 'Rust programı', 'en': 'Rust program'})
            self.assertEqual((out / name).read_bytes(), src.read_bytes())
            self.assertEqual((e['size'], e['sha256']), (src.stat().st_size, mr.sha256(src.read_bytes())))
        self.assertEqual(m['app_net']['version'], '2.0.0-beta.1')      # the others are untouched
        self.assertNotIn('app', m)
        # the Python program ignores app_rs (never an update for it)
        cat = releases.Catalog()
        cat.add(m['release'], m, out)
        self.assertEqual(cat.apps, [])
        # a changed zip fails verify
        (out / 'DDM32Plus-3.0.0-alpha.2-linux-x86_64.zip').write_bytes(b'changed')
        with self.assertRaisesRegex(SystemExit, 'differs from the manifest'):
            mr.verify(out, self.public)

    def test_packaging_zip_is_accepted(self):
        """rust/packaging/make_app_zip.py's zips (what the packaging scripts make) pass --app-rs-zip, deterministically"""
        sys.path.insert(0, str(rt.PC.parent / 'rust' / 'packaging'))
        try:
            import make_app_zip
        finally:
            sys.path.pop(0)
        extra = []
        for platform, ext in (('windows-x86_64', '.exe'), ('linux-x86_64', ''), ('macos-aarch64', '')):
            bins = self.top / f'bin-{platform}'
            bins.mkdir()
            (bins / f'ddm32-gui{ext}').write_bytes(b'gui ' + platform.encode())
            (bins / f'ddm32{ext}').write_bytes(b'cli ' + platform.encode())
            a, b = self.top / f'{platform}-a.zip', self.top / f'{platform}-b.zip'
            for z in (a, b):
                with patch('sys.stdout', io.StringIO()):
                    self.assertEqual(make_app_zip.main([platform, str(bins), str(z)]), 0)
            self.assertEqual(a.read_bytes(), b.read_bytes())               # the same binaries, the same SHA-256
            with zipfile.ZipFile(a) as z:
                self.assertEqual(z.namelist(), [f'DDM32Plus/DDM32Plus{ext}', f'DDM32Plus/ddm32{ext}'])
                self.assertEqual(z.read(f'DDM32Plus/DDM32Plus{ext}'), b'gui ' + platform.encode())
            mr.check_app_rs_zip(a, platform)
            extra += ['--app-rs-zip', f'{platform}={a}']
        with self.assertRaises(SystemExit):
            make_app_zip.main(['solaris-sparc', str(self.top), str(self.top / 'x.zip')])
        code, out, _ = self.run_build(*extra, '--app-rs-version', '3.0.0')
        self.assertEqual(sorted(mr.verify(out, self.public)['app_rs']), ['linux-x86_64', 'macos-aarch64',
                                                                         'windows-x86_64'])

    def test_rust_program_is_optional(self):
        code, out, _ = self.run_build()
        self.assertNotIn('app_rs', json.loads((out / 'ddm32-manifest.json').read_text(encoding='utf-8')))

    def test_rust_program_zip_refusals(self):
        ok = self.rs_zip('ok.zip', 'windows-x86_64')
        cases = [
            ('needs a version', ['--app-rs-zip', f'windows-x86_64={ok}'], '--app-rs-version'),
            ('version without zip', ['--app-rs-version', '3.0.0'], 'at least one --app-rs-zip'),
            ('no =', ['--app-rs-zip', str(ok), '--app-rs-version', '3.0.0'], 'PLATFORM=ZIP'),
            ('unknown os', ['--app-rs-zip', f'freebsd-x86_64={ok}', '--app-rs-version', '3.0.0'], 'platform'),
            ('no arch', ['--app-rs-zip', f'windows-={ok}', '--app-rs-version', '3.0.0'], 'platform'),
            ('unsafe key', ['--app-rs-zip', f'windows-x86/64={ok}', '--app-rs-version', '3.0.0'], 'platform'),
            ('twice', ['--app-rs-zip', f'windows-x86_64={ok}', '--app-rs-zip', f'windows-x86_64={ok}',
                       '--app-rs-version', '3.0.0'], 'twice'),
            ('bad version', ['--app-rs-zip', f'windows-x86_64={ok}', '--app-rs-version', '3.0'], ''),
            # a Windows zip for a Linux key: the Linux program would find no DDM32Plus/DDM32Plus
            ('wrong exe', ['--app-rs-zip', f'linux-x86_64={ok}', '--app-rs-version', '3.0.0'], 'DDM32Plus/DDM32Plus'),
        ]
        for i, member in enumerate(('DDM32Plus/yedekler/x.txt', 'DDM32Plus/settings.json', 'DDM32Plus/Paketler/a.dpk',
                                    'DDM32Plus/Guncelleme/a', 'DDM32Plus/unins000.exe', 'Other/x.txt',
                                    'DDM32Plus/../x', 'DDM32Plus/a:b', 'DDM32Plus/x/backup-manifest.json',
                                    'DDM32Plus/progmem.bin', 'DDM32Plus\\x.txt')):
            bad = self.rs_zip(f'bad-rs{i}.zip', 'windows-x86_64', member)
            cases.append((member, ['--app-rs-zip', f'windows-x86_64={bad}', '--app-rs-version', '3.0.0'], ''))
        for i, (what, extra, msg) in enumerate(cases):
            out = self.top / f'r{i}'
            with self.subTest(what=what), self.assertRaisesRegex((SystemExit, ValueError), msg):
                self.run_build(*extra, out=out)
            self.assertFalse(out.exists())

    def test_app_rs_schema_is_checked(self):
        """check_app_rs holds a manifest to the Rust program's rules (the Python checker ignores the key)"""
        base = rt.manifest([rt.firmware_entry(fx.release_app('1.0.0'))])
        entry = dict(rt.app_entry('3.0.0', b'r'), file='DDM32Plus-3.0.0-linux-x86_64.zip',
                     url=rt.url('v3.0.0', 'DDM32Plus-3.0.0-linux-x86_64.zip'))
        good = dict(base, app_rs={'linux-x86_64': entry})
        self.assertIs(mr.check_app_rs(good), good)
        rt.sm.check_manifest(good)                                    # older checkers accept it unchanged
        bads = [dict(base, app_rs=[entry]), dict(base, app_rs={'linux x86': entry}),
                dict(base, app_rs={'beos-x86': entry})]
        for field, value in (('url', 'https://example.com/DDM32Plus-3.0.0-linux-x86_64.zip'), ('sha256', 'x'),
                             ('notes', 'text'), ('version', '3.0'), ('size', 0)):
            bad = json.loads(json.dumps(good))
            bad['app_rs']['linux-x86_64'][field] = value
            bads.append(bad)
        for i, bad in enumerate(bads):
            with self.subTest(i=i), self.assertRaises(rt.sm.ManifestError):
                mr.check_app_rs(bad)

    def test_rust_program_reads_the_release(self):
        """The Rust program's own reader (ddm32 verify-manifest, when it is built in rust/target) verifies the
        release and unpacks this platform's app_rs zip with its self-update extractor"""
        import platform as pf
        import subprocess
        exe = 'ddm32.exe' if sys.platform == 'win32' else 'ddm32'
        found = [p for p in (rt.PC.parent / 'rust' / 'target' / d / exe for d in ('debug', 'release')) if p.is_file()]
        if not found:
            self.skipTest('the Rust command line is not built (cargo build -p ddm32-cli in rust/)')
        ddm32 = str(max(found, key=lambda p: p.stat().st_mtime))
        os_name = {'win32': 'windows', 'darwin': 'macos'}.get(sys.platform, 'linux')
        arch = {'amd64': 'x86_64', 'x86_64': 'x86_64', 'arm64': 'aarch64', 'aarch64': 'aarch64'}.get(
            pf.machine().lower(), pf.machine().lower())
        here = f'{os_name}-{arch}'
        zips = [('windows-x86_64', 'w.zip'), ('linux-x86_64', 'l.zip'), ('macos-aarch64', 'm.zip')]
        if here not in dict(zips):
            zips.append((here, 'h.zip'))
        extra = []
        for platform, name in zips:
            extra += ['--app-rs-zip', f'{platform}={self.rs_zip(name, platform)}']
        code, out, _ = self.run_build(*extra, '--app-rs-version', '3.0.0-alpha.2')
        r = subprocess.run([ddm32, 'verify-manifest', str(out), '--public-key', self.public],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        for platform, _ in zips:
            self.assertIn(f'app_rs {platform} 3.0.0-alpha.2 DDM32Plus-3.0.0-alpha.2-{platform}.zip OK', r.stdout)
        self.assertIn(f'app_rs {here}: unpacks as the self-update expects', r.stdout)
        # a changed zip: the Rust reader refuses it too
        (out / f'DDM32Plus-3.0.0-alpha.2-{here}.zip').write_bytes(b'changed')
        r = subprocess.run([ddm32, 'verify-manifest', str(out), '--public-key', self.public],
                           capture_output=True, text=True, timeout=120)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('differs from the manifest', r.stderr)

    def test_refusals(self):
        nonpublic = rt.write_plus(self.top / 'np', fx.release_app('1.0.0'), signed_for=False)
        j = json.loads((nonpublic / 'rfoundry_dm32.json').read_text())
        j['public_enabled'] = False
        (nonpublic / 'rfoundry_dm32.json').write_text(json.dumps(j))
        self.build = nonpublic
        with self.assertRaisesRegex(SystemExit, 'not a public build'):
            self.run_build(out=self.top / 'o1')
        self.build = rt.write_plus(self.top / 'bad', fx.release_app('1.0.0'), identical=False, signed_for=False)
        with self.assertRaisesRegex(ValueError, 'reproducible'):
            self.run_build(out=self.top / 'o2')
        self.build = rt.write_plus(self.top / 'ok', fx.release_app('1.0.0'), signed_for=False)
        (self.top / 'exists').mkdir()
        with self.assertRaisesRegex(SystemExit, 'existing --out'):
            self.run_build(out=self.top / 'exists')
        with self.assertRaisesRegex(SystemExit, 'pc/dist'):
            self.run_build(out=rt.PC / 'release' / 'never')
        self.assertFalse((rt.PC / 'release' / 'never').exists())


class MapPackReleaseTest(unittest.TestCase):
    """The map pack as a signed release asset inside the firmware entry and the release gate (docs/mappack-design.md
    E.2, G T16; owner decision 2026-11-26, H.5 option (b): RadioID.net and the owner's list ship, "sample" never)"""
    setUp = ReleaseToolTest.setUp
    tearDown = ReleaseToolTest.tearDown
    run_build = ReleaseToolTest.run_build

    @staticmethod
    def pack(source='RadioID.net', sample=0, flags=None, broken=False):
        sys.path.insert(0, str(rt.PC.parent))
        from tools import datapack, refpack
        rpt = ((2, sample, b'2026-11-19', source.encode()),
               [(3993340, 3285970, 145600000, -480, 885, 0, 0, b'TA1AAA', b'Ankara', 6),
                (4102083, 2887500, 439162500, -6080, 0, 1, 0x22, b'TA1BBB', b'Istanbul', 34)])
        image = refpack.build('1.1.0', rpt=rpt)
        if broken or flags is not None:
            payload = bytearray(image[64:])
            if broken:		# the steps' count past the payload, CRCs made valid again
                for i, (sid, rec, off, count) in enumerate(refpack.directory(bytes(payload))):
                    if sid == refpack.STEP:
                        at = refpack.HEAD + refpack.ENTRY * i + 8
                        payload[at:at + 4] = (40000).to_bytes(4, 'little')
            h, _ = datapack.parse(image)
            image = datapack.header(datapack.REF, bytes(payload), h['date'], h['name'], tag=h['tag'].encode(),
                                    flags=h['flags'] if flags is None else flags) + bytes(payload)
        return image

    def put_pack(self, image, freq_set='own+RadioID.net', recorded=None):
        (self.build / 'ddm32-ref.dpk').write_bytes(image)
        j = json.loads((self.build / 'rfoundry_dm32.json').read_text())
        j['ref_pack'] = {'sha256': mr.sha256(recorded if recorded is not None else image), 'size': len(image),
                         'repeater_sources': {'pack': 'x', 'freq_set': freq_set,
                                              'freq_set_sample': freq_set == 'sample'}}
        (self.build / 'rfoundry_dm32.json').write_text(json.dumps(j))

    def test_pack_is_a_signed_asset_in_the_firmware_entry(self):
        image = self.pack()
        self.put_pack(image)
        code, out, _ = self.run_build()
        self.assertEqual(code, 0)
        names = sorted(p.name for p in out.iterdir())
        self.assertIn('ddm32-ref-1.0.0-beta.1.dpk', names)
        with zipfile.ZipFile(out / 'ddm32-plus-1.0.0-beta.1.zip') as z:	# the firmware zip keeps its three files
            self.assertEqual(sorted(z.namelist()), sorted(mr.FIRMWARE_FILES))
        m = mr.verify(out, self.public)
        e = m['firmware'][0]['ref_pack']
        self.assertEqual((e['schema'], e['pack_type'], e['pack_format'], e['dir_version'], e['version']),
                         (1, 3, 1, 1, '1.0.0-beta.1'))
        self.assertEqual((e['size'], e['sha256'], e['min_firmware']), (len(image), mr.sha256(image), '1.0.0-beta.1'))
        self.assertTrue(e['url'].endswith('/ddm32-ref-1.0.0-beta.1.dpk'))
        # --verify walks the nested object: a changed pack file is found
        (out / 'ddm32-ref-1.0.0-beta.1.dpk').write_bytes(image[:-1] + bytes([image[-1] ^ 1]))
        with self.assertRaisesRegex(SystemExit, 'size or SHA-256'):
            mr.verify(out, self.public)

    def test_gate(self):
        # owner decision 2026-11-26 (b): the TRAC list ("own") ships, in the pack and in the refusal set
        for k, source in enumerate(('own+RadioID.net', 'own list', 'RadioID.net')):
            self.put_pack(self.pack(source))
            self.assertEqual(self.run_build(out=self.top / f'ok-{k}')[0], 0)
        # a pack without repeater sections is fine too
        sys.path.insert(0, str(rt.PC.parent))
        from tools import refpack
        self.put_pack(refpack.build('1.1.0', rpt=None))
        self.assertEqual(self.run_build(out=self.top / 'ok-norpt')[0], 0)
        refusals = [
            (self.pack(sample=1, source='sample'), {}, 'fictional sample'),
            (self.pack(source='somebody'), {}, 'not cleared'),
            (self.pack(broken=True), {}, 'would not take it'),
            (self.pack(flags=0x1234), {}, 'would not take it'),
            (self.pack(), {'recorded': b'another pack'}, 'not the one this firmware build made'),
            (self.pack(), {'freq_set': 'sample'}, 'fictional sample'),
        ]
        for k, (image, kw, why) in enumerate(refusals):
            self.put_pack(image, **kw)
            with self.assertRaisesRegex(SystemExit, why):
                self.run_build(out=self.top / f'refused-{k}')
            self.assertFalse((self.top / f'refused-{k}').exists())
        # the gate is a setting: under another owner answer (RadioID.net only) "own" is refused
        with self.assertRaisesRegex(SystemExit, 'not cleared'):
            mr.check_ref_pack(self.pack('own+RadioID.net'), pack_sources={'RadioID.net'})
        # S3: a map pack firmware is never released without its pack by accident: --no-ref-pack or a missing
        # ddm32-ref.dpk is refused; --allow-no-ref-pack overrides with a loud warning
        self.put_pack(self.pack())
        with self.assertRaisesRegex(SystemExit, 'no map pack is given'):
            self.run_build('--no-ref-pack', out=self.top / 'none-refused')
        (self.build / 'ddm32-ref.dpk').unlink()
        with self.assertRaisesRegex(SystemExit, 'no map pack is given'):
            self.run_build(out=self.top / 'missing-refused')
        j = json.loads((self.build / 'rfoundry_dm32.json').read_text())
        del j['ref_pack']
        j.setdefault('audits', {})['map_pack_tables_absent'] = True	# the layout's audit alone also counts
        (self.build / 'rfoundry_dm32.json').write_text(json.dumps(j))
        with self.assertRaisesRegex(SystemExit, 'no map pack is given'):
            self.run_build(out=self.top / 'audit-refused')
        with patch('sys.stderr', io.StringIO()) as err:
            code, out, _ = self.run_build('--no-ref-pack', '--allow-no-ref-pack', out=self.top / 'none')
        self.assertEqual(code, 0)
        self.assertIn('WITHOUT its map pack', err.getvalue())
        self.assertNotIn('ref_pack', json.loads((out / 'ddm32-manifest.json').read_text())['firmware'][0])
        # an older build (no map pack layout) still releases without one, silently
        del j['audits']['map_pack_tables_absent']
        (self.build / 'rfoundry_dm32.json').write_text(json.dumps(j))
        self.assertEqual(self.run_build('--no-ref-pack', out=self.top / 'older')[0], 0)

    def test_manifest_schema(self):
        from radiotools import signed_manifest as sm
        self.put_pack(self.pack())
        code, out, _ = self.run_build()
        m = json.loads((out / 'ddm32-manifest.json').read_text(encoding='utf-8'))
        e = m['firmware'][0]['ref_pack']
        self.assertTrue(sm.check_ref_pack(e))
        self.assertFalse(sm.check_ref_pack(dict(e, schema=2, file='anything-else.bin')))	# newer: ignored
        for bad in (dict(e, pack_type=1), dict(e, size=70000), dict(e, file='ddm32-ref.dpk'),
                    dict(e, coord_crc='12'), dict(e, sha256='x'), dict(e, schema='1')):
            with self.assertRaises(sm.ManifestError):
                sm.check_ref_pack(bad)
        # N2: the pack's version must be its firmware entry's
        self.assertTrue(sm.check_ref_pack(e, 'x', '1.0.0-beta.1'))
        with self.assertRaisesRegex(sm.ManifestError, 'version'):
            sm.check_ref_pack(dict(e, version='1.0.0'), 'x', '1.0.0-beta.1')
        other = json.loads(json.dumps(m))
        other['firmware'][0]['ref_pack']['version'] = '9.9.9'
        with self.assertRaises(sm.ManifestError):
            sm.check_manifest(other)
        # a reader that does not know ref_pack still finds the firmware entry it knows
        self.assertEqual(m['firmware'][0]['file'], 'ddm32-plus-1.0.0-beta.1.zip')


if __name__ == '__main__':
    unittest.main()
