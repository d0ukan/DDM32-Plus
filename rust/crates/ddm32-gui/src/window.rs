// SPDX-License-Identifier: GPL-3.0-or-later
//! The window's size and place, kept in settings.json `window` in the C# program's format, so both programs agree:
//! `{"x": .., "y": .., "w": .., "h": .., "max": bool}` in logical pixels (WPF's device independent pixels). Restored
//! at the start only when the saved size is at least the window's minimum and enough of it lies on the screens the
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
/// MIN and at most the screens' size; the place only when VISIBLE of the window lies on `screen` (all monitors).
/// Without the screens' extent (`screen` None: not Windows) the place is used when it is not absurd (within
/// -10000..100000).
pub fn restore_plan(saved: Option<&Value>, screen: Option<Rect>) -> (Option<Rect>, bool) {
    let Some(g) = parse(saved) else { return (None, false) };
    let r = g.rect;
    if r.w < MIN.0 || r.h < MIN.1 {
        return (None, g.max);
    }
    let on_screen = match screen {
        Some(s) => {
            let ox = (r.x + r.w).min(s.x + s.w) - r.x.max(s.x);
            let oy = (r.y + r.h).min(s.y + s.h) - r.y.max(s.y);
            ox >= VISIBLE.0 && oy >= VISIBLE.1
        }
        None => (-10000.0..100000.0).contains(&r.x) && (-10000.0..100000.0).contains(&r.y),
    };
    if !on_screen {
        return (None, g.max);
    }
    let (w, h) = match screen {
        Some(s) => (r.w.min(s.w.max(MIN.0)), r.h.min(s.h.max(MIN.1))),
        None => (r.w, r.h),
    };
    (Some(Rect { x: r.x, y: r.y, w, h }), g.max)
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

/// The extent of every monitor together in logical pixels (Windows: the virtual screen at the system's DPI, as WPF
/// measures it); None elsewhere.
pub fn virtual_screen() -> Option<Rect> {
    #[cfg(windows)]
    {
        win::virtual_screen()
    }
    #[cfg(not(windows))]
    {
        None
    }
}

/// Puts the window back where it was (main, before the event loop; tests never call it).
pub fn restore(ui: &crate::AppWindow) {
    let saved = crate::with(|s| s.settings.load().get("window").cloned());
    let (rect, max) = restore_plan(saved.as_ref(), virtual_screen());
    let w = ui.window();
    if let Some(r) = rect {
        w.set_size(slint::LogicalSize::new(r.w as f32, r.h as f32));
        w.set_position(slint::LogicalPosition::new(r.x as f32, r.y as f32));
    }
    if max {
        w.set_maximized(true);
    }
}

/// Saves the window's size and place (every way the window closes; not in the demo).
pub fn save(ui: &crate::AppWindow) {
    if crate::DEMO.load(std::sync::atomic::Ordering::SeqCst) {
        return;
    }
    let w = ui.window();
    let scale = w.scale_factor().max(0.1) as f64;
    let (p, s) = (w.position(), w.size());
    let now =
        Rect { x: p.x as f64 / scale, y: p.y as f64 / scale, w: s.width as f64 / scale, h: s.height as f64 / scale };
    let saved = crate::with(|st| st.settings.load().get("window").cloned());
    if let Some(g) = save_plan(now, w.is_maximized(), w.is_minimized(), saved.as_ref()) {
        let _ = crate::with(|st| st.settings.save("window", to_json(&g)));
    }
}

#[cfg(windows)]
#[allow(unsafe_code)]
mod win {
    use super::Rect;

    const SM_XVIRTUALSCREEN: i32 = 76;
    const SM_YVIRTUALSCREEN: i32 = 77;
    const SM_CXVIRTUALSCREEN: i32 = 78;
    const SM_CYVIRTUALSCREEN: i32 = 79;

    #[link(name = "user32")]
    extern "system" {
        fn GetSystemMetrics(index: i32) -> i32;
        fn GetDpiForSystem() -> u32;
    }

    pub fn virtual_screen() -> Option<Rect> {
        // SAFETY: plain value queries without pointers
        let (x, y, w, h, dpi) = unsafe {
            (
                GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_CYVIRTUALSCREEN),
                GetDpiForSystem(),
            )
        };
        if w <= 0 || h <= 0 {
            return None;
        }
        let k = 96.0 / dpi.max(96) as f64;
        Some(Rect { x: x as f64 * k, y: y as f64 * k, w: w as f64 * k, h: h as f64 * k })
    }
}
