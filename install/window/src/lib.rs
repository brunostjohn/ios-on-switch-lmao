// SPDX-License-Identifier: GPL-3.0-or-later
//! The kernel window and switchroot/ios/bl33.bin, made where the IPSW is, byte for byte as //boot:package makes them
//! (boot/defs.bzl's BootTrust, KernelLayout, KernelCompress, KernelWindow and BootBl33 steps).
//!
//! The release's loader is compiled for one window: [`bl33`] refuses any window whose constants differ from the
//! constants.json it ships, STAGE_ID (the SHA-256 of the staged stream) included.

mod adt;
mod checks;
mod chosen;
mod constants;
mod kernel;
mod layout;
mod lz4;
mod pack;
mod seed;
mod stage;
mod trust;

#[cfg(test)]
mod tests;

use sha2::{Digest, Sha384};
use switch_loader_abi::{BOOT_MANIFEST_HASH_BYTES, BOOT_MANIFEST_HASH_PROPERTY};

pub use constants::{Constants, NAMES, Row, STAGED, STORED};
pub use lz4::{Block, compress_blocks, frame_blocks};
pub use stage::{Boot, bl33};
pub use trust::{Bundle, trust_bundle};

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("native layout refused: {0}")]
    Layout(String),
    #[error("kernel window: {0}")]
    Window(String),
    #[error("lz4: {0}")]
    Lz4(String),
    #[error("{0}")]
    Stage(String),
    #[error("constants.json: {0}")]
    Constants(String),
    #[error("the release's loader was built for another kernel window ({} differ)", .0.join(", "))]
    LoaderBuiltForAnotherWindow(Vec<String>),
    #[error(transparent)]
    Trust(#[from] switch_trustcache::Error),
}

/// The IPSW kind the window is for, from its pins (ipsw/pins.bzl).
#[derive(Clone, Copy, Debug)]
pub struct Kind<'a> {
    pub name: &'a str,
    /// The prelinked kernel's lowest segment (derived kernel_virt_base).
    pub virt_base: u64,
    /// The System volume's UUID (volumes os_volume_uuid), which /chosen/root-matching names.
    pub root_volume_uuid: &'a str,
    /// iOS 27 on: /chosen/boot-manifest-hash holds the SHA-384 of the kernel and the bundle.
    pub boot_manifest_hash: bool,
}

pub struct Inputs<'a> {
    pub kernel: &'a [u8],
    /// The finished ADT (switch_adt::finish), its manifest hash slot still zero.
    pub adt: &'a [u8],
    pub trust_bundle: &'a [u8],
    /// NativeDataSeed: a DBRDISK1 image of the Data volume's first blocks.
    pub data_seed: &'a [u8],
    /// The built-in boot-args (tools/package/boot_args.py).
    pub command_line: &'a str,
    pub kind: Kind<'a>,
}

/// The checked layout: its image, the blocks to compress, and the loader's constants but the staged ones.
pub struct Layout {
    pub image: Vec<u8>,
    pub entry_pa: u64,
    pub blocks: Vec<(usize, usize)>,
    pub constants: Constants,
}

/// The staged window bl33.bin carries and the constants.json it gives.
pub struct Window {
    pub staged: Vec<u8>,
    pub constants: Constants,
}

/// build_native_loader.py with_boot_manifest_hash: the ADT with its zeroed slot holding `digest`.
fn with_boot_manifest_hash(adt: &[u8], digest: &[u8]) -> Result<Vec<u8>, Error> {
    let size = BOOT_MANIFEST_HASH_BYTES as usize;
    match adt::parse(adt)?.property("/chosen", BOOT_MANIFEST_HASH_PROPERTY) {
        Some((offset, length)) if length == size && adt[offset..offset + size].iter().all(|b| *b == 0) => {
            let mut patched = adt.to_vec();
            patched[offset..offset + size].copy_from_slice(digest);
            Ok(patched)
        }
        _ => Err(Error::Window(
            "boot manifest hash placeholder must be a zeroed, exact-size /chosen property".into(),
        )),
    }
}

/// build_native_loader.py layout: lay the window out, check it, and cut its nonzero extents into blocks.
pub fn layout(inputs: &Inputs) -> Result<Layout, Error> {
    let mut adt = inputs.adt.to_vec();
    let mut manifest_hash = None;
    if inputs.kind.boot_manifest_hash {
        let digest: [u8; BOOT_MANIFEST_HASH_BYTES as usize] = Sha384::new()
            .chain_update(inputs.kernel)
            .chain_update(inputs.trust_bundle)
            .finalize()
            .into();
        adt = with_boot_manifest_hash(&adt, &digest)?;
        manifest_hash = Some(digest);
    }
    let m = layout::build(
        inputs.kernel,
        &adt,
        inputs.command_line,
        inputs.trust_bundle,
        inputs.data_seed,
    )?;
    let constants = checks::window_checks(&m, &adt, inputs, manifest_hash.as_ref())?;
    let blocks = pack::blocks(&pack::extents(&m.image));
    Ok(Layout {
        entry_pa: m.entry_pa,
        image: m.image,
        blocks,
        constants,
    })
}

/// Each of the layout's blocks LZ4-compressed on its own, as lz4 -12 compresses it, in parallel.
pub fn compress(layout: &Layout) -> Result<Vec<Block>, Error> {
    lz4::compress_blocks(
        &layout
            .blocks
            .iter()
            .map(|&(at, n)| &layout.image[at..at + n])
            .collect::<Vec<_>>(),
    )
}

/// build_native_loader.py join: the staged window and its constants, from the layout's blocks compressed.
pub fn join(layout: &Layout, compressed: &[Block]) -> Result<Window, Error> {
    let (constants, staged) = pack::staged(&layout.constants, &layout.blocks, compressed)?;
    Ok(Window { staged, constants })
}

pub fn window(layout: &Layout) -> Result<Window, Error> {
    join(layout, &compress(layout)?)
}
