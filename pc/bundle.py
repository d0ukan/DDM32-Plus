"""Build the stand-alone DDM32 Plus program (no Python needed on the PC) and package it without private data.

  python pc/bundle.py build [--onefile]   PyInstaller: pc/dist/DDM32Plus/DDM32Plus.exe
  python pc/bundle.py check [FOLDER]      scan a program folder (default pc/dist/DDM32Plus) for private data
  python pc/bundle.py package             check, then zip pc/dist/DDM32Plus to pc/dist/DDM32Plus-<version>.zip

What goes in beside the GUI (read by the tools at run time, all public sources):
- radiotools/*.py: the guarded radio tools the program drives, unchanged, run as DDM32Plus.exe --run-tool NAME. They
  are kept out of PyInstaller's archive so that the app (signed-manifest checks) and the tools load the same files.
- research/upstream/radiofoundry/: exactly the pinned RadioFoundry files of radiotools/upstream.py (RAM monitor host,
  debug protocol, codec slot writer, board registry, DM32 codec maps, licence), each checked against its SHA-256 at
  build time (the build refuses a different file) and again at run time. The tree is found as the tools find it
  (DDM32_UPSTREAM, or research/upstream/radiofoundry of the repository).
- LICENSE (GPL-3.0), LICENSES/Open25-MIT.txt, LICENSING.md, THIRD_PARTY.md of the repository.
Never bundled: backups, release packages (.bin), RadioID/TRAC lists, stock firmware, codec bytes, logs, settings,
signing keys. `check` enforces that on the result. The user's data lives in %LOCALAPPDATA%\\DDM32Plus, not here.
"""
import argparse
import ast
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import sys
import zipfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
NAME = 'DDM32Plus'
DIST = HERE / 'dist'
WORK = HERE / 'build'
sys.path.insert(0, str(HERE))

TOOLS = ('install_plus', 'install_firstboot', 'dm32_readonly_backup', 'radio_detect', 'plus_txdiag', 'plus_live',
         'prepare_unit_image', 'signed_manifest', 'ed25519', 'upstream', 'native_modem', '__init__')
UPSTREAM_DEST = 'research/upstream/radiofoundry'
REPO_FILES = ('LICENSE', 'LICENSES/Open25-MIT.txt', 'LICENSING.md', 'THIRD_PARTY.md')
LOCAL_MODULES = {'radiotools', 'rfoundry', 'stubtool', 'f4stub', 'rfoundry_debug', 'hrc7000_codec'}
EXCLUDES = ('PyQt5.QtWebEngineWidgets', 'PyQt5.QtWebEngineCore', 'PyQt5.QtQml', 'PyQt5.QtQuick', 'PyQt5.QtMultimedia',
            'PyQt5.QtBluetooth', 'PyQt5.QtSql', 'PyQt5.QtTest', 'tkinter', 'unittest.mock', 'radiotools')
README_NAME = 'BENIOKU.txt'
README_TEXT = ('DDM32 Plus programlayıcı (Baofeng DM-32UV). DDM32Plus.exe\'yi çalıştırın.\n'
               'Yedekler, indirilen sürümler ve ayarlar bu klasörde DEĞİL, %LOCALAPPDATA%\\DDM32Plus içinde tutulur;\n'
               'program güncellemeleri onlara dokunmaz. Yedek klasörünü kimseyle paylaşmayın.\n'
               'Kaynak ve kullanım: https://github.com/d0ukan/DDM32-Plus (pc/README.md)\n')

# --- what may never be in a distributable folder -------------------------------------------------------------------
FORBIDDEN_DIRS = {'backups', 'backup', 'yedekler', 'artifacts', 'research-data', 'originals', 'logs', '__pycache__',
                  'surumler', 'guncelleme'}
FORBIDDEN_NAMES = {'user.csv', 'rptrs.json', 'repeaters.csv', 'settings.json', 'backup-manifest.json',
                   'install-receipt.json', 'preflight.json', 'celestrak-amateur.txt', 'tle.txt',
                   'release-signing-key', 'ddm32-manifest.json', 'ddm32-manifest.json.sig'}
FORBIDDEN_SUFFIXES = {'.bin', '.csv', '.map', '.elf', '.hex', '.img', '.raw', '.log', '.partial', '.download',
                      '.dfu', '.srec', '.pem', '.key', '.sig'}
# certifi's public Mozilla root list (MPL-2.0), the one .pem the program may carry (ddm32_app/tls.py)
CA_BUNDLE_FILES = {'certifi/cacert.pem', '_internal/certifi/cacert.pem'}
TEXT_SUFFIXES = {'.txt', '.py', '.json', '.md', '.csv', '.cfg', '.ini', '.toml'}
TEXT_MARKERS = (b'RADIO_ID,CALLSIGN', b'"rptrs"', b'"backup_verified"', b'"writes_started"')


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def upstream_module():
    from radiotools import upstream
    return upstream


def allowed_json():
    up = upstream_module()
    return {f'{UPSTREAM_DEST}/{rel}' for rel in up.PINNED if rel.endswith('.json')}


def verify_pins():
    """Every pinned upstream file present and equal to its SHA-256 (the tools check the same at run time)"""
    up = upstream_module()
    try:
        return up.verify()
    except (OSError, ValueError) as exc:
        raise SystemExit(f'Pinned upstream files: {exc}')


def bundled_files():
    """[(source, folder inside the program)] of every non-archive file the frozen program reads"""
    up = upstream_module()
    root = up.root()
    out = [(HERE / 'radiotools' / f'{t}.py', 'radiotools') for t in TOOLS]
    out += [(root / rel, str(Path(UPSTREAM_DEST, rel).parent).replace('\\', '/')) for rel in up.PINNED]
    out += [(REPO / f, '.') for f in REPO_FILES]
    missing = [str(s) for s, _ in out if not s.is_file()]
    if missing:
        raise SystemExit('Missing bundle inputs: ' + ', '.join(missing))
    return out


def imports_of(py):
    """Absolute module names a Python file imports anywhere (also inside functions)"""
    names = set()
    for node in ast.walk(ast.parse(Path(py).read_text(encoding='utf-8'))):
        if isinstance(node, ast.Import):
            names.update(a.name for a in node.names)
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            names.add(node.module)
            names.update(f'{node.module}.{a.name}' for a in node.names if a.name != '*')
    return names


def hidden_imports():
    """Every importable stdlib / pyserial module the bundled tools may import (PyInstaller does not analyse data
    files), plus pyserial's Windows backend"""
    found = {'serial', 'serial.serialwin32', 'serial.win32', 'serial.tools.list_ports',
             'serial.tools.list_ports_windows', 'runpy', 'ctypes', 'ctypes.wintypes', 'certifi'}
    for src, _ in bundled_files():
        if src.suffix != '.py':
            continue
        for name in imports_of(src):
            if name.split('.')[0] in LOCAL_MODULES:
                continue
            try:
                if importlib.util.find_spec(name) is not None:
                    found.add(name)
            except (ImportError, ValueError, AttributeError):
                pass
    return sorted(found)


def pyinstaller_args(onefile=False):
    args = ['--noconfirm', '--clean', '--windowed', '--onefile' if onefile else '--onedir', '--name', NAME,
            '--distpath', str(DIST), '--workpath', str(WORK), '--specpath', str(WORK), '--paths', str(HERE)]
    for m in EXCLUDES:
        args += ['--exclude-module', m]
    for m in hidden_imports():
        args += ['--hidden-import', m]
    for src, dest in bundled_files():
        args += ['--add-data', f'{src}{os.pathsep}{dest}']
    return args + [str(HERE / 'run_ddm32.py')]


def app_version():
    text = (HERE / 'ddm32_app' / '__init__.py').read_text(encoding='utf-8')
    return re.search(r"__version__ = '([^']+)'", text).group(1)


def scan(folder):
    """[(relative path, reason)] of anything private in a program folder; empty when it is clean"""
    folder = Path(folder)
    allowed = allowed_json()
    problems = []
    for p in sorted(folder.rglob('*')):
        rel = p.relative_to(folder).as_posix()
        parts = rel.split('/')
        if p.is_dir():
            if parts[-1].lower() in FORBIDDEN_DIRS:
                problems.append((rel, 'forbidden folder'))
            continue
        name = parts[-1].lower()
        if rel in CA_BUNDLE_FILES:
            continue
        if name in FORBIDDEN_NAMES:
            problems.append((rel, 'private data file'))
        elif p.suffix.lower() in FORBIDDEN_SUFFIXES:
            problems.append((rel, f'{p.suffix} file (images, backups, packages, lists, keys)'))
        elif p.suffix.lower() == '.json' and rel not in allowed:
            problems.append((rel, 'unexpected JSON'))
        elif 'research/originals' in rel or 'research-data' in rel:
            problems.append((rel, 'private source tree'))
        elif p.suffix.lower() in TEXT_SUFFIXES and p.stat().st_size < 64 << 20:
            data = p.read_bytes()
            for marker in TEXT_MARKERS:
                if marker in data:
                    problems.append((rel, 'contains ' + marker.decode()))
                    break
    return problems


def check(folder):
    problems = scan(folder)
    for rel, why in problems:
        print(f'PRIVATE? {rel}: {why}')
    if problems:
        raise SystemExit(f'{len(problems)} file(s) must not be distributed; nothing packaged')
    print(f'Clean: {folder}')


def build(onefile=False):
    verify_pins()
    import PyInstaller.__main__
    PyInstaller.__main__.run(pyinstaller_args(onefile))
    target = DIST if onefile else DIST / NAME
    if not onefile:
        (target / README_NAME).write_text(README_TEXT, encoding='utf-8')
        check(target)
    print(f'Built: {target}')
    return target


def package(folder=DIST / NAME, out=None):
    folder = Path(folder)
    if not (folder / f'{NAME}.exe').is_file():
        raise SystemExit(f'No {NAME}.exe in {folder}: build first')
    check(folder)
    out = Path(out or DIST / f'{NAME}-{app_version()}.zip')
    tmp = out.with_suffix('.zip.partial')
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in sorted(folder.rglob('*')):
            arc = Path(NAME) / p.relative_to(folder)
            if p.is_dir():
                z.write(p, arc.as_posix() + '/')
            else:
                z.write(p, arc.as_posix())
    os.replace(tmp, out)
    names = zipfile.ZipFile(out).namelist()
    bad = [n for n in names if any(re.search(r'(^|/)%s(/|$)' % re.escape(d), n.lower()) for d in FORBIDDEN_DIRS)
           or (Path(n).suffix.lower() in FORBIDDEN_SUFFIXES and n[len(NAME) + 1:] not in CA_BUNDLE_FILES)
           or not n.startswith(NAME + '/')]
    if bad:
        out.unlink()
        raise SystemExit('Zip check failed: ' + ', '.join(bad))
    print(f'Packaged: {out} ({out.stat().st_size // (1 << 20)} MB, {len(names)} entries)')
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--onefile', action='store_true')
    c = sub.add_parser('check')
    c.add_argument('folder', nargs='?', default=str(DIST / NAME))
    sub.add_parser('package')
    a = ap.parse_args(argv)
    if a.cmd == 'build':
        build(a.onefile)
    elif a.cmd == 'check':
        check(a.folder)
    else:
        package()


if __name__ == '__main__':
    main()
