// SPDX-License-Identifier: GPL-3.0-or-later
//! The program's icon as a Windows resource file (.res), made from the .ico without any resource compiler: build.rs
//! writes it and hands it to the MSVC linker, so DDM32Plus.exe itself carries the icon (Explorer, the taskbar, a
//! shortcut made by the self-update hand-over, the Apps list). Pure std; also used by build.rs (`#[path]`).

/// RT_ICON and RT_GROUP_ICON.
const RT_ICON: u16 = 3;
const RT_GROUP_ICON: u16 = 14;

/// One image of an .ico: its 16-byte directory entry fields and its bytes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IconImage {
    pub width: u8,
    pub height: u8,
    pub colors: u8,
    pub planes: u16,
    pub bit_count: u16,
    pub data: Vec<u8>,
}

/// The images of an .ico file (ICONDIR, type 1); an error for anything else.
pub fn parse_ico(ico: &[u8]) -> Result<Vec<IconImage>, String> {
    let u16_at = |o: usize| -> Result<u16, String> {
        ico.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]])).ok_or_else(|| "short .ico".to_string())
    };
    let u32_at = |o: usize| -> Result<u32, String> {
        ico.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]])).ok_or_else(|| "short .ico".to_string())
    };
    if u16_at(0)? != 0 || u16_at(2)? != 1 {
        return Err("not an .ico file".into());
    }
    let count = u16_at(4)? as usize;
    if count == 0 {
        return Err("an .ico without images".into());
    }
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let e = 6 + 16 * i;
        let size = u32_at(e + 8)? as usize;
        let offset = u32_at(e + 12)? as usize;
        let data = ico.get(offset..offset + size).ok_or("an .ico image outside the file")?.to_vec();
        out.push(IconImage {
            width: ico[e],
            height: ico[e + 1],
            colors: ico[e + 2],
            planes: u16_at(e + 4)?,
            bit_count: u16_at(e + 6)?,
            data,
        });
    }
    Ok(out)
}

/// One resource of a .res file: the header (numeric type and name, language 0) and the data padded to 4 bytes.
fn resource(out: &mut Vec<u8>, kind: u16, name: u16, flags: u16, data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_le_bytes()); // DataSize
    out.extend_from_slice(&32u32.to_le_bytes()); // HeaderSize
    out.extend_from_slice(&[0xFF, 0xFF]);
    out.extend_from_slice(&kind.to_le_bytes()); // TYPE (ordinal)
    out.extend_from_slice(&[0xFF, 0xFF]);
    out.extend_from_slice(&name.to_le_bytes()); // NAME (ordinal)
    out.extend_from_slice(&0u32.to_le_bytes()); // DataVersion
    out.extend_from_slice(&flags.to_le_bytes()); // MemoryFlags
    out.extend_from_slice(&0u16.to_le_bytes()); // LanguageId (neutral)
    out.extend_from_slice(&0u32.to_le_bytes()); // Version
    out.extend_from_slice(&0u32.to_le_bytes()); // Characteristics
    out.extend_from_slice(data);
    while !out.len().is_multiple_of(4) {
        out.push(0);
    }
}

/// A .res file with the .ico's images (RT_ICON 1..n) and one icon group (RT_GROUP_ICON 1, the exe's icon).
pub fn icon_res(ico: &[u8]) -> Result<Vec<u8>, String> {
    let images = parse_ico(ico)?;
    let mut out = Vec::new();
    // the empty first entry every .res file starts with
    resource(&mut out, 0, 0, 0, &[]);
    for (i, img) in images.iter().enumerate() {
        resource(&mut out, RT_ICON, (i + 1) as u16, 0x1010, &img.data);
    }
    let mut group = Vec::new();
    group.extend_from_slice(&0u16.to_le_bytes());
    group.extend_from_slice(&1u16.to_le_bytes());
    group.extend_from_slice(&(images.len() as u16).to_le_bytes());
    for (i, img) in images.iter().enumerate() {
        group.extend_from_slice(&[img.width, img.height, img.colors, 0]);
        group.extend_from_slice(&img.planes.to_le_bytes());
        group.extend_from_slice(&img.bit_count.to_le_bytes());
        group.extend_from_slice(&(img.data.len() as u32).to_le_bytes());
        group.extend_from_slice(&((i + 1) as u16).to_le_bytes());
    }
    resource(&mut out, RT_GROUP_ICON, 1, 0x1030, &group);
    Ok(out)
}
