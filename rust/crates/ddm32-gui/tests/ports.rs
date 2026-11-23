// SPDX-License-Identifier: GPL-3.0-or-later
//! The cable picker: the OS's ports sorted into CH340 cables and "Advanced" (virtual pairs marked, macOS's /dev/tty.*
//! twins left out), the saved choice in settings.json, the fallback to Auto when the saved port is gone, and the texts
//! of the header and the radio pages. Built from made-up port lists: no port is listed from this computer or opened.
use ddm32_core::radio::portpick::{self, PortOptions, PortPref, ResolvedPort, SerialPortEntry};
use ddm32_core::storage::{Os, Settings};
use ddm32_gui::port_view;

fn e(name: &str, ch340: bool, virt: bool) -> SerialPortEntry {
    SerialPortEntry::new(name, ch340, virt)
}

fn windows() -> PortOptions {
    portpick::options(
        &[
            e("COM15", true, false),
            e("COM3", false, false),
            e("COM14", true, false),
            e("COM12", false, true),
            e("CNCB0", false, false),
            e("COM14", true, false),
        ],
        Os::Windows,
    )
}

#[test]
fn ports_are_sorted_into_cables_and_advanced() {
    let o = windows();
    assert_eq!(vec!["COM14", "COM15"], o.cables);
    let others: Vec<(&str, bool)> = o.others.iter().map(|x| (x.name.as_str(), x.virtual_port)).collect();
    // virtual pairs (com0com by its device, or by its default name) last and marked, never a cable
    assert_eq!(vec![("COM3", false), ("COM12", true), ("CNCB0", true)], others);
    let mac = portpick::options(
        &[
            e("/dev/tty.usbserial-1410", true, false),
            e("/dev/cu.usbserial-1410", true, false),
            e("/dev/cu.Bluetooth", false, false),
        ],
        Os::MacOs,
    );
    assert_eq!(vec!["/dev/cu.usbserial-1410"], mac.cables);
    assert_eq!(vec!["/dev/cu.Bluetooth"], mac.others.iter().map(|x| x.name.as_str()).collect::<Vec<_>>());
    let linux = portpick::options(
        &[e("/dev/ttyUSB1", true, false), e("/dev/ttyUSB0", true, false), e("/dev/ttyS0", false, false)],
        Os::Linux,
    );
    assert_eq!(vec!["/dev/ttyUSB0", "/dev/ttyUSB1"], linux.cables);
}

#[test]
fn auto_and_a_chosen_port_and_the_fallback() {
    let o = windows();
    // Auto: the first CH340 cable
    let auto = portpick::resolve(&PortPref::default(), &o, None);
    assert_eq!(ResolvedPort { port: Some("COM14".into()), auto: true, missing: None, not_ch340: false }, auto);
    // a chosen cable, and a chosen non-CH340 port (Advanced) said as such
    assert_eq!(Some("COM15".into()), portpick::resolve(&PortPref::fixed("COM15"), &o, None).port);
    let adv = portpick::resolve(&PortPref::fixed("COM3"), &o, None);
    assert!(adv.not_ch340 && !adv.auto);
    // the saved port is gone: Auto, and the window says so
    let gone = portpick::resolve(&PortPref::fixed("COM7"), &o, None);
    assert_eq!(
        ResolvedPort { port: Some("COM14".into()), auto: true, missing: Some("COM7".into()), not_ch340: false },
        gone
    );
    let v = port_view("en", &o, &gone);
    assert_eq!("Cable: COM14 (auto)", v.head);
    assert_eq!("Port used: COM14 (auto). The saved port COM7 is not there; using Auto.", v.line);
    assert_eq!("The saved port COM7 is not there; using Auto.", v.note);
    // nothing at all: no port, nothing claimed
    let none = portpick::resolve(&PortPref::fixed("COM7"), &PortOptions::default(), None);
    assert_eq!(None, none.port);
    assert_eq!("Kablo: bulunamadı", port_view("tr", &PortOptions::default(), &none).head);
    // the header and the lists in both languages
    let v = port_view("tr", &o, &adv);
    assert_eq!("Kablo: COM3 (CH340 değil)", v.head);
    assert_eq!("Şu an: COM14", v.auto_detail);
    assert_eq!(vec![("COM14".to_string(), "CH340 programlama kablosu".to_string())], v.cables[..1].to_vec());
    assert_eq!("Sanal port (com0com): telsiz değil", v.others[1].1);
    assert_eq!("Cable: COM15", port_view("en", &o, &portpick::resolve(&PortPref::fixed("COM15"), &o, None)).head);
}

#[test]
fn the_choice_is_kept_in_settings() {
    let dir = std::env::temp_dir().join(format!("ddm32-ports-settings-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let s = Settings::new(dir.join("settings.json"));
    assert_eq!(PortPref::default(), s.port_pref());
    s.set_port_pref(&PortPref::fixed("COM15")).unwrap();
    assert_eq!(PortPref::fixed("COM15"), s.port_pref());
    let raw: serde_json::Value = serde_json::from_slice(&std::fs::read(dir.join("settings.json")).unwrap()).unwrap();
    assert_eq!(raw["port"], "COM15");
    assert_eq!(raw["port_auto"], false);
    s.set_port_pref(&PortPref::default()).unwrap();
    assert_eq!(PortPref::default(), s.port_pref());
    // port_auto false without a port (a hand-edited file) is Auto
    s.save("port_auto", serde_json::json!(false)).unwrap();
    assert!(s.port_pref().auto);
    let _ = std::fs::remove_dir_all(&dir);
}
