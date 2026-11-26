// SPDX-License-Identifier: GPL-3.0-or-later
#![allow(clippy::type_complexity, clippy::assertions_on_constants)]
//! The pack builder against tools/datapack.py (Fixtures/datapack-results.json from make_datapack_fixtures.py, synthetic
//! inputs only), building packs from files, the CPS pack writer on a fake radio and its region guard (the C#
//! DataPackTests, PackFilesTests, CpsPackTests). No serial port and no network.
mod common;
use common::*;
use ddm32_core::datapacks::datapack::{self as dp, PackSector, AIR, PYTHON_TAG, RADIO_ID};
use ddm32_core::datapacks::{air, files, pdf, radioid, PackError};
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::cps::{data_pack_tool, CpsPackPort};
use ddm32_core::radio::monitor::be32_bytes;
use ddm32_core::radio::port::{RadioPort, RawSerial, VirtualClock};
use ddm32_core::radio::tools::ToolContext;
use ddm32_core::radio::{ToolError, ToolResult};
use ddm32_core::releases::net::{HttpResponse, HttpTransport, NetError};
use serde_json::Value;
use std::cell::RefCell;
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::rc::Rc;
use std::sync::{Arc, Mutex, OnceLock};

fn r() -> &'static Value {
    static V: OnceLock<Value> = OnceLock::new();
    V.get_or_init(|| serde_json::from_slice(&std::fs::read(fixture(&["datapack-results.json"])).unwrap()).unwrap())
}

fn b(v: &Value) -> Vec<u8> {
    b64(v.as_str().unwrap())
}

fn strs(v: &Value) -> Vec<String> {
    v.as_array().unwrap().iter().map(|x| x.as_str().unwrap().to_string()).collect()
}

#[test]
fn radioid_packs_equal_datapack_py() {
    let mut n = 0;
    for c in r()["radioid"].as_array().unwrap() {
        let name = c["name"].as_str().unwrap();
        let csv = b(&c["csv"]);
        let countries = strs(&c["countries"]);
        if let Some(err) = c.get("error") {
            let e = radioid::read(&csv, &countries).unwrap_err();
            assert!(e.0.ends_with(&err.as_str().unwrap().replace("user.csv: ", "")), "{name}: {}", e.0);
            continue;
        }
        let (rows, skipped) = radioid::read(&csv, &countries).unwrap();
        let want: Vec<(i64, String, i64, String)> = c["rows"]
            .as_array()
            .unwrap()
            .iter()
            .map(|r| {
                (
                    r[0].as_i64().unwrap(),
                    r[1].as_str().unwrap().into(),
                    r[2].as_i64().unwrap(),
                    r[3].as_str().unwrap().into(),
                )
            })
            .collect();
        assert_eq!(
            want,
            rows.iter().map(|r| (r.id, r.call.clone(), r.packed, r.name.clone())).collect::<Vec<_>>(),
            "{name}"
        );
        let sk = &c["skipped"];
        assert_eq!(
            (
                sk["callsign"].as_u64().unwrap() as usize,
                sk["id"].as_u64().unwrap() as usize,
                sk["duplicate"].as_u64().unwrap() as usize
            ),
            (skipped.callsign, skipped.id, skipped.duplicate),
            "{name}"
        );
        let (payload, count, _) = dp::radioid_from_csv(&csv, &countries).unwrap();
        assert_eq!(c["count"].as_u64().unwrap() as usize, count);
        assert_eq!(b(&c["payload"]), payload, "{name} payload");
        assert_eq!(
            b(&c["image"]),
            dp::image(RADIO_ID, &payload, "20261121", "RadioID TR", 0, PYTHON_TAG, 0).unwrap(),
            "{name} image"
        );
        // image_cs: the C# program's tag; this program writes its own tag but the same header otherwise
        let cs = b(&c["image_cs"]);
        assert_eq!(
            cs,
            dp::image(RADIO_ID, &payload, "20261121", "RadioID TR", 7, "DDM32Plus-win 1", 0).unwrap(),
            "{name} C# tag"
        );
        n += 1;
    }
    assert_eq!(6, n);
}

#[test]
fn text_rules_equal_iddb() {
    let t = &r()["text"];
    for x in t["fold"].as_array().unwrap() {
        assert_eq!(x[1].as_str().unwrap(), radioid::fold(x[0].as_str().unwrap()), "{}", x[0]);
    }
    for x in t["country"].as_array().unwrap() {
        assert_eq!(x[1].as_str().unwrap(), radioid::country_key(x[0].as_str().unwrap()));
    }
    for x in t["short_name"].as_array().unwrap() {
        assert_eq!(x[2].as_str().unwrap(), radioid::short_name(x[0].as_str().unwrap(), x[1].as_str().unwrap()), "{x}");
    }
    for x in t["pack_call"].as_array().unwrap() {
        assert_eq!(x[1].as_i64(), radioid::pack_call(x[0].as_str().unwrap()), "{}", x[0]);
    }
}

#[test]
fn radioid_payload_widths_and_refusals() {
    let p = &r()["payloads"];
    let entries: Vec<(i64, String, String)> = p["entries"]
        .as_array()
        .unwrap()
        .iter()
        .map(|e| (e[0].as_i64().unwrap(), e[1].as_str().unwrap().into(), e[2].as_str().unwrap().into()))
        .collect();
    for w in p["widths"].as_array().unwrap() {
        let chars = w["chars"].as_u64().unwrap() as usize;
        match w.get("error") {
            Some(err) => assert_eq!(err.as_str().unwrap(), dp::radioid_payload(&entries, chars).unwrap_err().0),
            None => assert_eq!(b(&w["payload"]), dp::radioid_payload(&entries, chars).unwrap(), "width {chars}"),
        }
    }
    let refused = strs(&p["refused"]);
    let e = |v: Vec<(i64, &str, &str)>| {
        dp::radioid_payload(&v.into_iter().map(|(a, b, c)| (a, b.into(), c.into())).collect::<Vec<_>>(), 16)
            .unwrap_err()
            .0
    };
    assert_eq!(refused[0], e(vec![(1, "A", ""), (1, "B", "")]));
    assert_eq!(refused[1], e(vec![(0, "A", "")]));
    assert_eq!(refused[2], e(vec![(16777216, "A", "")]));
}

#[test]
fn air_packs_equal_datapack_py() {
    let a = &r()["air"];
    for (name, text) in a["texts"].as_object().unwrap() {
        let want: Vec<(i64, i64)> = a["parsed"][name]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| (x[0].as_i64().unwrap(), x[1].as_i64().unwrap()))
            .collect();
        assert_eq!(want, air::parse_ad218(text.as_str().unwrap()), "{name}");
    }
    for c in a["channel"].as_array().unwrap() {
        let (khz, why) = air::channel_khz(c[0].as_str().unwrap());
        assert_eq!(c[1].as_i64(), khz, "{}", c[0]);
        assert_eq!(c[2].as_str(), why, "{}", c[0]);
    }
    let fields: BTreeMap<String, Vec<(i64, i64)>> = a["fields"]
        .as_object()
        .unwrap()
        .iter()
        .map(|(k, v)| {
            (
                k.clone(),
                v.as_array().unwrap().iter().map(|x| (x[0].as_i64().unwrap(), x[1].as_i64().unwrap())).collect(),
            )
        })
        .collect();
    let payload = dp::air_payload(&fields).unwrap();
    assert_eq!(b(&a["payload"]), payload);
    assert_eq!(b(&a["image"]), dp::image(AIR, &payload, "20261121", "TR aerodromes", 0, PYTHON_TAG, 0).unwrap());
    assert_eq!(
        b(&a["image_cs"]),
        dp::image(AIR, &payload, "20261121", "TR aerodromes", 3, "DDM32Plus-win 1", 0).unwrap()
    );
    let refused = strs(&a["refused"]);
    let one = |code: &str| dp::air_payload(&BTreeMap::from([(code.to_string(), vec![(0, 118000)])])).unwrap_err().0;
    assert_eq!(refused[0], one(""));
    assert_eq!(refused[1], one("TOOLONGXX"));
}

#[test]
fn headers_equal_datapack_py() {
    for h in r()["headers"].as_array().unwrap() {
        let (t, payload) = (h["type"].as_u64().unwrap() as u8, b(&h["payload"]));
        let (date, name, seq, tag) = (
            h["date"].as_str().unwrap(),
            h["name"].as_str().unwrap(),
            h["seq"].as_u64().unwrap() as u32,
            h["tag"].as_str().unwrap(),
        );
        match h.get("error") {
            Some(err) => assert_eq!(
                err.as_str().unwrap().replace('\'', ""),
                dp::header(t, &payload, date, name, seq, tag, 0).unwrap_err().0.replace('\'', "")
            ),
            None => assert_eq!(b(&h["header"]), dp::header(t, &payload, date, name, seq, tag, 0).unwrap(), "{name}"),
        }
    }
}

#[test]
fn parse_and_info_equal_datapack_py() {
    for (name, c) in r()["parse"].as_object().unwrap() {
        let img = b(&c["image"]);
        let (h, payload) = dp::parse(&img);
        assert_eq!(c["info"].as_str().unwrap(), dp::info(&img), "{name}");
        let want = &c["header"];
        if want.is_null() {
            assert!(h.is_none() && payload.is_empty());
            continue;
        }
        let h = h.unwrap();
        assert_eq!(want["type"].as_u64().unwrap() as u8, h.ptype);
        assert_eq!(want["fmt"].as_u64().unwrap() as u8, h.format);
        assert_eq!(want["seq"].as_u64().unwrap() as u32, h.seq);
        assert_eq!(want["len"].as_u64().unwrap() as u32, h.length);
        assert_eq!(want["crc"].as_u64().unwrap() as u32, h.crc);
        assert_eq!(want["date"].as_str().unwrap(), h.date);
        assert_eq!(want["name"].as_str().unwrap(), h.name);
        assert_eq!(want["tag"].as_str().unwrap(), h.tag);
        assert_eq!(want["crc_ok"].as_bool().unwrap(), h.crc_ok);
        assert_eq!(b(&c["payload"]), payload, "{name}");
    }
}

#[test]
fn slot_choice_and_sequence_equal_datapack_py() {
    for s in r()["slots"].as_array().unwrap() {
        let imgs: Vec<Vec<u8>> = s["images"].as_array().unwrap().iter().map(b).collect();
        assert_eq!(s["inactive_radioid"].as_u64().unwrap() as usize, dp::inactive_slot(RADIO_ID, &imgs));
        assert_eq!(s["inactive_air"].as_u64().unwrap() as usize, dp::inactive_slot(AIR, &imgs));
        assert_eq!(s["next_seq"].as_u64().unwrap() as u32, dp::next_seq(&imgs).unwrap());
    }
}

#[test]
fn cps_plan_equals_datapack_py() {
    for p in r()["plans"].as_array().unwrap() {
        let plan = dp::cps_plan(p["address"].as_u64().unwrap(), &b(&p["image"])).unwrap();
        let want = p["plan"].as_array().unwrap();
        assert_eq!(want.len(), plan.len());
        for (i, w) in want.iter().enumerate() {
            assert_eq!(w[0].as_u64().unwrap(), plan[i].address);
            assert_eq!(b(&w[1]), plan[i].data, "sector {i}");
        }
    }
}

#[test]
fn the_region_guard_refuses_every_sector_outside() {
    assert!(dp::in_region(0xE70000, 4096));
    assert!(dp::in_region(0xEFF000, 4096));
    assert!(!dp::in_region(0xE6F000, 4096));
    assert!(!dp::in_region(0xE6FFFF, 2));
    assert!(!dp::in_region(0xF00000, 1));
    assert!(!dp::in_region(0xEFF001, 4096));
    assert!(!dp::in_region(0xE70000, 0));
    assert!(!dp::in_region(0x720000, 4096));
    assert!(dp::cps_plan(0xE6F000, &[0; 100]).is_err());
    assert!(dp::cps_plan(0xEFF000, &[0; 4097]).is_err());
    assert!(dp::cps_plan(0xF00000, &[0; 1]).is_err());
    assert!(dp::cps_plan(0, &[0; 1]).is_err());
    assert!(dp::plan_for_slot(AIR, 1, &vec![0; 0x4001]).is_err());
    assert!(dp::slot_address(AIR, 2).is_err());
    assert!(dp::slot_address(4, 0).is_err());
    let full = dp::plan_for_slot(AIR, 1, &vec![0; 0x4000]).unwrap();
    assert_eq!(vec![0xEF5000, 0xEF6000, 0xEF7000, 0xEF4000], full.iter().map(|s| s.address).collect::<Vec<_>>());
    let id = dp::plan_for_slot(RADIO_ID, 1, &vec![0; 0x30000]).unwrap();
    assert_eq!(48, id.len());
    assert!(id.iter().all(|s| (0xEB0000..0xEE0000).contains(&s.address)));
    assert!(dp::plan_for_slot(RADIO_ID, 1, &vec![0; 0x30001]).is_err());
}

#[test]
fn aerodrome_codes_come_from_the_file_name_or_the_text() {
    assert_eq!(Some("LTAC".into()), air::code_of("LT_AD_2_LTAC_en.pdf", None));
    assert_eq!(Some("LTBA".into()), air::code_of(r"C:\x\LTBA.pdf", None));
    assert_eq!(Some("LTFM".into()), air::code_of("LT-AD-2-LTFM-en.pdf", None));
    assert_eq!(Some("LTAD".into()), air::code_of("LTAD AD 2.pdf", None));
    assert_eq!(
        Some("LTAC".into()),
        air::code_of("page.pdf", Some("LTAC AD 2.17 x\nLTAC AD 2.18 ATS COMMUNICATION FACILITIES\nLTBA 118.1"))
    );
    assert_eq!(None, air::code_of("page.pdf", Some("no code here")));
}

/// A minimal one-page PDF with Helvetica text at the given positions (written by hand: no PDF writer needed).
fn synthetic_pdf(rows: &[(f64, f64, &str)]) -> Vec<u8> {
    let mut content = String::new();
    for (x, y, t) in rows {
        content.push_str(&format!("BT /F1 9 Tf {x} {y} Td ({}) Tj ET\n", t.replace('(', "\\(").replace(')', "\\)")));
    }
    let objs = [
        "<< /Type /Catalog /Pages 2 0 R >>".to_string(),
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>".to_string(),
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>".to_string(),
        format!("<< /Length {} >>\nstream\n{}endstream", content.len(), content),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>".to_string(),
    ];
    let mut out = b"%PDF-1.4\n".to_vec();
    let mut offsets = vec![];
    for (i, o) in objs.iter().enumerate() {
        offsets.push(out.len());
        out.extend(format!("{} 0 obj\n{}\nendobj\n", i + 1, o).into_bytes());
    }
    let xref = out.len();
    out.extend(format!("xref\n0 {}\n0000000000 65535 f \n", objs.len() + 1).into_bytes());
    for o in offsets {
        out.extend(format!("{o:010} 00000 n \n").into_bytes());
    }
    out.extend(format!("trailer\n<< /Size {} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n", objs.len() + 1).into_bytes());
    out
}

#[test]
fn a_synthetic_aip_pdf_gives_its_table_rows() {
    let rows = [
        (40.0, 800.0, "LTXX AD 2.17 AIR TRAFFIC SERVICES AIRSPACE"),
        (40.0, 780.0, "LTXX AD 2.18 ATS COMMUNICATION FACILITIES"),
        (40.0, 760.0, "Service designation"),
        (160.0, 760.0, "Call sign"),
        (300.0, 760.0, "Frequency"),
        (400.0, 760.0, "Hours"),
        (40.0, 740.0, "ATIS"),
        (160.0, 740.0, "TESTFIELD INFORMATION"),
        (300.0, 740.0, "123.600 MHZ"),
        (400.0, 740.0, "H24"),
        (40.0, 720.0, "TWR"),
        (160.0, 720.0, "TESTFIELD TOWER"),
        (300.0, 720.0, "118.100 MHZ"),
        (400.0, 720.0, "H24"),
        (300.0, 708.0, "121.500 MHZ"),
        (400.0, 708.0, "EMERG"),
        (40.0, 690.0, "GND"),
        (160.0, 690.0, "TESTFIELD GROUND"),
        (300.0, 690.0, "121.900 MHZ"),
        (40.0, 670.0, "LTXX AD 2.19 RADIO NAVIGATION AND LANDING AIDS"),
        (40.0, 650.0, "VOR"),
        (300.0, 650.0, "112.500 MHZ"),
    ];
    let t = tmp();
    let path = t.path().join("LT_AD_2_LTXX_en.pdf");
    std::fs::write(&path, synthetic_pdf(&rows)).unwrap();
    assert_eq!(1, pdf::pages(&path));
    let text = pdf::read(&path).unwrap();
    let ti = |n: &str| air::TYPES.iter().position(|t| *t == n).unwrap() as i64;
    assert_eq!(
        vec![(ti("ATIS"), 123600), (ti("TWR"), 118100), (ti("EMR"), 121500), (ti("GND"), 121900)],
        air::parse_ad218(&text),
        "{text}"
    );
    assert_eq!(Some("LTXX".into()), air::code_of(&path.to_string_lossy(), Some(&text)));
    let bad = t.path().join("bad.pdf");
    std::fs::write(&bad, "not a pdf").unwrap();
    assert!(pdf::read(&bad).is_err());
    assert_eq!(0, pdf::pages(&bad));
}

// --- building packs from the user's files ---

const CSV: &str = "RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY\n2860105,TA2XYZ,Ayşe,Kaya,Izmir,,Turkey\n2860001,ta1abc,Ahmet,Yilmaz,Ankara,,Turkey\n3109999,W1AW,Hiram,Maxim,Newington,CT,United States\n";

#[test]
fn packs_build_from_the_users_files() {
    let t = tmp();
    std::fs::write(t.path().join("user.csv"), CSV).unwrap();
    let rid = files::radio_id(&t.path().join("user.csv"), &[], "RadioID TR", Some("20261121")).unwrap();
    assert_eq!(2, rid.count);
    assert_eq!("RadioID TR", rid.header.name);
    assert_eq!(dp::TAG, rid.header.tag);
    assert_eq!(0, rid.header.seq);
    assert!(files::radio_id(&t.path().join("user.csv"), &["Atlantis".into()], "x", None).is_err());
    std::fs::write(t.path().join("LTAC.txt"), "LTAC AD 2.18\nTWR 118.100\nGND 121.900\n").unwrap();
    std::fs::write(t.path().join("other.txt"), "TWR 119.000\n").unwrap();
    let src = files::read_air_source(&t.path().join("LTAC.txt")).unwrap();
    assert_eq!(Some("LTAC".into()), src.code);
    assert_eq!(2, src.freqs.len());
    let a = files::air_pack(std::slice::from_ref(&src), "TR aerodromes", Some("20261121")).unwrap();
    assert_eq!(1, a.count);
    assert!(files::air_pack(&[files::read_air_source(&t.path().join("other.txt")).unwrap()], "x", None).is_err());
    assert!(files::air_pack(&[], "x", None).is_err());
    std::fs::write(t.path().join("a.dpk"), &a.image).unwrap();
    assert_eq!(a.image, files::load(&t.path().join("a.dpk")).unwrap().image);
    let mut bad = a.image.clone();
    *bad.last_mut().unwrap() ^= 1;
    std::fs::write(t.path().join("b.dpk"), bad).unwrap();
    assert!(files::load(&t.path().join("b.dpk")).is_err());
}

#[test]
fn a_backup_shows_its_slots() {
    let t = tmp();
    let mut q = vec![0xFFu8; 1 << 24];
    let img = dp::image(
        AIR,
        &dp::air_payload(&BTreeMap::from([("LTAC".to_string(), vec![(4, 118100)])])).unwrap(),
        "20261121",
        "Air",
        2,
        dp::TAG,
        0,
    )
    .unwrap();
    q[0xEF4000..0xEF4000 + img.len()].copy_from_slice(&img);
    std::fs::write(t.path().join("q128-pass1.bin"), &q).unwrap();
    let slots = files::slots_from_backup(t.path(), AIR).unwrap();
    assert_eq!(Some(1), dp::active_slot(AIR, &slots));
    assert_eq!(None, dp::active_slot(RADIO_ID, &files::slots_from_backup(t.path(), RADIO_ID).unwrap()));
    std::fs::write(t.path().join("q128-pass1.bin"), [0u8; 100]).unwrap();
    assert!(files::slots_from_backup(t.path(), AIR).is_err());
}

struct Fake {
    asked: Arc<Mutex<Vec<String>>>,
    answer: Box<dyn Fn(&str) -> (u16, Option<String>, Vec<u8>) + Send + Sync>,
}

impl HttpTransport for Fake {
    fn get(&self, url: &str, _a: &str) -> Result<HttpResponse, NetError> {
        self.asked.lock().unwrap().push(url.into());
        let (s, l, b) = (self.answer)(url);
        Ok(HttpResponse { status: s, reason: "X".into(), location: l, body: Box::new(std::io::Cursor::new(b)) })
    }
}

#[test]
fn the_radioid_list_comes_only_from_radioid_net() {
    let t = tmp();
    let asked = Arc::new(Mutex::new(vec![]));
    let d = files::RadioIdDownloader::new(Box::new(Fake {
        asked: asked.clone(),
        answer: Box::new(|u| {
            if u.ends_with("/static/user.csv") {
                (302, Some("https://www.radioid.net/static/u2.csv".into()), vec![])
            } else {
                (200, None, CSV.as_bytes().to_vec())
            }
        }),
    }));
    d.download(&t.path().join("user.csv"), None).unwrap();
    assert_eq!(CSV, std::fs::read_to_string(t.path().join("user.csv")).unwrap());
    assert_eq!(
        vec!["https://radioid.net/static/user.csv", "https://www.radioid.net/static/u2.csv"],
        *asked.lock().unwrap()
    );
    let asked = Arc::new(Mutex::new(vec![]));
    let evil = files::RadioIdDownloader::new(Box::new(Fake {
        asked: asked.clone(),
        answer: Box::new(|_| (301, Some("https://example.com/user.csv".into()), vec![])),
    }));
    assert!(evil.download(&t.path().join("x.csv"), None).is_err());
    assert_eq!(1, asked.lock().unwrap().len());
    let plain = files::RadioIdDownloader::new(Box::new(Fake {
        asked: Arc::new(Mutex::new(vec![])),
        answer: Box::new(|_| (302, Some("http://radioid.net/x".into()), vec![])),
    }));
    assert!(plain.download(&t.path().join("x.csv"), None).is_err());
    assert!(!t.path().join("x.csv").exists());
    assert!(files::check_radioid_url("https://user@radioid.net/static/user.csv").is_err());
}

// --- the CPS pack writer on a fake DDM32 Plus radio (cps_openfw.c's behaviour) ---

#[derive(Default)]
struct FakeCps {
    sectors: HashMap<u64, Vec<u8>>,
    replies: VecDeque<u8>,
    pending: Option<Vec<u8>>,
    pending_addr: u64,
    requests: Vec<Vec<u8>>,
    commits: Vec<u64>,
    radio_type: u32,
    corrupt_reads_of: Option<u64>,
    refuse_commit_number: Option<usize>,
    opened: bool,
    closes: usize,
    /// The CPS info's git field (12 bytes; "ddm32plus-b4" when not set: a version the tool cannot tell)
    git: Option<[u8; 15]>,
}

impl FakeCps {
    fn new() -> Self {
        FakeCps { radio_type: 11, ..Default::default() }
    }
    fn sector(&self, addr: u64) -> Vec<u8> {
        self.sectors.get(&(addr / 4096)).cloned().unwrap_or_else(|| vec![0xFF; 4096])
    }
    fn put(&mut self, addr: u64, data: &[u8]) {
        for (i, &x) in data.iter().enumerate() {
            let a = addr + i as u64;
            let s = self.sectors.entry(a / 4096).or_insert_with(|| vec![0xFF; 4096]);
            s[(a % 4096) as usize] = x;
        }
    }
    fn get(&self, addr: u64, n: usize) -> Vec<u8> {
        (0..n as u64).map(|i| self.sector(addr + i)[((addr + i) % 4096) as usize]).collect()
    }
    fn reply(&mut self, b: &[u8]) {
        self.replies.extend(b);
    }
}

impl RadioPort for FakeCps {
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
    fn write(&mut self, f: &[u8]) -> ToolResult<Option<usize>> {
        self.requests.push(f.to_vec());
        match f[0] {
            0x52 => {
                let addr = u32::from_be_bytes([f[2], f[3], f[4], f[5]]) as u64;
                let len = (f[6] as usize) << 8 | f[7] as usize;
                if f[1] == 9 {
                    let mut info = vec![0u8; 46];
                    info[..4].copy_from_slice(&3u32.to_le_bytes());
                    info[4..8].copy_from_slice(&self.radio_type.to_le_bytes());
                    match self.git {
                        Some(g) => info[8..23].copy_from_slice(&g),
                        None => info[8..20].copy_from_slice(b"ddm32plus-b4"),
                    }
                    info[24..35].copy_from_slice(b"Nov 21 2026");
                    info[40..44].copy_from_slice(&0xEF4018u32.to_le_bytes());
                    self.reply(&[0x52, 0, 46]);
                    self.reply(&info);
                } else if f[1] != 1 || addr + len as u64 > 1 << 24 || len == 0 {
                    self.reply(&[0x2D]);
                } else {
                    let mut data = self.get(addr, len);
                    if let Some(bad) = self.corrupt_reads_of {
                        if bad >= addr && bad < addr + len as u64 && self.commits.contains(&(bad / 4096 * 4096)) {
                            data[(bad - addr) as usize] ^= 0x40;
                        }
                    }
                    self.reply(&[0x52, (len >> 8) as u8, len as u8]);
                    self.reply(&data);
                }
            }
            0x43 => self.reply(&[0x2D]),
            0x58 => match f[1] {
                1 => {
                    let addr = ((f[2] as u64) << 16 | (f[3] as u64) << 8 | f[4] as u64) * 4096;
                    if self.pending.is_some() || !(0x720000..0xF00000).contains(&addr) {
                        self.pending = None;
                        self.reply(&[0x2D]);
                    } else {
                        self.pending = Some(self.sector(addr));
                        self.pending_addr = addr;
                        self.reply(&[0x58, 1]);
                    }
                }
                2 => {
                    if self.pending.is_none() {
                        self.reply(&[0x2D]);
                    } else {
                        let addr = u32::from_be_bytes([f[2], f[3], f[4], f[5]]) as u64;
                        let len = ((f[6] as usize) << 8 | f[7] as usize).min(f.len() - 8);
                        let pa = self.pending_addr;
                        let p = self.pending.as_mut().unwrap();
                        for i in 0..len {
                            let a = addr + i as u64;
                            if a >= pa && a < pa + 4096 {
                                p[(a - pa) as usize] = f[8 + i];
                            }
                        }
                        self.reply(&[0x58, 2]);
                    }
                }
                3 => {
                    if self.pending.is_none() || self.refuse_commit_number == Some(self.commits.len() + 1) {
                        self.pending = None;
                        self.reply(&[0x2D]);
                    } else {
                        let p = self.pending.take().unwrap();
                        self.sectors.insert(self.pending_addr / 4096, p);
                        self.commits.push(self.pending_addr);
                        self.reply(&[0x58, 3]);
                    }
                }
                _ => self.reply(&[0x2D]),
            },
            _ => self.reply(&[0x2D]),
        }
        Ok(Some(f.len()))
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        let k = n.min(self.replies.len());
        Ok(self.replies.drain(..k).collect())
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        self.replies.clear();
        Ok(())
    }
    fn flush(&mut self) -> ToolResult<()> {
        Ok(())
    }
}

impl RawSerial for FakeCps {
    fn open(&mut self) -> ToolResult<()> {
        self.opened = true;
        Ok(())
    }
    fn close(&mut self) -> ToolResult<()> {
        self.closes += 1;
        Ok(())
    }
}

fn radioid_image(n: usize, seq: u32, name: &str) -> Vec<u8> {
    let entries: Vec<(i64, String, String)> = (0..n)
        .map(|i| {
            (2860000 + i as i64 * 3, format!("TA{}A{}", i % 10, (b'A' + (i % 26) as u8) as char), format!("Name{i}"))
        })
        .collect();
    dp::image(RADIO_ID, &dp::radioid_payload(&entries, 16).unwrap(), "20261121", name, seq, dp::TAG, 0).unwrap()
}

fn air_image(seq: u32) -> Vec<u8> {
    dp::image(
        AIR,
        &dp::air_payload(&BTreeMap::from([("LTAC".to_string(), vec![(0, 123600), (4, 118100)])])).unwrap(),
        "20261121",
        "TR aerodromes",
        seq,
        dp::TAG,
        0,
    )
    .unwrap()
}

fn run(radio: &Rc<RefCell<FakeCps>>, args: &[&str]) -> (Vec<String>, Option<ToolError>) {
    let lines = Lines::default();
    let print = lines.print();
    let clock = VirtualClock::default();
    let r2 = radio.clone();
    let serial = move |_: &str| -> ToolResult<Box<dyn RawSerial>> { Ok(Box::new(Shared(r2.clone()))) };
    let ctx =
        ToolContext { print: &print, clock: &clock, serial: &serial, release_key_hex: None, codec: Codec::dm32() };
    let a: Vec<String> = args.iter().map(|s| s.to_string()).collect();
    let e = data_pack_tool(&a, &ctx).err();
    (lines.get(), e)
}

fn pack_file(t: &tempfile::TempDir, image: &[u8], name: &str) -> String {
    let p = t.path().join(name);
    std::fs::write(&p, image).unwrap();
    p.to_string_lossy().into_owned()
}

fn only_region_written(radio: &FakeCps) {
    assert!(radio.commits.iter().all(|a| (0xE70000..0xF00000).contains(a)));
    let mut d = radio.commits.clone();
    d.sort();
    d.dedup();
    assert_eq!(radio.commits.len(), d.len());
    assert!(!radio.requests.iter().any(|f| f[0] == 0x43 && f[1] == 6 && f.len() > 2 && f[2] != 4));
    assert!(!radio.requests.iter().any(|f| f[0] == 0x58 && !(1..=3).contains(&f[1])));
    assert!(!radio.requests.iter().any(|f| ![0x52, 0x43, 0x58].contains(&f[0])));
}

#[test]
fn cps_read_shows_both_slots_of_each_type() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    radio.borrow_mut().put(0xEB0000, &radioid_image(20, 3, "RadioID TR"));
    let (lines, err) = run(&radio, &["read", "--port", "COM9"]);
    assert!(err.is_none(), "{err:?}");
    assert!(lines.iter().any(|l| l.contains("\"radio_type\": 11")));
    let slots: Vec<&String> = lines.iter().filter(|l| l.starts_with("{\"slot\"")).collect();
    assert_eq!(6, slots.len());
    assert!(slots[2].contains("\"type\": \"ref\"") && slots[2].contains("\"state\": \"empty\""));
    assert!(slots[4].contains("\"type\": \"air\""));
    assert!(slots[1].contains("\"in_force\": true") && slots[1].contains("\"count\": 20"));
    assert!(slots[0].contains("\"valid\": false"));
    assert_eq!("{\"result\": \"READ\"}", lines.last().unwrap());
    let r = radio.borrow();
    assert!(r.commits.is_empty());
    assert!(r.requests.iter().all(|f| f[0] == 0x52));
    assert_eq!(1, r.closes);
}

#[test]
fn cps_a_first_pack_goes_to_slot_zero_header_last() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    let t = tmp();
    let image = radioid_image(700, 0, "RadioID TR");
    let (lines, err) = run(&radio, &["write", "--port", "COM9", "--pack", &pack_file(&t, &image, "p.dpk")]);
    assert!(err.is_none(), "{err:?}");
    let r = radio.borrow();
    assert_eq!(vec![0xE71000, 0xE72000, 0xE70000], r.commits);
    let target = dp::image(RADIO_ID, &image[64..], "20261121", "RadioID TR", 1, dp::TAG, 0).unwrap();
    assert_eq!(target, r.get(0xE70000, target.len()));
    assert!(lines.last().unwrap().starts_with("{\"result\": \"PASS\", \"slot\": 0, \"seq\": 1"));
    only_region_written(&r);
    let controls: Vec<u8> = r.requests.iter().filter(|f| f[0] == 0x43).map(|f| f[1]).collect();
    assert_eq!(vec![0, 1, 2, 2, 3, 6, 5], controls);
}

#[test]
fn cps_the_new_pack_goes_where_the_pack_in_force_is_not() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    let t = tmp();
    let old = radioid_image(30, 4, "old");
    radio.borrow_mut().put(0xE70000, &old);
    let (_, err) = run(
        &radio,
        &["write", "--port", "COM9", "--pack", &pack_file(&t, &radioid_image(40, 0, "RadioID TR"), "p.dpk")],
    );
    assert!(err.is_none());
    {
        let r = radio.borrow();
        assert!(r.commits.iter().all(|a| (0xEB0000..0xEF0000).contains(a)));
        assert_eq!(old, r.get(0xE70000, old.len()));
        assert_eq!(5, dp::parse(&r.get(0xEB0000, 64 + 8 + 40 * 15)).0.unwrap().seq);
    }
    radio.borrow_mut().commits.clear();
    let (_, err) = run(
        &radio,
        &["write", "--port", "COM9", "--pack", &pack_file(&t, &radioid_image(50, 0, "RadioID TR"), "q.dpk")],
    );
    assert!(err.is_none());
    let r = radio.borrow();
    assert!(r.commits.iter().all(|a| (0xE70000..0xEB0000).contains(a)));
    assert_eq!(6, dp::parse(&r.get(0xE70000, 64 + 8 + 50 * 15)).0.unwrap().seq);
    only_region_written(&r);
}

#[test]
fn cps_an_air_pack_uses_its_own_slots() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    let t = tmp();
    radio.borrow_mut().put(0xE70000, &radioid_image(10, 8, "RadioID TR"));
    let (lines, err) = run(&radio, &["write", "--port", "COM9", "--pack", &pack_file(&t, &air_image(0), "a.dpk")]);
    assert!(err.is_none());
    assert_eq!(vec![0xEF0000], radio.borrow().commits);
    assert!(lines.last().unwrap().contains("\"seq\": 1"));
    only_region_written(&radio.borrow());
}

#[test]
fn cps_a_bad_readback_stops_at_once_and_the_pack_in_force_stays() {
    let radio = Rc::new(RefCell::new(FakeCps { corrupt_reads_of: Some(0xEB1000 + 77), ..FakeCps::new() }));
    let t = tmp();
    let old = radioid_image(30, 4, "old");
    radio.borrow_mut().put(0xE70000, &old);
    let (lines, err) = run(
        &radio,
        &["write", "--port", "COM9", "--pack", &pack_file(&t, &radioid_image(700, 0, "RadioID TR"), "p.dpk")],
    );
    assert_eq!("VerifyError", err.unwrap().py_type);
    let r = radio.borrow();
    assert_eq!(vec![0xEB1000], r.commits);
    assert_eq!(old, r.get(0xE70000, old.len()));
    assert_eq!(Some(0), dp::active_slot(RADIO_ID, &[r.get(0xE70000, old.len()), r.get(0xEB0000, 4096)]));
    assert_eq!(&vec![0x43, 5], r.requests.last().unwrap());
    assert!(!lines.iter().any(|l| l.contains("PASS")));
    only_region_written(&r);
}

#[test]
fn cps_a_refused_commit_is_never_retried() {
    let radio = Rc::new(RefCell::new(FakeCps { refuse_commit_number: Some(2), ..FakeCps::new() }));
    let t = tmp();
    let (_, err) = run(
        &radio,
        &["write", "--port", "COM9", "--pack", &pack_file(&t, &radioid_image(700, 0, "RadioID TR"), "p.dpk")],
    );
    assert_eq!("CpsError", err.unwrap().py_type);
    let r = radio.borrow();
    assert_eq!(vec![0xE71000], r.commits);
    assert_eq!(2, r.requests.iter().filter(|f| f[0] == 0x58 && f[1] == 3).count());
    assert_eq!(2, r.requests.iter().filter(|f| f[0] == 0x58 && f[1] == 1).count());
    only_region_written(&r);
}

#[test]
fn cps_another_radio_is_refused_before_any_write() {
    let radio = Rc::new(RefCell::new(FakeCps { radio_type: 6, ..FakeCps::new() }));
    let t = tmp();
    let (_, err) = run(&radio, &["write", "--port", "COM9", "--pack", &pack_file(&t, &air_image(0), "a.dpk")]);
    assert_eq!("RadioError", err.unwrap().py_type);
    assert_eq!(1, radio.borrow().requests.len());
}

#[test]
fn cps_a_broken_pack_file_opens_no_port() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    let t = tmp();
    let mut img = radioid_image(10, 0, "RadioID TR");
    img[80] ^= 1;
    let (_, err) = run(&radio, &["write", "--port", "COM9", "--pack", &pack_file(&t, &img, "p.dpk")]);
    assert_eq!("ValueError", err.unwrap().py_type);
    assert!(!radio.borrow().opened);
    let (_, err) = run(
        &radio,
        &["write", "--port", "COM9", "--pack", &pack_file(&t, &[air_image(0), vec![0xFF]].concat(), "x.dpk")],
    );
    assert_eq!("ValueError", err.unwrap().py_type);
    assert!(!radio.borrow().opened);
    assert_eq!("SystemExit", run(&radio, &["erase", "--port", "COM9"]).1.unwrap().py_type);
}

fn v(g: &mut CpsPackPort, f: &[u8]) -> String {
    match g.validate(f) {
        Ok(()) => "ok".into(),
        Err(e) => e.message,
    }
}

fn x2(addr: u64, data: &[u8]) -> Vec<u8> {
    [vec![0x58, 2], be32_bytes(addr).to_vec(), vec![(data.len() >> 8) as u8, data.len() as u8], data.to_vec()].concat()
}
fn x1(addr: u64) -> Vec<u8> {
    let s = addr / 4096;
    vec![0x58, 1, (s >> 16) as u8, (s >> 8) as u8, s as u8]
}
fn r1(addr: u64, n: usize) -> Vec<u8> {
    [vec![0x52, 1], be32_bytes(addr).to_vec(), vec![(n >> 8) as u8, n as u8]].concat()
}

fn cps_port() -> CpsPackPort {
    CpsPackPort::new(Box::new(FakeCps::new()))
}

#[test]
fn cps_the_guard_refuses_what_it_must() {
    let mut g = cps_port();
    assert_eq!("ok", v(&mut g, &r1(0xE70000, 64)));
    assert_eq!("ok", v(&mut g, &r1(0xEFFC00, 1024)));
    assert_eq!("ok", v(&mut g, &[0x52, 9, 0, 0, 0, 0, 0, 0x20]));
    for f in [
        r1(0x720000, 32),
        r1(0xE6FFFF, 2),
        r1(0xEFFFFF, 2),
        r1(0xF00000, 1),
        r1(0xE70000, 4097),
        r1(0xE70000, 0),
        vec![0x52, 5, 0, 0, 0, 0, 0, 32],
        vec![0x52, 2, 0, 0, 0, 0, 0, 32],
        vec![0x52, 10, 0, 0, 0, 0, 0, 32],
        x1(0xE70000),
        x2(0xE70000, &[0; 32]),
        vec![0x58, 3],
        vec![0x43, 0],
        vec![0x43, 5],
        vec![0x57, 1, 0, 0x0E, 0x70],
        vec![0x44, 1, 2, 3],
        vec![0xE0, 4, 0, 0],
        vec![0x52, 1],
    ] {
        assert!(v(&mut g, &f).starts_with("refused"), "{f:?}");
    }
    let data: Vec<u8> = (0..4096).map(|i| i as u8).collect();
    let hdr: Vec<u8> = (0..4096).map(|i| (i * 3) as u8).collect();
    let sec = |a: u64, d: &[u8]| PackSector { address: a, data: d.to_vec() };
    assert!(cps_port().arm(&[sec(0xE6F000, &data)]).is_err());
    assert!(cps_port().arm(&[sec(0xF00000, &data)]).is_err());
    assert!(cps_port().arm(&[sec(0xE70800, &data)]).is_err());
    assert!(cps_port().arm(&[sec(0xE70000, &data), sec(0xE70000, &data)]).is_err());
    assert!(cps_port().arm(&[]).is_err());
    g.arm(&[sec(0xEB1000, &data), sec(0xEB0000, &hdr)]).unwrap();
    assert!(g.arm(&[sec(0xEB1000, &data)]).is_err());
    for f in [
        vec![0x43, 6, 0],
        vec![0x43, 6, 1],
        vec![0x43, 6, 2],
        vec![0x43, 6, 5],
        vec![0x43, 6, 7],
        vec![0x43, 7],
        vec![0x43, 254],
        vec![0x43, 0x50],
        vec![0x58, 4, 0, 0, 0, 0, 0, 1, 0],
        vec![0x58, 7, 0, 0, 0, 0, 0, 1, 0],
        vec![0x58, 9],
        x1(0xEB0000),
        x1(0xE70000),
        x1(0xEF0000),
        x1(0x720000),
        x1(0xF00000),
        x1(0),
        x2(0xEB1000, &data[..32]),
        vec![0x58, 3],
    ] {
        assert!(v(&mut g, &f).starts_with("refused"), "{f:?}");
    }
    for f in [vec![0x43, 0], vec![0x43, 1], vec![0x43, 2, 0, 0, 1, 1, 0, 0x41, 0], vec![0x43, 3], vec![0x43, 6, 4]] {
        assert_eq!("ok", v(&mut g, &f));
    }
    assert_eq!("ok", v(&mut g, &x1(0xEB1000)));
    assert!(v(&mut g, &x1(0xEB0000)).starts_with("refused"));
    assert!(v(&mut g, &x2(0xEB0FE0, &data[..64])).starts_with("refused"));
    assert!(v(&mut g, &x2(0xEB1000, &[data[..31].to_vec(), vec![0xAA]].concat())).starts_with("refused"));
    assert!(v(&mut g, &[vec![0x58, 2], be32_bytes(0xEB1000).to_vec(), vec![0, 40], data[..32].to_vec()].concat())
        .starts_with("refused"));
    assert_eq!("ok", v(&mut g, &x2(0xEB1000, &data[..2048])));
    assert!(v(&mut g, &[0x58, 3]).starts_with("refused"));
    assert_eq!("ok", v(&mut g, &x2(0xEB1800, &data[2048..])));
    assert_eq!("ok", v(&mut g, &[0x58, 3]));
    assert!(v(&mut g, &x1(0xEB1000)).starts_with("refused"));
    assert_eq!("ok", v(&mut g, &x1(0xEB0000)));
    assert_eq!("ok", v(&mut g, &x2(0xEB0000, &hdr)));
    assert_eq!("ok", v(&mut g, &[0x58, 3]));
    assert!(v(&mut g, &x1(0xEB2000)).starts_with("refused"));
    assert_eq!(2, g.committed);
    assert!(g.set_baud_rate(921600).is_err());
}

#[test]
fn cps_a_refused_frame_never_reaches_the_port() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    let mut g = CpsPackPort::new(Box::new(Shared(radio.clone())));
    assert!(g.write(&[0x43, 6, 1]).is_err());
    assert!(g.write(&x1(0xE70000)).is_err());
    assert!(radio.borrow().requests.is_empty());
    assert_eq!(2, g.refusals.len());
}

#[test]
fn pack_errors_are_plain_messages() {
    assert_eq!("x", PackError("x".into()).to_string());
}

// --- the map pack (type 3) through the CPS writer (docs/mappack-design.md E.3) ---

use ddm32_core::datapacks::refpack as rp;

fn git(v: &str) -> Option<[u8; 15]> {
    let mut g = [0u8; 15];
    g[..v.len()].copy_from_slice(v.as_bytes());
    Some(g)
}

fn release_pack() -> Vec<u8> {
    ref_image(&ref_payload(COORD, &two_rpts(), "own+RadioID.net"), 0, "build 1.1.0", rp::hint(COORD))
}

#[test]
fn cps_a_map_pack_goes_to_its_slot_header_last_with_its_flags() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    radio.borrow_mut().git = git("ddm32-plus-1.1.");
    let t = tmp();
    let image = release_pack();
    let coord = format!("0x{COORD:08X}");
    let (lines, err) =
        run(&radio, &["write", "--port", "COM9", "--pack", &pack_file(&t, &image, "r.dpk"), "--coord", &coord]);
    assert!(err.is_none(), "{err:?}");
    let r = radio.borrow();
    let n = image.len().div_ceil(4096) as u64;
    let mut want: Vec<u64> = (1..n).map(|i| 0xEA0000 + i * 4096).collect();
    want.push(0xEA0000);
    assert_eq!(want, r.commits);
    let written = r.get(0xEA0000, image.len());
    assert_eq!(written[6..8], rp::hint(COORD).to_le_bytes(), "flags carried");
    assert_eq!(1, u32::from_le_bytes([written[8], written[9], written[10], written[11]]));
    assert_eq!(written[64..], image[64..]);
    assert!(lines
        .iter()
        .any(|l| l.starts_with("{\"plan\": {\"type\": \"ref\", \"slot\": 0, \"address\": \"0xea0000\"")));
    assert!(lines.iter().any(|l| l.contains("\"type\": \"ref\"") && l.contains("\"state\": \"valid\"")));
    assert!(lines.last().unwrap().starts_with("{\"result\": \"PASS\", \"slot\": 0, \"seq\": 1"));
    only_region_written(&r);
}

#[test]
fn cps_a_map_pack_that_does_not_check_never_opens_the_port() {
    let t = tmp();
    // for another firmware's coordinate id
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    let (_, err) =
        run(&radio, &["write", "--port", "COM9", "--pack", &pack_file(&t, &release_pack(), "r.dpk"), "--coord", "0x1"]);
    assert!(err.unwrap().message.contains("not for this firmware"));
    assert!(!radio.borrow().opened);
    // structurally bad
    let mut p = ref_payload(COORD, &two_rpts(), "x");
    let d = rp::directory(&p).unwrap();
    let k = d.get(rp::RPT_KEY).unwrap().off as usize;
    p[k] ^= 1;
    let (_, err) =
        run(&radio, &["write", "--port", "COM9", "--pack", &pack_file(&t, &ref_image(&p, 0, "x", 0), "b.dpk")]);
    assert!(err.unwrap().message.contains("does not check"));
    // a RadioID pack over 192 KB (as 3.1 could build): refused for every firmware
    let payload = vec![0u8; 200_000];
    let mut h = dp::header(RADIO_ID, &[], "20261119", "big", 0, "t", 0).unwrap();
    h[12..16].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    h[16..20].copy_from_slice(&ddm32_core::radio::monitor::crc32(&payload, 0).to_le_bytes());
    let hc = ddm32_core::radio::monitor::crc32(&h[..60], 0);
    h[60..64].copy_from_slice(&hc.to_le_bytes());
    let big = [h, payload].concat();
    let (_, err) = run(&radio, &["write", "--port", "COM9", "--pack", &pack_file(&t, &big, "big.dpk")]);
    assert!(err.unwrap().message.contains("192 KB"));
    assert!(!radio.borrow().opened);
    assert!(radio.borrow().requests.is_empty());
}

#[test]
fn cps_the_running_firmwares_map_slot_is_kept() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    radio.borrow_mut().git = git("ddm32-plus-1.1.");
    let x = 0x0BAD_F00D;
    // slot A: the running 1.1's pack (coordinate X), slot B: empty; the target (1.2, COORD) rejects A
    radio.borrow_mut().put(0xEA0000, &ref_image(&ref_payload(x, &two_rpts(), "x"), 6, "build 1.1.0", 0));
    let t = tmp();
    let coord = format!("0x{COORD:08X}");
    let known = format!("1.1.0=0x{x:08X},1.2.0=0x{COORD:08X}");
    let (lines, err) = run(
        &radio,
        &[
            "write",
            "--port",
            "COM9",
            "--pack",
            &pack_file(&t, &release_pack(), "r.dpk"),
            "--coord",
            &coord,
            "--known-coords",
            &known,
        ],
    );
    assert!(err.is_none(), "{err:?}");
    assert!(radio.borrow().commits.iter().all(|a| (0xEE0000..0xEF0000).contains(a)));
    assert!(lines.last().unwrap().starts_with("{\"result\": \"PASS\", \"slot\": 1, \"seq\": 7"));
}

#[test]
fn cps_an_old_firmware_and_a_big_radioid_pack_are_said() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    radio.borrow_mut().git = git("ddm32-plus-1.0.");
    // a 1.0.6 RadioID pack of 200 KB in slot B (its header at 0xEB0000)
    let mut h = dp::header(RADIO_ID, &[], "20261119", "multi", 2, "t", 0).unwrap();
    h[12..16].copy_from_slice(&200_000u32.to_le_bytes());
    let hc = ddm32_core::radio::monitor::crc32(&h[..60], 0);
    h[60..64].copy_from_slice(&hc.to_le_bytes());
    radio.borrow_mut().put(0xEB0000, &h);
    let t = tmp();
    let (lines, err) = run(&radio, &["write", "--port", "COM9", "--pack", &pack_file(&t, &release_pack(), "r.dpk")]);
    assert!(err.is_none(), "{err:?}");
    assert!(lines.iter().any(|l| l == "{\"note\": \"ref_after_update\"}"));
    assert!(lines.iter().any(|l| l == "{\"warning\": \"radioid_big\", \"slot\": 1}"));
    // the read shows it as too big, not damaged
    let (lines, _) = run(&radio, &["read", "--port", "COM9"]);
    assert!(lines.iter().any(|l| l.contains("\"address\": \"0xeb0000\"") && l.contains("\"state\": \"big\"")));
}

#[test]
fn cps_update_ref_asks_before_replacing_a_user_built_pack() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    radio.borrow_mut().git = git("ddm32-plus-1.1.");
    let own = rpt_rec(0, 0, 430_100_000, 0, "TA9OWN", 1);
    let mine = ref_image(&ref_payload(COORD, std::slice::from_ref(&own), "own list"), 3, "refpack.py own", 0);
    radio.borrow_mut().put(0xEE0000, &mine);
    let t = tmp();
    let f = pack_file(&t, &release_pack(), "r.dpk");
    let coord = format!("0x{COORD:08X}");
    let known = format!("1.1.0=0x{COORD:08X}");
    let base = ["update-ref", "--port", "COM9", "--pack", &f, "--coord", &coord, "--known-coords", &known];
    // no choice: ASK, nothing written
    let (lines, err) = run(&radio, &base);
    assert!(err.is_none(), "{err:?}");
    assert!(lines
        .iter()
        .any(|l| l.starts_with("{\"ask\"") && l.contains("\"keep\": true") && l.contains("\"rebuild\": true")));
    assert_eq!("{\"result\": \"ASK\"}", lines.last().unwrap());
    assert!(radio.borrow().commits.is_empty());
    // keep: nothing written
    let (lines, _) = run(&radio, &[&base[..], &["--choice", "keep"]].concat());
    assert!(lines.last().unwrap().starts_with("{\"result\": \"KEPT\""));
    assert!(radio.borrow().commits.is_empty());
    // rebuild: the release sections with both repeater lists, into slot A (B is the running firmware's), tagged as
    // the user's (so the next update asks again)
    let (lines, err) = run(&radio, &[&base[..], &["--choice", "rebuild"]].concat());
    assert!(err.is_none(), "{err:?}");
    assert!(lines.last().unwrap().starts_with("{\"result\": \"PASS\", \"slot\": 0, \"seq\": 4"));
    let r = radio.borrow();
    assert!(r.commits.iter().all(|a| (0xEA0000..0xEB0000).contains(a)));
    let len = u32::from_le_bytes(r.get(0xEA000C, 4).try_into().unwrap()) as usize;
    let img = r.get(0xEA0000, 64 + len);
    let (h, payload) = dp::parse(&img);
    let h = h.unwrap();
    assert_eq!((dp::TAG, rp::hint(COORD)), (h.tag.as_str(), h.flags));
    let d = rp::directory(&payload).unwrap();
    assert_eq!(3, d.count(rp::RPT));
    assert!(d.bytes(&payload, rp::RPT).chunks(44).any(|c| c == own.as_slice()));
    assert_eq!(r.get(0xEE0000, mine.len()), mine, "the user's pack is untouched");
}

#[test]
fn cps_update_ref_replaces_a_release_pack_and_skips_the_same_one() {
    let radio = Rc::new(RefCell::new(FakeCps::new()));
    radio.borrow_mut().git = git("ddm32-plus-1.1.");
    // an older release pack with the TRAC rows: replaced without asking (owner decision 2026-11-26)
    let old = ref_image(&ref_payload(COORD, &two_rpts()[..1], "own+RadioID.net"), 2, "build 1.1.0", 0);
    radio.borrow_mut().put(0xEA0000, &old);
    let t = tmp();
    let f = pack_file(&t, &release_pack(), "r.dpk");
    let coord = format!("0x{COORD:08X}");
    let (lines, err) = run(&radio, &["update-ref", "--port", "COM9", "--pack", &f, "--coord", &coord]);
    assert!(err.is_none(), "{err:?}");
    assert!(lines.last().unwrap().starts_with("{\"result\": \"PASS\", \"slot\": 1, \"seq\": 3"));
    let n = radio.borrow().commits.len();
    // again: the radio has it, nothing written
    let (lines, err) = run(&radio, &["update-ref", "--port", "COM9", "--pack", &f, "--coord", &coord]);
    assert!(err.is_none(), "{err:?}");
    assert_eq!("{\"result\": \"SAME\"}", lines.last().unwrap());
    assert_eq!(n, radio.borrow().commits.len());
    // update-ref takes only a map pack and its coordinate id
    let (_, err) = run(&radio, &["update-ref", "--port", "COM9", "--pack", &f]);
    assert!(err.is_some());
    let (_, err) = run(&radio, &["update-ref", "--port", "COM9", "--pack", &f, "--coord", &coord, "--choice", "maybe"]);
    assert_eq!(Some("2".to_string()), err.map(|e| e.message));
}

#[test]
fn cps_version_and_coordinate_arguments() {
    use ddm32_core::radio::cps::{parse_coord, parse_known_coords, running_rules, running_version};
    assert_eq!(Some((1, 1)), running_version("ddm32-plus-1.1."));
    assert_eq!(Some((1, 0)), running_version("ddm32-plus-1.0."));
    assert_eq!(Some((1, 10)), running_version("ddm32-plus-1.10"));
    assert_eq!(None, running_version("ddm32plus-b4"));
    assert_eq!(None, running_version("open25-classic"));
    assert_eq!(Some(0x1A2B3C4D), parse_coord("0x1A2B3C4D"));
    assert_eq!(None, parse_coord("0x123456789"));
    let k = parse_known_coords("1.1.0=0x11, 1.1.1=0x12,1.2.0=0x20").unwrap();
    assert_eq!(rp::Running::MapPack(vec![0x11, 0x12]), running_rules(Some((1, 1)), &k));
    assert_eq!(rp::Running::MapPack(vec![]), running_rules(Some((1, 3)), &k));
    assert_eq!(rp::Running::NoMapPack, running_rules(Some((1, 0)), &k));
    assert_eq!(rp::Running::Unknown, running_rules(None, &k));
    assert!(parse_known_coords("1.1.0").is_err());
}
