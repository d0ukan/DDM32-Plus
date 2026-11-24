// SPDX-License-Identifier: GPL-3.0-or-later
#![allow(clippy::type_complexity, clippy::assertions_on_constants)]
//! The radio tools against the Python tools' recorded runs (Fixtures/radio, make_radio_fixtures.py): the same bytes to
//! the radio in the same order, the same printed lines, the same error, the same files (backup images, manifests,
//! receipts). Every run is a replay: no serial port exists here. Also radio_detect, the composed first-install image
//! and the embedded codec tables (the C# RadioToolTests).
mod common;
use common::*;
use ddm32_core::pyjson::{self, Py};
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::monitor::{ram_monitor, sha};
use ddm32_core::radio::port::{RawSerial, VirtualClock};
use ddm32_core::radio::tools::{backup_tool, install_tool, restore_stock_tool, ToolContext};
use ddm32_core::radio::unit_image;
use ddm32_core::radio::ToolError;
use serde_json::Value;
use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::OnceLock;

fn inputs() -> &'static (String, Vec<u8>, Vec<u8>, Vec<u8>, Vec<u8>) {
    // one set of inputs for the whole file (kept: the replays run in parallel threads and share it read only)
    static I: OnceLock<(String, Vec<u8>, Vec<u8>, Vec<u8>, Vec<u8>)> = OnceLock::new();
    I.get_or_init(|| {
        let r = RadioInputs::new();
        let root = r.dir.keep().to_string_lossy().into_owned();
        (root, r.program, r.data, r.stock, r.target)
    })
}

fn root() -> std::path::PathBuf {
    std::path::PathBuf::from(&inputs().0)
}

#[test]
fn inputs_are_the_python_inputs() {
    let h = &index()["inputs"];
    let (_, program, data, stock, target) = inputs();
    assert_eq!(h["program"], sha(program).as_str());
    assert_eq!(h["data"], sha(data).as_str());
    assert_eq!(h["stock"], sha(stock).as_str());
    assert_eq!(h["target"], sha(target).as_str());
    assert_eq!(h["plus_app"], sha(&plus_app()).as_str());
    assert_eq!(h["slot"], sha(&slot()).as_str());
    let (code, tables, _) = synthetic_codec();
    assert_eq!(h["code"], sha(&code).as_str());
    assert_eq!(h["tables"], sha(&tables).as_str());
    assert_eq!(index()["ram_monitor_sha256"], sha(&ram_monitor::image().unwrap()).as_str());
}

/// The embedded RadioFoundry files are the C# program's (the same pinned bytes).
#[test]
fn embedded_data_equals_the_csharp_copies() {
    let here = Path::new(env!("CARGO_MANIFEST_DIR")).join("data");
    let win = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../win/src/DDM32Plus.Core/Radio/Data");
    if !win.is_dir() {
        // the C# program was removed: nothing left to compare with (the pinned hashes above still hold the bytes)
        return;
    }
    for f in ["ram-monitor.bin", "codec-dm32.json"] {
        assert_eq!(std::fs::read(here.join(f)).unwrap(), std::fs::read(win.join(f)).unwrap(), "{f}");
    }
}

fn generic(s: &str, out_dir: &Path) -> String {
    let root = root();
    let mut t = s
        .replace(&out_dir.to_string_lossy().into_owned(), "{out}")
        .replace(&root.join("stock-backup").to_string_lossy().into_owned(), "{stock_backup}")
        .replace(&root.join("backup").to_string_lossy().into_owned(), "{backup}")
        .replace(&root.join("plus").to_string_lossy().into_owned(), "{plus}");
    if !cfg!(windows) {
        // the recording was made on Windows: its paths join with a backslash
        for p in ["{out}", "{backup}", "{stock_backup}", "{plus}"] {
            t = t.replace(&format!("{p}/"), &format!("{p}\\"));
        }
    }
    t
}

fn write_seconds(s: &str) -> String {
    let re = regex::Regex::new(r#""write_seconds": [-0-9.e+]+"#).unwrap();
    re.replace_all(s, "\"write_seconds\": X").into_owned()
}

fn replay(name: &str) {
    let sc = scenario(name);
    let root = root();
    let out_dir = root.join(format!("out-{name}"));
    let sub = |a: &str| {
        a.replace("{out}", &out_dir.to_string_lossy())
            .replace("{backup}", &root.join("backup").to_string_lossy())
            .replace("{stock_backup}", &root.join("stock-backup").to_string_lossy())
            .replace("{plus}", &root.join("plus").to_string_lossy())
    };
    let mut argv: Vec<String> = vec!["--port".into(), "SIMULATED".into()];
    argv.extend(sc["args"].as_array().unwrap().iter().map(|a| sub(a.as_str().unwrap())));
    let port: Rc<RefCell<Option<Rc<RefCell<ReplayPort>>>>> = Rc::new(RefCell::new(None));
    let lines = Lines::default();
    let clock = VirtualClock::new(index()["stamp"].as_str().unwrap());
    let codec_holder;
    let codec: &Codec = if sc["stock_codec"].as_bool().unwrap() {
        codec_holder = synthetic_codec_tables();
        &codec_holder
    } else {
        Codec::dm32()
    };
    let sc2 = sc.clone();
    let port2 = port.clone();
    let serial = move |n: &str| -> Result<Box<dyn RawSerial>, ToolError> {
        assert_eq!("SIMULATED", n);
        assert!(port2.borrow().is_none());
        let p = Rc::new(RefCell::new(ReplayPort::new(&sc2)));
        *port2.borrow_mut() = Some(p.clone());
        Ok(Box::new(Shared(p)))
    };
    let print = lines.print();
    let ctx =
        ToolContext { print: &print, clock: &clock, serial: &serial, release_key_hex: Some(test_public_hex()), codec };
    let result = match sc["tool"].as_str().unwrap() {
        "backup" => backup_tool(&argv, &ctx),
        "install_plus" => install_tool(&argv, &ctx),
        "restore_stock" => restore_stock_tool(&argv, &ctx),
        t => panic!("tool {t}"),
    };
    // the radio saw exactly what the Python tool sent
    match sc["events"] {
        Value::Null => assert!(port.borrow().is_none(), "{name}: a port was opened"),
        _ => port.borrow().as_ref().expect("no port").borrow().assert_done(),
    }
    // the same error, the same printed lines
    let error = result.err().map(|e| (e.py_type.clone(), generic(&e.message, &out_dir)));
    match &sc["error"] {
        Value::Null => assert_eq!(None, error, "{name}"),
        w => {
            assert_eq!(Some((w[0].as_str().unwrap().to_string(), w[1].as_str().unwrap().to_string())), error, "{name}")
        }
    }
    let want: Vec<String> = sc["lines"].as_array().unwrap().iter().map(|l| l.as_str().unwrap().to_string()).collect();
    let got: Vec<String> = lines.get().iter().map(|l| generic(l, &out_dir)).collect();
    assert_eq!(want, got, "{name}: printed lines");
    // the same files
    let files = sc["files"].as_object().unwrap();
    let mut mine: Vec<String> = if out_dir.is_dir() {
        std::fs::read_dir(&out_dir).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect()
    } else {
        vec![]
    };
    mine.sort();
    let mut wanted: Vec<String> = files.keys().cloned().collect();
    wanted.sort();
    assert_eq!(wanted, mine, "{name}: files");
    for (f, v) in files {
        let body = std::fs::read(out_dir.join(f)).unwrap();
        if let Some(text) = v.get("text") {
            let mine = String::from_utf8(body).unwrap();
            let mine = if cfg!(windows) {
                mine
            } else {
                mine.replace(&format!("{}/", out_dir.display()), &format!("{}\\", out_dir.display()))
            };
            assert_eq!(write_seconds(text.as_str().unwrap()), write_seconds(&mine), "{name}: {f}");
        } else {
            assert_eq!(v["bytes"].as_u64().unwrap(), body.len() as u64, "{name}: {f}");
            assert_eq!(v["sha256"].as_str().unwrap(), sha(&body), "{name}: {f}");
        }
    }
}

macro_rules! replays {
    ($($n:ident),* $(,)?) => {
        $( #[test] fn $n() { replay(stringify!($n)); } )*
        const NAMES: &[&str] = &[$(stringify!($n)),*];
    };
}

replays!(
    backup_bad_density,
    backup_damaged_frames,
    backup_ok,
    backup_radio_off,
    backup_rom_nak,
    backup_wait_refused,
    from_stock_classic_refused,
    from_stock_ok,
    from_stock_plan_only,
    from_stock_slot_corrupt,
    install_args_refused,
    install_live_data_changed,
    install_live_program_changed,
    install_ok,
    install_plan_only,
    install_readback_corrupt,
    install_slot_changed,
    install_stock_refused,
    install_wrong_flash_id,
    restore_ok,
    restore_plan_only,
    restore_readback_corrupt,
    restore_stock_args_refused,
    restore_stock_ok,
    restore_stock_other_chip,
    restore_stock_plan_only,
);

#[test]
fn every_recorded_run_is_covered() {
    let names = scenario_names();
    assert!(names.len() >= 25);
    let mut mine: Vec<&str> = NAMES.to_vec();
    mine.sort();
    assert_eq!(names, mine.iter().map(|s| s.to_string()).collect::<Vec<_>>(), "a recorded run has no replay test");
}

// --- radio_detect --------------------------------------------------------------------------------------------------

fn detect_program(name: &str) -> Vec<u8> {
    let (_, program, _, stock, _) = inputs();
    let s = slot();
    let ffs = vec![0xFFu8; 4096];
    match name {
        "classic_ddm32" => program_of(&app_classic(), None),
        "classic_ddm25" => program_of(&[pattern(0x900, 3), b"DDM25 Plus".to_vec()].concat(), None),
        "classic_open25" => program_of(&[b"open25-classic-r13".to_vec(), pattern(0x900, 3)].concat(), None),
        "classic_unknown" => program_of(&pattern(0x900, 3), None),
        "no_slot_classic_app" => program_of(&app_classic(), Some(&ffs)),
        "no_slot_unknown" => program_of(&pattern(0x900, 5), Some(&ffs)),
        "bad_slot_crc" => program_of(&app_classic(), Some(&[s[..0x40].to_vec(), vec![0], s[0x41..].to_vec()].concat())),
        "stock_047" => stock.clone(),
        "stock_moved" => {
            let mut m = stock.clone();
            let at = (STOCK_CODE_AT - 0x03000000) as usize;
            m[at..at + 8].fill(0);
            m
        }
        "classic_with_stock_codec" => program.clone(),
        _ => panic!("{name}"),
    }
}

fn program_of(app: &[u8], slot: Option<&[u8]>) -> Vec<u8> {
    common::program(app, slot)
}

#[test]
fn detect_answers_as_python() {
    let mut n = 0;
    let (_, _, _, stock, _) = inputs();
    for c in index()["detect"].as_array().unwrap() {
        let name = c["name"].as_str().unwrap();
        if name == "tool_output" {
            assert_eq!(
                c["printed"].as_str().unwrap(),
                unit_image::detect_tool_output(&root().join("backup"), Codec::dm32()).unwrap()
            );
            continue;
        }
        let syn = synthetic_codec_tables();
        let codec: &Codec = if c["stock"].as_bool().unwrap() { &syn } else { Codec::dm32() };
        if name == "compose_from_stock" {
            let (target, prof) = unit_image::compose_from_stock(stock, &plus_app(), codec).unwrap();
            let r = &c["result"];
            assert_eq!(r["target_sha256"].as_str().unwrap(), sha(&target));
            assert_eq!(r["profile"].as_str().unwrap(), prof.name);
            assert_eq!(
                pyjson::dumps2(&py(&r["slot"])),
                pyjson::dumps2(&Py::Dict(codec.parse_slot(&target[SLOT..]).unwrap()))
            );
            continue;
        }
        let program = detect_program(name);
        assert_eq!(c["program"].as_str().unwrap(), sha(&program), "{name}");
        assert_eq!(
            pyjson::dumps2(&py(&c["result"])),
            pyjson::dumps2(&Py::Dict(unit_image::detect(&program, codec).unwrap())),
            "{name}"
        );
        n += 1;
    }
    assert_eq!(10, n);
}

#[test]
fn embedded_codec_tables_are_the_pinned_ones() {
    let k = &index()["codec"];
    let names: Vec<&str> = k["profiles"].as_object().unwrap().keys().map(|s| s.as_str()).collect();
    assert_eq!(names, Codec::dm32().profiles.iter().map(|p| p.0.as_str()).collect::<Vec<_>>());
    let mut maps: Vec<&str> = k["maps"].as_object().unwrap().keys().map(|s| s.as_str()).collect();
    maps.sort();
    let mut mine: Vec<&str> = Codec::dm32().maps.iter().map(|m| m.id.as_str()).collect();
    mine.sort();
    assert_eq!(maps, mine);
    for (key, p) in k["profiles"].as_object().unwrap() {
        let m = &Codec::dm32().profiles.iter().find(|x| &x.0 == key).unwrap().1;
        assert_eq!(p["name"].as_str().unwrap(), m.name);
        assert_eq!(p["code"][0].as_u64().unwrap(), m.code.addr);
        assert_eq!(p["tables_sha256"].as_str().unwrap(), m.tables_sha256);
    }
    assert_eq!(0x030E8000, Codec::dm32().board.slot_lo);
    unit_image::check_codec(Codec::dm32()).unwrap();
}
