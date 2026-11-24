// SPDX-License-Identifier: GPL-3.0-or-later
//! The two operating system calls of the search log (SearchLogPage.xaml.cs KeepAwake, SearchLog.cs ListSeparator):
//! keep the PC awake while it logs all night (Windows call; a systemd-inhibit / caffeinate helper process on Linux /
//! macOS), and Windows' list separator for the CSV (None elsewhere: the caller's default).

/// Keeps the PC from sleeping while it lives (the screen may still go off). Windows: SetThreadExecutionState with
/// ES_SYSTEM_REQUIRED | ES_CONTINUOUS on the thread that made it, given back on drop; make it on the thread that runs
/// the whole log and drop it on the same thread. Linux: a `systemd-inhibit --what=sleep:idle` child process; macOS: a
/// `caffeinate -i` child process (see [`inhibit_command`]); the child is ended on drop, and it also ends by itself when
/// this program ends (it waits for this process). Where the tool is missing nothing happens (the PC may sleep as
/// before; the search log reconnects after a sleep).
pub struct KeepAwake {
    child: Option<std::process::Child>,
}

impl KeepAwake {
    pub fn new() -> Self {
        #[cfg(windows)]
        {
            win::execution_state(win::ES_CONTINUOUS | win::ES_SYSTEM_REQUIRED);
            KeepAwake { child: None }
        }
        #[cfg(not(windows))]
        {
            let child = inhibit_command(std::env::consts::OS, std::process::id()).and_then(|(prog, args)| {
                std::process::Command::new(prog)
                    .args(args)
                    .stdin(std::process::Stdio::null())
                    .stdout(std::process::Stdio::null())
                    .stderr(std::process::Stdio::null())
                    .spawn()
                    .ok()
            });
            KeepAwake { child }
        }
    }

    /// Whether something keeps the PC awake now (Windows: always; elsewhere: the helper process started).
    pub fn active(&self) -> bool {
        cfg!(windows) || self.child.is_some()
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
        if let Some(mut c) = self.child.take() {
            let _ = c.kill();
            let _ = c.wait();
        }
    }
}

/// The helper process that keeps a Linux or macOS PC awake while this process (`pid`) runs: (program, arguments), or
/// None on another OS. Both helpers end by themselves when `pid` ends, so a crash never leaves the PC awake for ever.
/// Linux: systemd-inhibit (systemd's own tool; a "block" lock on sleep and idle needs no root) holding the lock while
///   `tail --pid=PID -f /dev/null` waits for this process.
/// macOS: caffeinate -i (no idle sleep) -w PID (until that process ends).
pub fn inhibit_command(os: &str, pid: u32) -> Option<(&'static str, Vec<String>)> {
    match os {
        "linux" => Some((
            "systemd-inhibit",
            vec![
                "--what=sleep:idle".into(),
                "--who=DDM32 Plus".into(),
                "--why=Search log running".into(),
                "--mode=block".into(),
                "tail".into(),
                format!("--pid={pid}"),
                "-f".into(),
                "/dev/null".into(),
            ],
        )),
        "macos" => Some(("caffeinate", vec!["-i".into(), "-w".into(), pid.to_string()])),
        _ => None,
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

/// Windows' decimal separator of the user's regional settings ("," for Turkish); None elsewhere.
pub fn decimal_separator() -> Option<char> {
    #[cfg(windows)]
    {
        win::locale_char(win::LOCALE_SDECIMAL)
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
    pub const LOCALE_SDECIMAL: u32 = 0x0000_000E;

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
        locale_char(LOCALE_SLIST)
    }

    /// The first character of a locale string of the user's regional settings.
    pub fn locale_char(what: u32) -> Option<char> {
        let mut buf = [0u16; 8];
        // SAFETY: a null name is LOCALE_NAME_USER_DEFAULT; the buffer and its length in UTF-16 units are ours
        let n = unsafe { GetLocaleInfoEx(std::ptr::null(), what, buf.as_mut_ptr(), buf.len() as i32) };
        if n < 2 {
            return None;
        }
        char::decode_utf16(buf[..(n as usize - 1)].iter().copied()).next()?.ok()
    }
}
