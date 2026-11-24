// SPDX-License-Identifier: GPL-3.0-or-later
//! Worker threads always report back: a panic in the work reaches `done` as an error with its text, through the
//! window's event loop, so no busy state can stay set for ever. Also the program icon's Windows resource. No radio, no
//! network. One test with an event loop (the testing backend allows one per process).
use ddm32_gui::{winres, worker};
use std::cell::RefCell;
use std::rc::Rc;

#[test]
fn a_panicking_worker_still_reports_back() {
    assert_eq!(Ok(4), worker::caught(|| 2 + 2));
    let e = worker::caught(|| -> u8 { panic!("boom {}", 1) }).unwrap_err();
    assert_eq!("internal error: boom 1", e);
    assert_eq!("internal error: plain", worker::caught(|| -> u8 { std::panic::panic_any("plain") }).unwrap_err());
    assert_eq!("internal error: unknown panic", worker::caught(|| -> u8 { std::panic::panic_any(7u32) }).unwrap_err());

    // through the event loop: both results arrive, the panic as an error
    i_slint_backend_testing::init_integration_test_with_system_time();
    let got: Rc<RefCell<Vec<Result<u32, String>>>> = Rc::default();
    let g = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
    for n in 0..2u32 {
        let g = g.clone();
        worker::spawn(
            move || {
                if n == 1 {
                    panic!("worker {n} failed");
                }
                n
            },
            move |r| {
                let mut v = g.lock().unwrap();
                v.push(r);
                if v.len() == 2 {
                    let _ = slint::quit_event_loop();
                }
            },
        );
    }
    // a guard: the loop ends in any case
    let t = slint::Timer::default();
    t.start(slint::TimerMode::SingleShot, std::time::Duration::from_secs(20), || {
        let _ = slint::quit_event_loop();
    });
    slint::run_event_loop_until_quit().unwrap();
    got.borrow_mut().extend(g.lock().unwrap().drain(..));
    let mut v = got.borrow().clone();
    v.sort();
    assert_eq!(vec![Ok(0), Err("internal error: worker 1 failed".to_string())], v);
}

#[test]
fn the_icon_resource_holds_every_image() {
    let ico = std::fs::read(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../packaging/art/ddm32plus.ico"))
        .unwrap();
    let images = winres::parse_ico(&ico).unwrap();
    assert!(images.len() >= 4, "16 to 256 px");
    assert!(images.iter().any(|i| i.width == 0), "a 256 px image (width 0 in the directory)");
    let res = winres::icon_res(&ico).unwrap();
    // the empty first entry, then one RT_ICON per image and one RT_GROUP_ICON
    assert_eq!(&res[..8], &[0, 0, 0, 0, 32, 0, 0, 0]);
    assert_eq!(&res[8..16], &[0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0]);
    let mut at = 32;
    let mut kinds = vec![];
    while at < res.len() {
        let size = u32::from_le_bytes(res[at..at + 4].try_into().unwrap()) as usize;
        let kind = u16::from_le_bytes(res[at + 10..at + 12].try_into().unwrap());
        kinds.push(kind);
        if kind == 3 {
            assert_eq!(images[kinds.len() - 1].data, res[at + 32..at + 32 + size]);
        }
        at += 32 + size;
        at = at.div_ceil(4) * 4;
    }
    assert_eq!(images.len() + 1, kinds.len());
    assert!(kinds[..images.len()].iter().all(|k| *k == 3) && kinds[images.len()] == 14);
    assert!(winres::icon_res(b"not an icon").is_err());
    // the built window program carries it (Windows, MSVC: build.rs links the .res)
    if cfg!(all(windows, target_env = "msvc")) {
        let exe = std::fs::read(env!("CARGO_BIN_EXE_ddm32-gui")).unwrap();
        let big = images.iter().max_by_key(|i| i.data.len()).unwrap();
        assert!(exe.windows(big.data.len()).any(|w| w == big.data.as_slice()), "the icon is not in the exe");
    }
}
