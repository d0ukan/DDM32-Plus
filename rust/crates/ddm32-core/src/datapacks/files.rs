// SPDX-License-Identifier: GPL-3.0-or-later
//! Building packs from the user's files and reading what a verified backup holds (files only, no port), and the
//! RadioID.net list download (DataPacks/PackFiles.cs).

use super::datapack::{self as dp, PackHeader, AIR, HEADER_SIZE, RADIO_ID, REF, REGION_END};
use super::radioid::{RadioIdSkipped, DOWNLOAD_URL};
use super::refpack;
use super::{air, pdf, PackError};
use crate::releases::net::{check_url_hosts, open_checked, stream_to_file, HttpTransport, NetError, Url};
use std::collections::BTreeMap;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

/// An air band pack's source: one aerodrome's page(s), its code and the frequencies found.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AirSource {
    pub path: PathBuf,
    pub code: Option<String>,
    pub freqs: Vec<(i64, i64)>,
}

/// A built pack: the slot image (sequence 0, as datapack.py writes .dpk files) and what it holds.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BuiltPack {
    pub ptype: u8,
    pub image: Vec<u8>,
    pub header: PackHeader,
    pub count: usize,
    pub skipped: Option<RadioIdSkipped>,
}

/// A RadioID pack from RadioID.net's user.csv the user downloaded.
pub fn radio_id(csv_path: &Path, countries: &[String], name: &str, date: Option<&str>) -> Result<BuiltPack, PackError> {
    let (payload, count, skipped) = dp::radioid_from_csv(&std::fs::read(csv_path)?, countries)?;
    if count == 0 {
        return Err(PackError("no ID of the chosen countries in the list".into()));
    }
    let today = crate::storage::today();
    let image = dp::image(RADIO_ID, &payload, date.unwrap_or(&today), name, 0, dp::TAG, 0)?;
    let header = dp::parse(&image).0.unwrap();
    Ok(BuiltPack { ptype: RADIO_ID, image, header, count, skipped: Some(skipped) })
}

/// One AIP PDF (or a .txt of its text): its aerodrome code and the AD 2.18 frequencies.
pub fn read_air_source(path: &Path) -> Result<AirSource, PackError> {
    let is_txt = path.extension().is_some_and(|e| e.eq_ignore_ascii_case("txt"));
    let text = if is_txt { String::from_utf8_lossy(&std::fs::read(path)?).into_owned() } else { pdf::read(path)? };
    let name = path.to_string_lossy();
    Ok(AirSource { path: path.to_path_buf(), code: air::code_of(&name, Some(&text)), freqs: air::parse_ad218(&text) })
}

/// An air band pack from the sources (later pages of the same code replace earlier ones).
pub fn air_pack(sources: &[AirSource], name: &str, date: Option<&str>) -> Result<BuiltPack, PackError> {
    let mut fields: BTreeMap<String, Vec<(i64, i64)>> = BTreeMap::new();
    for s in sources {
        let code = s.code.clone().ok_or_else(|| {
            PackError(format!("{}: no aerodrome code (LTxx)", s.path.file_name().unwrap_or_default().to_string_lossy()))
        })?;
        fields.insert(code, s.freqs.clone());
    }
    if fields.is_empty() {
        return Err(PackError("no aerodrome page given".into()));
    }
    let payload = dp::air_payload(&fields)?;
    let today = crate::storage::today();
    let image = dp::image(AIR, &payload, date.unwrap_or(&today), name, 0, dp::TAG, 0)?;
    let header = dp::parse(&image).0.unwrap();
    Ok(BuiltPack { ptype: AIR, image, header, count: fields.len(), skipped: None })
}

/// A .dpk file (this program's, the C# program's, datapack.py's or a release's map pack): whole and checked, or
/// PackError. A RadioID pack over 192 KB is refused (firmware 1.1's slot, docs/mappack-design.md B.4 N2); a map pack
/// must pass the firmware's structural check (C.2).
pub fn load(path: &Path) -> Result<BuiltPack, PackError> {
    let image = std::fs::read(path)?;
    let name = path.file_name().unwrap_or_default().to_string_lossy().into_owned();
    check_image(&image, &name)
}

/// The checks of [`load`] on bytes already read (name: for the messages).
pub fn check_image(image: &[u8], name: &str) -> Result<BuiltPack, PackError> {
    let (h, payload) = dp::parse(image);
    let bad = || PackError(format!("{name}: not a whole data pack"));
    let h = h.ok_or_else(bad)?;
    if !h.crc_ok
        || h.format != 1
        || !(h.ptype == RADIO_ID || h.ptype == AIR || h.ptype == REF)
        || image.len() != HEADER_SIZE + h.length as usize
    {
        return Err(bad());
    }
    if h.ptype == RADIO_ID && h.length as usize > dp::RADIO_ID_PAYLOAD_MAX {
        return Err(PackError(format!(
            "{name}: RadioID pack of {} bytes: firmware 1.1 takes at most {} (192 KB); build it with fewer countries",
            h.length,
            dp::RADIO_ID_PAYLOAD_MAX
        )));
    }
    if image.len() > dp::slot_size(h.ptype).unwrap_or(0) {
        return Err(bad());
    }
    if h.ptype == REF {
        match refpack::validate(&payload, h.flags, &refpack::Rules::new(None)) {
            refpack::Verdict::Valid(_) => {}
            refpack::Verdict::Bad(e) => return Err(PackError(format!("{name}: map pack does not check: {e}"))),
            refpack::Verdict::Other(e) => {
                return Err(PackError(format!("{name}: map pack for another firmware version: {e}")))
            }
        }
    }
    let count = dp::count(&h, &payload).unwrap_or(0);
    Ok(BuiltPack { ptype: h.ptype, image: image.to_vec(), header: h, count, skipped: None })
}

/// Both slots of a type as a verified backup's data chip image holds them (q128-pass1.bin, files only).
pub fn slots_from_backup(folder: &Path, ptype: u8) -> Result<Vec<Vec<u8>>, PackError> {
    let mut f = std::fs::File::open(folder.join("q128-pass1.bin"))?;
    if f.metadata()?.len() < REGION_END {
        return Err(PackError("q128-pass1.bin: too short for the data pack region".into()));
    }
    let mut out = Vec::new();
    for slot in 0..2 {
        let mut buf = vec![0u8; dp::slot_size(ptype)?];
        f.seek(SeekFrom::Start(dp::slot_address(ptype, slot)?))?;
        f.read_exact(&mut buf)?;
        out.push(buf);
    }
    Ok(out)
}

pub const ALLOWED_HOSTS: [&str; 2] = ["radioid.net", "www.radioid.net"];
pub const LIMIT: u64 = 200 << 20;

pub fn check_radioid_url(url: &str) -> Result<Url, NetError> {
    check_url_hosts(url, &ALLOWED_HOSTS, "radioid.net")
}

/// RadioID.net's user list, downloaded only on the user's click: https://radioid.net only (every redirect checked
/// again), at most 200 MB. The list is the user's own copy; this program never redistributes it.
pub struct RadioIdDownloader {
    transport: Box<dyn HttpTransport>,
}

impl RadioIdDownloader {
    pub fn new(transport: Box<dyn HttpTransport>) -> Self {
        Self { transport }
    }

    pub fn real() -> Self {
        Self::new(Box::new(crate::releases::net::UreqTransport::new(600)))
    }

    pub fn download(&self, dest: &Path, progress: Option<&mut dyn FnMut(u64, u64)>) -> Result<(), NetError> {
        let mut resp =
            open_checked(self.transport.as_ref(), DOWNLOAD_URL, 5, &check_radioid_url, &|_| "text/csv, */*")?;
        stream_to_file(&mut resp.body, dest, LIMIT, "The list is larger than 200 MB", progress)
    }
}
