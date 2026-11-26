// SPDX-License-Identifier: GPL-3.0-or-later
//! `ddm32`: the DDM32 Plus programmer's command line (used by the tests and for the owner's first real runs).
//! File-only commands need no radio. The radio tools reach a serial port only in a build with the `radio` feature,
//! and only through the tool's own guard; every radio step is one command the owner starts by hand.

use ddm32_core::app_info;
use ddm32_core::datapacks::{datapack as dp, files as packs};
use ddm32_core::pyjson::{self, Py};
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::flow::FileSteps;
use ddm32_core::radio::port::{RadioAccess, RawSerial, SystemClock};
use ddm32_core::radio::tools::ToolContext;
use ddm32_core::radio::{cps, fast_install, tools, unit_image, ToolResult};
use ddm32_core::releases::{catalog, client, net::HttpReleaseNet, signed_manifest as sm};
use ddm32_core::storage::{backups, AppPaths, Os, Settings};
use ddm32_core::update::self_update as su;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

const USAGE: &str = "DDM32 Plus programmer (command line)

usage: ddm32 COMMAND [options]

file-only commands (no radio):
  version                              program version and build switches
  paths                                the data folder and its backups folder (backups)
  backups                              every verified backup found on this PC, newest first
  detect --backup DIR                  what the backed-up radio runs (radio_detect)
  plan --backup DIR [--plus RELEASE] [--mode install_plus|install_plus_from_stock|install_plus_quick|restore|restore_stock]
  releases [--offline]                 the signed releases (GitHub, or the verified cache)
  download VERSION                     download and verify a firmware release into releases/
  verify-manifest DIR [--public-key HEX]
                                       check a release folder's signed manifest, every asset's size and SHA-256,
                                       and this platform's program zip (app_rs) as the self-update unpacks it
  pack radioid --csv user.csv --out FILE.dpk [--country NAME]... [--name TEXT]
  pack air --out FILE.dpk [--name TEXT] PAGE.pdf|PAGE.txt ...
  pack info FILE.dpk

radio tools (one approved step each; a real port only in a build with the `radio` feature):
  tool backup|install_plus|restore_stock|fast_install|datapack ARGS...
";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::from(1)
        }
    }
}

fn opt(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1).cloned())
}

fn opts(args: &[String], name: &str) -> Vec<String> {
    args.windows(2).filter(|w| w[0] == name).map(|w| w[1].clone()).collect()
}

fn need(args: &[String], name: &str) -> Result<String, String> {
    opt(args, name).ok_or_else(|| format!("{name} is required\n\n{USAGE}"))
}

fn paths() -> Result<AppPaths, String> {
    AppPaths::for_this_program().ok_or_else(|| "no data folder (HOME is not set)".to_string())
}

fn run(args: &[String]) -> Result<ExitCode, String> {
    let Some(cmd) = args.first() else {
        print!("{USAGE}");
        return Ok(ExitCode::from(2));
    };
    let rest = &args[1..];
    match cmd.as_str() {
        // the self-update helper protocol (the same arguments as the C# program's, for the hand-over)
        su::HELPER_ARG => {
            let code = su::helper_main(rest, &mut |pid| Box::new(su::RealProcesses::new(pid)));
            Ok(ExitCode::from(code as u8))
        }
        su::CHECK_ARG => {
            let marker = rest.first().ok_or("--update-check FILE")?;
            su::write_marker(Path::new(marker), app_info::VERSION).map_err(|e| e.to_string())?;
            Ok(ExitCode::SUCCESS)
        }
        "version" | "--version" => {
            println!("{} {} ({})", app_info::NAME, app_info::VERSION, ddm32_core::releases::platform_key());
            println!(
                "radio access: {}",
                if RadioAccess::ENABLED { "compiled in" } else { "off (no serial port can be opened)" }
            );
            println!("developer features: {}", if app_info::DEVELOPER { "on" } else { "off" });
            println!("release key id: {}", ddm32_core::releases::PUBLIC_KEY_ID);
            Ok(ExitCode::SUCCESS)
        }
        "paths" => {
            let p = paths()?;
            println!("data folder: {}", p.root.display());
            println!("backups (backups): {}", p.backups().display());
            for o in p.find_other_data(Os::current(), &ddm32_core::storage::paths::real_env) {
                println!("older {} program's backups (read only): {}", o.program, o.backups.display());
            }
            Ok(ExitCode::SUCCESS)
        }
        "backups" => {
            let p = paths()?;
            let s = Settings::new(p.settings_file());
            let roots = backups::backup_roots(&p, &s, Os::current(), &ddm32_core::storage::paths::real_env);
            for b in backups::discover(&roots) {
                println!("{}  [{}]", b.folder.display(), b.origin);
            }
            let dirs: Vec<PathBuf> = roots.iter().map(|r| r.folder.clone()).collect();
            if let Some(q) = backups::last_for_quick_update(&dirs, Codec::dm32(), 8) {
                println!("quick update would use: {}", q.display());
            }
            Ok(ExitCode::SUCCESS)
        }
        "detect" => {
            let b = need(rest, "--backup")?;
            print!("{}", unit_image::detect_tool_output(Path::new(&b), Codec::dm32()).map_err(|e| e.record())?);
            Ok(ExitCode::SUCCESS)
        }
        "plan" => {
            let b = PathBuf::from(need(rest, "--backup")?);
            let mode = opt(rest, "--mode").unwrap_or_else(|| "install_plus".into());
            let plan = if mode.starts_with("restore") {
                FileSteps::restore_plan(&mode, &b)
            } else {
                FileSteps::plan(&mode, &b, Path::new(&need(rest, "--plus")?), None, Codec::dm32())
            }
            .map_err(|e| e.record())?;
            println!("{}", pyjson::dumps2(&Py::Dict(plan)));
            Ok(ExitCode::SUCCESS)
        }
        "releases" => {
            let p = paths()?;
            let cat = if rest.iter().any(|a| a == "--offline") {
                client::cached_catalog(&p.releases(), None)
            } else {
                client::load_catalog(&p.releases(), &HttpReleaseNet::real(), None, None).map_err(|e| e.0)?
            };
            for f in &cat.firmware {
                println!("{:<20} {:<12} {:?}", f.version(), f.release, catalog::label_for(f, &cat.firmware));
            }
            for (tag, why) in &cat.rejected {
                println!("refused {tag}: {why}");
            }
            Ok(ExitCode::SUCCESS)
        }
        "download" => {
            let v = rest.first().ok_or("download VERSION")?;
            let p = paths()?;
            std::fs::create_dir_all(p.releases()).map_err(|e| e.to_string())?;
            let net = HttpReleaseNet::real();
            let cat = client::load_catalog(&p.releases(), &net, None, None).map_err(|e| e.0)?;
            let fv = cat.find(v).ok_or_else(|| format!("{v} is not a signed release"))?;
            let folder = client::download_firmware(fv, &p.releases(), &net, None, None).map_err(|e| e.to_string())?;
            println!("verified: {}", folder.display());
            Ok(ExitCode::SUCCESS)
        }
        "verify-manifest" => verify_manifest(rest),
        "pack" => pack(rest),
        "tool" => tool(rest),
        "-h" | "--help" | "help" => {
            print!("{USAGE}");
            Ok(ExitCode::SUCCESS)
        }
        other => Err(format!("unknown command {other}\n\n{USAGE}")),
    }
}

/// A release folder as this program reads it: the signature (the embedded key, or --public-key for a test key), every
/// asset present in the folder against its size and SHA-256, and this platform's `app_rs` zip unpacked by the
/// self-update's own extractor (into a temporary folder that is removed again). make_release.py's output is checked
/// with this (pc/tests/test_release_tool.py).
fn verify_manifest(args: &[String]) -> Result<ExitCode, String> {
    let d = args.first().filter(|a| !a.starts_with("--")).ok_or("verify-manifest DIR [--public-key HEX]")?;
    let dir = Path::new(d);
    let key = opt(args, "--public-key");
    let m = sm::verify_files(dir, key.as_deref(), None).map_err(|e| e.0)?;
    let key_id = match &key {
        Some(k) => sm::key_id(&sm::public_key(Some(k)).map_err(|e| e.0)?),
        None => ddm32_core::releases::PUBLIC_KEY_ID.to_string(),
    };
    println!("{}: {} firmware version(s), signed with key {key_id}", m.release, m.firmware.len());
    let mut assets: Vec<(String, &ddm32_core::releases::AssetEntry)> =
        m.firmware.iter().map(|f| ("firmware".to_string(), &f.asset)).collect();
    for (k, a) in [("app", &m.app), ("app_net", &m.app_net)] {
        if let Some(a) = a {
            assets.push((k.to_string(), a));
        }
    }
    for (p, a) in &m.app_rs {
        assets.push((format!("app_rs {p}"), a));
    }
    for (what, a) in &assets {
        let path = dir.join(&a.file);
        if !path.is_file() {
            println!("  {what} {} {} (not in the folder)", a.version, a.file);
            continue;
        }
        let body = std::fs::read(&path).map_err(|e| e.to_string())?;
        if body.len() as u64 != a.size || sm::sha256_hex(&body) != a.sha256 {
            return Err(format!("{}: size or SHA-256 differs from the manifest", a.file));
        }
        println!("  {what} {} {} OK", a.version, a.file);
    }
    // each firmware's map pack (docs/mappack-design.md E.2): size, SHA-256 and the pack's own structure check
    for f in &m.firmware {
        if f.ref_pack_newer {
            println!("  ref_pack of {}: a newer schema than this program reads (ignored)", f.version());
        }
        let Some(rp) = &f.ref_pack else { continue };
        let path = dir.join(&rp.file);
        if !path.is_file() {
            println!("  ref_pack {} {} (not in the folder)", rp.version, rp.file);
            continue;
        }
        ddm32_core::releases::client::check_ref_file(&path, rp).map_err(|e| format!("{}: {e}", rp.file))?;
        println!("  ref_pack {} {} OK (coordinate id 0x{:08X})", rp.version, rp.file, rp.coord_crc);
    }
    let platform = ddm32_core::releases::platform_key();
    if let Some((_, a)) = m.app_rs.iter().find(|(p, _)| *p == platform) {
        let zip = dir.join(&a.file);
        if zip.is_file() {
            let tmp = std::env::temp_dir().join(format!("ddm32-verify-{}", std::process::id()));
            let r = su::safe_extract(&zip, &tmp);
            let _ = std::fs::remove_dir_all(&tmp);
            r.map_err(|e| format!("{}: {}", a.file, e.0))?;
            println!("  app_rs {platform}: unpacks as the self-update expects ({})", app_info::EXE_FILE);
        }
    }
    Ok(ExitCode::SUCCESS)
}

fn save(out: &str, built: &packs::BuiltPack) -> Result<ExitCode, String> {
    std::fs::write(out, &built.image).map_err(|e| e.to_string())?;
    println!("{}", dp::info(&built.image));
    Ok(ExitCode::SUCCESS)
}

fn pack(args: &[String]) -> Result<ExitCode, String> {
    let sub = args.first().map(String::as_str).unwrap_or("");
    let rest = if args.is_empty() { args } else { &args[1..] };
    match sub {
        "radioid" => {
            let name = opt(rest, "--name").unwrap_or_else(|| "RadioID".into());
            let built = packs::radio_id(Path::new(&need(rest, "--csv")?), &opts(rest, "--country"), &name, None)
                .map_err(|e| e.0)?;
            save(&need(rest, "--out")?, &built)
        }
        "air" => {
            let mut sources = vec![];
            let mut it = rest.iter();
            while let Some(a) = it.next() {
                if a == "--out" || a == "--name" {
                    it.next();
                    continue;
                }
                sources.push(packs::read_air_source(Path::new(a)).map_err(|e| e.0)?);
            }
            let name = opt(rest, "--name").unwrap_or_else(|| "Air band".into());
            let built = packs::air_pack(&sources, &name, None).map_err(|e| e.0)?;
            save(&need(rest, "--out")?, &built)
        }
        "info" => {
            let f = rest.first().ok_or("pack info FILE")?;
            println!("{}", dp::info(&std::fs::read(f).map_err(|e| e.to_string())?));
            Ok(ExitCode::SUCCESS)
        }
        _ => Err(format!("pack radioid|air|info\n\n{USAGE}")),
    }
}

fn tool(args: &[String]) -> Result<ExitCode, String> {
    let name = args.first().ok_or_else(|| USAGE.to_string())?;
    let argv = &args[1..];
    let clock = SystemClock::default();
    let print = |s: &str| println!("{s}");
    let read_only = name == "backup";
    let serial = move |port: &str| -> ToolResult<Box<dyn RawSerial>> {
        if read_only {
            RadioAccess::factory(port)
        } else {
            RadioAccess::write_factory(port)
        }
    };
    let ctx =
        ToolContext { print: &print, clock: &clock, serial: &serial, release_key_hex: None, codec: Codec::dm32() };
    let r = match name.as_str() {
        "backup" => tools::backup_tool(argv, &ctx),
        "install_plus" => tools::install_tool(argv, &ctx),
        "restore_stock" => tools::restore_stock_tool(argv, &ctx),
        "fast_install" => fast_install::fast_install_tool(argv, &ctx),
        "datapack" => cps::data_pack_tool(argv, &ctx),
        other => return Err(format!("unknown tool {other}")),
    };
    match r {
        Ok(()) => Ok(ExitCode::SUCCESS),
        Err(e) if e.py_type == "SystemExit" => Ok(ExitCode::from(2)),
        Err(e) => Err(e.record()),
    }
}
