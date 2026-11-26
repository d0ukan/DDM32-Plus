# SPDX-License-Identifier: GPL-3.0-or-later
"""The signed DDM32 Plus release manifest: format, signature check, and the public firmware gate.

A GitHub release of d0ukan/DDM32-Plus carries ddm32-manifest.json and ddm32-manifest.json.sig. The .sig file is the
Ed25519 signature (RFC 8032, radiotools/ed25519.py) of the manifest file's exact bytes, written as 128 hex digits. Only
the release tool (pc/release/make_release.py) signs, with the maintainer's private key kept outside the repository
(%USERPROFILE%\\.ddm32\\release-signing-key). The app and the installer verify with PUBLIC_KEY_HEX below.

The public firmware gate (what PLUS_REVIEWED is in the lab): a DDM32 Plus application may be installed by this program
only when its raw SHA-256 (rfoundry_dm32.bin) is the application_sha256 of a firmware entry in a manifest whose
signature verifies with PUBLIC_KEY_HEX. The installer (install_plus.py) checks this itself, from the manifest and
signature files beside the package, every time it runs; the app's own check only decides which buttons are enabled.
While PUBLIC_KEY_HEX is empty (no key embedded yet), nothing verifies and nothing can be installed.

Manifest (format ddm32-manifest/1), all fields required unless marked optional:
  format, repo ("d0ukan/DDM32-Plus"), release (the tag), created_utc
  firmware: [ {version, channel ("beta" | "stable"), file, url, sha256, size (the release zip),
               application_sha256, application_size (rfoundry_dm32.bin inside it),
               settings_format, codeplug_format (integers: the storage formats this version writes),
               min_compatible_version (the oldest version that reads what this one saves),
               notes: {tr, en}} ]
  app (optional): {version, file, url, sha256, size, notes (optional)}: the Python program's zip
  app_net (optional): the same fields: the Windows (.NET) program's zip (win/). Each program updates itself only from
                  its own entry. Older programs ignore this key, so adding it keeps every published checker working.
"""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from radiotools import ed25519  # noqa: E402

# The release signing key's public half (64 hex digits), printed by
#   python pc/release/make_release.py --keygen
# (see pc/README.en.md, "Release signing"). Key created by d0ukan on 2026-11-20.
PUBLIC_KEY_HEX = '70fce9e822ac07f0787a991795b27ab7650f35dfc83bcd7ef0ad550fa675b411'

FORMAT = 'ddm32-manifest/1'
REPO = 'd0ukan/DDM32-Plus'
MANIFEST_NAME = 'ddm32-manifest.json'
SIG_NAME = MANIFEST_NAME + '.sig'
CHANNELS = ('beta', 'stable')
URL_PREFIX = f'https://github.com/{REPO}/releases/download/'
RE_HEX64 = re.compile(r'^[0-9a-f]{64}$')
RE_VERSION = re.compile(r'^(\d{1,4})\.(\d{1,4})\.(\d{1,4})(?:-([0-9A-Za-z.-]{1,40}))?$')
RE_FILE = re.compile(r'^[0-9A-Za-z][0-9A-Za-z._-]{0,120}$')
MAX_MANIFEST = 1 << 20


class ManifestError(ValueError):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def public_key(hex_key=None):
    """The 32-byte key to verify with (the embedded one by default); ManifestError when none is set"""
    text = (PUBLIC_KEY_HEX if hex_key is None else hex_key).strip().lower()
    if not text:
        raise ManifestError('No release signing key is embedded in this program yet; nothing can be verified')
    if not RE_HEX64.match(text):
        raise ManifestError('The embedded release public key is malformed')
    return bytes.fromhex(text)


def key_id(key):
    return sha256(key)[:16]


def parse_version(text):
    """A sortable key for 'MAJOR.MINOR.PATCH[-PRERELEASE]' (a prerelease sorts before its release)"""
    m = RE_VERSION.match(str(text))
    if not m:
        raise ManifestError(f'Bad version {text!r}')
    pre = m.group(4)
    if pre is None:
        tail = (1,)
    else:
        tail = (0,) + tuple((0, int(p), '') if p.isdigit() else (1, 0, p) for p in pre.split('.'))
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) + tail


def signature_from_text(text):
    s = text.decode('ascii', 'replace').strip().lower() if isinstance(text, bytes) else str(text).strip().lower()
    if not re.fullmatch(r'[0-9a-f]{128}', s):
        raise ManifestError('The manifest signature file is malformed')
    return bytes.fromhex(s)


def _need(cond, what):
    if not cond:
        raise ManifestError(f'Manifest field invalid: {what}')


def _asset(entry, where):
    _need(isinstance(entry, dict), where)
    _need(isinstance(entry.get('version'), str), f'{where}.version')
    parse_version(entry['version'])
    _need(isinstance(entry.get('file'), str) and RE_FILE.match(entry['file']) and '..' not in entry['file'],
          f'{where}.file')
    url = entry.get('url')
    _need(isinstance(url, str) and url.startswith(URL_PREFIX) and url.endswith('/' + entry['file']) and
          '..' not in url and '?' not in url and '#' not in url, f'{where}.url')
    _need(isinstance(entry.get('sha256'), str) and RE_HEX64.match(entry['sha256']), f'{where}.sha256')
    _need(isinstance(entry.get('size'), int) and 0 < entry['size'] <= 512 << 20, f'{where}.size')


# The map pack of a firmware entry ("ref_pack", firmware 1.1+, docs/mappack-design.md E.2): a separate signed asset
# (the firmware zip keeps its three files, so older programs still install the firmware and ignore this object)
REF_PACK_SCHEMA = 1
REF_PACK_MAX = 65472 + 64				# the map pack slot: payload max + its 64-byte header
RE_REF_FILE = re.compile(r'^ddm32-ref-\d{1,4}\.\d{1,4}\.\d{1,4}(?:-[0-9A-Za-z.-]{1,40})?\.dpk$')
RE_COORD = re.compile(r'^0x[0-9a-f]{8}$')
RE_DATE = re.compile(r'^\d{8}$')


def check_ref_pack(e, where='ref_pack'):
    """A firmware entry's map pack object; True when this program understands it, False when its schema is newer
    (ignored: "update the programmer"); ManifestError when it is malformed"""
    _need(isinstance(e, dict), where)
    schema = e.get('schema')
    _need(isinstance(schema, int) and not isinstance(schema, bool) and schema >= 1, f'{where}.schema')
    if schema > REF_PACK_SCHEMA:
        return False
    _asset(e, where)
    _need(RE_REF_FILE.match(e['file']) is not None, f'{where}.file')
    _need(e['size'] <= REF_PACK_MAX, f'{where}.size')
    for k, want in (('pack_type', 3), ('pack_format', 1)):
        _need(e.get(k) == want and not isinstance(e.get(k), bool), f'{where}.{k}')
    _need(isinstance(e.get('dir_version'), int) and not isinstance(e.get('dir_version'), bool) and
          1 <= e['dir_version'] <= 255, f'{where}.dir_version')
    _need(isinstance(e.get('coord_crc'), str) and RE_COORD.match(e['coord_crc']) is not None, f'{where}.coord_crc')
    _need(isinstance(e.get('date'), str) and RE_DATE.match(e['date']) is not None, f'{where}.date')
    _need(isinstance(e.get('min_firmware'), str), f'{where}.min_firmware')
    parse_version(e['min_firmware'])
    return True


def check_manifest(m):
    """Schema check of a parsed (already signature-verified) manifest; returns it"""
    _need(isinstance(m, dict) and m.get('format') == FORMAT, 'format')
    _need(m.get('repo') == REPO, 'repo')
    _need(isinstance(m.get('release'), str) and RE_FILE.match(m['release']), 'release')
    _need(isinstance(m.get('created_utc'), str), 'created_utc')
    fw = m.get('firmware')
    _need(isinstance(fw, list), 'firmware')
    seen = set()
    for i, e in enumerate(fw):
        where = f'firmware[{i}]'
        _asset(e, where)
        _need(e.get('channel') in CHANNELS, f'{where}.channel')
        _need(isinstance(e.get('application_sha256'), str) and RE_HEX64.match(e['application_sha256']),
              f'{where}.application_sha256')
        _need(isinstance(e.get('application_size'), int) and 0 < e['application_size'] <= 0xE8000 - 0xC000,
              f'{where}.application_size')
        for k in ('settings_format', 'codeplug_format'):
            _need(isinstance(e.get(k), int) and not isinstance(e.get(k), bool) and 0 <= e[k] < 1 << 16,
                  f'{where}.{k}')
        _need(isinstance(e.get('min_compatible_version'), str), f'{where}.min_compatible_version')
        parse_version(e['min_compatible_version'])
        notes = e.get('notes')
        _need(isinstance(notes, dict) and isinstance(notes.get('tr'), str) and isinstance(notes.get('en'), str),
              f'{where}.notes')
        _need(e['version'] not in seen, f'{where}.version (duplicate)')
        seen.add(e['version'])
        if 'ref_pack' in e:
            check_ref_pack(e['ref_pack'], f'{where}.ref_pack')
    for key in ('app', 'app_net'):
        app = m.get(key)
        if app is not None:
            _asset(app, key)
            if 'notes' in app:
                _need(isinstance(app['notes'], dict), f'{key}.notes')
    return m


def verify_bytes(data, sig_text, hex_key=None):
    """The parsed manifest when the signature over data verifies and the schema holds; ManifestError otherwise. The
    signature is checked before the JSON is parsed"""
    key = public_key(hex_key)
    if not isinstance(data, (bytes, bytearray)) or len(data) > MAX_MANIFEST:
        raise ManifestError('Manifest too large or not bytes')
    sig = signature_from_text(sig_text)
    if not ed25519.verify(key, bytes(data), sig):
        raise ManifestError('Manifest signature is not valid')
    try:
        m = json.loads(bytes(data).decode('utf-8'))
    except (UnicodeDecodeError, ValueError):
        raise ManifestError('Signed manifest is not valid JSON') from None
    return check_manifest(m)


def verify_files(folder, hex_key=None):
    """The manifest in folder (ddm32-manifest.json + .sig), verified"""
    folder = Path(folder)
    try:
        data = (folder / MANIFEST_NAME).read_bytes()
        sig = (folder / SIG_NAME).read_bytes()
    except OSError:
        raise ManifestError(f'No signed manifest ({MANIFEST_NAME} and {SIG_NAME}) in {folder}') from None
    return verify_bytes(data, sig, hex_key)


def firmware_for(manifest, application_sha256):
    """The firmware entry of a verified manifest whose application_sha256 is this hash, else None"""
    for e in manifest['firmware']:
        if e['application_sha256'] == application_sha256:
            return e
    return None


def release_gate(application, folder, hex_key=None):
    """The public firmware gate: the manifest beside the package verifies with the embedded key and lists this exact
    application. Returns the firmware entry; ManifestError otherwise"""
    m = verify_files(folder, hex_key)
    entry = firmware_for(m, sha256(application))
    if entry is None or entry['application_size'] != len(application):
        raise ManifestError('This application is not listed in the signed DDM32 Plus release manifest')
    return entry


def sign_bytes(data, secret):
    """The .sig text for data (release tool and tests only)"""
    return ed25519.sign(secret, bytes(data)).hex() + '\n'
