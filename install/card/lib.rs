// SPDX-License-Identifier: GPL-3.0-or-later
//! The installer's side of the kernel's card layout contract, for a card whose iOS partition already exists.
//!
//! ```text
//! let mut card = Card::open(&mut dev)?;      // a cut-short earlier run reads fine; see write.rs
//! let plan = card.plan(choice, image_bytes)?;
//! card.mark(&mut dev, &plan)?;               // target named iOS-setup: no root of ours yet
//! /* the APFS container goes into plan.target */
//! card.finish(&mut dev, &plan)?;             // other roots back to userdata, then the target iOS + APFS-typed
//! ```
//!
//! LBA 0 is never written; LBA 1 and the backup header change only in their array CRC and header CRC.

mod card;
mod device;
mod entry;
mod error;
mod gpt;
mod write;

pub use card::{Card, Plan, Role, Target};
pub use device::{Device, StreamDevice};
pub use entry::{
    APFS_TYPE, BASIC_DATA_TYPE, DEMOTED_NAME, EMUMMC_TYPE, ENTRY_BYTES, Entry, FRESH_NAMES, LINUX_TYPE, ROOT_NAME,
    SETUP_NAME,
};
pub use error::Error;
pub use gpt::SECTOR;
