// SPDX-License-Identifier: GPL-3.0-or-later
//! The pages that bring the window to the C# 2.0.0 program's level (v1.1.0 candidate): versions (states, check,
//! rejected releases, switch), going back (previous / stock / other folder, the Yes / No question), data packs
//! (countries, names, slots from a backup, a .dpk, the write question), my channels (zone and mode filters, sort,
//! all columns, another backup), health (cable check, read every 30 s), the window's size and place, offline help,
//! the menu entries and `--show`. No radio (no port is opened anywhere here), no network: release lists are made and
//! signed here, backups are written into a temporary folder.
use base64::Engine;
use ddm32_core::codeplug::{Codeplug, Table, EN, IMAGE_SIZE};
use ddm32_core::datapacks::datapack as dp;
use ddm32_core::releases::catalog::Catalog;
use ddm32_core::releases::{signed_manifest as sm, CURRENT};
use ddm32_core::storage::{AppPaths, Settings};
use ddm32_gui::updates::Installed;
use ddm32_gui::{channels, help, i18n, packs, restore, versions, window, App, AppWindow, Packs, Radio};
use serde_json::{json, Value};
use slint::{ComponentHandle, Model};
use std::io::Read;
use std::path::{Path, PathBuf};

const DISCLAIMER: &str = "684c0603763f3987";

fn init() {
    thread_local!(static DONE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) });
    if !DONE.with(|d| d.replace(true)) {
        i_slint_backend_testing::init_no_event_loop();
    }
}

fn root(name: &str) -> PathBuf {
    let r = std::env::temp_dir().join(format!("ddm32-parity-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&r);
    std::fs::create_dir_all(&r).unwrap();
    r
}

fn window(root: &Path, mut settings: Value) -> AppWindow {
    if settings.get("disclaimer").is_none() {
        settings["disclaimer"] =
            json!({"version": DISCLAIMER, "accepted": true, "accepted_utc": "2026-11-24T00:00:00Z"});
    }
    std::fs::write(root.join("settings.json"), settings.to_string()).unwrap();
    init();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup(&ui, AppPaths::new(root), false);
    ui
}

fn entry(version: &str, channel: &str) -> Value {
    let app = format!("app {version}").into_bytes();
    let name = format!("ddm32-plus-{version}.zip");
    json!({
        "version": version, "channel": channel, "file": name,
        "url": format!("{}v{version}/{name}", CURRENT.url_prefix()),
        "sha256": sm::sha256_hex(b"zip"), "size": 3,
        "application_sha256": sm::sha256_hex(&app), "application_size": app.len(),
        "settings_format": 1, "codeplug_format": 1, "min_compatible_version": version,
        "notes": {"tr": format!("not {version}"), "en": format!("note {version}")},
    })
}

fn catalog(versions: &[(&str, &str)]) -> Catalog {
    let secret: Vec<u8> = (0..32u8).collect();
    let key = hex::encode(ddm32_core::crypto::ed25519::secret_to_public(&secret));
    let mut cat = Catalog::new("github");
    for (v, ch) in versions {
        let m = json!({"format": CURRENT.format, "repo": CURRENT.repo, "release": format!("v{v}"),
            "created_utc": "2026-11-21T00:00:00Z", "firmware": [entry(v, ch)]});
        let data = serde_json::to_vec_pretty(&m).unwrap();
        let sig = sm::sign_bytes(&data, &secret);
        cat.add(&format!("v{v}"), &sm::verify_bytes(&data, sig.as_bytes(), Some(&key), None).unwrap(), None);
    }
    cat.sort();
    cat.checked_utc = Some("2026-11-24T09:41:07Z".into());
    cat
}

/// The synthetic "sample" codeplug image of the Python viewer's fixtures.
fn sample_image() -> Vec<u8> {
    let f = std::fs::File::open(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/codeplug-results.json.gz"),
    )
    .unwrap();
    let mut s = String::new();
    flate2::read::GzDecoder::new(f).read_to_string(&mut s).unwrap();
    let v: Value = serde_json::from_str(&s).unwrap();
    let mut img = vec![0xFFu8; IMAGE_SIZE];
    for run in v["images"]["sample"]["sparse"].as_array().unwrap() {
        let a = run[0].as_u64().unwrap() as usize;
        let data = base64::engine::general_purpose::STANDARD.decode(run[1].as_str().unwrap()).unwrap();
        img[a..a + data.len()].copy_from_slice(&data);
    }
    img
}

/// A verified backup folder holding `q128` as its data chip image (the program image is not needed here).
fn fake_backup(dir: &Path, q128: &[u8]) {
    std::fs::create_dir_all(dir).unwrap();
    std::fs::write(dir.join("q128-pass1.bin"), q128).unwrap();
    std::fs::write(dir.join("backup-manifest.json"), json!({"backup_verified": true}).to_string()).unwrap();
}

// ---- i18n -------------------------------------------------------------------------------------------------------------

#[test]
fn texts_with_several_values() {
    assert_eq!("Showing 3 of 9 rows", i18n::tn("en", "ch_shown", &["3", "9"]));
    // a value holding "{}" is never filled again
    assert_eq!("Showing {} of 9 rows", i18n::tn("en", "ch_shown", &["{}", "9"]));
    // missing values leave the rest as it is
    assert_eq!("Showing 3 of {} rows", i18n::tn("en", "ch_shown", &["3"]));
}

// ---- versions ---------------------------------------------------------------------------------------------------------

#[test]
fn every_version_says_its_state() {
    let cat = catalog(&[("1.2.0-alpha.1", "beta"), ("1.1.0-beta.1", "beta"), ("1.0.1", "stable"), ("1.0.0", "stable")]);
    let on = Installed::Plus { version: "1.0.1".into(), label: "Latest".into() };
    let rows = versions::rows("en", &cat, &on, &["1.0.0".to_string()]);
    let r: Vec<(String, String, String, bool)> =
        rows.iter().map(|r| (r.version.to_string(), r.kind.to_string(), r.state.to_string(), r.installable)).collect();
    assert_eq!(
        r,
        vec![
            (
                "1.2.0-alpha.1".into(),
                "test".into(),
                "newer than the radio's, not downloaded, test / alpha: may not have been tried on a radio, asks first"
                    .into(),
                true
            ),
            ("1.1.0-beta.1".into(), "beta".into(), "newer than the radio's, not downloaded".into(), true),
            ("1.0.1".into(), "latest".into(), "on the radio, not downloaded".into(), true),
            ("1.0.0".into(), "stable".into(), "older than the radio's, downloaded".into(), true),
        ]
    );
    // the notes in the window's language, both under the selected row
    assert_eq!("note 1.0.1", rows[2].notes.as_str());
    assert_eq!("TR: not 1.0.1\nEN: note 1.0.1", rows[2].notes_both.as_str());
    // nothing known about the radio: no newer / older, only the files
    let rows = versions::rows("tr", &cat, &Installed::NotRead, &[]);
    assert_eq!("indirilmedi", rows[2].state.as_str());
}

#[test]
fn the_check_line_and_the_rejected_releases() {
    let mut cat = catalog(&[("1.0.1", "stable")]);
    assert_eq!(("Checking...".to_string(), "info"), versions::status("en", true, None, &cat));
    let (t, k) = versions::status("en", false, Some("NetError: timed out"), &cat);
    assert!(t.starts_with("The check failed: NetError: timed out\n") && k == "danger", "{t}");
    assert_eq!(("Checked (1 signed versions)".to_string(), "success"), versions::status("en", false, None, &cat));
    cat.source = "cache".into();
    assert_eq!("info", versions::status("en", false, None, &cat).1);
    assert_eq!("Last check: 2026-11-24 09:41 UTC", versions::last_check("en", cat.checked_utc.as_deref()));
    assert_eq!("Son denetim: henüz yok", versions::last_check("tr", None));
    assert_eq!("No version failed the signature check.", versions::rejected_text("en", &cat));
    cat.rejected.push(("v0.9.9".into(), "signature does not verify".into()));
    cat.rejected.push(("v0.9.8".into(), "no signed manifest".into()));
    assert_eq!(
        "Versions not listed (signature failed):\nv0.9.9: signature does not verify\nv0.9.8: no signed manifest",
        versions::rejected_text("en", &cat)
    );
}

#[test]
fn switch_opens_the_wizard_at_that_version_even_off_the_channel() {
    let root = root("switch");
    let ui = window(&root, json!({"language": "en", "channel": 0}));
    let app = ui.global::<App>();
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("1.1.0-beta.1", "beta"), ("1.0.1", "stable")])), false);
    app.invoke_navigate(11);
    assert_eq!(11, app.get_page());
    assert_eq!(2, app.get_ver_rows().row_count(), "the page lists every version, whatever the channel");
    assert_eq!("success", app.get_ver_status_kind().as_str());
    // the beta is not on the Latest channel: the wizard gets it for this time, selected, at its first step
    app.set_ver_selected(0);
    app.invoke_ver_switch();
    assert_eq!((1, 0), (app.get_page(), app.get_step()));
    assert!(!app.get_busy(), "no download, no radio step");
    let i = app.get_selected_version() as usize;
    assert_eq!("1.1.0-beta.1", app.get_versions().row_data(i).unwrap().version.as_str());
    // opening the wizard from Home again: the channel's own list
    app.invoke_navigate(0);
    app.invoke_navigate(1);
    assert_eq!(1, app.get_versions().row_count());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn a_failed_check_is_said_on_the_versions_page_and_the_time_kept() {
    let root = root("check");
    let ui = window(&root, json!({"language": "en"}));
    let app = ui.global::<App>();
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("1.0.1", "stable")])), false);
    let saved = Settings::new(root.join("settings.json")).load();
    assert_eq!("2026-11-24T09:41:07Z", saved["last_check_utc"], "the C# key");
    ddm32_gui::catalog_loaded(&ui, Err("NetError: api.github.com: timed out".into()), false);
    app.invoke_navigate(11);
    assert!(app.get_ver_status().starts_with("The check failed: NetError"));
    assert_eq!("Last check: 2026-11-24 09:41 UTC", app.get_ver_last_check().as_str());
    assert!(!app.get_ver_checking());
    let _ = std::fs::remove_dir_all(&root);
}

// ---- going back -------------------------------------------------------------------------------------------------------

#[test]
fn shortcuts_take_the_recorded_backups() {
    let root = root("restore");
    let settings = Settings::new(root.join("settings.json"));
    std::fs::write(root.join("settings.json"), "{}").unwrap();
    let old = root.join("yedekler").join("dm32-20261101");
    let new = root.join("backups").join("dm32-20261101");
    fake_backup(&new, b"x");
    // an old Turkish path follows the moved folder
    assert_eq!(new, restore::map_legacy(&root, &old.display().to_string()));
    assert_eq!(PathBuf::from("Z:/nowhere"), restore::map_legacy(&root, "Z:/nowhere"));
    // nothing recorded: no backup for either shortcut
    assert!(restore::default_backup("previous", &settings, &root).is_none());
    assert!(restore::default_backup("stock", &settings, &root).is_none());
    // recorded the C# way: the old path still finds the backup
    restore::remember_previous_backup(&settings, &old).unwrap();
    assert_eq!(Some(new.clone()), restore::default_backup("previous", &settings, &root));
    let stock = root.join("backups").join("stock-20261020");
    fake_backup(&stock, b"x");
    let unverified = root.join("backups").join("half");
    std::fs::create_dir_all(&unverified).unwrap();
    restore::remember_stock_backup(&settings, &unverified).unwrap();
    restore::remember_stock_backup(&settings, &stock).unwrap();
    restore::remember_stock_backup(&settings, &stock).unwrap();
    assert_eq!(2, restore::stock_backups(&settings).len(), "kept once");
    assert_eq!(Some(stock.clone()), restore::default_backup("stock", &settings, &root), "the first verified one");
    // the C# format: a list of strings
    assert!(settings.load()["stock_backups"].is_array());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn a_shortcut_without_a_backup_says_why_and_restore_asks_first() {
    let root = root("restore-ui");
    let ui = window(&root, json!({"language": "en"}));
    let app = ui.global::<App>();
    app.invoke_navigate(2);
    app.invoke_res_previous();
    assert_eq!("No suitable backup found.\nNo install was made with this program.", app.get_message().as_str());
    app.invoke_res_stock();
    assert!(app.get_message().ends_with("Show your own backup with \"Choose another backup folder...\"."));
    assert!(!app.get_restore_ready());
    // the Restore button without a plan: no question, nothing starts
    app.invoke_restore();
    assert!(!app.get_res_confirm_open() && !app.get_busy());
    // another folder that is not a verified backup is refused
    let not = root.join("somewhere");
    std::fs::create_dir_all(&not).unwrap();
    assert!(!restore::other_folder(&ui, &not));
    assert_eq!("This folder is not a verified backup.", app.get_message().as_str());
    // a planned restore asks Yes / No (the backup's path, the plan, the short disclaimer) and starts nothing
    app.set_restore_ready(true);
    ddm32_gui::with_state(|s| s.restore.backup_dir = Some(root.join("backups").join("dm32-x")));
    app.set_res_status("Chosen backup: dm32-x".into());
    app.invoke_restore();
    assert!(app.get_res_confirm_open());
    let q = app.get_res_confirm_text().to_string();
    assert!(q.starts_with("Restore the radio from this backup?\n\nChosen backup: "), "{q}");
    assert!(q.contains("dm32-x") && q.ends_with("⚠ You are responsible. Is your backup ready?"), "{q}");
    assert!(!app.get_busy());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn the_restore_question_texts() {
    let p = PathBuf::from("C:/b/dm32-1");
    let t = restore::confirm_text("tr", &p, "Seçilen yedek: dm32-1");
    assert!(t.starts_with("Telsiz bu yedekten geri yüklensin mi?"));
    let mut plan = ddm32_core::pyjson::PyDict::new();
    plan.set("changed_sectors", 23);
    plan.set("first", "0x0C000");
    plan.set("last", "0x22000");
    let s = restore::status_text("en", Some(&p), Some(&plan), &("Res_NotStock", vec![]));
    assert_eq!(
        "Chosen backup: dm32-1\n23 sectors will be written (0x0C000 - 0x22000). The bootloader and the data chip \
         (calibration, channels) are not written.\nThis backup is not stock firmware: \"Back to stock firmware\" works \
         only with a backup of the stock firmware.",
        s
    );
}

// ---- data packs -------------------------------------------------------------------------------------------------------

fn radioid_pack(dir: &Path) -> PathBuf {
    let csv = dir.join("user.csv");
    std::fs::write(
        &csv,
        "RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY\n2862001,TA2DOU,Dogukan,X,Ankara,,Turkey\n\
         2862002,TA1AAA,Ali,Y,Istanbul,,Turkey\n3101234,K1ABC,John,Z,Boston,MA,United States\n",
    )
    .unwrap();
    let pk =
        ddm32_core::datapacks::files::radio_id(&csv, &packs::countries("Turkey, "), "RadioID TR", Some("20261124"))
            .unwrap();
    let f = dir.join("radioid-20261124.dpk");
    std::fs::write(&f, &pk.image).unwrap();
    f
}

#[test]
fn pack_words_and_numbers() {
    assert_eq!(vec!["Turkey".to_string(), "Cyprus".into()], packs::countries(" Turkey , ,Cyprus,"));
    assert!(packs::countries("").is_empty(), "nothing typed: the build refuses (no ID of the chosen countries)");
    assert_eq!("RadioID", packs::pack_name("   ", "RadioID"));
    assert_eq!("ABCDEFGHIJKLMNOP", packs::pack_name("ABCDEFGHIJKLMNOPQRST", "x"), "16 characters at most");
    assert_eq!("81,6 KB", packs::kb("tr", 83_520));
    assert_eq!("81.6 KB", packs::kb("en", 83_520));
    assert_eq!("2 KB", packs::kb("en", 2048));
    assert_eq!("2026-11-24", packs::date("20261124"));
    let slot = |valid: bool, crc: bool, force: bool| {
        json!({"type": "radioid", "slot": 1, "address": "0xEA0000", "valid": valid, "crc_ok": crc, "in_force": force,
            "seq": 4, "name": "RadioID TR", "date": "20261124", "bytes": 1984, "count": 2})
    };
    assert_eq!("RadioID slot 1 (0xEA0000): empty", packs::slot_line("en", &slot(false, false, false)));
    assert_eq!(
        "RadioID slot 1 (0xEA0000): \"RadioID TR\" damaged, not used",
        packs::slot_line("en", &slot(true, false, false))
    );
    assert_eq!(
        "RadioID slot 1 (0xEA0000): \"RadioID TR\", 2026-11-24, 2 entries, 2 KB, sequence 4: IN FORCE",
        packs::slot_line("en", &slot(true, true, true))
    );
    assert!(packs::slot_line("tr", &slot(true, true, false)).ends_with("sıra 4: eski"));
}

#[test]
fn a_dpk_loads_and_the_write_asks_first() {
    let root = root("packs");
    let ui = window(&root, json!({"language": "en", "pack_countries": "Turkey, Cyprus", "pack_radioid_name": "TR+CY"}));
    let p = ui.global::<Packs>();
    let app = ui.global::<App>();
    // the C# program's saved countries and name
    assert_eq!(("Turkey, Cyprus", "TR+CY"), (p.get_countries().as_str(), p.get_radioid_name().as_str()));
    assert_eq!("TR aerodromes", p.get_air_name().as_str());
    app.invoke_navigate(3);
    assert!(!p.get_radioid_pack() && !p.get_air_pack());
    assert_eq!("No pack built or chosen yet.", p.get_chosen_text().as_str());
    let f = radioid_pack(&root);
    packs::load_dpk(&ui, &f);
    assert!(p.get_radioid_pack() && !p.get_air_pack());
    assert!(
        p.get_chosen_text().starts_with("RadioID pack \"RadioID TR\", 2026-11-24: 2 IDs, "),
        "{}",
        p.get_chosen_text()
    );
    // the write: the question first (the pack, the port, both slots, the sectors); nothing starts before Yes
    p.invoke_write_radioid();
    assert!(p.get_write_confirm_open());
    let q = p.get_write_confirm_text().to_string();
    assert!(q.contains("Slots 0xE70000 / 0x") && q.contains("1 sectors") && q.ends_with("Write now?"), "{q}");
    assert!(!app.get_busy(), "no radio step before Yes");
    // a file that is not a whole pack is refused
    let bad = root.join("bad.dpk");
    std::fs::write(&bad, b"DPK1 not a pack").unwrap();
    packs::load_dpk(&ui, &bad);
    assert!(p.get_result().starts_with("Failed: bad.dpk: not a whole data pack"), "{}", p.get_result());
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn a_backup_shows_its_packs() {
    let root = root("packs-backup");
    let pk = ddm32_core::datapacks::files::load(&radioid_pack(&root)).unwrap();
    let mut q128 = vec![0xFFu8; dp::REGION_END as usize];
    let a = dp::slot_address(dp::RADIO_ID, 0).unwrap() as usize;
    q128[a..a + pk.image.len()].copy_from_slice(&pk.image);
    let b = root.join("dm32-20261124");
    fake_backup(&b, &q128);
    let lines = packs::backup_lines("en", &b).unwrap();
    assert_eq!("Backup dm32-20261124:", lines[0]);
    assert!(
        lines[1].starts_with("RadioID slot 0 (0xe70000): \"RadioID TR\", 2026-11-24, 2 entries")
            && lines[1].ends_with("IN FORCE"),
        "{lines:?}"
    );
    assert!(lines[2].ends_with("empty") && lines[3].starts_with("Air band slot 0") && lines.len() == 5, "{lines:?}");
    // not verified: said so, nothing read
    std::fs::remove_file(b.join("backup-manifest.json")).unwrap();
    assert_eq!(Err("not a verified backup".to_string()), packs::backup_lines("en", &b));
    let _ = std::fs::remove_dir_all(&root);
}

// ---- my channels ------------------------------------------------------------------------------------------------------

#[test]
fn channels_filter_sort_and_all_columns() {
    let cp = Codeplug::decode(&sample_image(), &EN).unwrap();
    let q = channels::Query::default();
    let (cols, rows, total) = channels::view(&cp, Table::Channels, &q);
    assert_eq!(vec!["No", "Name", "Frequency", "Mode", "Settings"], cols);
    assert_eq!(total, rows.len());
    // all columns: every field but the two made from others
    let all = channels::shown_columns(&cp, Table::Channels, true);
    assert_eq!(cp.columns(Table::Channels).len() - 2, all.len());
    assert!(!all.iter().any(|c| c.0 == "freq" || c.0 == "summary"));
    let (w, min) = channels::widths(&cp, Table::Channels, true);
    assert!((w.iter().sum::<f32>() - 1.0).abs() < 1e-4 && min > 900.0, "all columns are wider than the window");
    assert_eq!(0.0, channels::widths(&cp, Table::Channels, false).1);
    // the mode filter
    let (mode, _) = channels::modes(&cp)[0].clone();
    let (_, shown) =
        channels::query_rows(&cp, Table::Channels, &channels::Query { mode: Some(&mode), ..Default::default() });
    assert!(!shown.is_empty() && shown.iter().all(|r| r["mode"] == mode));
    // the zone filter: that zone's channels in its order
    if let Some(z) = cp.zones.iter().find(|z| !z.channels.is_empty()) {
        let (all, shown) =
            channels::query_rows(&cp, Table::Channels, &channels::Query { zone: Some(z.slot), ..Default::default() });
        assert_eq!(z.channels.len(), all.len());
        let numbers: Vec<i64> = shown.iter().map(|r| r["number"].parse().unwrap()).collect();
        assert_eq!(z.channels, numbers);
    }
    // the sort: numbers as numbers, the other way round on the second click
    let (_, desc) =
        channels::query_rows(&cp, Table::Channels, &channels::Query { sort: Some((0, false)), ..Default::default() });
    let n: Vec<i64> = desc.iter().map(|r| r["number"].parse().unwrap()).collect();
    let mut sorted = n.clone();
    sorted.sort_by(|a, b| b.cmp(a));
    assert_eq!(sorted, n);
    assert_eq!(std::cmp::Ordering::Less, channels::compare_cells("9", "10"));
    assert_eq!(std::cmp::Ordering::Less, channels::compare_cells("alpha", "Beta"));
    // the modes in the C# order
    let order: Vec<String> = channels::modes(&cp).into_iter().map(|m| m.0).collect();
    let rank = |m: &str| channels::MODE_ORDER.iter().position(|x| *x == m).unwrap_or(99);
    assert!(order.windows(2).all(|w| rank(&w[0]) <= rank(&w[1])), "{order:?}");
}

#[test]
fn another_backup_opens_and_a_zone_double_click_filters() {
    let root = root("channels");
    let ui = window(&root, json!({"language": "en"}));
    let r = ui.global::<Radio>();
    let b = root.join("elsewhere").join("dm32-x");
    fake_backup(&b, &sample_image());
    ui.global::<App>().invoke_navigate(9);
    channels::open_backup(&ui, &b);
    assert!(r.get_table_loaded());
    assert_eq!("Source: dm32-x (the last backup; the radio is not asked)", r.get_channels_source().as_str());
    let cp = Codeplug::decode(&sample_image(), &EN).unwrap();
    assert_eq!(format!("Showing {0} of {0} rows", cp.channels.len()), r.get_table_count().as_str());
    // the zones tab, a double click on its first zone: the channels tab with that zone's channels
    r.invoke_choose_table(1);
    assert_eq!(cp.zones.len(), r.get_table_rows().row_count());
    r.invoke_zone_activate(0);
    assert_eq!(0, r.get_table());
    assert_eq!(cp.zones[0].channels.len(), r.get_table_rows().row_count());
    assert!(r.get_zone_text().contains(&cp.zones[0].name));
    // the zone picker: "All channels" again
    r.invoke_open_zone_picker();
    assert!(r.get_zone_picker_open());
    assert_eq!("zone:-1", r.get_pick_rows().row_data(0).unwrap().key.as_str());
    r.invoke_pick("zone:-1".into());
    assert!(!r.get_zone_picker_open());
    assert_eq!(cp.channels.len(), r.get_table_rows().row_count());
    // a click on a column's title sorts; the second click turns it round
    r.invoke_sort(1);
    assert_eq!((1, true), (r.get_sort_column(), r.get_sort_ascending()));
    r.invoke_sort(1);
    assert_eq!((1, false), (r.get_sort_column(), r.get_sort_ascending()));
    // all columns
    r.invoke_toggle_columns(true);
    assert_eq!(cp.columns(Table::Channels).len() - 2, r.get_table_columns().row_count());
    assert!(r.get_table_min_width() > 900.0);
    // the chosen backup stays when the page is opened again
    ui.global::<App>().invoke_navigate(0);
    ui.global::<App>().invoke_navigate(9);
    assert!(r.get_channels_source().contains("dm32-x"));
    let _ = std::fs::remove_dir_all(&root);
}

// ---- health -----------------------------------------------------------------------------------------------------------

#[test]
fn read_every_30_s_only_on_the_health_page_with_a_free_port() {
    let root = root("health");
    let ui = window(&root, json!({"language": "en"}));
    let app = ui.global::<App>();
    let r = ui.global::<Radio>();
    app.invoke_navigate(7);
    r.invoke_health_auto_changed(true);
    assert!(r.get_health_auto());
    // no cable known: no read
    assert!(!ddm32_gui::radio_pages::health_auto_tick(&ui));
    ddm32_gui::with_state(|s| s.port_now.port = Some("COM14".into()));
    // another job holds the port, or the disclaimer is not accepted: no read
    app.set_busy(true);
    assert!(!ddm32_gui::radio_pages::health_auto_tick(&ui));
    app.set_busy(false);
    r.set_slog_running(true);
    assert!(!ddm32_gui::radio_pages::health_auto_tick(&ui));
    r.set_slog_running(false);
    r.set_disclaimer_ok(false);
    assert!(!ddm32_gui::radio_pages::health_auto_tick(&ui));
    r.set_disclaimer_ok(true);
    // leaving the page ends it (it is never kept)
    app.invoke_navigate(0);
    assert!(!r.get_health_auto());
    assert!(ddm32_gui::with_state(|s| s.health_timer.is_none()));
    assert!(!ddm32_gui::radio_pages::health_auto_tick(&ui));
    let _ = std::fs::remove_dir_all(&root);
}

// ---- the window -------------------------------------------------------------------------------------------------------

#[test]
fn window_size_and_place_in_the_csharp_format() {
    // what the C# program writes (doubles, "max")
    let saved = json!({"x": 120.0, "y": 80.5, "w": 1024.0, "h": 768.0, "max": false});
    let screen = Some(window::Rect { x: 0.0, y: 0.0, w: 1920.0, h: 1040.0 });
    assert_eq!(
        (Some(window::Rect { x: 120.0, y: 80.5, w: 1024.0, h: 768.0 }), false),
        window::restore_plan(Some(&saved), screen)
    );
    // off every screen (a monitor unplugged): the OS places it; maximized is kept
    let gone = json!({"x": 2500, "y": 100, "w": 900, "h": 700, "max": true});
    assert_eq!((None, true), window::restore_plan(Some(&gone), screen));
    // only a sliver on screen is not enough
    let sliver = json!({"x": 1850, "y": 100, "w": 900, "h": 700});
    assert_eq!(None, window::restore_plan(Some(&sliver), screen).0);
    // smaller than the window's minimum, or damaged: not used
    assert_eq!(None, window::restore_plan(Some(&json!({"x": 0, "y": 0, "w": 300, "h": 200})), screen).0);
    assert_eq!((None, false), window::restore_plan(Some(&json!({"x": "a"})), screen));
    assert_eq!((None, false), window::restore_plan(None, screen));
    // larger than the screens: cut to them
    let big = json!({"x": 0, "y": 0, "w": 4000, "h": 3000});
    assert_eq!(Some(window::Rect { x: 0.0, y: 0.0, w: 1920.0, h: 1040.0 }), window::restore_plan(Some(&big), screen).0);
    // the save: the normal place now; maximized / minimized keep the last normal place
    let now = window::Rect { x: 10.0, y: 20.0, w: 900.0, h: 720.0 };
    let g = window::save_plan(now, false, false, None).unwrap();
    assert_eq!(json!({"x": 10.0, "y": 20.0, "w": 900.0, "h": 720.0, "max": false}), window::to_json(&g));
    let g =
        window::save_plan(window::Rect { x: -8.0, y: -8.0, w: 1936.0, h: 1056.0 }, true, false, Some(&saved)).unwrap();
    assert_eq!(json!({"x": 120.0, "y": 80.5, "w": 1024.0, "h": 768.0, "max": true}), window::to_json(&g));
    assert!(
        window::save_plan(window::Rect { x: -32000.0, y: -32000.0, w: 160.0, h: 28.0 }, false, true, None).is_none()
    );
    // the C# program reads it back
    assert_eq!(window::parse(Some(&window::to_json(&g))), Some(g));
}

#[test]
fn offline_help_and_the_menu() {
    let s = help::sections("en", "C:/Programs/DDM32 Plus/backups");
    assert_eq!(8, s.len());
    assert_eq!("1. The radio was not caught (timeout)", s[0].0);
    assert!(s[6].1.starts_with("In the backups folder inside the program folder: C:/Programs/DDM32 Plus/backups\n"));
    assert!(help::sections("tr", "x").iter().all(|(t, b)| !t.is_empty() && !b.contains("help_")));
    use ddm32_gui::{parse_menu_action, MenuAction};
    assert_eq!(Some(MenuAction::Page(11)), parse_menu_action("page:11"));
    assert_eq!(Some(MenuAction::Page(12)), parse_menu_action("page:12"));
    assert_eq!(None, parse_menu_action("page:13"));
    assert_eq!(Some(MenuAction::Lang("en".into())), parse_menu_action("lang:en"));
    assert_eq!(None, parse_menu_action("lang:de"));
    // a release build has no local release folder
    if !ddm32_core::app_info::DEVELOPER {
        assert_eq!(None, parse_menu_action("local-release"));
    } else {
        assert_eq!(Some(MenuAction::LocalRelease), parse_menu_action("local-release"));
    }
    let menu = std::fs::read_to_string(Path::new(env!("CARGO_MANIFEST_DIR")).join("ui/menubar.slint")).unwrap();
    for a in [
        r#"action: "page:11""#,
        r#"action: "page:12""#,
        r#"action: "lang:tr""#,
        r#"action: "lang:en""#,
        r#"action: "local-release""#,
    ] {
        assert!(menu.contains(a), "{a}");
    }
    // the release build's title carries no developer mark
    assert_eq!(ddm32_core::app_info::DEVELOPER, !ddm32_gui::dev_title("en").is_empty());
}

#[test]
fn the_language_menu_and_show_flags() {
    let root = root("show");
    let ui = window(&root, json!({"language": "tr"}));
    let app = ui.global::<App>();
    app.invoke_menu_action("lang:en".into());
    assert_eq!("en", ddm32_gui::with_state(|s| s.lang.clone()));
    assert_eq!("en", Settings::new(root.join("settings.json")).load()["language"]);
    assert!(ddm32_gui::show_from_args(&ui, "versions"));
    assert_eq!(11, app.get_page());
    assert!(ddm32_gui::show_from_args(&ui, "live:demo"));
    assert_eq!(8, app.get_page());
    assert!(!ui.global::<Radio>().get_live_running(), "made-up readings: nothing holds the port");
    assert!(ui.global::<Radio>().get_live_facts().row_count() > 0);
    assert!(ddm32_gui::show_from_args(&ui, "help"));
    assert_eq!(12, app.get_page());
    assert_eq!(8, app.get_help_items().row_count());
    assert!(!ddm32_gui::show_from_args(&ui, "nothing"));
    assert_eq!(12, app.get_page());
    let _ = std::fs::remove_dir_all(&root);
}
