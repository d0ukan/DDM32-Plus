// SPDX-License-Identifier: GPL-3.0-or-later
//! DDM32 Plus programmer core: everything that is not the user interface. Signed releases (Ed25519 manifests, the
//! release list, downloads with hash checks, version order and labels), the self-update swap, the data folder and
//! settings, the radio tools with their guards (backup, install over Plus, from stock, restore, quick update, install
//! without backup) and the data packs (RadioID CSV, AD 2.18 text / PDF, the CPS pack writer and its region guard).
//!
//! A port of the C# program (win/src/DDM32Plus.Core), itself held byte for byte to the Python tools (pc/radiotools).
// unsafe only in radio::cable::windows (the Windows device list) and sys::win (keep awake, the list separator);
// everything else is safe code
#![deny(unsafe_code)]

pub mod app_info;
pub mod codeplug;
pub mod crypto;
pub mod datapacks;
pub mod pyjson;
pub mod pytext;
pub mod radio;
pub mod releases;
pub mod storage;
pub mod sys;
pub mod update;
