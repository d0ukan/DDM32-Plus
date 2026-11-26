// SPDX-License-Identifier: GPL-3.0-or-later
//! Signed releases: the manifest format, its Ed25519 check, version order and labels, the release list and the
//! downloads with hash checks (pc/radiotools/signed_manifest.py, pc/ddm32_app/releases.py, the C# Releases/).

pub mod catalog;
pub mod client;
pub mod local;
pub mod net;
pub mod signed_manifest;
pub mod version;

pub use catalog::{AppRelease, Catalog, FirmwareVersion, ReleaseLabel};
pub use client::{Package, VerifyError, VerifyFailure};
pub use net::{NetError, ReleaseNet};
pub use version::ReleaseVersion;

use std::fmt;

/// A refused manifest, signature or version (signed_manifest.ManifestError).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ManifestError(pub String);

impl fmt::Display for ManifestError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for ManifestError {}

/// The names a signed release manifest uses: [`CURRENT`] since the rename to DDM32 Plus, [`LEGACY`] for the first
/// release (v1.0.0-beta.1, still named DDM25 Plus): listed and verified, never offered for install or as an update.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ManifestSchema {
    pub format: &'static str,
    pub repo: &'static str,
    pub manifest_name: &'static str,
    pub is_legacy: bool,
}

impl ManifestSchema {
    pub fn sig_name(&self) -> String {
        format!("{}.sig", self.manifest_name)
    }
    pub fn url_prefix(&self) -> String {
        format!("https://github.com/{}/releases/download/", self.repo)
    }
}

pub const CURRENT: ManifestSchema = ManifestSchema {
    format: "ddm32-manifest/1",
    repo: "d0ukan/DDM32-Plus",
    manifest_name: "ddm32-manifest.json",
    is_legacy: false,
};
pub const LEGACY: ManifestSchema = ManifestSchema {
    format: "ddm25-manifest/1",
    repo: "d0ukan/DDM25-Plus",
    manifest_name: "ddm25-manifest.json",
    is_legacy: true,
};
pub const SCHEMAS: [&ManifestSchema; 2] = [&CURRENT, &LEGACY];

/// The release signing key's public half (pc/radiotools/signed_manifest.py PUBLIC_KEY_HEX).
pub const PUBLIC_KEY_HEX: &str = "70fce9e822ac07f0787a991795b27ab7650f35dfc83bcd7ef0ad550fa675b411";
/// The first 16 hex digits of SHA-256(public key): shown to the user.
pub const PUBLIC_KEY_ID: &str = "dda8c4a85c4600df";

pub const API_URL: &str = "https://api.github.com/repos/d0ukan/DDM32-Plus/releases?per_page=100";
pub const ALLOWED_HOSTS: [&str; 4] =
    ["api.github.com", "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"];
pub const CHANNELS: [&str; 2] = ["beta", "stable"];
pub const PACKAGE_FILES: [&str; 3] = ["rfoundry_dm32.bin", "rfoundry_dm32.json", "rfoundry_dm32.map"];

pub const MAX_API: usize = 8 << 20;
pub const MAX_MANIFEST: usize = 1 << 20;
pub const MAX_SIG: usize = 1024;
pub const MAX_ASSET_SIZE: u64 = 512 << 20;
pub const MAX_PACKAGE_MEMBER: u64 = 64 << 20;
pub const APP_START: u64 = 0xC000;
pub const CODEC_SLOT: u64 = 0xE8000;
pub const MANIFEST_CACHE: &str = "manifests";

/// Release notes in both languages.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Notes {
    pub tr: String,
    pub en: String,
}

impl Notes {
    pub fn for_lang(&self, lang: &str) -> &str {
        if lang == "en" {
            if !self.en.is_empty() {
                &self.en
            } else {
                &self.tr
            }
        } else if !self.tr.is_empty() {
            &self.tr
        } else {
            &self.en
        }
    }
}

/// A downloadable asset of a signed manifest (firmware zip or program zip).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AssetEntry {
    pub version: String,
    pub file: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    pub notes: Option<Notes>,
}

/// A firmware entry of a signed manifest (all fields checked by `check_manifest`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FirmwareEntry {
    pub asset: AssetEntry,
    pub channel: String,
    pub application_sha256: String,
    pub application_size: u64,
    pub settings_format: u32,
    pub codeplug_format: u32,
    pub min_compatible_version: String,
    pub notes: Notes,
    /// The map pack that belongs to this firmware (docs/mappack-design.md E.2), schema 1.
    pub ref_pack: Option<RefPackEntry>,
    /// The entry carries a `ref_pack` of a newer schema than this program reads: ignored, "update the programmer".
    pub ref_pack_newer: bool,
}

/// The highest `ref_pack` schema this program reads.
pub const REF_PACK_SCHEMA: u64 = 1;
/// A map pack asset is at most one map slot (64 KB: header + payload).
pub const REF_PACK_MAX: u64 = 0x10000;

/// A firmware entry's `ref_pack` object: a separate signed asset (the zip keeps its three files). The manifest's
/// signature covers its SHA-256.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RefPackEntry {
    pub schema: u64,
    pub version: String,
    pub file: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    pub pack_type: u8,
    pub pack_format: u8,
    pub dir_version: u8,
    pub coord_crc: u32,
    pub date: String,
    /// The oldest firmware that uses the pack (N3); writing it to an older one is allowed and harmless.
    pub min_firmware: String,
}

impl FirmwareEntry {
    pub fn version(&self) -> &str {
        &self.asset.version
    }
}

/// The program builds of this (Rust) program in a manifest: `app_rs`, one asset per platform key
/// (`windows-x86_64`, `linux-x86_64`, `macos-aarch64`, ...). A key the Python and C# checkers ignore.
pub type AppRs = Vec<(String, AssetEntry)>;

/// A verified manifest. `app` is the Python program's zip, `app_net` the C# program's zip, `app_rs` this program's
/// builds per platform: no program updates itself to another one.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Manifest {
    pub schema: ManifestSchema,
    pub release: String,
    pub created_utc: String,
    pub firmware: Vec<FirmwareEntry>,
    pub app: Option<AssetEntry>,
    pub app_net: Option<AssetEntry>,
    pub app_rs: AppRs,
}

/// This build's platform key in `app_rs`.
pub fn platform_key() -> String {
    let os = if cfg!(windows) {
        "windows"
    } else if cfg!(target_os = "macos") {
        "macos"
    } else {
        "linux"
    };
    format!("{os}-{}", std::env::consts::ARCH)
}
