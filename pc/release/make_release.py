#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make a signed DDM32 Plus release: the firmware zip, optionally the program zip, ddm32-manifest.json and its
Ed25519 signature. Uploads NOTHING: the files land in --out; publishing them on GitHub is a separate, approved step.

  --keygen [--key PATH]
      Creates the release signing key (32 random bytes, hex) at %USERPROFILE%\\.ddm32\\release-signing-key (or PATH,
      never inside the repository), restricts it to the current user (icacls on Windows, mode 600 elsewhere) and
      prints the public key. Paste that into pc/radiotools/signed_manifest.py PUBLIC_KEY_HEX and build the program
      again: only then can the program verify (and so install) anything. Refuses to overwrite an existing key.

  build --version V --channel beta|stable --build-dir DIR --settings-format N --codeplug-format N
        --min-compatible V --notes-tr TEXT --notes-en TEXT [--app-zip ZIP --app-version V
        [--app-notes-tr TEXT --app-notes-en TEXT]] [--app-net-zip ZIP --app-net-version V
        [--app-net-notes-tr TEXT --app-net-notes-en TEXT]] [--app-rs-zip PLATFORM=ZIP ... --app-rs-version V
        [--app-rs-notes-tr TEXT --app-rs-notes-en TEXT]] [--tag TAG] --out NEWDIR [--key PATH]
      DIR is a public DDM32 Plus build (tools/build_radiofoundry.py --plus --public: rfoundry_dm32.bin/.json/.map).
      Checks it like the installer does (reproducible Plus build, hashes, codec layout, size) and requires
      public_enabled. The release zip carries the application, a reduced linker map (the __codec_* symbol lines the
      installer checks) and a reduced build manifest (no build-machine paths). Signs the manifest and verifies the
      signature with the key's public half before it finishes.
      --app-zip is the Python program (manifest "app"); --app-net-zip the Windows (.NET) program (manifest
      "app_net", win/). Each program updates itself only from its own entry, never from the other one.
      --app-rs-zip PLATFORM=ZIP (repeat it, one per platform) is the Rust program (rust/, manifest "app_rs":
      {PLATFORM: entry}); PLATFORM is the program's own key, windows|linux|macos-ARCH as Rust names the CPU
      (windows-x86_64, linux-x86_64, linux-aarch64, macos-aarch64, macos-x86_64). The zip is what
      rust/packaging/make_app_zip.py makes: DDM32Plus/DDM32Plus.exe (Windows) or DDM32Plus/DDM32Plus (Linux,
      macOS), everything under DDM32Plus/, no name of the user's data; it is published as
      DDM32Plus-VERSION-PLATFORM.zip. Older programs ignore "app_rs".

  verify DIR [--public-key HEX]
      Verifies DIR/ddm32-manifest.json(.sig) and every asset in DIR against the manifest's sizes and hashes.

settings_format / codeplug_format: integers that change when the firmware's stored settings / channel layout changes;
min_compatible_version: the oldest firmware version that reads what this version saves. The program warns (and asks
twice) when a version change goes back across these.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zipfile

PC = Path(__file__).resolve().parents[1]
REPO = PC.parent
sys.path.insert(0, str(PC))
from radiotools import ed25519  # noqa: E402
from radiotools import install_plus  # noqa: E402
from radiotools import signed_manifest as sm  # noqa: E402

DEFAULT_KEY = Path.home() / '.ddm32' / 'release-signing-key'
# The key created before the rename to DDM32 Plus (2026-11-20) is in .ddm25: used when there is none at the new place
LEGACY_KEY = Path.home() / '.ddm25' / 'release-signing-key'
if not DEFAULT_KEY.exists() and LEGACY_KEY.exists():
    DEFAULT_KEY = LEGACY_KEY
FIRMWARE_FILES = ('rfoundry_dm32.bin', 'rfoundry_dm32.json', 'rfoundry_dm32.map')
ZIP_TIME = (2026, 1, 1, 0, 0, 0)
EXE = 'DDM32Plus'


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def _inside_repo(path):
    try:
        Path(path).resolve().relative_to(REPO.resolve())
        return True
    except ValueError:
        return False


# --- key -----------------------------------------------------------------------------------------------------------
def restrict(path):
    """Only the current user may read the key"""
    if os.name == 'nt':
        user = os.environ.get('USERNAME') or ''
        if user:
            subprocess.run(['icacls', str(path), '/inheritance:r', '/grant:r', f'{user}:F'], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        os.chmod(path, 0o600)


def keygen(path, restrict_fn=restrict):
    path = Path(path)
    if _inside_repo(path):
        raise SystemExit('Refusing: the signing key must live outside the repository')
    path.parent.mkdir(parents=True, exist_ok=True)
    secret = os.urandom(32)
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)     # never overwrite a key
    with os.fdopen(fd, 'w') as f:
        f.write(secret.hex() + '\n')
    try:
        restrict_fn(path)
        restricted = True
    except Exception:
        restricted = False
    public = ed25519.secret_to_public(secret).hex()
    return public, restricted


def load_key(path):
    path = Path(path)
    if _inside_repo(path):
        raise SystemExit('Refusing: the signing key must live outside the repository')
    text = path.read_text().strip().lower()
    if not re.fullmatch(r'[0-9a-f]{64}', text):
        raise SystemExit('The signing key file is malformed')
    return bytes.fromhex(text)


# --- release assets ------------------------------------------------------------------------------------------------
def _zip_write(z, name, data):
    info = zipfile.ZipInfo(name, ZIP_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)


def firmware_package(build_dir, version):
    """(zip bytes, application bytes, release build manifest) for a checked public build"""
    build_dir = Path(build_dir)
    app = install_plus.build_application(build_dir)
    build = json.loads((build_dir / 'rfoundry_dm32.json').read_text())
    if not build.get('public_enabled'):
        raise SystemExit('Refusing: not a public build (tools/build_radiofoundry.py --plus --public)')
    if build.get('compiler_warnings') not in (None, 0):
        raise SystemExit('Refusing: the build has compiler warnings')
    full_map = (build_dir / 'rfoundry_dm32.map').read_bytes()
    lines = [m.group(0).strip() for m in
             re.finditer(r'^\s*0x[0-9a-fA-F]+\s+__codec_\w+\s*=.*$', full_map.decode('utf-8', 'replace'), re.M)]
    reduced_map = ('# DDM32 Plus %s: codec layout symbols from the linker map (full map: source build)\n' % version +
                   '\n'.join('                ' + l for l in lines) + '\n').encode()
    release_build = {
        'kind': 'DDM32 Plus release', 'version': version, 'plus_enabled': True, 'public_enabled': True,
        'forced_rebuild_identical': True, 'upstream_commit': build.get('upstream_commit'),
        'outputs': {'rfoundry_dm32.bin': sha256(app), 'rfoundry_dm32.map': sha256(reduced_map)},
        'source_build_manifest_sha256': sha256((build_dir / 'rfoundry_dm32.json').read_bytes()),
        'source_map_sha256': sha256(full_map)}
    release_json = (json.dumps(release_build, indent=2) + '\n').encode()
    from io import BytesIO
    buf = BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        _zip_write(z, 'rfoundry_dm32.bin', app)
        _zip_write(z, 'rfoundry_dm32.json', release_json)
        _zip_write(z, 'rfoundry_dm32.map', reduced_map)
    data = buf.getvalue()
    # the release folder must pass the installer's own build checks
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        with zipfile.ZipFile(BytesIO(data)) as z:
            z.extractall(td)
        if install_plus.build_application(td) != app:
            raise SystemExit('Release package does not pass the installer\'s build checks')
    return data, app, release_build


# --- the map pack (firmware 1.1+, docs/mappack-design.md E.2) ----------------------------------------------------
# The release gate's allowed repeater sources (tools/rpt_list.py's source names). Owner decision 2026-11-26 (H.5
# option (b), replacing the recommended (d)): both sources ship as in 1.0.x, the owner's TRAC list ("own") with
# RadioID.net, in the public map pack's records and in the program-flash refusal set; the TRAC list states no
# licence and the owner takes responsibility for publishing it. A "sample" list (the fictional one a build without
# research-data gets) is never released. Change these two sets if the owner's answer changes.
PACK_SOURCES = {'RadioID.net', 'own list', 'own+RadioID.net'}
FREQ_SET_SOURCES = {'RadioID.net', 'own list', 'own+RadioID.net'}


def _refpack():
    sys.path.insert(0, str(REPO))
    from tools import datapack, refpack
    return datapack, refpack


def check_ref_pack(image, build=None, pack_sources=None, freq_sources=None):
    """A map pack .dpk for a release: a whole map pack (header, CRCs) the firmware takes (refpack.validate, its
    version hint), the one the build made (build manifest ref_pack.sha256), and its repeater data from sources the
    owner cleared (the gate, S6): the pack's RPT_INFO source and the build's recorded refusal-set source, each
    against its own list; "sample" refused. Returns the pack's facts for the manifest entry"""
    dp, rp = _refpack()
    pack_sources = PACK_SOURCES if pack_sources is None else pack_sources
    freq_sources = FREQ_SET_SOURCES if freq_sources is None else freq_sources
    h, payload = dp.parse(image)
    if not h or h['type'] != dp.REF or h['fmt'] != 1 or not h['crc_ok'] or len(image) != 64 + h['len']:
        raise SystemExit('Refusing the map pack: not a whole map pack (header, length or CRC)')
    if len(image) > sm.REF_PACK_MAX:
        raise SystemExit('Refusing the map pack: larger than its slot')
    k = rp.defines(rp.MAP_H)
    verdict, why = rp.validate(payload, h['flags'], k)
    if verdict != 'ok':
        raise SystemExit(f'Refusing the map pack: the firmware would not take it ({verdict}: {why})')
    if h['flags'] != rp.hint(k):
        raise SystemExit('Refusing the map pack: its version hint is not this firmware\'s')
    info = rp.rpt_info(payload)
    if info is not None and (info[1] or info[3] == 'sample'):
        raise SystemExit('Refusing the map pack: its repeater list is the fictional sample')
    if info is not None and info[3] not in pack_sources:
        raise SystemExit(f'Refusing the map pack: repeater records from "{info[3]}" are not cleared for a release')
    if build is not None:
        rec = build.get('ref_pack') or {}
        if rec.get('sha256') != sha256(image):
            raise SystemExit('Refusing the map pack: it is not the one this firmware build made')
        src = rec.get('repeater_sources') or {}
        if src.get('freq_set_sample') or src.get('freq_set') == 'sample':
            raise SystemExit('Refusing: the firmware\'s repeater refusal set was made from the fictional sample')
        if src.get('freq_set') not in freq_sources:
            raise SystemExit(f'Refusing: the firmware\'s refusal set comes from "{src.get("freq_set")}", not cleared')
    return {'pack_type': 3, 'pack_format': 1, 'dir_version': payload[4], 'coord_crc': '0x%08x' % rp.coord_id(k),
            'date': h['date'], 'repeaters': info[3] if info else 'none'}


def check_app_zip(path):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
    if f'{EXE}/{EXE}.exe' not in names or any(not n.startswith(f'{EXE}/') or '..' in n for n in names):
        raise SystemExit(f'The program zip must hold {EXE}/{EXE}.exe and nothing outside {EXE}/')
    # Same rules as pc/bundle.py: a backup folder, a backup manifest, raw images or logs anywhere. The backup tool
    # itself (radiotools/dm32_readonly_backup.py) is public source and belongs in the program.
    private_dirs = {'backups', 'backup', 'yedekler'}
    bad = [n for n in names if Path(n).suffix.lower() in ('.bin', '.log')
           or private_dirs & {p.lower() for p in n.split('/')[:-1]}
           or Path(n).name.lower() == 'backup-manifest.json']
    if bad:
        raise SystemExit('The program zip holds files that must not be published: ' + ', '.join(bad[:5]))
    return Path(path).read_bytes()


# What the Windows program keeps in its install folder: an update must never bring a file of that name
# (win/src/DDM32Plus.Core/AppInfo.cs ReservedNames)
NET_RESERVED = {'yedekler', 'surumler', 'guncelleme', 'settings.json', 'unins000.exe', 'unins000.dat'}


def check_app_net_zip(path):
    """The Windows program's zip: DDM32Plus/DDM32Plus.exe (the single-file program) and nothing named like the user's
    data in the install folder; the same private-file rules as the Python program's zip"""
    data = check_app_zip(path)
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
    bad = [n for n in names if len(n.split('/')) > 1 and n.split('/')[1].lower() in NET_RESERVED
           or '\\' in n or ':' in n]
    if bad:
        raise SystemExit("The Windows program zip holds names of the user's data: " + ', '.join(bad[:5]))
    return data


# The Rust program's update zips (app_rs): the rules of rust/crates/ddm32-core/src/update/self_update.rs safe_extract
# and app_info.rs RESERVED_NAMES, so a zip this tool accepts is one the program unpacks.
RS_PLATFORM = re.compile(r'^(windows|linux|macos)-[0-9A-Za-z_]{1,32}$')
RS_RESERVED = {'yedekler', 'surumler', 'guncelleme', 'paketler', 'settings.json', 'unins000.exe', 'unins000.dat'}


def _private_files(names):
    """Same rules as pc/bundle.py: a backup folder, a backup manifest, raw images or logs anywhere"""
    private_dirs = {'backups', 'backup', 'yedekler'}
    return [n for n in names if Path(n).suffix.lower() in ('.bin', '.log')
            or private_dirs & {p.lower() for p in n.split('/')[:-1]}
            or Path(n).name.lower() == 'backup-manifest.json']


def parse_app_rs(values):
    """--app-rs-zip PLATFORM=ZIP values -> [(platform, Path)], in the order given; one zip per platform"""
    out = []
    for v in values or []:
        platform, sep, path = v.partition('=')
        if not sep or not path:
            raise SystemExit(f'--app-rs-zip takes PLATFORM=ZIP, not {v!r}')
        if not RS_PLATFORM.match(platform) or not sm.RE_FILE.match(platform):
            raise SystemExit(f'--app-rs-zip: platform {platform!r} is not windows-ARCH, linux-ARCH or macos-ARCH')
        if any(p == platform for p, _ in out):
            raise SystemExit(f'--app-rs-zip: platform {platform} given twice')
        out.append((platform, Path(path)))
    return out


def check_app_rs_zip(path, platform):
    """The Rust program's zip for one platform, as its self-update unpacks it"""
    exe = f'{EXE}/{EXE}.exe' if platform.startswith('windows-') else f'{EXE}/{EXE}'
    with zipfile.ZipFile(path) as z:
        # the names as stored: on Windows zipfile shows a stored backslash as "/" (the program would see it raw)
        names = [i.orig_filename for i in z.infolist()]
    if exe not in names:
        raise SystemExit(f'The {platform} program zip must hold {exe}')
    for n in names:
        parts = [p for p in n.split('/') if p not in ('', '.')]
        if (not parts or parts[0] != EXE or n.startswith('/') or '\\' in n
                or any(p == '..' or ':' in p for p in parts)
                or (len(parts) > 1 and parts[1].lower() in RS_RESERVED)):
            raise SystemExit(f"The {platform} program zip holds a path outside {EXE}/ or a name of the user's data: "
                             f'{n}')
    bad = _private_files(names)
    if bad:
        raise SystemExit('The program zip holds files that must not be published: ' + ', '.join(bad[:5]))
    return Path(path).read_bytes()


def check_app_rs(m):
    """The "app_rs" part of a manifest as the Rust program checks it (signed_manifest.rs): an object of
    {platform: asset}, each platform a safe name, each asset the same fields as "app" (notes optional)"""
    rs = m.get('app_rs')
    if rs is None:
        return m
    if not isinstance(rs, dict):
        raise sm.ManifestError('Manifest field invalid: app_rs')
    for platform, e in rs.items():
        if not (isinstance(platform, str) and sm.RE_FILE.match(platform) and RS_PLATFORM.match(platform)):
            raise sm.ManifestError('Manifest field invalid: app_rs')
        sm._asset(e, f'app_rs.{platform}')
        if 'notes' in e and not isinstance(e['notes'], dict):
            raise sm.ManifestError(f'Manifest field invalid: app_rs.{platform}.notes')
    return m


def _app_entry(tag, name, version, data, notes_tr, notes_en):
    return {'version': version, 'file': name, 'url': f'{sm.URL_PREFIX}{tag}/{name}', 'sha256': sha256(data),
            'size': len(data), 'notes': {'tr': notes_tr or '', 'en': notes_en or ''}}


MOJIBAKE = ('Ã', 'Ä', 'Å', '�')	# UTF-8 read as cp1252 / Latin-1 (ç -> Ã§, ı -> Ä±, ş -> ÅŸ), or a lost byte


def check_notes(args):
    """Refuse release notes that were mangled on the way in (v1.0.0-beta.4's Turkish notes reached the manifest
    double-encoded through a Windows shell). Pass them from Python as an argument list, or with PYTHONUTF8=1."""
    for name in ('notes_tr', 'notes_en', 'app_notes_tr', 'app_notes_en', 'app_net_notes_tr', 'app_net_notes_en',
                 'app_rs_notes_tr', 'app_rs_notes_en'):
        text = getattr(args, name, None) or ''
        if any(m in text for m in MOJIBAKE):
            raise SystemExit(f'--{name.replace("_", "-")} looks double-encoded: {text[:60]!r}')


def build(args, secret):
    check_notes(args)
    out = Path(args.out)
    if out.exists():
        raise SystemExit(f'Refusing an existing --out: {out}')
    if _inside_repo(out) and not str(out.resolve()).startswith(str((PC / 'dist').resolve())):
        raise SystemExit('Put the release under pc/dist/ or outside the repository')
    sm.parse_version(args.version)
    sm.parse_version(args.min_compatible)
    tag = args.tag or f'v{args.version}'
    fw_zip, app, _ = firmware_package(args.build_dir, args.version)
    fw_name = f'ddm32-plus-{args.version}.zip'
    entry = {'version': args.version, 'channel': args.channel, 'file': fw_name,
             'url': f'{sm.URL_PREFIX}{tag}/{fw_name}', 'sha256': sha256(fw_zip), 'size': len(fw_zip),
             'application_sha256': sha256(app), 'application_size': len(app),
             'settings_format': args.settings_format, 'codeplug_format': args.codeplug_format,
             'min_compatible_version': args.min_compatible,
             'notes': {'tr': args.notes_tr, 'en': args.notes_en}}
    manifest = {'format': sm.FORMAT, 'repo': sm.REPO, 'release': tag,
                'created_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'firmware': [entry]}
    assets = {fw_name: fw_zip}
    # The map pack (firmware 1.1+): --ref-pack, else the one the build made beside the image. A firmware that reads its
    # map, repeaters and airfields from the pack (its build has the map pack layout) is never released without it:
    # --no-ref-pack is for older builds, or with --allow-no-ref-pack (a loud warning: the map stays empty)
    ref = getattr(args, 'ref_pack', None)
    build_json = json.loads((Path(args.build_dir) / 'rfoundry_dm32.json').read_text())
    needs_pack = bool(build_json.get('ref_pack') or (build_json.get('audits') or {}).get('map_pack_tables_absent'))
    if ref is None and not getattr(args, 'no_ref_pack', False) and (Path(args.build_dir) / 'ddm32-ref.dpk').is_file():
        ref = Path(args.build_dir) / 'ddm32-ref.dpk'
    if ref is None and needs_pack:
        if not getattr(args, 'allow_no_ref_pack', False):
            raise SystemExit('Refusing: this firmware reads its map, repeaters and airfields from the map pack, and no '
                             'map pack is given (ddm32-ref.dpk missing or --no-ref-pack); --allow-no-ref-pack overrides')
        print('WARNING: releasing a map pack firmware WITHOUT its map pack: the map, Repeaters and Airfields stay '
              'empty until a user loads one (--allow-no-ref-pack)', file=sys.stderr)
    if ref is not None:
        image = Path(ref).read_bytes()
        facts = check_ref_pack(image, json.loads((Path(args.build_dir) / 'rfoundry_dm32.json').read_text()))
        ref_name = f'ddm32-ref-{args.version}.dpk'
        entry['ref_pack'] = {'schema': sm.REF_PACK_SCHEMA, 'version': args.version, 'file': ref_name,
                             'url': f'{sm.URL_PREFIX}{tag}/{ref_name}', 'sha256': sha256(image), 'size': len(image),
                             'pack_type': facts['pack_type'], 'pack_format': facts['pack_format'],
                             'dir_version': facts['dir_version'], 'coord_crc': facts['coord_crc'],
                             'date': facts['date'],
                             'min_firmware': getattr(args, 'ref_min_firmware', None) or args.version}
        assets[ref_name] = image
    if args.app_zip:
        if not args.app_version:
            raise SystemExit('--app-zip needs --app-version')
        sm.parse_version(args.app_version)
        data = check_app_zip(args.app_zip)
        app_name = f'{EXE}-{args.app_version}.zip'
        manifest['app'] = _app_entry(tag, app_name, args.app_version, data, args.app_notes_tr, args.app_notes_en)
        assets[app_name] = data
    if getattr(args, 'app_net_zip', None):
        if not args.app_net_version:
            raise SystemExit('--app-net-zip needs --app-net-version')
        sm.parse_version(args.app_net_version)
        data = check_app_net_zip(args.app_net_zip)
        net_name = f'{EXE}-win-{args.app_net_version}.zip'
        manifest['app_net'] = _app_entry(tag, net_name, args.app_net_version, data, args.app_net_notes_tr,
                                         args.app_net_notes_en)
        assets[net_name] = data
    rs_zips = parse_app_rs(getattr(args, 'app_rs_zip', None))
    if rs_zips:
        if not args.app_rs_version:
            raise SystemExit('--app-rs-zip needs --app-rs-version')
        sm.parse_version(args.app_rs_version)
        manifest['app_rs'] = {}
        for platform, path in rs_zips:
            data = check_app_rs_zip(path, platform)
            rs_name = f'{EXE}-{args.app_rs_version}-{platform}.zip'
            manifest['app_rs'][platform] = _app_entry(tag, rs_name, args.app_rs_version, data, args.app_rs_notes_tr,
                                                      args.app_rs_notes_en)
            assets[rs_name] = data
    elif getattr(args, 'app_rs_version', None):
        raise SystemExit('--app-rs-version needs at least one --app-rs-zip PLATFORM=ZIP')
    sm.check_manifest(manifest)
    check_app_rs(manifest)
    data = (json.dumps(manifest, indent=2, ensure_ascii=False) + '\n').encode('utf-8')
    sig = sm.sign_bytes(data, secret)
    public = ed25519.secret_to_public(secret).hex()
    sm.verify_bytes(data, sig, public)           # the signature verifies before anything is kept
    out.mkdir(parents=True)
    for name, body in assets.items():
        (out / name).write_bytes(body)
    (out / sm.MANIFEST_NAME).write_bytes(data)
    (out / sm.SIG_NAME).write_text(sig)
    return out, public, manifest


def verify(folder, public_hex=None):
    folder = Path(folder)
    m = check_app_rs(sm.verify_files(folder, public_hex))
    refs = [e['ref_pack'] for e in m['firmware'] if e.get('ref_pack') and sm.check_ref_pack(e['ref_pack'])]
    for e in (m['firmware'] + refs + [m[k] for k in ('app', 'app_net') if m.get(k)] +
              list((m.get('app_rs') or {}).values())):
        body = (folder / e['file']).read_bytes()
        if len(body) != e['size'] or sha256(body) != e['sha256']:
            raise SystemExit(f'{e["file"]}: size or SHA-256 differs from the manifest')
    for e in refs:		# each firmware's map pack: whole, taken by the firmware, the repeater gate
        facts = check_ref_pack((folder / e['file']).read_bytes())
        if facts['coord_crc'] != e['coord_crc'] or facts['dir_version'] != e['dir_version']:
            raise SystemExit(f'{e["file"]}: its directory does not match the manifest entry')
    return m


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keygen', action='store_true')
    ap.add_argument('--key', type=Path, default=DEFAULT_KEY)
    sub = ap.add_subparsers(dest='cmd')
    b = sub.add_parser('build')
    b.add_argument('--version', required=True)
    b.add_argument('--channel', choices=sm.CHANNELS, required=True)
    b.add_argument('--build-dir', type=Path, required=True)
    b.add_argument('--settings-format', type=int, required=True)
    b.add_argument('--codeplug-format', type=int, required=True)
    b.add_argument('--min-compatible', required=True)
    b.add_argument('--notes-tr', required=True)
    b.add_argument('--notes-en', required=True)
    b.add_argument('--app-zip', type=Path)
    b.add_argument('--app-version')
    b.add_argument('--app-notes-tr')
    b.add_argument('--app-notes-en')
    b.add_argument('--app-net-zip', type=Path, help='the Windows (.NET) program: DDM32Plus/DDM32Plus.exe')
    b.add_argument('--app-net-version')
    b.add_argument('--app-net-notes-tr')
    b.add_argument('--app-net-notes-en')
    b.add_argument('--app-rs-zip', action='append', metavar='PLATFORM=ZIP',
                   help='the Rust program for one platform (repeat per platform), e.g. windows-x86_64=DDM32Plus.zip')
    b.add_argument('--app-rs-version')
    b.add_argument('--app-rs-notes-tr')
    b.add_argument('--app-rs-notes-en')
    b.add_argument('--ref-pack', type=Path, help='the map pack .dpk (default: DIR/ddm32-ref.dpk when the build made one)')
    b.add_argument('--no-ref-pack', action='store_true', help='a release without the map pack (firmware before 1.1)')
    b.add_argument('--allow-no-ref-pack', action='store_true',
                   help='release a map pack firmware without its map pack anyway (warns: its map stays empty)')
    b.add_argument('--ref-min-firmware', help='the oldest firmware that uses the pack (default: --version)')
    b.add_argument('--tag')
    b.add_argument('--out', type=Path, required=True)
    b.add_argument('--key', dest='build_key', type=Path, default=None)
    v = sub.add_parser('verify')
    v.add_argument('folder', type=Path)
    v.add_argument('--public-key')
    args = ap.parse_args(argv)
    if args.keygen:
        public, restricted = keygen(args.key)
        print(f'Signing key written: {args.key}' + ('' if restricted else '  (WARNING: could not restrict its ACL)'))
        print(f'Public key: {public}')
        print('Paste it into pc/radiotools/signed_manifest.py PUBLIC_KEY_HEX and build the program again.')
        print('Keep the key file private and backed up offline; it is never part of the repository.')
        return 0
    if args.cmd == 'build':
        secret = load_key(args.build_key or args.key)
        out, public, m = build(args, secret)
        print(f'Release files in {out} (key id {sm.key_id(bytes.fromhex(public))}):')
        for p in sorted(out.iterdir()):
            print(f'  {p.name}  {p.stat().st_size} B')
        print(f'Tag: {m["release"]}. Nothing was uploaded. Publish these files on GitHub as the assets of that '
              'release (a separate, approved step).')
        return 0
    if args.cmd == 'verify':
        m = verify(args.folder, args.public_key)
        print(f'OK: {m["release"]}, {len(m["firmware"])} firmware' + (', program' if m.get('app') else '') +
              (', Windows program' if m.get('app_net') else '') +
              (', Rust program: ' + ', '.join(m['app_rs']) if m.get('app_rs') else ''))
        return 0
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
