// SPDX-License-Identifier: GPL-3.0-or-later
//! The DM-32UV programming cable is a WCH CH340 (USB VID_1A86&PID_7523; the CH341 in serial mode is PID_5523).
//! Windows lists it even without its driver (problem code 28), so a missing driver is found without opening a port
//! (Radio/Cable.cs). Listing devices never opens a port.

/// A Plug and Play device as the OS lists it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PnpDevice {
    pub device_id: String,
    pub name: String,
    pub error_code: i32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CableState {
    Ok,
    NoCable,
    NoDriver,
    Problem,
    NoPort,
}

/// What the cable check found: the state, the port of a working cable, the Windows problem code, all ports.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CableStatus {
    pub state: CableState,
    pub port: Option<String>,
    pub error_code: i32,
    pub ports: Vec<String>,
}

const IDS: [&str; 2] = ["VID_1A86&PID_7523", "VID_1A86&PID_5523"];

pub fn is_ch340(d: &PnpDevice) -> bool {
    let id = d.device_id.to_uppercase();
    id.starts_with("USB\\") && IDS.iter().any(|i| id.contains(i))
}

/// "(COM14)" in the device name.
pub fn port_of(d: &PnpDevice) -> Option<String> {
    // compiled once (the window lists devices every few seconds)
    static RE: std::sync::OnceLock<regex::Regex> = std::sync::OnceLock::new();
    let re = RE.get_or_init(|| regex::Regex::new(r"\((COM\d+)\)").unwrap());
    re.captures(&d.name).map(|c| c[1].to_string())
}

pub fn evaluate(devices: &[PnpDevice]) -> CableStatus {
    let none = |state, code| CableStatus { state, port: None, error_code: code, ports: vec![] };
    let cables: Vec<&PnpDevice> = devices.iter().filter(|d| is_ch340(d)).collect();
    if cables.is_empty() {
        return none(CableState::NoCable, 0);
    }
    let mut ports: Vec<String> = cables.iter().filter(|d| d.error_code == 0).filter_map(|d| port_of(d)).collect();
    ports.sort_by_key(|p| p[3..].parse::<u32>().unwrap_or(u32::MAX));
    if !ports.is_empty() {
        return CableStatus { state: CableState::Ok, port: Some(ports[0].clone()), error_code: 0, ports };
    }
    // 28: no driver installed; 1 (not configured) and 18 (reinstall) mean the same to the user
    if let Some(m) = cables.iter().find(|d| matches!(d.error_code, 28 | 1 | 18)) {
        return none(CableState::NoDriver, m.error_code);
    }
    if let Some(p) = cables.iter().find(|d| d.error_code != 0) {
        return none(CableState::Problem, p.error_code);
    }
    none(CableState::NoPort, 0)
}

/// The cable's devices as Windows lists them (SetupAPI with "USB" devices present, CfgMgr32 for the problem code):
/// only reads the device list, opens no port. A cable without its driver is listed with problem code 28.
/// Empty on other operating systems (there the CH340 driver is part of the kernel).
pub fn list_devices() -> Vec<PnpDevice> {
    #[cfg(windows)]
    {
        windows::devices("USB").into_iter().filter(|d| d.device_id.to_uppercase().contains("VID_1A86")).collect()
    }
    #[cfg(not(windows))]
    {
        Vec::new()
    }
}

/// The COM ports of com0com's virtual port pairs (Windows: devices of the "COM0COM" enumerator, e.g. "com0com - serial
/// port emulator CNCA0 (COM12)"). Only reads the device list, opens no port. Empty on other operating systems.
pub fn virtual_ports() -> Vec<String> {
    #[cfg(windows)]
    {
        windows::devices("COM0COM").iter().filter_map(port_of).collect()
    }
    #[cfg(not(windows))]
    {
        Vec::new()
    }
}

/// The cable check of this computer (Windows; elsewhere always "no cable listed", the window then lists ports).
pub fn check() -> CableStatus {
    evaluate(&list_devices())
}

#[cfg(windows)]
#[allow(unsafe_code)]
mod windows {
    use super::PnpDevice;
    use windows_sys::Win32::Devices::DeviceAndDriverInstallation::{
        CM_Get_DevNode_Status, SetupDiDestroyDeviceInfoList, SetupDiEnumDeviceInfo, SetupDiGetClassDevsW,
        SetupDiGetDeviceInstanceIdW, SetupDiGetDeviceRegistryPropertyW, CR_SUCCESS, DIGCF_ALLCLASSES, DIGCF_PRESENT,
        DN_HAS_PROBLEM, SPDRP_DEVICEDESC, SPDRP_FRIENDLYNAME, SP_DEVINFO_DATA,
    };

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().chain(std::iter::once(0)).collect()
    }

    fn from_wide(b: &[u16]) -> String {
        let n = b.iter().position(|&c| c == 0).unwrap_or(b.len());
        String::from_utf16_lossy(&b[..n])
    }

    /// Every present device of an enumerator ("USB", "COM0COM"): instance ID, name, problem code (0 = working).
    pub fn devices(enumerator: &str) -> Vec<PnpDevice> {
        let mut out = Vec::new();
        let usb = wide(enumerator);
        // SAFETY: plain SetupAPI calls with buffers sized here; the list handle is destroyed at the end.
        unsafe {
            let set = SetupDiGetClassDevsW(
                std::ptr::null(),
                usb.as_ptr(),
                std::ptr::null_mut(),
                DIGCF_ALLCLASSES | DIGCF_PRESENT,
            );
            if set == -1 {
                return out;
            }
            let mut i = 0u32;
            loop {
                let mut info =
                    SP_DEVINFO_DATA { cbSize: std::mem::size_of::<SP_DEVINFO_DATA>() as u32, ..Default::default() };
                if SetupDiEnumDeviceInfo(set, i, &mut info) == 0 {
                    break;
                }
                i += 1;
                let mut id = [0u16; 512];
                if SetupDiGetDeviceInstanceIdW(set, &info, id.as_mut_ptr(), id.len() as u32, std::ptr::null_mut()) == 0
                {
                    continue;
                }
                let mut name = String::new();
                for prop in [SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC] {
                    let mut buf = [0u16; 512];
                    let ok = SetupDiGetDeviceRegistryPropertyW(
                        set,
                        &info,
                        prop,
                        std::ptr::null_mut(),
                        buf.as_mut_ptr() as *mut u8,
                        (buf.len() * 2) as u32,
                        std::ptr::null_mut(),
                    );
                    if ok != 0 {
                        name = from_wide(&buf);
                        break;
                    }
                }
                let (mut status, mut problem) = (0u32, 0u32);
                let code = if CM_Get_DevNode_Status(&mut status, &mut problem, info.DevInst, 0) == CR_SUCCESS
                    && status & DN_HAS_PROBLEM != 0
                {
                    problem as i32
                } else {
                    0
                };
                out.push(PnpDevice { device_id: from_wide(&id), name, error_code: code });
            }
            SetupDiDestroyDeviceInfoList(set);
        }
        out
    }
}
