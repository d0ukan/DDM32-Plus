// SPDX-License-Identifier: GPL-3.0-or-later
//! The signed releases on GitHub (pc/ddm32_app/releases.py, Releases/ReleaseClient.cs): the list, the verified
//! cache, firmware downloads with hash checks, the program download. Network access only through [`ReleaseNet`].

use super::catalog::{Catalog, FirmwareVersion};
use super::net::{NetError, Progress, ReleaseNet};
use super::signed_manifest::{self as sm, get, sha256_file, sha256_hex};
use super::version::ReleaseVersion;
use super::{
    AssetEntry, FirmwareEntry, ManifestError, ManifestSchema, API_URL, CURRENT, MANIFEST_CACHE, MAX_API, MAX_MANIFEST,
    MAX_PACKAGE_MEMBER, MAX_SIG, PACKAGE_FILES, SCHEMAS,
};
use serde_json::Value;
use std::fmt;
use std::io::Read;
use std::path::{Path, PathBuf};

/// Why a download was refused (the UI shows a translated text for the kind).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VerifyFailure {
    HashMismatch,
    UnexpectedContents,
    MemberTooLarge,
    ApplicationMismatch,
    PackageNotInstallable,
    NotInstallable,
}

/// A download that does not match the signed manifest (releases.VerifyError), or a network failure.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VerifyError {
    Verify(VerifyFailure, String),
    Net(NetError),
    Io(String),
}

impl VerifyError {
    pub fn kind(&self) -> Option<VerifyFailure> {
        match self {
            VerifyError::Verify(k, _) => Some(*k),
            _ => None,
        }
    }
}

impl fmt::Display for VerifyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            VerifyError::Verify(_, m) | VerifyError::Io(m) => f.write_str(m),
            VerifyError::Net(n) => n.fmt(f),
        }
    }
}
impl std::error::Error for VerifyError {}

impl From<NetError> for VerifyError {
    fn from(e: NetError) -> Self {
        VerifyError::Net(e)
    }
}
impl From<std::io::Error> for VerifyError {
    fn from(e: std::io::Error) -> Self {
        VerifyError::Io(format!("IOException: {e}"))
    }
}

fn verr(kind: VerifyFailure, msg: &str) -> VerifyError {
    VerifyError::Verify(kind, msg.into())
}

/// A downloaded release folder (releases.Package): rfoundry_dm32.bin/.json/.map with the signed manifest that lists
/// the application. Installable only when the gate passes and all three files are there.
#[derive(Debug, Clone)]
pub struct Package {
    pub folder: PathBuf,
    pub problems: Vec<String>,
    pub entry: Option<FirmwareEntry>,
    pub sha256: String,
    pub size: u64,
    pub version: String,
}

fn io_name(e: &std::io::Error) -> &'static str {
    match e.kind() {
        std::io::ErrorKind::NotFound => "FileNotFoundException",
        std::io::ErrorKind::PermissionDenied => "UnauthorizedAccessException",
        _ => "IOException",
    }
}

impl Package {
    pub fn new(folder: &Path, key: Option<&str>) -> Package {
        let mut p = Package {
            folder: folder.to_path_buf(),
            problems: vec![],
            entry: None,
            sha256: String::new(),
            size: 0,
            version: String::new(),
        };
        match std::fs::read(p.image()) {
            Err(e) => p.problems.push(format!("unreadable ({})", io_name(&e))),
            Ok(app) => {
                p.sha256 = sha256_hex(&app);
                p.size = app.len() as u64;
                match sm::release_gate(&app, folder, key) {
                    Ok(e) => {
                        p.entry = Some(e);
                        for name in PACKAGE_FILES {
                            if !folder.join(name).is_file() {
                                p.problems.push(format!("missing {name}"));
                            }
                        }
                    }
                    Err(ManifestError(m)) => p.problems.push(format!("signature check failed: {m}")),
                }
            }
        }
        p.version = match &p.entry {
            Some(e) => e.version().to_string(),
            None => folder.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(),
        };
        p
    }

    pub fn image(&self) -> PathBuf {
        self.folder.join("rfoundry_dm32.bin")
    }

    pub fn installable(&self) -> bool {
        self.entry.is_some() && self.problems.is_empty()
    }

    pub fn channel(&self) -> &str {
        self.entry.as_ref().map_or("", |e| e.channel.as_str())
    }
}

fn cache_dir(releases_root: &Path) -> PathBuf {
    releases_root.join(MANIFEST_CACHE)
}

fn store(cache: &Path, tag: &str, schema: &ManifestSchema, data: &[u8], sig: &[u8]) -> std::io::Result<PathBuf> {
    let d = cache.join(tag);
    std::fs::create_dir_all(&d)?;
    std::fs::write(d.join(schema.manifest_name), data)?;
    std::fs::write(d.join(schema.sig_name()), sig)?;
    Ok(d)
}

pub fn now_utc() -> String {
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs()) as i64;
    format_utc(secs)
}

/// `yyyy-MM-ddTHH:mm:ssZ` of Unix seconds.
pub fn format_utc(secs: i64) -> String {
    let days = secs.div_euclid(86400);
    let rem = secs.rem_euclid(86400);
    let (y, m, d) = civil_from_days(days);
    format!("{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z", rem / 3600, rem % 3600 / 60, rem % 60)
}

/// (year, month, day) of days since 1970-01-01 (Howard Hinnant's algorithm).
pub fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

fn py_truthy(v: Option<&Value>) -> bool {
    match v {
        None | Some(Value::Null) | Some(Value::Bool(false)) => false,
        Some(Value::Bool(true)) => true,
        Some(Value::String(s)) => !s.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Object(o)) => !o.is_empty(),
        Some(Value::Number(n)) => n.as_f64().is_some_and(|f| f != 0.0),
    }
}

/// Python's str() of a JSON value, for the tag name.
fn py_str(v: Option<&Value>) -> Option<String> {
    Some(match v? {
        Value::String(s) => s.clone(),
        Value::Null => "None".into(),
        Value::Bool(true) => "True".into(),
        Value::Bool(false) => "False".into(),
        other => other.to_string(),
    })
}

fn not_a_list() -> NetError {
    NetError("GitHub answered with something that is not a release list".into())
}

fn assets_of(assets: Option<&Value>) -> Result<Vec<(String, Value)>, NetError> {
    let mut map: Vec<(String, Value)> = Vec::new();
    let Some(a) = assets else { return Ok(map) };
    match a {
        Value::Array(items) => {
            for item in items {
                if let Some(Value::String(n)) = get(item, "name") {
                    // the last one wins, like a dict comprehension
                    map.retain(|(k, _)| k != n);
                    map.push((n.clone(), item.clone()));
                }
            }
            Ok(map)
        }
        // Python iterates a string or an object without finding assets, and fails on null / numbers
        Value::String(_) | Value::Object(_) => Ok(map),
        _ => Err(not_a_list()),
    }
}

/// The signed releases on GitHub: one API call, then each release's manifest and signature. Verified manifests are
/// kept under releases_root/manifests/TAG/ for offline use. A release with the old DDM25 names is listed as legacy.
pub fn load_catalog(
    releases_root: &Path,
    net: &dyn ReleaseNet,
    key: Option<&str>,
    now: Option<&dyn Fn() -> String>,
) -> Result<Catalog, NetError> {
    let mut cat = Catalog::new("github");
    cat.checked_utc = Some(now.map_or_else(now_utc, |f| f()));
    let body = net.fetch(API_URL, MAX_API)?;
    let listing = sm::parse_json(&body).ok_or_else(not_a_list)?;
    let Value::Array(releases) = listing else { return Err(not_a_list()) };
    let cache = cache_dir(releases_root);
    for r in &releases {
        if !r.is_object() || py_truthy(get(r, "draft")) {
            continue;
        }
        let tag = py_str(get(r, "tag_name")).unwrap_or_default();
        let assets = assets_of(get(r, "assets"))?;
        let has = |n: &str| assets.iter().any(|(k, _)| k == n);
        let Some(schema) = SCHEMAS.iter().find(|s| has(s.manifest_name) && has(&s.sig_name())) else {
            cat.rejected.push((tag, "no signed manifest".into()));
            continue;
        };
        let result = (|| -> Result<(Vec<u8>, Vec<u8>, super::Manifest), String> {
            let urls: Vec<String> = [schema.manifest_name.to_string(), schema.sig_name()]
                .iter()
                .map(|n| {
                    let a = &assets.iter().find(|(k, _)| k == n).unwrap().1;
                    match get(a, "browser_download_url") {
                        Some(Value::String(u)) => u.clone(),
                        _ => String::new(),
                    }
                })
                .collect();
            // The asset URLs come from this repository's API (renamed from DDM25-Plus): always the current name
            let prefix = format!("{}{}/", CURRENT.url_prefix(), tag);
            if urls.iter().any(|u| !u.starts_with(&prefix)) {
                return Err("manifest asset outside this release".into());
            }
            let data = net.fetch(&urls[0], MAX_MANIFEST).map_err(|e| e.0)?;
            let sig = net.fetch(&urls[1], MAX_SIG).map_err(|e| e.0)?;
            let m = sm::verify_bytes(&data, &sig, key, Some(schema)).map_err(|e| e.0)?;
            if m.release != tag {
                return Err(format!("manifest is for release {}, not {}", m.release, tag));
            }
            Ok((data, sig, m))
        })();
        match result {
            Err(reason) => cat.rejected.push((tag, reason)),
            Ok((data, sig, m)) => {
                let dir =
                    store(&cache, &tag, schema, &data, &sig).map_err(|e| NetError(format!("IOException: {e}")))?;
                cat.add(&tag, &m, Some(dir));
            }
        }
    }
    cat.sort();
    Ok(cat)
}

/// The catalog from the manifests verified earlier (each verified again now): for offline use.
pub fn cached_catalog(releases_root: &Path, key: Option<&str>) -> Catalog {
    let mut cat = Catalog::new("cache");
    let cache = cache_dir(releases_root);
    let mut dirs: Vec<PathBuf> = std::fs::read_dir(&cache)
        .map(|rd| rd.filter_map(Result::ok).map(|e| e.path()).filter(|p| p.is_dir()).collect())
        .unwrap_or_default();
    dirs.sort_by_key(|d| d.file_name().map(|n| n.to_string_lossy().to_lowercase()).unwrap_or_default());
    for d in dirs {
        let name = d.file_name().unwrap().to_string_lossy().into_owned();
        let schema = SCHEMAS.iter().find(|s| d.join(s.manifest_name).is_file()).copied().unwrap_or(&CURRENT);
        match sm::verify_files(&d, key, Some(schema)) {
            Ok(m) if m.release == name => cat.add(&name, &m, Some(d.clone())),
            Ok(_) => cat.rejected.push((name, "release mismatch".into())),
            Err(e) => cat.rejected.push((name, e.0)),
        }
    }
    cat.sort();
    cat
}

/// Downloaded release folders, newest version first.
pub fn list_packages(releases_root: &Path, key: Option<&str>) -> Vec<Package> {
    let mut out: Vec<Package> = std::fs::read_dir(releases_root)
        .map(|rd| {
            rd.filter_map(Result::ok)
                .map(|e| e.path())
                .filter(|d| d.is_dir() && d.join("rfoundry_dm32.bin").is_file())
                .map(|d| Package::new(&d, key))
                .collect()
        })
        .unwrap_or_default();
    // Python: key (1, version) or (0, ()) when the name is not a version; reverse=True, stable
    out.sort_by(|a, b| {
        let va = ReleaseVersion::try_parse(&a.version);
        let vb = ReleaseVersion::try_parse(&b.version);
        let o = match (va, vb) {
            (Some(x), Some(y)) => x.cmp(&y),
            (x, y) => (x.is_some() as u8).cmp(&(y.is_some() as u8)),
        };
        o.reverse()
    });
    out
}

pub fn package_folder(releases_root: &Path, version: &str) -> PathBuf {
    releases_root.join(version)
}

fn read_bounded(r: &mut dyn Read, limit: u64) -> Result<Vec<u8>, VerifyError> {
    let mut out = Vec::new();
    let mut block = vec![0u8; 1 << 16];
    loop {
        let n = r
            .read(&mut block)
            .map_err(|_| verr(VerifyFailure::UnexpectedContents, "The release package is damaged"))?;
        if n == 0 {
            return Ok(out);
        }
        out.extend_from_slice(&block[..n]);
        if out.len() as u64 > limit {
            return Err(verr(VerifyFailure::MemberTooLarge, "A file in the release package is too large"));
        }
    }
}

/// The verified release folder for fv (downloaded when not there yet); a failed download leaves nothing behind.
pub fn download_firmware(
    fv: &FirmwareVersion,
    releases_root: &Path,
    net: &dyn ReleaseNet,
    progress: Progress<'_>,
    key: Option<&str>,
) -> Result<PathBuf, VerifyError> {
    if fv.is_legacy() {
        return Err(verr(
            VerifyFailure::NotInstallable,
            "This release uses the old DDM25 names; it cannot be installed by this program",
        ));
    }
    let folder = package_folder(releases_root, fv.version());
    if folder.is_dir() {
        let p = Package::new(&folder, key);
        if p.installable() && p.sha256 == fv.application_sha256() {
            return Ok(folder);
        }
        std::fs::remove_dir_all(&folder)?;
    }
    let tmp = releases_root.join(format!("{}.partial", fv.version()));
    if tmp.is_dir() {
        std::fs::remove_dir_all(&tmp)?;
    }
    std::fs::create_dir_all(&tmp)?;
    let result = download_into(fv, &tmp, &folder, net, progress, key);
    if result.is_err() {
        let _ = std::fs::remove_dir_all(&tmp);
    }
    result
}

fn download_into(
    fv: &FirmwareVersion,
    tmp: &Path,
    folder: &Path,
    net: &dyn ReleaseNet,
    progress: Progress<'_>,
    key: Option<&str>,
) -> Result<PathBuf, VerifyError> {
    let entry = &fv.entry.asset;
    let zpath = tmp.join(&entry.file);
    net.download(&entry.url, &zpath, entry.size, progress)?;
    if std::fs::metadata(&zpath)?.len() != entry.size || sha256_file(&zpath)? != entry.sha256 {
        return Err(verr(VerifyFailure::HashMismatch, "The downloaded file's SHA-256 does not match the signed list"));
    }
    {
        let unexpected =
            || verr(VerifyFailure::UnexpectedContents, "The release package does not hold the three expected files");
        let mut z = zip::ZipArchive::new(std::fs::File::open(&zpath)?).map_err(|_| unexpected())?;
        let mut names: Vec<String> = z.file_names().map(str::to_string).collect();
        names.sort();
        let mut want: Vec<&str> = PACKAGE_FILES.to_vec();
        want.sort();
        if names.len() != z.len() || names != want {
            return Err(unexpected());
        }
        for n in PACKAGE_FILES {
            let mut ze = z.by_name(n).map_err(|_| unexpected())?;
            if ze.size() > MAX_PACKAGE_MEMBER {
                return Err(verr(VerifyFailure::MemberTooLarge, "A file in the release package is too large"));
            }
            let data = read_bounded(&mut ze, MAX_PACKAGE_MEMBER)?;
            std::fs::write(tmp.join(n), data)?;
        }
    }
    std::fs::remove_file(&zpath)?;
    let app = std::fs::read(tmp.join("rfoundry_dm32.bin"))?;
    if sha256_hex(&app) != fv.application_sha256() || app.len() as u64 != fv.application_size() {
        return Err(verr(
            VerifyFailure::ApplicationMismatch,
            "The application in the package does not match the signed list",
        ));
    }
    let src =
        fv.manifest_dir.as_ref().ok_or_else(|| verr(VerifyFailure::PackageNotInstallable, "No manifest folder"))?;
    for n in [fv.schema.manifest_name.to_string(), fv.schema.sig_name()] {
        std::fs::copy(src.join(&n), tmp.join(&n))?;
    }
    let pkg = Package::new(tmp, key);
    if !pkg.installable() {
        return Err(VerifyError::Verify(VerifyFailure::PackageNotInstallable, pkg.problems.join(", ")));
    }
    std::fs::rename(tmp, folder)?;
    Ok(folder.to_path_buf())
}

/// The program zip of a signed manifest's app entry, verified (size and SHA-256).
pub fn download_app(
    entry: &AssetEntry,
    updates_dir: &Path,
    net: &dyn ReleaseNet,
    progress: Progress<'_>,
) -> Result<PathBuf, VerifyError> {
    std::fs::create_dir_all(updates_dir)?;
    let dest = updates_dir.join(&entry.file);
    if dest.is_file() {
        std::fs::remove_file(&dest)?;
    }
    net.download(&entry.url, &dest, entry.size, progress)?;
    if std::fs::metadata(&dest)?.len() != entry.size || sha256_file(&dest)? != entry.sha256 {
        let _ = std::fs::remove_file(&dest);
        return Err(verr(
            VerifyFailure::HashMismatch,
            "The downloaded program's SHA-256 does not match the signed list",
        ));
    }
    Ok(dest)
}

/// The folder the map packs of the releases are kept in (beside the firmware folders; never a firmware folder).
pub const REF_PACKS: &str = "refpacks";

/// The map pack of fv's release (its `ref_pack`), verified: the size and SHA-256 the signed manifest lists, a whole
/// type-3 pack, and the firmware's structural check with the release's coordinate id. A failed download or check
/// leaves nothing behind. Kept as releases_root/refpacks/VERSION/FILE (downloaded again only when it does not check).
pub fn download_ref_pack(
    fv: &FirmwareVersion,
    releases_root: &Path,
    net: &dyn ReleaseNet,
    progress: Progress<'_>,
) -> Result<PathBuf, VerifyError> {
    let rp = fv
        .entry
        .ref_pack
        .as_ref()
        .ok_or_else(|| verr(VerifyFailure::NotInstallable, "This release has no map pack"))?;
    if fv.is_legacy() {
        return Err(verr(VerifyFailure::NotInstallable, "This release uses the old DDM25 names"));
    }
    let dir = releases_root.join(REF_PACKS).join(fv.version());
    std::fs::create_dir_all(&dir)?;
    let dest = dir.join(&rp.file);
    if dest.is_file() && check_ref_file(&dest, rp).is_ok() {
        return Ok(dest);
    }
    if dest.is_file() {
        std::fs::remove_file(&dest)?;
    }
    let r = net
        .download(&rp.url, &dest, rp.size, progress)
        .map_err(VerifyError::from)
        .and_then(|_| check_ref_file(&dest, rp));
    if r.is_err() {
        let _ = std::fs::remove_file(&dest);
    }
    r.map(|_| dest)
}

/// A downloaded map pack against its manifest entry.
pub fn check_ref_file(path: &Path, rp: &super::RefPackEntry) -> Result<(), VerifyError> {
    use crate::datapacks::{datapack as dp, refpack};
    let data = std::fs::read(path)?;
    if data.len() as u64 != rp.size || sha256_hex(&data) != rp.sha256 {
        return Err(verr(
            VerifyFailure::HashMismatch,
            "The downloaded map pack's SHA-256 does not match the signed list",
        ));
    }
    let bad = || verr(VerifyFailure::UnexpectedContents, "The downloaded map pack does not check");
    let (h, payload) = dp::parse(&data);
    let h = h.ok_or_else(bad)?;
    if !h.crc_ok
        || h.ptype != dp::REF
        || h.format != rp.pack_format
        || data.len() != dp::HEADER_SIZE + h.length as usize
    {
        return Err(bad());
    }
    let rules = refpack::Rules { dir_version: rp.dir_version, coord: Some(rp.coord_crc) };
    if !refpack::validate(&payload, h.flags, &rules).is_valid() {
        return Err(bad());
    }
    Ok(())
}
