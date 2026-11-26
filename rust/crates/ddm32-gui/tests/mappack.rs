// SPDX-License-Identifier: GPL-3.0-or-later
//! The map pack in the window (docs/mappack-design.md E.3): the slot words ("Harita paketi" / "Map pack", "for
//! another version", "too big", "your own pack"), the third row of the Data packs page, the wizard's map step (order,
//! the tool's lines, its argv) and the start-up question's line. No radio, no network.
use ddm32_core::datapacks::datapack as dp;
use ddm32_core::datapacks::refpack as rp;
use ddm32_core::releases::catalog::Catalog;
use ddm32_core::releases::{signed_manifest as sm, CURRENT};
use ddm32_core::storage::AppPaths;
use ddm32_gui::mappack::{self, MapOrder, MapState};
use ddm32_gui::updates::{self, Installed};
use ddm32_gui::{i18n, packs, App, AppWindow, Packs};
use serde_json::{json, Value};
use slint::ComponentHandle;
use std::path::{Path, PathBuf};

const COORD: u32 = 0x1A2B_3C4D;

fn init() {
    thread_local!(static DONE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) });
    if !DONE.with(|d| d.replace(true)) {
        i_slint_backend_testing::init_no_event_loop();
    }
}

fn root(name: &str) -> PathBuf {
    let r = std::env::temp_dir().join(format!("ddm32-mappack-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&r);
    std::fs::create_dir_all(&r).unwrap();
    r
}

fn window(root: &Path) -> AppWindow {
    let settings = json!({"language": "en",
        "disclaimer": {"version": "684c0603763f3987", "accepted": true, "accepted_utc": "2026-11-24T00:00:00Z"}});
    std::fs::write(root.join("settings.json"), settings.to_string()).unwrap();
    init();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup_with_env(&ui, AppPaths::new(root), false, ddm32_gui::no_env);
    ui
}

/// A small valid map pack: airfields only (the map and repeater groups absent: still valid, C.2).
fn map_image(tag: &str) -> Vec<u8> {
    let mut air = vec![0u8; 44];
    air[10] = 1;
    let p = rp::assemble(COORD, &[(rp::AIR, 44, air), (rp::AIR_FREQ, 4, vec![0; 4]), (rp::AIR_KEY, 10, vec![0; 10])])
        .unwrap();
    assert!(rp::validate(&p, 0, &rp::Rules::new(Some(COORD))).is_valid());
    dp::image(dp::REF, &p, "20261119", "DDM32 TR 1.1.0", 0, tag, rp::hint(COORD)).unwrap()
}

fn catalog(with_ref: bool) -> Catalog {
    let secret: Vec<u8> = (0..32u8).collect();
    let key = hex::encode(ddm32_core::crypto::ed25519::secret_to_public(&secret));
    let app = b"app 1.1.0".to_vec();
    let mut e = json!({
        "version": "1.1.0", "channel": "stable", "file": "ddm32-plus-1.1.0.zip",
        "url": format!("{}v1.1.0/ddm32-plus-1.1.0.zip", CURRENT.url_prefix()),
        "sha256": sm::sha256_hex(b"zip"), "size": 3,
        "application_sha256": sm::sha256_hex(&app), "application_size": app.len(),
        "settings_format": 1, "codeplug_format": 1, "min_compatible_version": "1.1.0",
        "notes": {"tr": "", "en": ""},
    });
    if with_ref {
        let img = map_image("build 1.1.0");
        e["ref_pack"] = json!({"schema": 1, "version": "1.1.0", "file": "ddm32-ref-1.1.0.dpk",
            "url": format!("{}v1.1.0/ddm32-ref-1.1.0.dpk", CURRENT.url_prefix()), "sha256": sm::sha256_hex(&img),
            "size": img.len(), "pack_type": 3, "pack_format": 1, "dir_version": 1, "coord_crc": format!("0x{COORD:08x}"),
            "date": "20261119", "min_firmware": "1.1.0"});
    }
    let m = json!({"format": CURRENT.format, "repo": CURRENT.repo, "release": "v1.1.0",
        "created_utc": "2026-11-21T00:00:00Z", "firmware": [e]});
    let data = serde_json::to_vec_pretty(&m).unwrap();
    let sig = sm::sign_bytes(&data, &secret);
    let mut cat = Catalog::new("github");
    cat.add("v1.1.0", &sm::verify_bytes(&data, sig.as_bytes(), Some(&key), None).unwrap(), None);
    cat
}

fn slot(t: &str, state: &str, tag: &str) -> Value {
    json!({"type": t, "slot": 0, "address": "0xea0000", "valid": state != "empty", "crc_ok": state != "bad",
        "in_force": state == "valid", "seq": 2, "name": "DDM32 TR", "date": "20261119", "bytes": 1984, "count": 252,
        "tag": tag, "state": state, "user_built": !rp::is_release_built(tag)})
}

#[test]
fn map_slots_have_their_own_words() {
    assert_eq!(
        "Map pack slot 0 (0xea0000): \"DDM32 TR\", 2026-11-19, 252 entries, 2 KB, sequence 2: IN FORCE",
        packs::slot_line("en", &slot("ref", "valid", "build 1.1.0"))
    );
    assert!(packs::slot_line("tr", &slot("ref", "valid", "build 1.1.0")).starts_with("Harita paketi yuva 0"));
    assert!(packs::slot_line("en", &slot("ref", "valid", "refpack.py own")).ends_with("IN FORCE (your own pack)"));
    assert!(packs::slot_line("en", &slot("ref", "other", "x")).contains("for another version, not used"));
    assert!(packs::slot_line("tr", &slot("ref", "other", "x")).contains("başka bir sürüm için"));
    assert!(packs::slot_line("en", &slot("ref", "bad", "x")).contains("damaged, not used"));
    assert_eq!("Map pack slot 0 (0xea0000): empty", packs::slot_line("en", &slot("ref", "empty", "")));
    assert!(packs::slot_line("en", &slot("radioid", "big", "t")).contains("too big for this firmware"));
    // the air band type keeps its name (3.1 printed every non-RadioID type as "air": F.5)
    assert!(packs::slot_line("en", &slot("air", "valid", "t")).starts_with("Air band slot 0"));
    assert_eq!("Map pack", packs::type_word("en", "ref"));
}

#[test]
fn a_map_dpk_takes_the_third_row() {
    let root = root("row");
    let ui = window(&root);
    let p = ui.global::<Packs>();
    assert_eq!("No map pack chosen yet.", p.get_map_result().as_str());
    assert!(!p.get_map_pack());
    let f = root.join("ddm32-ref-1.1.0.dpk");
    std::fs::write(&f, map_image("build 1.1.0")).unwrap();
    packs::load_dpk(&ui, &f);
    assert!(p.get_map_pack());
    assert!(p.get_map_result().starts_with("Map pack \"DDM32 TR 1.1.0\", 2026-11-19: 0 repeaters, 1 airfields"));
    assert!(p.get_chosen_text().contains("Map pack"));
    // a user's pack says so
    std::fs::write(&f, map_image("DDM32Plus-rs 1")).unwrap();
    packs::load_dpk(&ui, &f);
    assert!(p.get_map_result().starts_with("Your own map pack"));
    // a broken one is refused with the reason
    let mut bad = map_image("x");
    let n = bad.len();
    bad[n - 1] ^= 1;
    std::fs::write(&f, bad).unwrap();
    packs::load_dpk(&ui, &f);
    assert!(p.get_result().contains("not a whole data pack"));
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn the_wizard_step_follows_h3() {
    assert_eq!(MapOrder::PackFirst, mappack::order(&Installed::Plus { version: "1.0.6".into(), label: "".into() }));
    assert_eq!(MapOrder::FirmwareFirst, mappack::order(&Installed::Other("stock".into())));
    assert_eq!(MapOrder::FirmwareFirst, mappack::order(&Installed::NotRead));
    assert_eq!(MapOrder::FirmwareFirst, mappack::order(&Installed::Interrupted));
    let root = root("wizard");
    let ui = window(&root);
    let app = ui.global::<App>();
    ddm32_gui::with_state(|s| {
        s.shown = catalog(true).firmware;
        s.known.installed = Installed::Plus { version: "1.0.6".into(), label: "".into() };
    });
    app.set_selected_version(0);
    mappack::show(&ui);
    assert!(app.get_map_available() && app.get_map_first() && !app.get_map_done());
    ddm32_gui::with_state(|s| {
        s.map.done_for = Some("1.1.0".into());
        s.known.installed = Installed::Other("stock".into());
    });
    mappack::show(&ui);
    assert!(app.get_map_done() && !app.get_map_first());
    ddm32_gui::with_state(|s| s.shown = catalog(false).firmware);
    mappack::show(&ui);
    assert!(!app.get_map_available(), "a release without a map pack shows no card");
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn the_tool_lines_and_argv_of_the_step() {
    let mut st = MapState::default();
    for l in [
        "{\"radio\": {\"git\": \"ddm32-plus-1.1.\"}}",
        "{\"note\": \"ref_after_update\"}",
        "{\"plan\": {\"type\": \"ref\", \"slot\": 1, \"sectors\": [\"0xee1000\", \"0xee0000\"]}}",
        "pack: sector 1/2 0xEE1000 written and read back",
        "{\"result\": \"PASS\", \"slot\": 1, \"seq\": 3}",
    ] {
        mappack::feed(&mut st, "en", l);
    }
    assert_eq!((1, 2), (st.sectors_done, st.sectors_all));
    let (t, k) = mappack::result_text("en", &st);
    assert!(t.starts_with("PASS: the map pack is written") && t.contains("older than 1.1") && k == "success");
    let mut st = MapState::default();
    mappack::feed(&mut st, "en", "{\"ask\": {\"name\": \"mine\", \"tag\": \"x\", \"keep\": false, \"rebuild\": true}}");
    mappack::feed(&mut st, "en", "{\"result\": \"ASK\"}");
    assert_eq!(Some((false, true)), st.ask);
    assert_eq!("warning", mappack::result_text("en", &st).1);
    let mut st = MapState::default();
    mappack::feed(&mut st, "tr", "{\"result\": \"SAME\"}");
    assert!(mappack::result_text("tr", &st).0.contains("zaten var"));
    let a = mappack::argv(
        "COM14",
        Path::new("r.dpk"),
        COORD,
        &["--known-coords".into(), "1.1.0=0x1".into()],
        "Harita",
        Some("rebuild"),
    );
    assert_eq!(
        vec![
            "update-ref",
            "--port",
            "COM14",
            "--pack",
            "r.dpk",
            "--coord",
            "0x1A2B3C4D",
            "--line",
            "Harita",
            "--known-coords",
            "1.1.0=0x1",
            "--choice",
            "rebuild"
        ],
        a
    );
    assert_eq!(format!("1.1.0=0x{COORD:08X}"), packs::known_coords(&catalog(true)));
    assert_eq!("", packs::known_coords(&catalog(false)));
}

#[test]
fn the_start_up_question_names_the_map_pack() {
    let cat = catalog(true);
    let d = updates::firmware_detail("en", cat.find("1.1.0"));
    assert!(d.ends_with("the wizard writes it too."), "{d}");
    assert_eq!(i18n::t("en", "prompt_firmware_d"), updates::firmware_detail("en", catalog(false).find("1.1.0")));
    assert_eq!(i18n::t("tr", "prompt_firmware_d"), updates::firmware_detail("tr", None));
    let mut newer = catalog(false).firmware[0].clone();
    newer.entry.ref_pack_newer = true;
    assert!(updates::firmware_detail("en", Some(&newer)).contains("update the program first"));
}

#[test]
fn every_new_text_is_in_both_languages() {
    for k in [
        "pack_type_ref",
        "pack_desc_ref",
        "pack_desc_ref_user",
        "pack_slot_big",
        "pack_slot_other",
        "pack_slot_user_built",
        "pack_map_title",
        "pack_map_text",
        "pack_load_map",
        "pack_map_none",
        "pack_write_map",
        "pack_ref_after_update",
        "pack_radioid_big_warning",
        "map_title",
        "map_first_text",
        "map_after_text",
        "map_write",
        "map_downloading",
        "map_download_failed",
        "map_writing",
        "map_done",
        "map_same",
        "map_kept",
        "map_ask_wait",
        "map_failed",
        "map_screen_line",
        "map_ask_title",
        "map_ask_text",
        "map_ask_text_other",
        "map_ask_rebuild",
        "map_ask_keep",
        "map_ask_release",
        "map_ask_release_warning",
        "map_ask_none",
        "prompt_firmware_map",
        "prompt_firmware_map_newer",
    ] {
        let (tr, en) = (i18n::t("tr", k), i18n::t("en", k));
        assert!(tr != k && en != k && tr != en || k == "pack_type_ref" && tr != en, "{k}");
    }
}
