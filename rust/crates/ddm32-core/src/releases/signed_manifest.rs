// SPDX-License-Identifier: GPL-3.0-or-later
//! The signed DDM32 Plus release manifest: signature check and schema check (pc/radiotools/signed_manifest.py,
//! Releases/SignedManifest.cs). The manifest file's exact bytes are signed with Ed25519; the .sig file holds the
//! signature as 128 hex digits. The signature is checked before the JSON is parsed.
//!
//! The JSON checks follow Python's types on purpose (the tests compare with the Python code's results): an integer
//! field takes only an integer literal (5, not 5.0 or 5e0); size fields also take true (Python's bool is an int),
//! the format fields do not. NaN/Infinity literals are refused (the release tool never writes one).

use super::version::ReleaseVersion;
use super::{
    AssetEntry, FirmwareEntry, Manifest, ManifestError, ManifestSchema, Notes, RefPackEntry, APP_START, CHANNELS,
    CODEC_SLOT, CURRENT, MAX_ASSET_SIZE, MAX_MANIFEST, PUBLIC_KEY_HEX, REF_PACK_MAX, REF_PACK_SCHEMA,
};
use crate::crypto::ed25519;
use crate::pytext::py_strip;
use num_bigint::BigInt;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::path::Path;

fn err(s: impl Into<String>) -> ManifestError {
    ManifestError(s.into())
}

/// The text a `^...$` pattern checks: Python's and .NET's `$` also match before one final newline.
fn body(s: &str) -> &str {
    s.strip_suffix('\n').unwrap_or(s)
}

/// `^[0-9a-f]{64}$`
pub fn is_hex64(s: &str) -> bool {
    let s = body(s);
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn is_hex128(s: &str) -> bool {
    s.len() == 128 && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// `^[0-9A-Za-z][0-9A-Za-z._-]{0,120}$`
pub fn is_safe_file_name(s: &str) -> bool {
    let b = body(s).as_bytes();
    !b.is_empty()
        && b.len() <= 121
        && b[0].is_ascii_alphanumeric()
        && b[1..].iter().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'-'))
}

pub fn sha256_hex(data: &[u8]) -> String {
    hex::encode(Sha256::digest(data))
}

pub fn sha256_file(path: &Path) -> std::io::Result<String> {
    let mut f = std::fs::File::open(path)?;
    let mut h = Sha256::new();
    std::io::copy(&mut f, &mut h)?;
    Ok(hex::encode(h.finalize()))
}

/// The 32-byte key to verify with (the embedded one by default).
pub fn public_key(hex_key: Option<&str>) -> Result<Vec<u8>, ManifestError> {
    let text = py_strip(hex_key.unwrap_or(PUBLIC_KEY_HEX)).to_lowercase();
    if text.is_empty() {
        return Err(err("No release signing key is embedded in this program yet; nothing can be verified"));
    }
    if !is_hex64(&text) {
        return Err(err("The embedded release public key is malformed"));
    }
    Ok(hex::decode(text).unwrap())
}

pub fn key_id(key: &[u8]) -> String {
    sha256_hex(key)[..16].to_string()
}

/// The 64 signature bytes from the .sig file's bytes (ASCII hex, surrounding whitespace allowed).
pub fn signature_from_bytes(text: &[u8]) -> Result<Vec<u8>, ManifestError> {
    // Python: text.decode('ascii', 'replace').strip().lower()
    let s: String = text.iter().map(|&b| if b < 0x80 { b as char } else { '\u{FFFD}' }).collect();
    signature_from_text(&s)
}

pub fn signature_from_text(text: &str) -> Result<Vec<u8>, ManifestError> {
    let s = py_strip(text).to_lowercase();
    if !is_hex128(&s) {
        return Err(err("The manifest signature file is malformed"));
    }
    Ok(hex::decode(s).unwrap())
}

/// The .sig text for data (tests and tools only).
pub fn sign_bytes(data: &[u8], secret: &[u8]) -> String {
    hex::encode(ed25519::sign(secret, data)) + "\n"
}

/// JSON as Python's `json.loads` of UTF-8 bytes reads it, as far as this program needs: strict UTF-8, no BOM,
/// nesting at most 1000 deep (the C# program's MaxDepth; deeper input is refused, never a stack overflow).
pub fn parse_json(data: &[u8]) -> Option<Value> {
    use serde::Deserialize;
    let text = std::str::from_utf8(data).ok()?;
    if text.starts_with('\u{FEFF}') || nesting(data) > 1000 {
        return None;
    }
    let mut de = serde_json::Deserializer::from_str(text);
    de.disable_recursion_limit();
    let v = Value::deserialize(&mut de).ok()?;
    de.end().ok()?;
    Some(v)
}

/// The deepest nesting of arrays and objects (outside strings).
fn nesting(data: &[u8]) -> usize {
    let (mut depth, mut max, mut in_str, mut esc) = (0usize, 0usize, false, false);
    for &b in data {
        if in_str {
            if esc {
                esc = false;
            } else if b == b'\\' {
                esc = true;
            } else if b == b'"' {
                in_str = false;
            }
            continue;
        }
        match b {
            b'"' => in_str = true,
            b'[' | b'{' => {
                depth += 1;
                max = max.max(depth);
            }
            b']' | b'}' => depth = depth.saturating_sub(1),
            _ => {}
        }
    }
    max
}

/// The parsed manifest when the signature over data verifies with the key and the schema holds.
pub fn verify_bytes(
    data: &[u8],
    sig_text: &[u8],
    hex_key: Option<&str>,
    schema: Option<&ManifestSchema>,
) -> Result<Manifest, ManifestError> {
    let key = public_key(hex_key)?;
    if data.len() > MAX_MANIFEST {
        return Err(err("Manifest too large or not bytes"));
    }
    let sig = signature_from_bytes(sig_text)?;
    if !ed25519::verify(&key, data, &sig).unwrap_or(false) {
        return Err(err("Manifest signature is not valid"));
    }
    let doc = parse_json(data).ok_or_else(|| err("Signed manifest is not valid JSON"))?;
    check_manifest(&doc, schema.unwrap_or(&CURRENT))
}

/// The manifest in folder (manifest and .sig of the schema), verified.
pub fn verify_files(
    folder: &Path,
    hex_key: Option<&str>,
    schema: Option<&ManifestSchema>,
) -> Result<Manifest, ManifestError> {
    let schema = schema.unwrap_or(&CURRENT);
    let data = std::fs::read(folder.join(schema.manifest_name));
    let sig = std::fs::read(folder.join(schema.sig_name()));
    match (data, sig) {
        (Ok(d), Ok(s)) => verify_bytes(&d, &s, hex_key, Some(schema)),
        _ => Err(err(format!(
            "No signed manifest ({} and {}) in {}",
            schema.manifest_name,
            schema.sig_name(),
            folder.display()
        ))),
    }
}

pub fn firmware_for<'a>(manifest: &'a Manifest, application_sha256: &str) -> Option<&'a FirmwareEntry> {
    manifest.firmware.iter().find(|e| e.application_sha256 == application_sha256)
}

/// The public firmware gate: the manifest beside the package verifies with the key and lists this exact application.
pub fn release_gate(application: &[u8], folder: &Path, hex_key: Option<&str>) -> Result<FirmwareEntry, ManifestError> {
    let m = verify_files(folder, hex_key, None)?;
    match firmware_for(&m, &sha256_hex(application)) {
        Some(e) if e.application_size == application.len() as u64 => Ok(e.clone()),
        _ => Err(err("This application is not listed in the signed DDM32 Plus release manifest")),
    }
}

// --- schema ------------------------------------------------------------------------------------------------------

fn need(cond: bool, what: &str) -> Result<(), ManifestError> {
    if cond {
        Ok(())
    } else {
        Err(err(format!("Manifest field invalid: {what}")))
    }
}

/// `dict.get(name)` (the last member when repeated, like json.loads), or None.
pub(crate) fn get<'a>(obj: &'a Value, name: &str) -> Option<&'a Value> {
    obj.as_object().and_then(|o| o.get(name))
}

fn str_of(v: Option<&Value>) -> Option<&str> {
    v.and_then(Value::as_str)
}

/// The value when Python's json would give an int (bool too when allow_bool), else None.
pub(crate) fn py_int(v: Option<&Value>, allow_bool: bool) -> Option<BigInt> {
    match v? {
        Value::Bool(b) => allow_bool.then(|| BigInt::from(*b as u8)),
        Value::Number(n) => {
            let raw = n.to_string();
            if raw.contains(['.', 'e', 'E']) {
                return None;
            }
            raw.parse().ok()
        }
        _ => None,
    }
}

fn to_u64(b: &BigInt) -> u64 {
    u64::try_from(b).unwrap_or(0)
}

/// Schema check of a parsed (already signature-verified) manifest.
pub fn check_manifest(m: &Value, schema: &ManifestSchema) -> Result<Manifest, ManifestError> {
    need(m.is_object() && str_of(get(m, "format")) == Some(schema.format), "format")?;
    need(str_of(get(m, "repo")) == Some(schema.repo), "repo")?;
    let release = str_of(get(m, "release"));
    need(release.is_some_and(is_safe_file_name), "release")?;
    let created = str_of(get(m, "created_utc"));
    need(created.is_some(), "created_utc")?;
    let fw = get(m, "firmware").and_then(Value::as_array);
    need(fw.is_some(), "firmware")?;
    let mut seen = std::collections::HashSet::new();
    let mut entries = Vec::new();
    for (i, e) in fw.unwrap().iter().enumerate() {
        let w = format!("firmware[{i}]");
        let a = asset(e, &w, schema)?;
        let channel = str_of(get(e, "channel"));
        need(channel.is_some_and(|c| CHANNELS.contains(&c)), &format!("{w}.channel"))?;
        let app_sha = str_of(get(e, "application_sha256"));
        need(app_sha.is_some_and(is_hex64), &format!("{w}.application_sha256"))?;
        let app_size = py_int(get(e, "application_size"), true);
        need(
            app_size.as_ref().is_some_and(|s| *s > BigInt::from(0) && *s <= BigInt::from(CODEC_SLOT - APP_START)),
            &format!("{w}.application_size"),
        )?;
        let mut formats = [0u32; 2];
        for (k, name) in ["settings_format", "codeplug_format"].iter().enumerate() {
            let v = py_int(get(e, name), false);
            need(
                v.as_ref().is_some_and(|v| *v >= BigInt::from(0) && *v < BigInt::from(1 << 16)),
                &format!("{w}.{name}"),
            )?;
            formats[k] = to_u64(v.as_ref().unwrap()) as u32;
        }
        let minc = str_of(get(e, "min_compatible_version"));
        need(minc.is_some(), &format!("{w}.min_compatible_version"))?;
        ReleaseVersion::parse(minc.unwrap())?;
        let notes = get(e, "notes");
        need(
            notes.is_some_and(|n| n.is_object() && str_of(get(n, "tr")).is_some() && str_of(get(n, "en")).is_some()),
            &format!("{w}.notes"),
        )?;
        need(!seen.contains(&a.version), &format!("{w}.version (duplicate)"))?;
        seen.insert(a.version.clone());
        let (ref_pack, ref_pack_newer) = match get(e, "ref_pack") {
            None | Some(Value::Null) => (None, false),
            Some(r) => match check_ref_pack(r, &format!("{w}.ref_pack"), schema)? {
                Some(rp) => {
                    // the pack belongs to this firmware version (signed_manifest.py check_ref_pack)
                    need(rp.version == a.version, &format!("{w}.ref_pack.version"))?;
                    (Some(rp), false)
                }
                None => (None, true),
            },
        };
        let n = notes.unwrap();
        entries.push(FirmwareEntry {
            asset: a,
            channel: channel.unwrap().to_string(),
            application_sha256: app_sha.unwrap().to_string(),
            application_size: to_u64(app_size.as_ref().unwrap()),
            settings_format: formats[0],
            codeplug_format: formats[1],
            min_compatible_version: minc.unwrap().to_string(),
            notes: Notes { tr: str_of(get(n, "tr")).unwrap().into(), en: str_of(get(n, "en")).unwrap().into() },
            ref_pack,
            ref_pack_newer,
        });
    }
    let mut app = None;
    let mut app_net = None;
    for (key, is_net) in [("app", false), ("app_net", true)] {
        let Some(el) = get(m, key) else { continue };
        if el.is_null() {
            continue;
        }
        let a = app_asset(el, key, schema)?;
        if is_net {
            app_net = Some(a);
        } else {
            app = Some(a);
        }
    }
    // this program's builds per platform (the Python and C# checkers ignore the key)
    let mut app_rs = Vec::new();
    if let Some(el) = get(m, "app_rs") {
        if !el.is_null() {
            let obj = el.as_object();
            need(obj.is_some(), "app_rs")?;
            for (platform, v) in obj.unwrap() {
                need(is_safe_file_name(platform), "app_rs")?;
                app_rs.push((platform.clone(), app_asset(v, &format!("app_rs.{platform}"), schema)?));
            }
        }
    }
    Ok(Manifest {
        schema: schema.clone(),
        release: release.unwrap().to_string(),
        created_utc: created.unwrap().to_string(),
        firmware: entries,
        app,
        app_net,
        app_rs,
    })
}

/// A firmware entry's `ref_pack` object (docs/mappack-design.md E.2): Ok(None) for a schema this program does not
/// read yet (ignored, the firmware still installs), a ManifestError for a malformed schema-1 object.
pub fn check_ref_pack(r: &Value, w: &str, schema: &ManifestSchema) -> Result<Option<RefPackEntry>, ManifestError> {
    need(r.is_object(), w)?;
    let sv = py_int(get(r, "schema"), false);
    need(sv.as_ref().is_some_and(|s| *s >= BigInt::from(1)), &format!("{w}.schema"))?;
    if *sv.as_ref().unwrap() > BigInt::from(REF_PACK_SCHEMA) {
        return Ok(None);
    }
    let a = asset(r, w, schema)?;
    need(a.size <= REF_PACK_MAX, &format!("{w}.size"))?;
    // ^ddm32-ref-<version>\.dpk$ (pc/radiotools/signed_manifest.py RE_REF_FILE)
    let ref_file = body(&a.file)
        .strip_prefix("ddm32-ref-")
        .and_then(|f| f.strip_suffix(".dpk"))
        .is_some_and(|v| ReleaseVersion::parse(v).is_ok());
    need(ref_file, &format!("{w}.file"))?;
    let small = |name: &str, lo: u64, hi: u64| -> Result<u64, ManifestError> {
        let v = py_int(get(r, name), false);
        need(v.as_ref().is_some_and(|v| *v >= BigInt::from(lo) && *v <= BigInt::from(hi)), &format!("{w}.{name}"))?;
        Ok(to_u64(v.as_ref().unwrap()))
    };
    let pack_type = small("pack_type", 3, 3)? as u8;
    let pack_format = small("pack_format", 1, 1)? as u8;
    let dir_version = small("dir_version", 1, 255)? as u8;
    let coord = str_of(get(r, "coord_crc"));
    // ^0x[0-9a-f]{8}$
    let coord_crc = coord.and_then(|c| {
        let h = body(c).strip_prefix("0x")?;
        (h.len() == 8 && h.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)))
            .then(|| u32::from_str_radix(h, 16).ok())
            .flatten()
    });
    need(coord_crc.is_some(), &format!("{w}.coord_crc"))?;
    let date = str_of(get(r, "date"));
    need(
        date.is_some_and(|d| body(d).len() == 8 && body(d).bytes().all(|b| b.is_ascii_digit())),
        &format!("{w}.date"),
    )?;
    let minf = str_of(get(r, "min_firmware"));
    need(minf.is_some(), &format!("{w}.min_firmware"))?;
    ReleaseVersion::parse(minf.unwrap())?;
    Ok(Some(RefPackEntry {
        schema: to_u64(sv.as_ref().unwrap()),
        version: a.version,
        file: a.file,
        url: a.url,
        sha256: a.sha256,
        size: a.size,
        pack_type,
        pack_format,
        dir_version,
        coord_crc: coord_crc.unwrap(),
        date: body(date.unwrap()).to_string(),
        min_firmware: minf.unwrap().to_string(),
    }))
}

fn app_asset(el: &Value, key: &str, schema: &ManifestSchema) -> Result<AssetEntry, ManifestError> {
    let mut a = asset(el, key, schema)?;
    if let Some(n) = get(el, "notes") {
        need(n.is_object(), &format!("{key}.notes"))?;
        a.notes = Some(Notes {
            tr: str_of(get(n, "tr")).unwrap_or("").to_string(),
            en: str_of(get(n, "en")).unwrap_or("").to_string(),
        });
    }
    Ok(a)
}

fn asset(entry: &Value, w: &str, schema: &ManifestSchema) -> Result<AssetEntry, ManifestError> {
    need(entry.is_object(), w)?;
    let version = str_of(get(entry, "version"));
    need(version.is_some(), &format!("{w}.version"))?;
    ReleaseVersion::parse(version.unwrap())?;
    let file = str_of(get(entry, "file"));
    need(file.is_some_and(|f| is_safe_file_name(f) && !f.contains("..")), &format!("{w}.file"))?;
    let f = file.unwrap();
    let url = str_of(get(entry, "url"));
    let prefix = schema.url_prefix();
    need(
        url.is_some_and(|u| {
            u.starts_with(&prefix)
                && u.ends_with(&format!("/{f}"))
                && !u.contains("..")
                && !u.contains('?')
                && !u.contains('#')
        }),
        &format!("{w}.url"),
    )?;
    let sha = str_of(get(entry, "sha256"));
    need(sha.is_some_and(is_hex64), &format!("{w}.sha256"))?;
    let size = py_int(get(entry, "size"), true);
    need(
        size.as_ref().is_some_and(|s| *s > BigInt::from(0) && *s <= BigInt::from(MAX_ASSET_SIZE)),
        &format!("{w}.size"),
    )?;
    Ok(AssetEntry {
        version: version.unwrap().to_string(),
        file: f.to_string(),
        url: url.unwrap().to_string(),
        sha256: sha.unwrap().to_string(),
        size: to_u64(size.as_ref().unwrap()),
        notes: None,
    })
}
