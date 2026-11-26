// SPDX-License-Identifier: GPL-3.0-or-later
//! Test data shared by the acceptance tests: the fixtures in rust/tests/fixtures (a copy of the C# program's
//! win/tests/DDM32Plus.Core.Tests/Fixtures, synthetic only, written by the Python generators there), the same input builders as the C# TestData /
//! RadioFixtures, a fake network, the recorded-run replay port and the fake radios. No serial port exists here.
#![allow(dead_code)]

use ddm32_core::pyjson::{self, Py, PyDict};
use ddm32_core::radio::error::{ToolError, ToolResult};
use ddm32_core::radio::monitor::{be32, be32_bytes, crc32, le32, ram_monitor as rm, sha, sum8, SSI_PROG, TIMER2_VALUE};
use ddm32_core::radio::port::{Clock, RadioPort, RawSerial};
use ddm32_core::releases::net::{NetError, Progress, ReleaseNet};
use ddm32_core::releases::{signed_manifest as sm, CURRENT};
use serde_json::{json, Value};
use std::cell::RefCell;
use std::collections::HashMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::OnceLock;

pub fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

pub fn fixture(parts: &[&str]) -> PathBuf {
    let mut p = fixtures();
    for x in parts {
        p.push(x);
    }
    p
}

pub fn python() -> &'static Value {
    static V: OnceLock<Value> = OnceLock::new();
    V.get_or_init(|| serde_json::from_slice(&std::fs::read(fixture(&["python-results.json"])).unwrap()).unwrap())
}

pub fn b64(s: &str) -> Vec<u8> {
    use base64::Engine;
    base64::engine::general_purpose::STANDARD.decode(s).unwrap()
}

pub fn b64e(b: &[u8]) -> String {
    use base64::Engine;
    base64::engine::general_purpose::STANDARD.encode(b)
}

// A TEST key: its secret is public (pc/tests/rtfixtures.py), so nothing it signs can be a release
pub fn test_secret() -> Vec<u8> {
    (0..32u8).collect()
}
pub fn other_secret() -> Vec<u8> {
    (1..33u8).collect()
}
pub fn test_public_hex() -> String {
    hex::encode(ddm32_core::crypto::ed25519::secret_to_public(&test_secret()))
}

pub fn pattern(n: usize, k: usize) -> Vec<u8> {
    (0..n).map(|i| ((i * k + (i >> 8) + 1) & 255) as u8).collect()
}

pub fn release_app(version: &str) -> Vec<u8> {
    format!("DDM32 Plus test {version}").repeat(64).into_bytes()
}

pub fn url(release: &str, name: &str) -> String {
    format!("{}{}/{}", CURRENT.url_prefix(), release, name)
}

pub fn firmware_entry(app: &[u8], version: &str, channel: &str, release: Option<&str>, zip: Option<&[u8]>) -> Value {
    let release = release.map(str::to_string).unwrap_or(format!("v{version}"));
    let name = format!("ddm32-plus-{version}.zip");
    let default_zip: Vec<u8> = [b"zip:".as_slice(), &app[..16.min(app.len())]].concat();
    let zip = zip.unwrap_or(&default_zip);
    json!({
        "version": version, "channel": channel, "file": name, "url": url(&release, &name),
        "sha256": sm::sha256_hex(zip), "size": zip.len(),
        "application_sha256": sm::sha256_hex(app), "application_size": app.len(),
        "settings_format": 1, "codeplug_format": 1, "min_compatible_version": version,
        "notes": {"tr": format!("Sürüm {version}"), "en": format!("Version {version}")},
    })
}

pub fn app_entry(version: &str, zip: &[u8], release: Option<&str>) -> Value {
    let release = release.map(str::to_string).unwrap_or(format!("v{version}"));
    let name = format!("DDM32Plus-{version}.zip");
    json!({
        "version": version, "file": name, "url": url(&release, &name),
        "sha256": sm::sha256_hex(zip), "size": zip.len(), "notes": {"tr": "Program", "en": "Program"},
    })
}

pub fn manifest(firmware: Vec<Value>, release: &str) -> Value {
    json!({
        "format": CURRENT.format, "repo": CURRENT.repo, "release": release,
        "created_utc": "2026-11-20T00:00:00Z", "firmware": firmware,
    })
}

/// (manifest bytes, signature text bytes)
pub fn signed(m: &Value, secret: Option<&[u8]>) -> (Vec<u8>, Vec<u8>) {
    let data = (serde_json::to_string_pretty(m).unwrap() + "\n").into_bytes();
    let sig = sm::sign_bytes(&data, secret.unwrap_or(&test_secret())).into_bytes();
    (data, sig)
}

pub fn write_signed(folder: &Path, m: &Value, secret: Option<&[u8]>) {
    std::fs::create_dir_all(folder).unwrap();
    let (data, sig) = signed(m, secret);
    std::fs::write(folder.join(CURRENT.manifest_name), data).unwrap();
    std::fs::write(folder.join(CURRENT.sig_name()), sig).unwrap();
}

pub fn zip_of(members: &[(&str, &[u8])]) -> Vec<u8> {
    use std::io::Write;
    let mut buf = std::io::Cursor::new(Vec::new());
    {
        let mut z = zip::ZipWriter::new(&mut buf);
        for (name, data) in members {
            z.start_file(*name, zip::write::SimpleFileOptions::default()).unwrap();
            z.write_all(data).unwrap();
        }
        z.finish().unwrap();
    }
    buf.into_inner()
}

/// A release zip with the three package files for app.
pub fn release_zip(app: &[u8]) -> Vec<u8> {
    zip_of(&[
        ("rfoundry_dm32.bin", app),
        ("rfoundry_dm32.json", b"{\"plus_enabled\": true}"),
        ("rfoundry_dm32.map", b"map"),
    ])
}

pub fn tmp() -> tempfile::TempDir {
    tempfile::Builder::new().prefix("ddm32-test-").tempdir().unwrap()
}

/// A fake network: url -> bytes; records downloads; nothing leaves the process.
#[derive(Default)]
pub struct FakeNet {
    pub files: RefCell<HashMap<String, Vec<u8>>>,
    pub downloads: RefCell<Vec<String>>,
    pub payload: RefCell<Option<Vec<u8>>>,
}

impl ReleaseNet for FakeNet {
    fn fetch(&self, url: &str, limit: usize) -> Result<Vec<u8>, NetError> {
        let files = self.files.borrow();
        let data = files.get(url).ok_or_else(|| NetError(format!("HTTP Error 404: {url}")))?;
        if data.len() > limit {
            return Err(NetError(format!("Answer larger than {limit} bytes: {url}")));
        }
        Ok(data.clone())
    }
    fn download(&self, url: &str, dest: &Path, size: u64, progress: Progress<'_>) -> Result<(), NetError> {
        self.downloads.borrow_mut().push(url.to_string());
        let data = match self.payload.borrow().clone() {
            Some(p) => p,
            None => self.files.borrow()[url].clone(),
        };
        std::fs::write(dest, &data).unwrap();
        if let Some(p) = progress {
            p(data.len() as u64, size);
        }
        Ok(())
    }
}

// ---- the radio fixtures (make_radio_fixtures.py, rtfixtures.py) --------------------------------------------------

pub const START: usize = 0xC000;
pub const SIZE: usize = 0x100000;
pub const SECTOR: usize = 4096;
pub const SLOT: usize = 0xE8000;
pub const DATA_SIZE: usize = 0x1000000;
pub const STOCK_CODE_AT: u64 = 0x030C68B8;
pub const STOCK_TABLES_AT: u64 = 0x03072658;

pub fn index() -> &'static Value {
    static V: OnceLock<Value> = OnceLock::new();
    V.get_or_init(|| serde_json::from_slice(&std::fs::read(fixture(&["radio", "index.json"])).unwrap()).unwrap())
}

pub fn scenario(name: &str) -> Value {
    let f = std::fs::File::open(fixture(&["radio", &format!("{name}.json.gz")])).unwrap();
    let mut s = String::new();
    flate2::read::GzDecoder::new(f).read_to_string(&mut s).unwrap();
    serde_json::from_str(&s).unwrap()
}

pub fn scenario_names() -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(fixture(&["radio"]))
        .unwrap()
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.ends_with(".json.gz"))
        .map(|n| n[..n.len() - 8].to_string())
        .collect();
    v.sort();
    v
}

pub fn rep(n: usize) -> Vec<u8> {
    (0..n).map(|i| (i & 255) as u8).collect()
}

fn ff(n: usize) -> Vec<u8> {
    vec![0xFF; n]
}

pub fn boot() -> Vec<u8> {
    [pattern(0x1000, 7), rep(0xB000)].concat()
}
pub fn app_classic() -> Vec<u8> {
    [pattern(0x800, 3), rep(0x7000), b"DDM32 Plus".to_vec()].concat()
}
pub fn plus_app() -> Vec<u8> {
    [pattern(0x800, 9), rep(0x5000), b"DDM32 Plus test build".to_vec()].concat()
}

fn le(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

/// rtfixtures.make_slot: a codec slot v2 as hrc7000_codec builds it.
pub fn make_slot(tag: u32, magic: u32) -> Vec<u8> {
    let code = [le(&[0x152514D8, 0x74C776CD]), pattern(0x800 - 8, 5)].concat();
    let tables = pattern(0x302, 11);
    let rels = le(&[8, 0x40, 0x100]);
    let code_off = 128;
    let tab_off = code_off + code.len();
    let rel_off = tab_off + ((tables.len() + 3) & !3);
    let crc = crc32(&rels, crc32(&tables, crc32(&code, 0)));
    let header = le(&[
        magic,
        2,
        code_off as u32,
        code.len() as u32,
        0x16cdc,
        tab_off as u32,
        tables.len() as u32,
        0x18000e48,
        0x10,
        0x20,
        0x30,
        0x40,
        0x50,
        0x60,
        0x70,
        tag,
        crc,
        rel_off as u32,
        (rels.len() / 4) as u32,
    ]);
    [header.clone(), vec![0u8; 128 - header.len()], code, tables.clone(), ff(rel_off - tab_off - tables.len()), rels]
        .concat()
}

pub fn slot() -> Vec<u8> {
    make_slot(47, 0x43524444)
}

pub fn program(app: &[u8], slot_bytes: Option<&[u8]>) -> Vec<u8> {
    let s = slot_bytes.map(|s| s.to_vec()).unwrap_or_else(slot);
    let mut p = [boot(), app.to_vec(), ff(SLOT - START - app.len()), s].concat();
    p.resize(SIZE, 0xFF);
    p
}

pub fn make_data() -> Vec<u8> {
    let mut d = ff(DATA_SIZE);
    d[..SECTOR].copy_from_slice(&pattern(SECTOR, 13));
    d[0x800000..0x800000 + 3 * SECTOR].copy_from_slice(&pattern(3 * SECTOR, 17));
    d
}

pub fn synthetic_codec() -> (Vec<u8>, Vec<u8>, Vec<(u64, u64)>) {
    let run = 0x16CDCu64;
    let mut code = pattern(0x2000, 29);
    code[..8].copy_from_slice(&le(&[0x152514D8, 0x74C776CD]));
    let refs = vec![(0x40, run + 0x100), (0x84, run), (0x1000, run + 0x2000 - 4)];
    for &(off, v) in &refs {
        code[off as usize..off as usize + 4].copy_from_slice(&(v as u32).to_le_bytes());
    }
    (code, pattern(0x2F78, 31), refs)
}

pub fn stock_program(code: &[u8], tables: &[u8]) -> Vec<u8> {
    let mut p = [boot(), pattern(0x800, 19), rep(0x3000), ff(SIZE - START - 0x3800)].concat();
    let c = (STOCK_CODE_AT - 0x03000000) as usize;
    p[c..c + code.len()].copy_from_slice(code);
    let t = (STOCK_TABLES_AT - 0x03000000) as usize;
    p[t..t + tables.len()].copy_from_slice(tables);
    p
}

pub fn synthetic_codec_tables() -> ddm32_core::radio::codec::Codec {
    let (code, tables, refs) = synthetic_codec();
    ddm32_core::radio::codec::Codec::synthetic(
        &code,
        &tables,
        &refs,
        STOCK_CODE_AT,
        STOCK_TABLES_AT,
        "DM32.01.01.047.bin",
        47,
    )
}

/// rtfixtures.write_backup: both passes of both chips and a verified manifest with the chip IDs.
pub fn write_backup(folder: &Path, program: &[u8], data: &[u8], chips: bool) {
    std::fs::create_dir_all(folder).unwrap();
    let mut captures = vec![];
    for (memory, image) in [("progmem", program), ("q128", data)] {
        for n in 1..=2 {
            let name = format!("{memory}-pass{n}.bin");
            std::fs::write(folder.join(&name), image).unwrap();
            let sectors: Vec<Py> = image.chunks(SECTOR).map(|c| Py::Str(sha(c))).collect();
            captures.push(Py::Dict(
                PyDict::new()
                    .with("file", name)
                    .with("bytes", image.len())
                    .with("sha256", sha(image))
                    .with("sector_sha256", Py::List(sectors)),
            ));
        }
    }
    let mut m = PyDict::new().with("backup_verified", true).with("captures", Py::List(captures));
    if chips {
        m.set(
            "chips",
            PyDict::new()
                .with("progmem", PyDict::new().with("jedec", "c84014").with("bytes", SIZE))
                .with("q128", PyDict::new().with("jedec", "ef4018").with("bytes", DATA_SIZE)),
        );
    }
    std::fs::write(folder.join("backup-manifest.json"), pyjson::dumps(&Py::Dict(m), None)).unwrap();
}

/// rtfixtures.write_plus with the signed manifest the Python run used (the test key's signature).
pub fn write_plus(folder: &Path, app: &[u8]) {
    std::fs::create_dir_all(folder).unwrap();
    std::fs::write(folder.join("rfoundry_dm32.bin"), app).unwrap();
    let mut map = String::from("\nMemory Configuration\n\n .text          0x0300c000     0x7000\n");
    for (name, value) in ddm32_core::radio::plan::CODEC_SYMBOLS {
        map.push_str(&format!("                0x{value:08x}                {name} = 0x{value:x}\n"));
    }
    std::fs::write(folder.join("rfoundry_dm32.map"), map.as_bytes()).unwrap();
    let j = PyDict::new()
        .with("plus_enabled", true)
        .with("classic_enabled", true)
        .with("forced_rebuild_identical", true)
        .with("public_enabled", true)
        .with(
            "outputs",
            PyDict::new().with("rfoundry_dm32.bin", sha(app)).with("rfoundry_dm32.map", sha(map.as_bytes())),
        );
    std::fs::write(folder.join("rfoundry_dm32.json"), pyjson::dumps(&Py::Dict(j), None)).unwrap();
    let inputs = &index()["inputs"];
    std::fs::write(folder.join("ddm32-manifest.json"), b64(inputs["plus_manifest"].as_str().unwrap())).unwrap();
    std::fs::write(folder.join("ddm32-manifest.json.sig"), inputs["plus_sig"].as_str().unwrap()).unwrap();
}

/// The inputs of the recorded runs, in a temporary folder.
pub struct RadioInputs {
    pub dir: tempfile::TempDir,
    pub program: Vec<u8>,
    pub data: Vec<u8>,
    pub stock: Vec<u8>,
    pub target: Vec<u8>,
}

impl RadioInputs {
    pub fn new() -> Self {
        let dir = tempfile::Builder::new().prefix("ddm32-radio-").tempdir().unwrap();
        let program = program(&app_classic(), None);
        let data = make_data();
        let (code, tables, _) = synthetic_codec();
        let stock = stock_program(&code, &tables);
        let mut target = program[..START].to_vec();
        target.extend_from_slice(&plus_app());
        target.resize(SLOT, 0xFF);
        target.extend_from_slice(&program[SLOT..]);
        let r = RadioInputs { dir, program, data, stock, target };
        write_backup(&r.backup(), &r.program, &r.data, true);
        write_backup(&r.stock_backup(), &r.stock, &r.data, true);
        write_plus(&r.plus(), &plus_app());
        r
    }
    pub fn root(&self) -> PathBuf {
        self.dir.path().to_path_buf()
    }
    pub fn backup(&self) -> PathBuf {
        self.root().join("backup")
    }
    pub fn stock_backup(&self) -> PathBuf {
        self.root().join("stock-backup")
    }
    pub fn plus(&self) -> PathBuf {
        self.root().join("plus")
    }
}

/// Lines collected from a tool's print.
#[derive(Default, Clone)]
pub struct Lines(pub Rc<RefCell<Vec<String>>>);

impl Lines {
    pub fn print(&self) -> impl Fn(&str) + '_ {
        move |s: &str| self.0.borrow_mut().extend(s.split('\n').map(str::to_string))
    }
    pub fn get(&self) -> Vec<String> {
        self.0.borrow().clone()
    }
}

/// A raw serial port shared with the test (the tool owns a Box of it; the test inspects it afterwards).
pub struct Shared<T: RawSerial>(pub Rc<RefCell<T>>);

impl<T: RawSerial> RadioPort for Shared<T> {
    fn timeout(&self) -> f64 {
        self.0.borrow().timeout()
    }
    fn set_timeout(&mut self, t: f64) {
        self.0.borrow_mut().set_timeout(t)
    }
    fn baud_rate(&self) -> u32 {
        self.0.borrow().baud_rate()
    }
    fn set_baud_rate(&mut self, b: u32) -> ToolResult<()> {
        self.0.borrow_mut().set_baud_rate(b)
    }
    fn write(&mut self, d: &[u8]) -> ToolResult<Option<usize>> {
        self.0.borrow_mut().write(d)
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        self.0.borrow_mut().read(n)
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        self.0.borrow_mut().reset_input_buffer()
    }
    fn flush(&mut self) -> ToolResult<()> {
        self.0.borrow_mut().flush()
    }
}

impl<T: RawSerial> RawSerial for Shared<T> {
    fn open(&mut self) -> ToolResult<()> {
        self.0.borrow_mut().open()
    }
    fn close(&mut self) -> ToolResult<()> {
        self.0.borrow_mut().close()
    }
}

/// A serial port that is a recorded run of the Python tool against the pinned FakeRadio: every write must be the
/// recorded write (same bytes, same order), after which the radio's recorded answer can be read; input resets, open
/// and close must come where they came.
pub struct ReplayPort {
    events: Vec<Value>,
    next: usize,
    rx: std::collections::VecDeque<u8>,
    pub writes: usize,
    pub failure: Option<String>,
    timeout: f64,
    baud: u32,
}

impl ReplayPort {
    pub fn new(sc: &Value) -> Self {
        let events = sc["events"].as_array().unwrap().clone();
        assert_eq!(events[0][0], "init");
        let rx = b64(events[0][1].as_str().unwrap()).into_iter().collect();
        ReplayPort { events, next: 1, rx, writes: 0, failure: None, timeout: 0.02, baud: 115200 }
    }

    fn expect(&mut self, kind: &str, detail: &str) -> ToolResult<Value> {
        if self.failure.is_some() {
            return Err(ToolError::new("ReplayMismatch", self.failure.clone().unwrap()));
        }
        if self.next >= self.events.len() {
            let f = format!("the Rust tool did more than the Python tool: {kind} {detail} after event {}", self.next);
            self.failure = Some(f.clone());
            return Err(ToolError::new("ReplayMismatch", f));
        }
        let e = self.events[self.next].clone();
        if e[0] != kind {
            let f = format!("event {}: the Python tool did '{}', the Rust tool '{kind}' {detail}", self.next, e[0]);
            self.failure = Some(f.clone());
            return Err(ToolError::new("ReplayMismatch", f));
        }
        self.next += 1;
        Ok(e)
    }

    pub fn assert_done(&self) {
        assert!(self.failure.is_none(), "{}", self.failure.clone().unwrap());
        assert_eq!(
            self.next,
            self.events.len(),
            "the Rust tool stopped at event {} of {}",
            self.next,
            self.events.len()
        );
    }
}

impl RadioPort for ReplayPort {
    fn timeout(&self) -> f64 {
        self.timeout
    }
    fn set_timeout(&mut self, t: f64) {
        self.timeout = t;
    }
    fn baud_rate(&self) -> u32 {
        self.baud
    }
    fn set_baud_rate(&mut self, b: u32) -> ToolResult<()> {
        self.baud = b;
        Ok(())
    }
    fn write(&mut self, data: &[u8]) -> ToolResult<Option<usize>> {
        let e = self.expect("w", &hex::encode_upper(&data[..24.min(data.len())]))?;
        let want = b64(e[1].as_str().unwrap());
        if want != data {
            let f = format!(
                "write {} (event {}) differs: Python {}..., Rust {}...",
                self.writes,
                self.next - 1,
                hex::encode_upper(&want[..32.min(want.len())]),
                hex::encode_upper(&data[..32.min(data.len())])
            );
            self.failure = Some(f.clone());
            return Err(ToolError::new("ReplayMismatch", f));
        }
        self.rx.extend(b64(e[2].as_str().unwrap()));
        self.writes += 1;
        Ok(None) // the FakeRadio's write returns None, like some pyserial ports
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        let k = n.min(self.rx.len());
        Ok(self.rx.drain(..k).collect())
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        self.expect("reset", "")?;
        self.rx.clear();
        Ok(())
    }
    fn flush(&mut self) -> ToolResult<()> {
        Ok(())
    }
}

impl RawSerial for ReplayPort {
    fn open(&mut self) -> ToolResult<()> {
        self.expect("open", "").map(|_| ())
    }
    fn close(&mut self) -> ToolResult<()> {
        self.expect("close", "").map(|_| ())
    }
}

/// A simulated DM-32UV for the tools (the C# FakeRadio, itself checked against the Python FakeRadio).
pub struct FakeRadio {
    clock: Rc<dyn Clock>,
    pub images: HashMap<&'static str, Vec<u8>>,
    pub writes: Vec<(&'static str, u8, usize)>,
    pub pokes: Vec<(u64, u64)>,
    pub ops: Vec<u8>,
    pub sent: Vec<Vec<u8>>,
    pub errors: Vec<String>,
    pub opened: bool,
    pub closed: bool,
    pub data_jedec: String,
    pub corrupt_after_writes: Option<usize>,
    pub silent: bool,
    out: std::collections::VecDeque<u8>,
    inb: Vec<u8>,
    loaded: Vec<u8>,
    running: bool,
    rom_taken: bool,
    wel: bool,
    stub_len: usize,
    run: u64,
    end: u64,
    board: u8,
    flags: u8,
    mem: HashMap<u64, u64>,
    timeout: f64,
}

const OK: u8 = 0x05;
const BAD_SUM: u8 = 0x0A;
const BAD_OP: u8 = 0x0B;

fn frame(status: u8, body: &[u8]) -> Vec<u8> {
    let mut v = vec![0x02, status, (body.len() >> 8) as u8, body.len() as u8, sum8(body), 0x03];
    v.extend_from_slice(body);
    v
}

impl FakeRadio {
    pub fn new(program: &[u8], data: &[u8], clock: Rc<dyn Clock>) -> Self {
        let image = rm::image().unwrap();
        let mut out = std::collections::VecDeque::new();
        out.extend(rm::ROM_HELLO);
        FakeRadio {
            clock,
            images: HashMap::from([("progmem", program.to_vec()), ("q128", data.to_vec())]),
            writes: vec![],
            pokes: vec![],
            ops: vec![],
            sent: vec![],
            errors: vec![],
            opened: false,
            closed: false,
            data_jedec: "ef4018".into(),
            corrupt_after_writes: None,
            silent: false,
            out,
            inb: vec![],
            loaded: vec![],
            running: false,
            rom_taken: false,
            wel: false,
            stub_len: image.len().div_ceil(256) * 256,
            run: 0x30000,
            end: le32(&image[0xBC..]) as u64,
            board: 0,
            flags: 0,
            mem: HashMap::from([(0x14020000, 0), (0x11000038, 0xFFFFFE07)]),
            timeout: 0.02,
        }
    }

    fn select(p: &[u8]) -> (&'static str, bool) {
        let m = if be32(p) as u64 == SSI_PROG { "progmem" } else { "q128" };
        let sel = (p[4] & 1) != 0 && (m == "progmem" || ((p[4] & 2) != 0 && p[5] == 18));
        (m, sel)
    }

    fn step(&mut self) -> bool {
        if !self.running {
            if self.inb.len() < 4 {
                return false;
            }
            if self.inb[..4] == rm::ROM_REPLY {
                self.inb.drain(..4);
                if self.rom_taken {
                    self.out.extend(rm::ROM_NAK);
                }
                self.rom_taken = true;
                return true;
            }
            let (op, chk) = (self.inb[1], self.inb[2]);
            let n: i64 = if op == 0x18 {
                4 + 8 + 256
            } else if op == 0x1B {
                4 + 4
            } else {
                -1
            };
            if n < 0 {
                self.errors.push(format!("unexpected {}", hex::encode_upper(&self.inb[..8.min(self.inb.len())])));
                self.inb.clear();
                return false;
            }
            let n = n as usize;
            if self.inb.len() < n {
                return false;
            }
            let payload: Vec<u8> = self.inb[4..n].to_vec();
            self.inb.drain(..n);
            if sum8(&payload) != chk {
                self.errors.push("rom checksum".into());
                return true;
            }
            if op == 0x18 {
                if be32(&payload) as usize != 0x10000 + self.loaded.len() || payload[4..8] != [0, 0, 0, 0x40] {
                    self.errors.push("load block address".into());
                }
                self.loaded.extend_from_slice(&payload[8..]);
            } else {
                if be32(&payload) != 0x10000 || self.loaded.len() != self.stub_len {
                    self.errors.push("start command".into());
                }
                self.running = true;
            }
            self.out.extend(rm::ROM_ACK);
            return true;
        }
        if self.inb.len() < 6 {
            return false;
        }
        if self.inb[0] != 0x02 {
            self.inb.remove(0);
            return true;
        }
        let (op, ln, chk) = (self.inb[1], (self.inb[2] as usize) << 8 | self.inb[3] as usize, self.inb[4]);
        if self.inb.len() < 6 + ln {
            return false;
        }
        let p: Vec<u8> = self.inb[6..6 + ln].to_vec();
        self.inb.drain(..6 + ln);
        if sum8(&p) != chk {
            self.out.extend(frame(BAD_SUM, &[]));
            return true;
        }
        self.ops.push(op);
        if (op == 7 || op == 9) && !self.writes.is_empty() {
            if let Some(at) = self.corrupt_after_writes.take() {
                self.images.get_mut("progmem").unwrap()[at] ^= 0x01;
            }
        }
        let mut status = OK;
        let mut body: Vec<u8> = vec![];
        match op {
            0x01 => {
                body.extend_from_slice(b"C7STUB");
                body.extend_from_slice(&[6, (1032 >> 8) as u8, (1032 & 255) as u8]);
                body.extend_from_slice(&be32_bytes(0x10000));
                body.extend_from_slice(&be32_bytes(0x0808));
                body.extend_from_slice(&[self.board, self.flags]);
                body.extend_from_slice(&be32_bytes(self.run));
                body.extend_from_slice(&be32_bytes(self.loaded.len() as u64));
                body.extend_from_slice(&be32_bytes(self.end - 0x1004));
                body.extend_from_slice(&be32_bytes(self.end - 0x1000));
                body.extend_from_slice(&be32_bytes(self.end));
                body.extend_from_slice(&be32_bytes(600));
            }
            0x0D => {
                if p.len() != 16 || p[14] > 3 {
                    status = BAD_OP;
                } else {
                    self.board = p[12];
                    self.flags = p[13];
                }
            }
            0x02 => {
                let a = be32(&p) as u64;
                let c = (p[4] as usize) << 8 | p[5] as usize;
                if a == TIMER2_VALUE {
                    let v = (-((self.clock.monotonic() * 24e6) as i64)) & 0xFFFF_FFFF;
                    self.mem.insert(a, v as u64);
                }
                if self.run <= a && a < self.run + self.loaded.len() as u64 {
                    let s = (a - self.run) as usize;
                    body.extend(self.loaded.iter().skip(s).take(c));
                } else {
                    for i in (0..c).step_by(4) {
                        body.extend_from_slice(&(*self.mem.get(&(a + i as u64)).unwrap_or(&0) as u32).to_le_bytes());
                    }
                }
            }
            0x03 => {
                let a = be32(&p) as u64;
                let mut i = 0;
                while i + 8 <= p.len() {
                    let v = le32(&p[4 + i..]) as u64;
                    self.mem.insert(a + i as u64, v);
                    self.pokes.push((a + i as u64, v));
                    i += 4;
                }
            }
            0x07 => {
                let (m, sel) = Self::select(&p);
                let addr = be32(&p[6..]) as usize;
                let cnt = (p[10] as usize) << 8 | p[11] as usize;
                for i in 0..cnt {
                    let sec: Vec<u8> = if sel {
                        self.images[m][addr + i * SECTOR..addr + (i + 1) * SECTOR].to_vec()
                    } else {
                        ff(SECTOR)
                    };
                    body.extend_from_slice(&be32_bytes(crc32(&sec, 0) as u64));
                    body.push(if sec.iter().any(|&b| b != 0xFF) { 0 } else { 1 });
                }
            }
            0x09 => {
                let (m, sel) = Self::select(&p);
                let addr = be32(&p[6..]) as usize;
                let cnt = p[10] as usize;
                for i in 0..cnt {
                    let sec: Vec<u8> = if sel {
                        self.images[m][addr + i * SECTOR..addr + (i + 1) * SECTOR].to_vec()
                    } else {
                        ff(SECTOR)
                    };
                    let mut rec = be32_bytes(crc32(&sec, 0) as u64).to_vec();
                    if sec.iter().all(|&b| b == 0xFF) {
                        rec.extend_from_slice(&[2, 0, 0]);
                    } else {
                        rec.extend_from_slice(&[0, (SECTOR >> 8) as u8, (SECTOR & 255) as u8]);
                        rec.extend_from_slice(&sec);
                    }
                    self.out.extend(frame(OK, &rec));
                }
                return true;
            }
            0x05 => {
                let (m, sel) = Self::select(&p);
                let txn = (p[6] as usize) << 8 | p[7] as usize;
                let rxn = (p[8] as usize) << 8 | p[9] as usize;
                let tx = p[10..10 + txn].to_vec();
                if !sel {
                    body.extend(ff(rxn));
                } else if tx[0] == 0x03 {
                    let a = (tx[1] as usize) << 16 | (tx[2] as usize) << 8 | tx[3] as usize;
                    body.extend_from_slice(&self.images[m][a..a + rxn]);
                } else if tx[0] == 0x06 {
                    self.wel = true;
                } else if tx[0] == 0x05 {
                    body.push(if self.wel { 2 } else { 0 });
                } else if tx[0] == 0x20 || tx[0] == 0x02 {
                    let a = (tx[1] as usize) << 16 | (tx[2] as usize) << 8 | tx[3] as usize;
                    if self.wel {
                        let img = self.images.get_mut(m).unwrap();
                        if tx[0] == 0x20 {
                            img[a..a + SECTOR].fill(0xFF);
                        } else {
                            for i in 4..tx.len() {
                                img[a + i - 4] &= tx[i];
                            }
                        }
                        self.writes.push((m, tx[0], a));
                    }
                    self.wel = false;
                } else if tx[0] == 0x9F {
                    if m == "progmem" {
                        body.extend_from_slice(&[0xC8, 0x40, 0x14]);
                    } else {
                        body.extend(hex::decode(&self.data_jedec).unwrap());
                    }
                } else {
                    self.errors.push(format!("unknown flash command {}", hex::encode_upper(&tx)));
                }
            }
            _ => status = BAD_OP,
        }
        self.out.extend(frame(status, &body));
        true
    }
}

impl RadioPort for FakeRadio {
    fn timeout(&self) -> f64 {
        self.timeout
    }
    fn set_timeout(&mut self, t: f64) {
        self.timeout = t;
    }
    fn baud_rate(&self) -> u32 {
        115200
    }
    fn set_baud_rate(&mut self, _b: u32) -> ToolResult<()> {
        Ok(())
    }
    fn write(&mut self, b: &[u8]) -> ToolResult<Option<usize>> {
        self.sent.push(b.to_vec());
        if self.silent {
            return Ok(None);
        }
        self.inb.extend_from_slice(b);
        while self.step() {}
        Ok(None)
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        let k = n.min(self.out.len());
        Ok(self.out.drain(..k).collect())
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        self.out.clear();
        Ok(())
    }
    fn flush(&mut self) -> ToolResult<()> {
        Ok(())
    }
}

impl RawSerial for FakeRadio {
    fn open(&mut self) -> ToolResult<()> {
        self.opened = true;
        if self.silent {
            self.out.clear();
        }
        Ok(())
    }
    fn close(&mut self) -> ToolResult<()> {
        self.closed = true;
        Ok(())
    }
}

/// A clock shared between the tool and the fake radio.
pub struct SharedClock(pub Rc<ddm32_core::radio::port::VirtualClock>);

impl Clock for SharedClock {
    fn monotonic(&self) -> f64 {
        self.0.monotonic()
    }
    fn sleep(&self, s: f64) {
        self.0.sleep(s)
    }
    fn utc_stamp(&self) -> String {
        self.0.utc_stamp()
    }
}

/// A port that sends nowhere (the guard tests).
#[derive(Default)]
pub struct NullPort {
    pub sent: Vec<Vec<u8>>,
}

impl RadioPort for NullPort {
    fn timeout(&self) -> f64 {
        0.0
    }
    fn set_timeout(&mut self, _t: f64) {}
    fn baud_rate(&self) -> u32 {
        115200
    }
    fn set_baud_rate(&mut self, _b: u32) -> ToolResult<()> {
        Ok(())
    }
    fn write(&mut self, d: &[u8]) -> ToolResult<Option<usize>> {
        self.sent.push(d.to_vec());
        Ok(Some(d.len()))
    }
    fn read(&mut self, _n: usize) -> ToolResult<Vec<u8>> {
        Ok(vec![])
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        Ok(())
    }
    fn flush(&mut self) -> ToolResult<()> {
        Ok(())
    }
}

impl RawSerial for NullPort {
    fn open(&mut self) -> ToolResult<()> {
        Ok(())
    }
    fn close(&mut self) -> ToolResult<()> {
        Ok(())
    }
}

/// JSON from the fixtures as the program's ordered value (for dumps on both sides).
pub fn py(v: &Value) -> Py {
    Py::from_json(v)
}

// ---- map pack (type 3) fixtures: a tiny pack that passes the firmware's structural check -------------------------

pub const COORD: u32 = 0x1A2B_3C4D;

/// One repeater record (rpt_entry_t, 44 bytes): position, RX, mode, callsign, province.
pub fn rpt_rec(lat: i32, lon: i32, rx: u32, mode: u8, name: &str, prov: u8) -> Vec<u8> {
    let mut r = vec![0u8; 44];
    r[0..4].copy_from_slice(&lat.to_le_bytes());
    r[4..8].copy_from_slice(&lon.to_le_bytes());
    r[8..12].copy_from_slice(&rx.to_le_bytes());
    r[16] = mode;
    let n = name.as_bytes();
    r[18..18 + n.len().min(10)].copy_from_slice(&n[..n.len().min(10)]);
    r[42] = prov;
    r
}

pub fn rpt_info_rec(count: u32, source: &str) -> Vec<u8> {
    let mut r = vec![0u8; 32];
    r[0..4].copy_from_slice(&count.to_le_bytes());
    r[5..15].copy_from_slice(b"2026-11-19");
    let s = source.as_bytes();
    r[16..16 + s.len().min(15)].copy_from_slice(&s[..s.len().min(15)]);
    r
}

/// The sections of a valid pack: 81 provinces of one ring of one step, one district each, the repeaters given, two
/// airfields. (id, record size, bytes), in the order tools/refpack.py writes them.
pub fn ref_sections(rpts: &[Vec<u8>], source: &str) -> Vec<(u16, u16, Vec<u8>)> {
    use ddm32_core::datapacks::refpack as rp;
    let mut meta = vec![0u8; 32];
    meta[rp::META_PROVINCES..rp::META_PROVINCES + 2].copy_from_slice(&81u16.to_le_bytes());
    meta[rp::META_RINGS..rp::META_RINGS + 2].copy_from_slice(&81u16.to_le_bytes());
    meta[rp::META_STEPS..rp::META_STEPS + 2].copy_from_slice(&81u16.to_le_bytes());
    meta[rp::META_DISTRICTS..rp::META_DISTRICTS + 2].copy_from_slice(&81u16.to_le_bytes());
    let mut provs = Vec::new();
    let mut rings = Vec::new();
    let mut pnames = Vec::new();
    let mut dnames = Vec::new();
    let mut dfirst = Vec::new();
    let mut nfirst = Vec::new();
    for i in 0..81u16 {
        let mut p = vec![0u8; 18];
        p[0..2].copy_from_slice(&i.to_le_bytes());
        p[2] = 1;
        p[3] = (i + 1) as u8;
        p[4..6].copy_from_slice(&(pnames.len() as u16).to_le_bytes());
        p[14..16].copy_from_slice(&10i16.to_le_bytes());
        p[16..18].copy_from_slice(&10i16.to_le_bytes());
        provs.extend(p);
        pnames.extend(format!("P{i}\0").into_bytes());
        let mut r = vec![0u8; 8];
        r[4..6].copy_from_slice(&i.to_le_bytes());
        r[6..8].copy_from_slice(&1u16.to_le_bytes());
        rings.extend(r);
        dfirst.extend(i.to_le_bytes());
        nfirst.extend((dnames.len() as u16).to_le_bytes());
        dnames.extend(format!("D{i}\0").into_bytes());
    }
    dfirst.extend(81u16.to_le_bytes());
    let keys: Vec<u8> = rpts.iter().flat_map(|r| rp::rpt_key(r)).collect();
    let mut air = vec![0u8; 88];
    air[8..10].copy_from_slice(&0u16.to_le_bytes());
    air[10] = 2;
    air[44 + 8..44 + 10].copy_from_slice(&2u16.to_le_bytes());
    air[44 + 10] = 1;
    vec![
        (rp::MAP_META, 32, meta),
        (rp::PROVINCES, 18, provs),
        (rp::RINGS, 8, rings),
        (rp::STEPS, 2, vec![1u8; 162]),
        (rp::PROV_NAMES, 1, pnames),
        (rp::DIST_POS, 4, vec![0u8; 81 * 4]),
        (rp::DIST_FIRST, 2, dfirst),
        (rp::DIST_NAME_FIRST, 2, nfirst),
        (rp::DIST_NAMES, 1, dnames),
        (rp::RPT_INFO, 32, rpt_info_rec(rpts.len() as u32, source)),
        (rp::RPT, 44, rpts.concat()),
        (rp::RPT_KEY, 10, keys),
        (rp::AIR, 44, air),
        (rp::AIR_FREQ, 4, vec![0u8; 12]),
        (rp::AIR_KEY, 10, vec![0u8; 20]),
    ]
}

pub fn ref_payload(coord: u32, rpts: &[Vec<u8>], source: &str) -> Vec<u8> {
    ddm32_core::datapacks::refpack::assemble(coord, &ref_sections(rpts, source)).unwrap()
}

pub fn two_rpts() -> Vec<Vec<u8>> {
    vec![
        rpt_rec(3_900_000, 3_200_000, 145_600_000, 0, "TA2RPT", 6),
        rpt_rec(4_100_000, 2_900_000, 439_000_000, 1, "TA1DMR", 34),
    ]
}

/// A whole map pack image (.dpk: sequence 0), header tag and flags as given.
pub fn ref_image(payload: &[u8], seq: u32, tag: &str, flags: u16) -> Vec<u8> {
    use ddm32_core::datapacks::datapack as dp;
    dp::image(dp::REF, payload, "20261119", "DDM32 TR 1.1.0", seq, tag, flags).unwrap()
}
