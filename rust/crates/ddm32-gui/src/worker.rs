// SPDX-License-Identifier: GPL-3.0-or-later
//! Worker threads that always report back. A panic inside a worker (a bug) must never leave the window "busy" for
//! ever (the close button refused, every radio button off): the work runs under `catch_unwind`, and its end, the
//! result or the panic's text, always reaches the window's thread.

use std::panic::{catch_unwind, AssertUnwindSafe};

/// Runs `work`, turning a panic into an error with the panic's text.
pub fn caught<T>(work: impl FnOnce() -> T) -> Result<T, String> {
    catch_unwind(AssertUnwindSafe(work)).map_err(|p| {
        let text = p
            .downcast_ref::<&str>()
            .map(|s| s.to_string())
            .or_else(|| p.downcast_ref::<String>().cloned())
            .unwrap_or_else(|| "unknown panic".into());
        format!("internal error: {text}")
    })
}

/// Runs `work` on a new thread; `done` gets its result (or the panic's text) in the window's thread. Without an
/// event loop (tests without one) `done` is not called.
pub fn spawn<T: Send + 'static>(
    work: impl FnOnce() -> T + Send + 'static,
    done: impl FnOnce(Result<T, String>) + Send + 'static,
) {
    std::thread::spawn(move || {
        let r = caught(work);
        let _ = slint::invoke_from_event_loop(move || done(r));
    });
}

/// A panicked radio or debug tool as a tool error (the window then clears its busy state and shows the line).
pub fn panic_error(text: &str) -> ddm32_core::radio::ToolError {
    ddm32_core::radio::ToolError::value(text.to_string())
}
