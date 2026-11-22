// SPDX-License-Identifier: GPL-3.0-or-later
//! What the radio pages show, from the C# program's synthetic fixtures (no radio): the channel table and its search,
//! the health light and the live facts.
use ddm32_core::codeplug::{Codeplug, Table, TR};
use ddm32_core::radio::live;
use ddm32_gui::radio_pages::{health_view, live_view, slog_searches, slog_stats_line, table_view};
use std::io::Read;

fn fixtures() -> std::path::PathBuf {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../win/tests/DDM32Plus.Core.Tests/Fixtures")
}

fn sample() -> Vec<u8> {
    use base64::Engine;
    let f = std::fs::File::open(fixtures().join("codeplug-results.json.gz")).unwrap();
    let mut s = String::new();
    flate2::read::GzDecoder::new(f).read_to_string(&mut s).unwrap();
    let v: serde_json::Value = serde_json::from_str(&s).unwrap();
    let mut img = vec![0xFFu8; 1 << 24];
    for run in v["images"]["sample"]["sparse"].as_array().unwrap() {
        let a = run[0].as_u64().unwrap() as usize;
        let d = base64::engine::general_purpose::STANDARD.decode(run[1].as_str().unwrap()).unwrap();
        img[a..a + d.len()].copy_from_slice(&d);
    }
    img
}

#[test]
fn the_channel_table_shows_five_plain_columns_and_searches() {
    let cp = Codeplug::decode(&sample(), &TR).unwrap();
    let (cols, rows) = table_view(&cp, Table::Channels, "");
    assert_eq!(vec!["No", "Ad", "Frekans", "Mod", "Ayar"], cols);
    assert_eq!(cp.channels.len(), rows.len());
    assert!(rows.iter().all(|r| r.len() == 5));
    let (_, p25) = table_view(&cp, Table::Channels, "p25");
    assert_eq!(vec!["4", "5"], p25.iter().map(|r| r[0].as_str()).collect::<Vec<_>>());
    let (zcols, _) = table_view(&cp, Table::Zones, "");
    assert_eq!(4, zcols.len());
}

#[test]
fn the_health_light_and_the_live_facts() {
    let lines = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
    let ok = live::health_summary(&lines(&[
        "--- view tasks (20) status 0",
        "tasks.free.IDLE=452",
        "tasks.heap_min_free=4048",
    ]));
    assert_eq!("green", health_view("tr", &ok).0);
    let low = live::health_summary(&lines(&["--- view tasks (20) status 0", "tasks.free.IDLE=150"]));
    assert_eq!("amber", health_view("en", &low).0);
    let crash = live::health_summary(&lines(&["--- view crash (21) status 0", "crash.valid=1", "crash.task=ui"]));
    let (v, text, facts, _) = health_view("en", &crash);
    assert_eq!("red", v);
    assert!(text.contains("restarted"));
    assert!(facts.iter().any(|(_, x)| x.starts_with("ui")));
    let poll = lines(&["--- view core (0) status 0", "rssi_dbm=-87", "rx_hz=434250000", "tx_hz=434250000"]);
    let s = live::live_summary(&live::parse_blocks(&poll), &Default::default());
    let (sig, smeter, dbm, facts) = live_view("tr", &s);
    assert_eq!("S9+6", smeter);
    assert_eq!("-87 dBm", dbm);
    assert!((sig - 0.53).abs() < 0.001);
    assert!(facts.iter().any(|(_, x)| x == "434,25000 MHz / 434,25000 MHz"));
}

#[test]
fn the_cable_hint_follows_the_device_list() {
    use ddm32_core::radio::cable::{evaluate, PnpDevice};
    let dev = |name: &str, code: i32| PnpDevice {
        device_id: r"USB\VID_1A86&PID_7523".into(),
        name: name.into(),
        error_code: code,
    };
    let (t, k) = ddm32_gui::cable_text("en", &evaluate(&[dev("USB2.0-Serial", 28)]), &[]);
    assert!(t.contains("CH340 driver is missing (Windows code 28)") && k == "warning", "{t}");
    let (t, k) = ddm32_gui::cable_text("tr", &evaluate(&[dev("USB-SERIAL CH340 (COM14)", 0)]), &[]);
    assert_eq!(("Kablo bulundu: COM14. Telsiz kapalıyken takılı kalabilir.", "success"), (t.as_str(), k));
    let (t, _) = ddm32_gui::cable_text("en", &evaluate(&[]), &["/dev/ttyUSB0".to_string()]);
    assert!(t.contains("/dev/ttyUSB0"));
    let (_, k) = ddm32_gui::cable_text("en", &evaluate(&[]), &[]);
    assert_eq!("info", k);
}

#[test]
fn listing_windows_devices_opens_nothing() {
    // only reads the device list (SetupAPI); empty on other systems
    let devices = ddm32_core::radio::cable::list_devices();
    assert!(devices.iter().all(|d| d.device_id.to_uppercase().contains("VID_1A86")));
}

#[test]
fn the_search_log_shows_a_stats_line_in_both_languages_and_none_without_stats() {
    use ddm32_core::radio::searchlog::{SearchLogBook, Stamp};
    let st = "fast,60000,4,4100,161,12,3,7,1,1,4,2,3,3800,6200,6200,1400,420,810";
    let mut lines: Vec<String> =
        ["p25s.on=1", "p25s.at=433512500", "p25s.seq=1"].iter().map(|s| s.to_string()).collect();
    let mut b = SearchLogBook::new();
    b.add(&lines, Stamp(1_795_298_400));
    // an older firmware: no stats line, the searches text as before
    assert_eq!(None, slog_stats_line("en", &b));
    assert!(!slog_searches("en", &b).contains('\n'));
    lines.push(format!("p25s.st={st}"));
    b.add(&lines, Stamp(1_795_298_401));
    assert_eq!(
        Some(
            "P25: Fast | pass 4.1 s (161 steps) | probes 12: found 3, no sync 7, partial 1, gone 1 | skipped 4, early 2, watch 3 \
             | probing 38% | first find 6.2 s | seen->found 1.4 s | sync 0.42 s (max 0.81 s)"
                .to_string()
        ),
        slog_stats_line("en", &b)
    );
    assert_eq!(
        Some(
            "P25: Hızlı | tur 4,1 sn (161 adım) | deneme 12: bulundu 3, senk yok 7, kısmi 1, kayboldu 1 | atlandı 4, erken 2, \
             izleme 3 | deneme payı %38 | ilk bulma 6,2 sn | görülme->bulma 1,4 sn | senk 0,42 sn (en çok 0,81 sn)"
                .to_string()
        ),
        slog_stats_line("tr", &b)
    );
    assert!(slog_searches("tr", &b).ends_with(&slog_stats_line("tr", &b).unwrap()));
}
