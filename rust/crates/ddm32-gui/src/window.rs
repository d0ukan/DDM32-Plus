// SPDX-License-Identifier: GPL-3.0-or-later
//! The window's size and place, kept in settings.json `window` in the C# program's format, so both programs agree:
//! `{"x": .., "y": .., "w": .., "h": .., "max": bool}` in logical pixels (WPF's device independent pixels). Restored
//! at the start only when the saved size is at least the window's minimum and its title bar lies on a monitor the
//! computer has now (a monitor unplugged since: the window opens where the OS puts it). Saved when the window closes;
//! a maximized or minimized window keeps the last normal place.

use serde_json::{json, Value};
use slint::ComponentHandle;

/// A rectangle in logical pixels.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Rect {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
}

/// The saved size and place.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Geometry {
    pub rect: Rect,
    pub max: bool,
}

/// The window's minimum (app.slint AppWindow min-width / min-height).
pub const MIN: (f64, f64) = (640.0, 560.0);

/// How much of the window must lie on the screens to be put back there: a grip of its title bar.
pub const VISIBLE: (f64, f64) = (120.0, 40.0);

/// settings.json `window` read as the C# program writes it; None when missing or not whole.
pub fn parse(v: Option<&Value>) -> Option<Geometry> {
    let o = v?.as_object()?;
    let n = |k: &str| o.get(k).and_then(Value::as_f64).filter(|x| x.is_finite());
    Some(Geometry {
        rect: Rect { x: n("x")?, y: n("y")?, w: n("w")?, h: n("h")? },
        max: o.get("max").and_then(Value::as_bool).unwrap_or(false),
    })
}

/// The JSON object the C# program reads.
pub fn to_json(g: &Geometry) -> Value {
    json!({"x": g.rect.x, "y": g.rect.y, "w": g.rect.w, "h": g.rect.h, "max": g.max})
}

/// What the start puts back: (the place and size, or None for the OS's default; maximized). The size is at least
/// MIN; the place only when the window's title bar lies on one monitor (its top within the monitor, the 40 px strip
/// below it inside, at least 120 px of the window's width on it), so it can always be grabbed. The size is cut to that
/// monitor. Without the monitors (`monitors` empty: not Windows) the place is used when it is not absurd (within
/// -10000..100000).
pub fn restore_plan(saved: Option<&Value>, monitors: &[Rect]) -> (Option<Rect>, bool) {
    let Some(g) = parse(saved) else { return (None, false) };
    let r = g.rect;
    if r.w < MIN.0 || r.h < MIN.1 {
        return (None, g.max);
    }
    if monitors.is_empty() {
        let sane = (-10000.0..100000.0).contains(&r.x) && (-10000.0..100000.0).contains(&r.y);
        return (sane.then_some(r), g.max);
    }
    let holds = |m: &Rect| {
        let ox = (r.x + r.w).min(m.x + m.w) - r.x.max(m.x);
        r.y >= m.y && r.y + VISIBLE.1 <= m.y + m.h && ox >= VISIBLE.0
    };
    match monitors.iter().find(|m| holds(m)) {
        Some(m) => (Some(Rect { x: r.x, y: r.y, w: r.w.min(m.w.max(MIN.0)), h: r.h.min(m.h.max(MIN.1)) }), g.max),
        None => (None, g.max),
    }
}

/// What the close saves: the normal place and size now, or (maximized / minimized) the last saved place with the
/// new maximized flag.
pub fn save_plan(now: Rect, maximized: bool, minimized: bool, saved: Option<&Value>) -> Option<Geometry> {
    if maximized || minimized {
        let old = parse(saved)?;
        return Some(Geometry { rect: old.rect, max: maximized || (minimized && old.max) });
    }
    if now.w < 1.0 || now.h < 1.0 {
        return None;
    }
    Some(Geometry { rect: now, max: false })
}

/// Every monitor's work area in logical pixels (Windows: at the system's DPI, as WPF measures it); empty elsewhere.
pub fn monitors() -> Vec<Rect> {
    #[cfg(windows)]
    {
        win::monitors()
    }
    #[cfg(not(windows))]
    {
        Vec::new()
    }
}

/// Physical pixels per logical pixel for the saved place: the system's DPI on Windows (one basis for every monitor,
/// for the save and the check alike, as WPF does), else None (the window's own scale factor).
pub fn system_scale() -> Option<f64> {
    #[cfg(windows)]
    {
        Some(win::system_scale())
    }
    #[cfg(not(windows))]
    {
        None
    }
}

/// Puts the window back where it was (main, before the event loop; tests never call it).
pub fn restore(ui: &crate::AppWindow) {
    let saved = crate::with(|s| s.settings.load().get("window").cloned());
    let (rect, max) = restore_plan(saved.as_ref(), &monitors());
    let w = ui.window();
    if let Some(r) = rect {
        match system_scale() {
            Some(k) => {
                w.set_size(slint::PhysicalSize::new((r.w * k).round() as u32, (r.h * k).round() as u32));
                w.set_position(slint::PhysicalPosition::new((r.x * k).round() as i32, (r.y * k).round() as i32));
            }
            None => {
                w.set_size(slint::LogicalSize::new(r.w as f32, r.h as f32));
                w.set_position(slint::LogicalPosition::new(r.x as f32, r.y as f32));
            }
        }
    }
    if max {
        w.set_maximized(true);
    }
}

/// Saves the window's size and place (every way the window closes; not in the demo). Never over an unreadable
/// settings.json (Settings refuses the C# key then).
pub fn save(ui: &crate::AppWindow) {
    if crate::DEMO.load(std::sync::atomic::Ordering::SeqCst) {
        return;
    }
    let w = ui.window();
    let scale = system_scale().unwrap_or(w.scale_factor() as f64).max(0.1);
    let (p, s) = (w.position(), w.size());
    let now =
        Rect { x: p.x as f64 / scale, y: p.y as f64 / scale, w: s.width as f64 / scale, h: s.height as f64 / scale };
    let saved = crate::with(|st| st.settings.load().get("window").cloned());
    if let Some(g) = save_plan(now, w.is_maximized(), w.is_minimized(), saved.as_ref()) {
        let r = crate::with(|st| st.settings.save("window", to_json(&g)));
        crate::noted(r, "window");
    }
}

#[cfg(windows)]
#[allow(unsafe_code)]
mod win {
    use super::Rect;

    #[repr(C)]
    #[derive(Default, Clone, Copy)]
    struct WinRect {
        left: i32,
        top: i32,
        right: i32,
        bottom: i32,
    }

    #[repr(C)]
    struct MonitorInfo {
        size: u32,
        monitor: WinRect,
        work: WinRect,
        flags: u32,
    }

    type EnumProc = unsafe extern "system" fn(isize, isize, *mut WinRect, isize) -> i32;

    #[link(name = "user32")]
    extern "system" {
        fn EnumDisplayMonitors(hdc: isize, clip: *const WinRect, proc_: EnumProc, data: isize) -> i32;
        fn GetMonitorInfoW(monitor: isize, info: *mut MonitorInfo) -> i32;
        fn GetDpiForSystem() -> u32;
    }

    unsafe extern "system" fn collect(monitor: isize, _hdc: isize, _r: *mut WinRect, data: isize) -> i32 {
        // SAFETY: `data` is the Vec passed by monitors() below, alive for the whole enumeration
        let out = unsafe { &mut *(data as *mut Vec<WinRect>) };
        let mut info = MonitorInfo {
            size: std::mem::size_of::<MonitorInfo>() as u32,
            monitor: WinRect::default(),
            work: WinRect::default(),
            flags: 0,
        };
        // SAFETY: a handle Windows just gave us and our own correctly sized struct
        if unsafe { GetMonitorInfoW(monitor, &mut info) } != 0 {
            out.push(info.work);
        }
        1
    }

    pub fn system_scale() -> f64 {
        // SAFETY: a plain value query
        let dpi = unsafe { GetDpiForSystem() };
        dpi.max(96) as f64 / 96.0
    }

    pub fn monitors() -> Vec<Rect> {
        let mut found: Vec<WinRect> = Vec::new();
        // SAFETY: the callback only pushes into `found`, which outlives the call
        unsafe {
            EnumDisplayMonitors(0, std::ptr::null(), collect, &mut found as *mut Vec<WinRect> as isize);
        }
        let k = system_scale();
        found
            .into_iter()
            .filter(|r| r.right > r.left && r.bottom > r.top)
            .map(|r| Rect {
                x: r.left as f64 / k,
                y: r.top as f64 / k,
                w: (r.right - r.left) as f64 / k,
                h: (r.bottom - r.top) as f64 / k,
            })
            .collect()
    }
}
