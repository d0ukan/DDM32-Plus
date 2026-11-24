#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes python-results.json: what the Python code in pc/ answers for a set of inputs, so the C# port can be tested
against it (win/tests/DDM32Plus.Core.Tests). Run from anywhere:

    python rust/tests/fixtures/make_fixtures.py

Uses only the test signing key of pc/tests/rtfixtures.py (its secret is public, so nothing it signs is a release).
The published legacy manifest (legacy/ddm25-manifest.json and .sig, release v1.0.0-beta.1) is checked with the
Python schema code with its constants switched to the old DDM25 names.
"""
import base64
import json
from pathlib import Path
import sys
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]  # firmware/ (this copy lives in rust/tests/fixtures)
PC = REPO / 'pc'
sys.path.insert(0, str(PC))
sys.path.insert(0, str(PC / 'tests'))

import fixtures as fx  # noqa: E402  (sets DDM32_DATA to a temporary folder)
import rtfixtures as rt  # noqa: E402
from radiotools import ed25519, signed_manifest as sm  # noqa: E402
from ddm32_app import releases  # noqa: E402


def b64(data):
    return base64.b64encode(data).decode('ascii')


def pattern(n, k):
    return bytes((i * k + (i >> 8) + 1) & 255 for i in range(n))


# --- Ed25519 -------------------------------------------------------------------------------------------------------
def ed25519_cases():
    sign = []
    for n in (0, 1, 2, 31, 32, 33, 64, 255, 1000):
        msg = pattern(n, 7)
        for secret in (rt.TEST_SECRET, rt.OTHER_SECRET, bytes(32), b'\xff' * 32):
            sign.append({'secret': secret.hex(), 'public': ed25519.secret_to_public(secret).hex(), 'msg': msg.hex(),
                         'sig': ed25519.sign(secret, msg).hex()})
    verify = []
    pk = ed25519.secret_to_public(rt.TEST_SECRET)
    msg = b'DDM32 Plus manifest'
    sig = ed25519.sign(rt.TEST_SECRET, msg)
    cases = [('good', pk, msg, sig), ('other msg', pk, msg + b'\0', sig), ('short sig', pk, msg, sig[:63]),
             ('long sig', pk, msg, sig + b'\0'), ('zero sig', pk, msg, bytes(64)),
             ('other key', ed25519.secret_to_public(rt.OTHER_SECRET), msg, sig)]
    for i in (0, 1, 31, 32, 33, 63):
        bad = bytearray(sig)
        bad[i] ^= 0x40
        cases.append((f'flip {i}', pk, msg, bytes(bad)))
    s = int.from_bytes(sig[32:], 'little') + ed25519.q
    if s < 1 << 256:
        cases.append(('non-canonical S', pk, msg, sig[:32] + s.to_bytes(32, 'little')))
    cases.append(('R not on curve', pk, msg, (2).to_bytes(32, 'little') + sig[32:]))
    cases.append(('key not on curve', (2).to_bytes(32, 'little'), msg, sig))
    cases.append(('key y >= p', (ed25519.p).to_bytes(32, 'little'), msg, sig))
    for name, k, m, sg in cases:
        verify.append({'name': name, 'public': k.hex(), 'msg': m.hex(), 'sig': sg.hex(),
                       'ok': ed25519.verify(k, m, sg)})
    return {'sign': sign, 'verify': verify}


# --- versions ------------------------------------------------------------------------------------------------------
VERSIONS = ['0.9.0', '1.0.0-beta.1', '1.0.0-beta.2', '1.0.0-beta.10', '1.0.0-rc.1', '1.0.0', '1.0.1', '1.10.0',
            '1.0.0-alpha', '1.0.0-alpha.1', '1.0.0-alpha.beta', '1.0.0-beta', '1.0.0-0', '1.0.0-1a', '1.0.0-A',
            '1.0.0-a', '1.0.0--', '1.0.0-..', '1.0.0-007', '1.0.0-7', '2.0.0-alpha.1', '10.0.0', '0.0.0',
            '9999.9999.9999', '1.0.0-x.1.y', '1.0.0-x.y', '1.0.0-1.x', '1.0.0\n', '1.0.0-beta.1\n',
            '١.0.0', '1.0.0-' + 'a' * 40]
INVALID = ['v1.0.0', '1.0', '1.0.0.0', '12345.0.0', '1.0.0-', '1.0.0-' + 'a' * 41, '1.0.0 ', ' 1.0.0',
           '1.0.0+build', '', '1.0.0\n\n', '1.0.0-béta', '-1.0.0', '1..0']


def version_cases():
    valid = []
    for v in VERSIONS:
        sm.parse_version(v)
        valid.append(v)
    invalid = []
    for v in INVALID:
        try:
            sm.parse_version(v)
            raise SystemExit(f'expected {v!r} to be refused')
        except sm.ManifestError:
            invalid.append(v)
    keys = [sm.parse_version(v) for v in valid]
    cmp = [[(a > b) - (a < b) for b in keys] for a in keys]
    return {'valid': valid, 'invalid': invalid, 'compare': cmp}


# --- manifests -----------------------------------------------------------------------------------------------------
APP = pattern(0x5000, 5)


def entry(**kw):
    return rt.firmware_entry(APP, **kw)


def mutate(m, path, value, delete=False):
    m = json.loads(json.dumps(m))
    target = m
    for p in path[:-1]:
        target = target[p]
    if delete:
        del target[path[-1]]
    else:
        target[path[-1]] = value
    return m


def raw(m):
    return (json.dumps(m, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def manifest_cases():
    base = rt.manifest([entry(), entry(version='1.1.0-beta.1', channel='beta')])
    with_app = rt.manifest([entry()], app=rt.app_entry('1.0.0', b'zip'))
    cases = [('good', raw(base)), ('good with app', raw(with_app)),
             ('good with app null', raw(dict(base, app=None))),
             ('good empty firmware', raw(rt.manifest([]))),
             ('good with app_net', raw(dict(with_app, app_net=rt.app_entry('2.0.0-alpha.1', b'net')))),
             ('app notes not a dict', raw(mutate(with_app, ['app', 'notes'], 'x'))),
             ('app without notes', raw(mutate(with_app, ['app', 'notes'], None, delete=True))),
             ('app bad url', raw(mutate(with_app, ['app', 'url'], 'https://example.com/x.zip'))),
             ('app_net bad url', raw(dict(with_app, app_net=dict(rt.app_entry('2.0.0', b'n'),
                                                                 url='https://example.com/DDM32Plus-2.0.0.zip')))),
             ('app_net notes not a dict', raw(dict(with_app, app_net=dict(rt.app_entry('2.0.0', b'n'), notes=[])))),
             ('app_net bad version', raw(dict(with_app, app_net=dict(rt.app_entry('2.0.0', b'n'), version='2')))),
             ('app_net null', raw(dict(with_app, app_net=None))),
             ('extra keys ignored', raw(dict(base, extra=[1, 2, {'a': None}])))]
    for field, value in (('channel', 'nightly'), ('channel', None), ('url', 'https://example.com/x.zip'),
                         ('url', rt.url('v1.0.0', '../evil.zip')), ('url', rt.url('v1.0.0', 'x.zip?a')),
                         ('url', rt.url('v1.0.0', 'other.zip')), ('file', '../x.zip'), ('file', '.x.zip'),
                         ('file', 'a..zip'), ('sha256', 'ab'), ('sha256', 'A' * 64), ('settings_format', 'one'),
                         ('settings_format', True), ('settings_format', 1.0), ('settings_format', -1),
                         ('settings_format', 65536), ('settings_format', 65535), ('codeplug_format', 0),
                         ('min_compatible_version', 'x'), ('min_compatible_version', 2),
                         ('application_size', 0xE8000), ('application_size', 0xE8000 - 0xC000),
                         ('application_size', 0), ('application_size', True), ('application_size', 1.5),
                         ('size', True), ('size', False), ('size', 512 << 20), ('size', (512 << 20) + 1),
                         ('size', 10 ** 30), ('size', '5'), ('version', '1.0'), ('version', 1),
                         ('notes', {'tr': 'only'}), ('notes', {'tr': 'a', 'en': 2}), ('notes', 'text'),
                         ('application_sha256', 'f' * 63)):
        cases.append((f'firmware.{field}={value!r}', raw(mutate(base, ['firmware', 0, field], value))))
    for field in ('channel', 'notes', 'size', 'url'):
        cases.append((f'firmware.{field} missing', raw(mutate(base, ['firmware', 0, field], None, delete=True))))
    for field, value in (('repo', 'someone/else'), ('format', 'x'), ('format', None), ('release', '../x'),
                         ('release', 'v1.0.0\n'), ('created_utc', 5), ('firmware', {}), ('firmware', None)):
        cases.append((f'{field}={value!r}', raw(mutate(base, [field], value))))
    dup = json.loads(json.dumps(base))
    dup['firmware'].append(dict(dup['firmware'][0]))
    cases.append(('duplicate version', raw(dup)))
    cases.append(('not an object', b'[1, 2]\n'))
    cases.append(('not json', b'{"format": \n'))
    cases.append(('bom', b'\xef\xbb\xbf' + raw(base)))
    cases.append(('bad utf-8', raw(base).replace(b'S\xc3\xbcr\xc3\xbcm', b'S\xc3\xbcr\xfcm')))
    cases.append(('repeated key last wins (good)',
                  raw(base).replace(b'"format": "ddm32-manifest/1"', b'"format": "x", "format": "ddm32-manifest/1"')))
    cases.append(('repeated key last wins (bad)',
                  raw(base).replace(b'"format": "ddm32-manifest/1"', b'"format": "ddm32-manifest/1", "format": "x"')))
    cases.append(('compact json', json.dumps(base, separators=(',', ':')).encode()))
    out = []
    for name, data in cases:
        sig = sm.sign_bytes(data, rt.TEST_SECRET)
        out.append(run_case(name, data, sig, rt.TEST_PUBLIC_HEX))
    good = raw(base)
    sig = sm.sign_bytes(good, rt.TEST_SECRET)
    for name, s in (('sig other key', sm.sign_bytes(good, rt.OTHER_SECRET)), ('sig upper case', sig.upper()),
                    ('sig spaces', '  \t' + sig.strip() + '\r\n\x1c'), ('sig truncated', sig.strip()[:-2]),
                    ('sig not hex', 'zz' * 64), ('sig empty', ''), ('sig with newline inside', sig[:64] + '\n' + sig[64:]),
                    ('sig flipped', ('0' if sig[0] != '0' else '1') + sig[1:])):
        out.append(run_case(name, good, s, rt.TEST_PUBLIC_HEX))
    out.append(run_case('changed after signing', good.replace(b'"stable"', b'"beta"  '), sig, rt.TEST_PUBLIC_HEX))
    out.append(run_case('appended byte', good + b' ', sig, rt.TEST_PUBLIC_HEX))
    out.append(run_case('no key', good, sig, ''))
    out.append(run_case('malformed key', good, sig, 'abc'))
    out.append(run_case('key with spaces and upper case', good, sig, '  ' + rt.TEST_PUBLIC_HEX.upper() + '\n'))
    out.append(run_case('release key (signed with test key)', good, sig, sm.PUBLIC_KEY_HEX))
    return out


def run_case(name, data, sig, key, schema='current'):
    sig_bytes = sig.encode('utf-8') if isinstance(sig, str) else sig
    case = {'name': name, 'data': b64(data), 'sig': b64(sig_bytes), 'key': key, 'schema': schema}
    try:
        m = sm.verify_bytes(data, sig_bytes, key)
        case.update(ok=True, release=m['release'], versions=[e['version'] for e in m['firmware']],
                    app=(m.get('app') or {}).get('version'))
    except sm.ManifestError as exc:
        case.update(ok=False, error=str(exc))
    return case


LEGACY = {'FORMAT': 'ddm25-manifest/1', 'REPO': 'd0ukan/DDM25-Plus',
          'URL_PREFIX': 'https://github.com/d0ukan/DDM25-Plus/releases/download/'}


def legacy_cases():
    data = (HERE / 'legacy' / 'ddm25-manifest.json').read_bytes()
    sig = (HERE / 'legacy' / 'ddm25-manifest.json.sig').read_bytes()
    out = [run_case('published v1.0.0-beta.1 with the current names', data, sig, None)]
    with patch.multiple(sm, **LEGACY):
        out.append(run_case('published v1.0.0-beta.1', data, sig, None, 'legacy'))
        out.append(run_case('published v1.0.0-beta.1 changed', data.replace(b'beta.1', b'beta.2'), sig, None,
                            'legacy'))
    for c in out:
        c['key'] = None
    return out


# --- the release list ----------------------------------------------------------------------------------------------
def catalog_case():
    sm.PUBLIC_KEY_HEX = rt.TEST_PUBLIC_HEX
    files, listing = {}, []

    def add(tag, data, sig, names=('ddm32-manifest.json', 'ddm32-manifest.json.sig'), urls=None, **extra):
        sig = sig.encode() if isinstance(sig, str) else sig
        urls = urls or [rt.url(tag, n) for n in names]
        files[urls[0]] = data
        files[urls[1]] = sig
        listing.append(dict({'tag_name': tag, 'assets': [{'name': names[0], 'browser_download_url': urls[0]},
                                                          {'name': names[1], 'browser_download_url': urls[1]}]},
                            **extra))

    m = rt.manifest([entry(version='1.2.0', release='v1.2.0'), entry(version='1.2.1-beta.1', channel='beta',
                                                                       release='v1.2.0')], release='v1.2.0')
    d, s = rt.signed(m)
    add('v1.2.0', d, s)
    other = rt.manifest([entry(version='1.3.0')], release='v1.3.0')
    d, s = rt.signed(other, rt.OTHER_SECRET)
    add('v1.3.0', d, s)                                          # wrong key
    wrong_tag = rt.manifest([entry(version='1.4.0')], release='v9.9.9')
    d, s = rt.signed(wrong_tag)
    add('v1.4.0', d, s)                                          # signed, but for another release
    d, s = rt.signed(m)
    add('v1.5.0', d.replace(b'1.2.0', b'1.5.0'), s)              # changed after signing
    d, s = rt.signed(rt.manifest([entry(version='1.6.0')], release='v1.6.0'))
    add('v1.6.0', d, s, urls=['https://github.com/someone/else/releases/download/v1.6.0/ddm32-manifest.json',
                               rt.url('v1.6.0', 'ddm32-manifest.json.sig')])
    d, s = rt.signed(rt.manifest([entry(version='1.7.0')], release='v1.7.0'))
    add('v1.7.0', d, s, draft=True)                              # drafts are skipped
    listing.append({'tag_name': 'v0.1.0', 'assets': [{'name': 'readme.txt', 'browser_download_url': 'x'}]})
    listing.append('not a release')
    d, s = rt.signed(rt.manifest([entry(version='1.2.0', release='v1.8.0'), entry(version='1.8.0', release='v1.8.0')],
                                 release='v1.8.0'))
    add('v1.8.0', d, s)                                          # 1.2.0 again: the first one listed stays
    files[releases.API_URL] = json.dumps(listing).encode()
    cat = releases.load_catalog(fx.DATA_DIR, lambda url, limit=None: files[url])
    return {'files': {u: b64(v) for u, v in files.items()}, 'key': rt.TEST_PUBLIC_HEX,
            'firmware': [[f.version, f.release, f.channel] for f in cat.firmware],
            'rejected': [[t, r] for t, r in cat.rejected]}


def main():
    out = {'note': 'Generated by make_fixtures.py from the Python code in pc/; do not edit by hand.',
           'test_public_key': rt.TEST_PUBLIC_HEX, 'test_secret': rt.TEST_SECRET.hex(),
           'other_secret': rt.OTHER_SECRET.hex(), 'release_public_key': sm.PUBLIC_KEY_HEX,
           'release_key_id': sm.key_id(sm.public_key()),
           'constants': {'format': sm.FORMAT, 'repo': sm.REPO, 'manifest_name': sm.MANIFEST_NAME,
                         'sig_name': sm.SIG_NAME, 'url_prefix': sm.URL_PREFIX, 'api_url': releases.API_URL,
                         'allowed_hosts': list(releases.ALLOWED_HOSTS), 'package_files': list(releases.PACKAGE_FILES)},
           'ed25519': ed25519_cases(), 'versions': version_cases(), 'manifests': manifest_cases(),
           'legacy': legacy_cases(), 'catalog': catalog_case()}
    path = HERE / 'python-results.json'
    path.write_bytes((json.dumps(out, indent=1, ensure_ascii=True) + '\n').encode('ascii'))
    print(f'wrote {path} ({path.stat().st_size} bytes)')


if __name__ == '__main__':
    main()
