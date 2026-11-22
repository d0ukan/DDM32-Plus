// SPDX-License-Identifier: GPL-3.0-or-later
//! The two operating system calls of the search log (SearchLogPage.xaml.cs KeepAwake, SearchLog.cs ListSeparator):
//! keep Windows awake while it logs all night, and Windows' list separator for the CSV. Elsewhere both are no-ops
//! (None: the caller's default).

/// Keeps the PC from sleeping while it lives (Windows: SetThreadExecutionState with ES_SYSTEM_REQUIRED |
/// ES_CONTINUOUS on the thread that made it, given back on drop; the screen may still go off). Make it on the thread
/// that runs the whole log and drop it on the same thread.
pub struct KeepAwake(());

impl KeepAwake {
    pub fn new() -> Self {
        #[cfg(windows)]
        win::execution_state(win::ES_CONTINUOUS | win::ES_SYSTEM_REQUIRED);
        KeepAwake(())
    }
}

impl Default for KeepAwake {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for KeepAwake {
    fn drop(&mut self) {
        #[cfg(windows)]
        win::execution_state(win::ES_CONTINUOUS);
    }
}

/// Windows' list separator of the user's regional settings (";" for Turkish, "," for English); None elsewhere.
pub fn list_separator() -> Option<char> {
    #[cfg(windows)]
    {
        win::list_separator()
    }
    #[cfg(not(windows))]
    {
        None
    }
}

#[cfg(windows)]
#[allow(unsafe_code)]
mod win {
    pub const ES_CONTINUOUS: u32 = 0x8000_0000;
    pub const ES_SYSTEM_REQUIRED: u32 = 0x0000_0001;
    const LOCALE_SLIST: u32 = 0x0000_000C;

    #[link(name = "kernel32")]
    extern "system" {
        fn SetThreadExecutionState(flags: u32) -> u32;
        fn GetLocaleInfoEx(name: *const u16, lctype: u32, data: *mut u16, cch: i32) -> i32;
    }

    pub fn execution_state(flags: u32) {
        // SAFETY: a plain flag word; the call only records the thread's state
        unsafe {
            SetThreadExecutionState(flags);
        }
    }

    pub fn list_separator() -> Option<char> {
        let mut buf = [0u16; 8];
        // SAFETY: a null name is LOCALE_NAME_USER_DEFAULT; the buffer and its length in UTF-16 units are ours
        let n = unsafe { GetLocaleInfoEx(std::ptr::null(), LOCALE_SLIST, buf.as_mut_ptr(), buf.len() as i32) };
        if n < 2 {
            return None;
        }
        char::decode_utf16(buf[..(n as usize - 1)].iter().copied()).next()?.ok()
    }
}
