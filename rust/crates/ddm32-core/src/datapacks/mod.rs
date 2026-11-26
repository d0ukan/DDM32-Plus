// SPDX-License-Identifier: GPL-3.0-or-later
//! User data packs (tools/datapack.py; the C# DataPacks/): the pack format, the RadioID and air band builders, the
//! PDF text reader and building packs from the user's files.
pub mod air;
pub mod datapack;
pub mod files;
pub mod pdf;
pub mod radioid;
pub mod refpack;

/// A pack that cannot be built (tools/datapack.py PackError / iddb.IddbError).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackError(pub String);

impl std::fmt::Display for PackError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for PackError {}

impl From<std::io::Error> for PackError {
    fn from(e: std::io::Error) -> Self {
        PackError(e.to_string())
    }
}
