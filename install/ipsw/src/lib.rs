// SPDX-License-Identifier: GPL-3.0-or-later
//! An IPSW member's content from its stored bytes, and an OS image's APFS container, each checked against the
//! member's catalog pins.

mod container;
mod error;
mod extract;
mod pipe;

pub use container::{member_container, os_container, sniff, udif_disk};
pub use error::{Error, Result};
pub use extract::extract;
pub use pipe::Written;
pub use switch_catalog::{Container, Member};
