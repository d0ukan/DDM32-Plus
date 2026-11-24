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
    ddm32_gui::setup_with_env(&ui, AppPaths::new(root), false, ddm32_gui::no_env);
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
        "RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY\n2869999,TA1TST,Test,X,Test City,,Turkey\n\
         2869998,TA1TSU,Test,Y,Test City,,Turkey\n3109999,K1TST,Test,Z,Test City,MA,United States\n",
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
    assert_eq!("Source: dm32-x (the folder you opened; the radio is not asked)", r.get_channels_source().as_str());
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
    let one = [window::Rect { x: 0.0, y: 0.0, w: 1920.0, h: 1040.0 }];
    let screen = &one[..];
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
    // the title bar above the monitor's top, or below its bottom: it could not be grabbed
    assert_eq!(None, window::restore_plan(Some(&json!({"x": 100, "y": -30, "w": 900, "h": 700})), screen).0);
    assert_eq!(None, window::restore_plan(Some(&json!({"x": 100, "y": 1010, "w": 900, "h": 700})), screen).0);
    // two monitors side by side, a smaller one on the right lower down: the gap above it is no place for a title bar
    let two = [one[0], window::Rect { x: 1920.0, y: 300.0, w: 1280.0, h: 720.0 }];
    assert_eq!(None, window::restore_plan(Some(&json!({"x": 2100, "y": 100, "w": 900, "h": 700})), &two).0);
    let right = window::restore_plan(Some(&json!({"x": 2100, "y": 320, "w": 900, "h": 900})), &two).0.unwrap();
    assert_eq!(720.0, right.h, "cut to the monitor it is on");
    // no monitors known (Linux, macOS): only an absurd place is refused
    assert!(window::restore_plan(Some(&saved), &[]).0.is_some());
    assert!(window::restore_plan(Some(&json!({"x": 1e9, "y": 0, "w": 900, "h": 700})), &[]).0.is_none());
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

// ---- second round (verifier) ------------------------------------------------------------------------------------------

/// A lookup that answers LOCALAPPDATA / APPDATA with folders inside the test's own temporary folder: whatever the
/// window finds as "another program's backups" must lie under it.
fn sandbox_env(name: &str) -> Option<String> {
    SANDBOX.with(|s| s.borrow().as_ref().map(|r| r.join(name).display().to_string()))
}

thread_local! {
    static SANDBOX: std::cell::RefCell<Option<PathBuf>> = const { std::cell::RefCell::new(None) };
}

#[test]
fn every_test_window_stays_in_its_own_folder() {
    // the tests never use the real environment (the owner's backups): every window of the test files is made with
    // setup_with_env and no_env (or this sandbox)
    let tests = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests");
    for e in std::fs::read_dir(&tests).unwrap() {
        let p = e.unwrap().path();
        let text = std::fs::read_to_string(&p).unwrap();
        // the real environment, the real program folder, a plain setup call (imported or not): setup_with_env only
        let forbidden = [
            format!("{}_env", "real"),
            format!("AppPaths::{}", "detect"),
            format!("AppPaths::{}", "for_this_program"),
            format!("env::var(\"{}", "LOCALAPPDATA"),
            format!("env::var(\"{}", "APPDATA"),
            format!("env::var_os(\"{}", "LOCALAPPDATA"),
        ];
        for f in &forbidden {
            assert!(!text.contains(f.as_str()), "{}: {f}", p.display());
        }
        let plain = regex::Regex::new(&format!(r"(^|[^_A-Za-z]){}\(", "setup")).unwrap();
        assert!(!plain.is_match(&text), "{}: a plain setup call (the real environment)", p.display());
        let imported = regex::Regex::new(&format!(r"use ddm32_gui::\{{[^}}]*{}", "setup")).unwrap();
        assert!(!imported.is_match(&text), "{}: setup imported", p.display());
    }
    // a C# install and a Python folder inside the sandbox: found, and nothing outside it
    let root = root("sandbox");
    SANDBOX.with(|s| *s.borrow_mut() = Some(root.join("env")));
    let net =
        root.join("env").join("LOCALAPPDATA").join("Programs").join("DDM32 Plus").join("backups").join("dm32-net");
    fake_backup(&net, &sample_image());
    let data = root.join("data");
    std::fs::create_dir_all(&data).unwrap();
    std::fs::write(
        data.join("settings.json"),
        json!({"language": "en", "disclaimer": {"version": DISCLAIMER, "accepted": true}}).to_string(),
    )
    .unwrap();
    init();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup_with_env(&ui, AppPaths::new(&data), false, sandbox_env);
    ui.global::<App>().invoke_navigate(2);
    ui.global::<App>().invoke_navigate(9);
    let found = ddm32_gui::with_state(|s| s.found.iter().map(|b| b.folder.clone()).collect::<Vec<_>>());
    if cfg!(windows) {
        assert!(!found.is_empty(), "the sandboxed C# backup is found");
    }
    for f in &found {
        assert!(f.starts_with(&root), "{} is outside the test's folder", f.display());
    }
    if let Some(src) = ddm32_gui::with_state(|s| s.channels.source.clone()) {
        assert!(src.starts_with(&root), "{}", src.display());
    }
    SANDBOX.with(|s| *s.borrow_mut() = None);
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn a_damaged_settings_file_never_turns_into_a_csharp_one() {
    let root = root("damaged");
    let path = root.join("settings.json");
    std::fs::write(&path, b"{ \"language\": \"en\", \"disc").unwrap();
    let damaged = std::fs::read(&path).unwrap();
    // start: nothing is written over it
    init();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup_with_env(&ui, AppPaths::new(&root), false, ddm32_gui::no_env);
    assert_eq!(0, ui.global::<App>().get_channel());
    // close: the window's place (a C# key) is refused, the file stays as it was
    window::save(&ui);
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("1.0.1", "stable")])), false);
    assert_eq!(damaged, std::fs::read(&path).unwrap());
    // start again: still Latest
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup_with_env(&ui, AppPaths::new(&root), false, ddm32_gui::no_env);
    assert_eq!(0, ui.global::<App>().get_channel());
    // a readable file without `channel` gets it with the first C# key, as it read before (Latest here)
    let s = Settings::new(&path);
    std::fs::write(&path, r#"{"language": "en"}"#).unwrap();
    s.save("window", json!({"x": 1, "y": 2, "w": 900, "h": 700, "max": false})).unwrap();
    assert_eq!(0, s.load()["channel"]);
    assert_eq!(0, s.channel());
    // and a C# file keeps its Beta
    std::fs::write(&path, r#"{"language": "en", "stock_backups": []}"#).unwrap();
    s.save("last_check_utc", json!("2026-11-24T00:00:00Z")).unwrap();
    assert_eq!(1, s.load()["channel"]);
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn a_late_yes_starts_nothing_while_the_port_is_held() {
    let root = root("late-yes");
    let ui = window(&root, json!({"language": "en"}));
    let app = ui.global::<App>();
    let r = ui.global::<Radio>();
    let p = ui.global::<Packs>();
    for hold in 0..4 {
        match hold {
            0 => app.set_busy(true),
            1 => r.set_busy(true),
            2 => r.set_live_running(true),
            _ => r.set_slog_running(true),
        }
        app.set_message("".into());
        p.invoke_write_yes();
        assert_eq!(i18n::t("en", "slog_busy_other"), app.get_message().as_str(), "write, hold {hold}");
        app.set_message("".into());
        app.invoke_res_confirm_yes();
        assert_eq!(i18n::t("en", "slog_busy_other"), app.get_message().as_str(), "restore, hold {hold}");
        app.set_message("".into());
        p.invoke_read_radio();
        assert_eq!(i18n::t("en", "slog_busy_other"), app.get_message().as_str(), "read, hold {hold}");
        // the wizard's backup and install: refused before anything of the wizard moves
        for (step, name) in [(2, "backup"), (4, "install")] {
            app.set_step(step);
            app.set_message("".into());
            if step == 2 {
                app.invoke_start_backup();
            } else {
                app.invoke_start_install();
            }
            assert_eq!(i18n::t("en", "slog_busy_other"), app.get_message().as_str(), "{name}, hold {hold}");
            assert!(
                ddm32_gui::with_state(|s| s.wizard.running.is_none() && s.wizard.backup_dir.is_none()),
                "{name}, hold {hold}: the wizard moved"
            );
        }
        assert!(ddm32_gui::with_state(|s| !s.writing && s.restore.running.is_none()));
        app.set_busy(false);
        r.set_busy(false);
        r.set_live_running(false);
        r.set_slog_running(false);
    }
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn the_selection_follows_its_version_and_the_question_names_the_backup_once() {
    let root = root("follow");
    let ui = window(&root, json!({"language": "en", "channel": 2}));
    let app = ui.global::<App>();
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("1.0.1", "stable"), ("1.0.0", "stable")])), false);
    app.invoke_navigate(11);
    app.set_ver_selected(1);
    // a newer version arrives above it: the same version stays selected
    ddm32_gui::catalog_loaded(
        &ui,
        Ok(catalog(&[("1.1.0-beta.1", "beta"), ("1.0.1", "stable"), ("1.0.0", "stable")])),
        false,
    );
    let i = app.get_ver_selected();
    assert_eq!("1.0.0", app.get_ver_rows().row_data(i as usize).unwrap().version.as_str());
    // a version that went away: nothing selected (Switch is off)
    ddm32_gui::catalog_loaded(&ui, Ok(catalog(&[("1.0.1", "stable")])), false);
    assert_eq!(-1, app.get_ver_selected());
    // the restore question: the backup once (its full path), then the plan
    app.invoke_navigate(2);
    let b = root.join("backups").join("dm32-x");
    ddm32_gui::with_state(|s| s.restore.backup_dir = Some(b.clone()));
    app.set_restore_ready(true);
    app.set_res_status(format!("{}\nplan", i18n::tf("en", "res_chosen", "dm32-x")).into());
    app.invoke_restore();
    let q = app.get_res_confirm_text().to_string();
    assert_eq!(1, q.matches("Chosen backup").count(), "{q}");
    // the channels page says a folder the user opened is one
    let o = root.join("opened").join("dm32-y");
    fake_backup(&o, &sample_image());
    app.set_res_confirm_open(false);
    app.invoke_navigate(9);
    channels::open_backup(&ui, &o);
    assert_eq!(
        "Source: dm32-y (the folder you opened; the radio is not asked)",
        ui.global::<Radio>().get_channels_source().as_str()
    );
    let _ = std::fs::remove_dir_all(&root);
}

// ---- third round: a backup that really detects as stock firmware (a synthetic codec; the real stock bytes are never
// in the repository) --------------------------------------------------------------------------------------------------

const START: usize = 0xC000;
const SIZE: usize = 0x100000;
const SECTOR: usize = 4096;
const DATA_SIZE: usize = 0x1000000;
const STOCK_CODE_AT: u64 = 0x030C68B8;
const STOCK_TABLES_AT: u64 = 0x03072658;

fn pattern(n: usize, k: usize) -> Vec<u8> {
    (0..n).map(|i| ((i * k + (i >> 8) + 1) & 255) as u8).collect()
}

fn le(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

/// The core tests' synthetic stock codec (tests/common synthetic_codec / synthetic_codec_tables).
fn synthetic_codec() -> (&'static ddm32_core::radio::codec::Codec, Vec<u8>, Vec<u8>) {
    let run = 0x16CDCu64;
    let mut code = pattern(0x2000, 29);
    code[..8].copy_from_slice(&le(&[0x152514D8, 0x74C776CD]));
    let refs = vec![(0x40, run + 0x100), (0x84, run), (0x1000, run + 0x2000 - 4)];
    for &(off, v) in &refs {
        code[off as usize..off as usize + 4].copy_from_slice(&(v as u32).to_le_bytes());
    }
    let tables = pattern(0x2F78, 31);
    let codec = ddm32_core::radio::codec::Codec::synthetic(
        &code,
        &tables,
        &refs,
        STOCK_CODE_AT,
        STOCK_TABLES_AT,
        "DM32.01.01.047.bin",
        47,
    );
    (Box::leak(Box::new(codec)), code, tables)
}

/// A stock firmware program image holding the synthetic codec where the stock firmware keeps it.
fn stock_program(code: &[u8], tables: &[u8]) -> Vec<u8> {
    let rep: Vec<u8> = (0..0x3000).map(|i| (i & 255) as u8).collect();
    let boot = [pattern(0x1000, 7), (0..0xB000).map(|i| (i & 255) as u8).collect()].concat();
    let mut p = [boot, pattern(0x800, 19), rep, vec![0xFF; SIZE - START - 0x3800]].concat();
    let c = (STOCK_CODE_AT - 0x03000000) as usize;
    p[c..c + code.len()].copy_from_slice(code);
    let t = (STOCK_TABLES_AT - 0x03000000) as usize;
    p[t..t + tables.len()].copy_from_slice(tables);
    p
}

/// A complete verified two-pass backup (both chips), as the backup tool writes it.
fn write_backup(folder: &Path, program: &[u8], data: &[u8]) {
    std::fs::create_dir_all(folder).unwrap();
    let mut captures = vec![];
    for (memory, image) in [("progmem", program), ("q128", data)] {
        for n in 1..=2 {
            let name = format!("{memory}-pass{n}.bin");
            std::fs::write(folder.join(&name), image).unwrap();
            let sectors: Vec<String> = image.chunks(SECTOR).map(sm::sha256_hex).collect();
            captures.push(json!({"file": name, "bytes": image.len(), "sha256": sm::sha256_hex(image),
                "sector_sha256": sectors}));
        }
    }
    let m = json!({"backup_verified": true, "captures": captures,
        "chips": {"progmem": {"jedec": "c84014", "bytes": SIZE}, "q128": {"jedec": "ef4018", "bytes": DATA_SIZE}}});
    std::fs::write(folder.join("backup-manifest.json"), m.to_string()).unwrap();
}

#[test]
fn stock_backups_are_recorded_by_the_wizard_only() {
    let root = root("stock-record");
    let ui = window(&root, json!({"language": "en"}));
    let app = ui.global::<App>();
    let settings = Settings::new(root.join("settings.json"));
    let (codec, code, tables) = synthetic_codec();
    ddm32_gui::use_codec(codec);
    let b = root.join("backups").join("dm32-stock");
    write_backup(&b, &stock_program(&code, &tables), &vec![0xFFu8; DATA_SIZE]);
    // the fixture really is stock firmware
    let d = ddm32_core::radio::flow::FileSteps::detect_backup(&b, codec).unwrap();
    assert_eq!(Some("stock"), d.str("layout"));
    // chosen with "Choose another backup folder" or from the list: planned as stock, never recorded
    restore::other_folder(&ui, &b);
    let seen = ddm32_gui::with_state(|s| s.restore.detection.as_ref().and_then(|d| d.str("layout")).map(String::from));
    assert_eq!(Some("stock".to_string()), seen, "the restore page saw a stock backup");
    restore::choose(&ui, None, &b);
    assert!(restore::stock_backups(&settings).is_empty(), "a backup the user showed is not recorded");
    // the wizard's own backup, at its check: recorded
    ddm32_gui::with_state(|s| {
        s.wizard.set_port(Some("COM1")).unwrap();
        s.wizard.start_backup(&b).unwrap();
        s.wizard.backup_finished(&ddm32_core::radio::flow::StepResult::backup(&b, None));
    });
    app.invoke_navigate(1);
    ddm32_gui::with_state(|s| {
        s.wizard.set_port(Some("COM1")).unwrap();
        s.wizard.start_backup(&b).unwrap();
        s.wizard.backup_finished(&ddm32_core::radio::flow::StepResult::backup(&b, None));
    });
    app.set_step(2);
    app.invoke_wizard_next();
    assert_eq!(vec![b.display().to_string()], restore::stock_backups(&settings));
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn settings_files_the_csharp_program_reads_as_empty() {
    let root = root("settings-kinds");
    let path = root.join("settings.json");
    let s = Settings::new(&path);
    let window = json!({"x": 1, "y": 2, "w": 900, "h": 700, "max": false});
    // 0 bytes and whitespace only: a fresh file; the C# key goes in with channel Latest
    for text in [&b""[..], &b" \r\n\t "[..]] {
        std::fs::write(&path, text).unwrap();
        assert!(!s.is_garbage());
        s.save("window", window.clone()).unwrap();
        assert_eq!((0, 0), (s.load()["channel"].as_i64().unwrap(), s.channel()));
    }
    // a UTF-8 BOM (Notepad) is read past: the keys stay
    std::fs::write(&path, [&[0xEF, 0xBB, 0xBF][..], br#"{"language": "en", "include_beta": true}"#].concat()).unwrap();
    assert_eq!(Some("en".to_string()), s.language());
    assert_eq!(1, s.channel());
    s.save("last_check_utc", json!("2026-11-24T00:00:00Z")).unwrap();
    assert_eq!(("en", 1), (s.load()["language"].as_str().unwrap(), s.load()["channel"].as_i64().unwrap()));
    // garbage: refused for a C# key, the file untouched, and the refusal is logged (never swallowed)
    std::fs::write(&path, b"not json at all").unwrap();
    assert!(s.is_garbage());
    assert!(s.save("window", window.clone()).is_err());
    assert_eq!(b"not json at all".to_vec(), std::fs::read(&path).unwrap());
    init();
    let ui = AppWindow::new().unwrap();
    ddm32_gui::setup_with_env(&ui, AppPaths::new(&root), false, ddm32_gui::no_env);
    window::save(&ui);
    assert_eq!(b"not json at all".to_vec(), std::fs::read(&path).unwrap());
    let log = std::fs::read_to_string(root.join("logs").join("settings.log")).unwrap();
    assert!(log.contains("window not saved"), "{log}");
    let _ = std::fs::remove_dir_all(&root);
}
