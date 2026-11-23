// SPDX-License-Identifier: GPL-3.0-or-later
//! The cable picker: which serial port a radio step uses. "Auto" (the default) is the CH340 cable as before; the user
//! may instead choose one port by name (settings.json `port`, `port_auto`). Ports are only ever found through the
//! OS's port and device lists: nothing here opens a port, not even to "probe" it.

use crate::storage::Os;

/// A serial port as the OS lists it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SerialPortEntry {
    pub name: String,
    /// A DM-32UV programming cable (WCH CH340 / CH341, USB VID 1A86 PID 7523 / 5523).
    pub ch340: bool,
    /// One end of a virtual port pair (com0com, tty0tty): never a radio.
    pub virtual_port: bool,
}

impl SerialPortEntry {
    pub fn new(name: &str, ch340: bool, virtual_port: bool) -> Self {
        Self { name: name.into(), ch340, virtual_port }
    }
}

/// What the picker offers: the CH340 cables first (the normal list), then every other port the OS reports (the
/// "Advanced" list: not a CH340 cable; virtual pairs marked and last).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PortOptions {
    pub cables: Vec<String>,
    pub others: Vec<SerialPortEntry>,
}

impl PortOptions {
    pub fn contains(&self, name: &str) -> bool {
        self.cables.iter().any(|c| c == name) || self.others.iter().any(|o| o.name == name)
    }
}

/// The user's choice (settings.json): automatic, or one port by name.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PortPref {
    pub auto: bool,
    pub port: Option<String>,
}

impl Default for PortPref {
    fn default() -> Self {
        Self { auto: true, port: None }
    }
}

impl PortPref {
    pub fn fixed(name: &str) -> Self {
        Self { auto: false, port: Some(name.into()) }
    }
}

/// The port a radio step uses now.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ResolvedPort {
    /// None: no port found (no cable).
    pub port: Option<String>,
    /// Chosen automatically (the setting is Auto, or the saved port is gone).
    pub auto: bool,
    /// The saved port that the OS does not list now (the window falls back to Auto and says so).
    pub missing: Option<String>,
    /// The port in use is not a CH340 cable (chosen by hand in "Advanced").
    pub not_ch340: bool,
}

/// A name of a virtual port by itself: com0com's default names (CNCA0 / CNCB0), tty0tty's /dev/tnt*.
pub fn is_virtual_name(name: &str) -> bool {
    let up = name.to_uppercase();
    up.starts_with("CNCA") || up.starts_with("CNCB") || name.starts_with("/dev/tnt")
}

/// COM3 < COM14 (by number), else by name.
fn sort_key(name: &str) -> (u8, String, u64) {
    let up = name.to_uppercase();
    match up.strip_prefix("COM").and_then(|n| n.parse::<u64>().ok()) {
        Some(n) => (0, String::new(), n),
        None => {
            // /dev/ttyUSB2 < /dev/ttyUSB10
            let digits: String =
                name.chars().rev().take_while(|c| c.is_ascii_digit()).collect::<Vec<_>>().into_iter().rev().collect();
            let stem = name[..name.len() - digits.len()].to_string();
            (1, stem, digits.parse().unwrap_or(0))
        }
    }
}

/// The picker's lists from the OS's ports: names once each (a port listed twice keeps every flag it was given),
/// macOS's call-in /dev/tty.* twins left out (the /dev/cu.* port is the one to use), sorted by number.
pub fn options(entries: &[SerialPortEntry], os: Os) -> PortOptions {
    let mut merged: Vec<SerialPortEntry> = Vec::new();
    for e in entries {
        if e.name.trim().is_empty() || (os == Os::MacOs && e.name.starts_with("/dev/tty.")) {
            continue;
        }
        let virt = e.virtual_port || is_virtual_name(&e.name);
        match merged.iter_mut().find(|m| m.name == e.name) {
            Some(m) => {
                m.ch340 |= e.ch340;
                m.virtual_port |= virt;
            }
            None => merged.push(SerialPortEntry { name: e.name.clone(), ch340: e.ch340, virtual_port: virt }),
        }
    }
    merged.sort_by_key(|e| (e.virtual_port, sort_key(&e.name)));
    let cables = merged.iter().filter(|e| e.ch340 && !e.virtual_port).map(|e| e.name.clone()).collect();
    let others = merged.into_iter().filter(|e| !e.ch340 || e.virtual_port).collect();
    PortOptions { cables, others }
}

/// The port to use: the saved one while the OS lists it, else the automatic choice (`auto_port`, Windows' cable check,
/// or the first CH340 cable) with the saved one named as missing.
pub fn resolve(pref: &PortPref, opts: &PortOptions, auto_port: Option<&str>) -> ResolvedPort {
    let automatic = || auto_port.map(str::to_string).or_else(|| opts.cables.first().cloned());
    match (&pref.auto, &pref.port) {
        (false, Some(p)) if opts.contains(p) => {
            ResolvedPort { port: Some(p.clone()), auto: false, missing: None, not_ch340: !opts.cables.contains(p) }
        }
        (false, Some(p)) => ResolvedPort { port: automatic(), auto: true, missing: Some(p.clone()), not_ch340: false },
        _ => ResolvedPort { port: automatic(), auto: true, missing: None, not_ch340: false },
    }
}

/// Every serial port this computer lists, without opening any: the OS's port list (only in a build with the `radio`
/// feature), Windows' CH340 devices (SetupAPI, in every build) and com0com's virtual pairs (marked).
pub fn list_serial_ports() -> Vec<SerialPortEntry> {
    list_serial_ports_with(&super::cable::check())
}

/// As [`list_serial_ports`], with a cable check the caller already made (one device listing per scan).
pub fn list_serial_ports_with(status: &super::cable::CableStatus) -> Vec<SerialPortEntry> {
    let mut v: Vec<SerialPortEntry> = Vec::new();
    #[cfg(feature = "radio")]
    for p in serialport::available_ports().unwrap_or_default() {
        let ch340 = match &p.port_type {
            serialport::SerialPortType::UsbPort(u) => u.vid == 0x1A86 && (u.pid == 0x7523 || u.pid == 0x5523),
            _ => false,
        };
        v.push(SerialPortEntry { name: p.port_name, ch340, virtual_port: false });
    }
    for p in &status.ports {
        v.push(SerialPortEntry::new(p, true, false));
    }
    for p in super::cable::virtual_ports() {
        v.push(SerialPortEntry::new(&p, false, true));
    }
    v
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sort_by_number() {
        let e: Vec<_> = ["COM14", "COM3", "/dev/ttyUSB10", "/dev/ttyUSB2"]
            .iter()
            .map(|n| SerialPortEntry::new(n, true, false))
            .collect();
        assert_eq!(options(&e, Os::Windows).cables, vec!["COM3", "COM14", "/dev/ttyUSB2", "/dev/ttyUSB10"]);
    }
}
