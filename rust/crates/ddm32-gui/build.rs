// SPDX-License-Identifier: GPL-3.0-or-later
#[path = "src/winres.rs"]
mod winres;

fn main() {
    // Debug builds (and so the tests) keep Slint's element debug info: the keyboard / screen-reader test walks the
    // element tree with it. Release builds stay without it.
    let debug = std::env::var("PROFILE").is_ok_and(|p| p == "debug");
    let config = slint_build::CompilerConfiguration::new().with_debug_info(debug);
    slint_build::compile_with_config("ui/app.slint", config).expect("the Slint UI compiles");

    // Windows (MSVC): the program's icon inside DDM32Plus.exe, as a .res the linker takes (no resource compiler)
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let target_env = std::env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
    let ico = std::path::Path::new("../../packaging/art/ddm32plus.ico");
    println!("cargo:rerun-if-changed={}", ico.display());
    println!("cargo:rerun-if-changed=src/winres.rs");
    if target_os == "windows" && target_env == "msvc" {
        let bytes = std::fs::read(ico).expect("packaging/art/ddm32plus.ico");
        let res = winres::icon_res(&bytes).expect("the program icon is a valid .ico");
        let out = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("ddm32plus.res");
        std::fs::write(&out, res).expect("write the icon resource");
        println!("cargo:rustc-link-arg-bins={}", out.display());
    }
}
