// SPDX-License-Identifier: GPL-3.0-or-later
//! The radio tools (ports of pc/radiotools held to them byte for byte; the C# Radio/), their guards and the flows.
//! Nothing here opens a serial port by itself: a tool gets a port only from the factory its caller passes, and a
//! real port exists only with the `radio` feature (see [`port::RadioAccess`]).
pub mod cable;
pub mod codec;
pub mod cps;
pub mod debug;
pub mod error;
pub mod fast_install;
pub mod flow;
pub mod guards;
pub mod live;
pub mod monitor;
pub mod plan;
pub mod port;
pub mod portpick;
pub mod progress;
pub mod searchlog;
pub mod tools;
pub mod unit_image;

pub use error::{ToolError, ToolResult};
