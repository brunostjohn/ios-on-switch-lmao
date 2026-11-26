// SPDX-License-Identifier: GPL-3.0-or-later
//! bl33.bin as hekate loads it at 0xAA000000 (tools/package/bl33.py stage, build_native_loader.py stage_into_bl33):
//! the release's patched U-Boot, the boot script, loader, exporter and DTB in their slots past it, then the staged
//! window between its header and trailer pages (boot/loader/stage.h).

use switch_loader_abi::{
    BL33_BASE, DTB_SLOT, EXPORTER_RUN, EXPORTER_SLOT, LOADER_SLOT, PACK_PAGE, SCRIPT_SLOT, STAGE_END, STAGE_HEADER,
    STAGE_SOURCE, STAGE_WORDS_HEAD, STAGE_WORDS_MAGIC, STAGE_WORDS_TAIL, STAGE_WORDS_VERSION,
};

use crate::checks::require;
use crate::{Error, Window};

const PAGE: usize = PACK_PAGE as usize;

/// The release's files bl33.bin is made of besides the window.
pub struct Boot<'a> {
    /// u-boot.bin: switchroot's bl33.bin after bl33.py patch.
    pub u_boot: &'a [u8],
    pub script: &'a [u8],
    pub loader: &'a [u8],
    pub exporter: &'a [u8],
    pub dtb: &'a [u8],
    /// The constants.json the loader was built for.
    pub loader_constants: &'a [u8],
}

fn words(values: &[u64]) -> Vec<u8> {
    let mut page: Vec<u8> = values.iter().flat_map(|v| (*v as u32).to_le_bytes()).collect();
    page.resize(PAGE, 0);
    page
}

fn u64_at(data: &[u8], at: usize) -> u64 {
    data.get(at..at + 8)
        .map_or(0, |b| u64::from_le_bytes(b.try_into().expect("eight bytes")))
}

fn check_exporter(exporter: &[u8]) -> Result<(), Error> {
    let word = |at: usize| {
        exporter
            .get(at..at + 4)
            .map(|b| u32::from_be_bytes(b.try_into().expect("four bytes")))
    };
    let (run, capacity) = EXPORTER_RUN;
    let fits = matches!((word(12), word(16), word(20)), (Some(size), Some(load), Some(entry))
        if u64::from(load) == run && u64::from(entry) == run && u64::from(size) <= capacity);
    require(
        fits,
        "exporter.uimg does not load and run where loader_abi.EXPORTER_RUN says",
    )
}

/// U-Boot's image and pre-relocation BSS (its arm64 start.S header: _end_ofs at 0x10, _bss_end_ofs at 0x20) end
/// below the first slot, the slots keep apart, and everything stays below hekate's ramoops.
fn check_layout(u_boot: &[u8], slots: &[(u64, u64, &[u8])], staged: usize) -> Result<(), Error> {
    let header_at = (STAGE_HEADER - BL33_BASE) as usize;
    let (end_ofs, bss_end_ofs) = (u64_at(u_boot, 0x10), u64_at(u_boot, 0x20));
    let floor = slots
        .iter()
        .map(|(address, _, _)| (address - BL33_BASE) as usize)
        .min()
        .unwrap_or(header_at);
    let floor = floor.min(header_at) as u64;
    if !(0 < end_ofs
        && end_ofs <= u_boot.len() as u64
        && u_boot.len() as u64 <= floor
        && 0 < bss_end_ofs
        && bss_end_ofs <= floor)
    {
        return Err(Error::Stage(format!(
            "bl33: U-Boot (_end {end_ofs:#x}, BSS end {bss_end_ofs:#x}) reaches a slot or the window"
        )));
    }
    let mut spans: Vec<(u64, u64, usize)> = slots.iter().map(|(a, c, content)| (*a, *c, content.len())).collect();
    spans.sort();
    spans.push((STAGE_HEADER, 0, 0));
    for pair in spans.windows(2) {
        let ((address, capacity, length), (following, _, _)) = (pair[0], pair[1]);
        if length == 0 || length as u64 > capacity || address + capacity > following {
            return Err(Error::Stage(format!(
                "bl33: {length} bytes for the {capacity:#x}-byte slot at {address:#x}, or it overlaps the next"
            )));
        }
    }
    require(
        STAGE_SOURCE + (staged + PAGE) as u64 <= STAGE_END,
        "bl33: staged window reaches ramoops at 0xB0000000",
    )
}

/// bl33.bin for `window`, refused unless the release's loader was built for exactly this window.
pub fn bl33(window: &Window, boot: &Boot) -> Result<Vec<u8>, Error> {
    let constants = &window.constants;
    constants.check_against(boot.loader_constants)?;
    let stream_bytes = constants.get("STREAM_BYTES").expect("a staged window's");
    let id = constants.stage_id().expect("a staged window's");
    let staged = window.staged.len();
    require(
        constants.get("SOURCE") == Some(STAGE_SOURCE) && constants.get("PACKED_BYTES") == Some(staged as u64),
        "loader was not built for the bl33-staged window",
    )?;
    require(
        staged % PAGE == 0 && stream_bytes as usize <= staged && staged < stream_bytes as usize + PAGE,
        "staged window must be its stream padded to 4 KiB",
    )?;
    check_exporter(boot.exporter)?;
    let slots = [
        (SCRIPT_SLOT.0, SCRIPT_SLOT.1, boot.script),
        (LOADER_SLOT.0, LOADER_SLOT.1, boot.loader),
        (EXPORTER_SLOT.0, EXPORTER_SLOT.1, boot.exporter),
        (DTB_SLOT.0, DTB_SLOT.1, boot.dtb),
    ];
    check_layout(boot.u_boot, &slots, staged)?;

    let source = (STAGE_SOURCE - BL33_BASE) as usize;
    let mut image = vec![0u8; source + staged + PAGE];
    image[..boot.u_boot.len()].copy_from_slice(boot.u_boot);
    for (address, _, content) in slots {
        let at = (address - BL33_BASE) as usize;
        image[at..at + content.len()].copy_from_slice(content);
    }
    let (low, high) = (id & 0xFFFF_FFFF, id >> 32);
    let header = words(&[
        STAGE_WORDS_MAGIC,
        STAGE_WORDS_HEAD,
        STAGE_WORDS_VERSION,
        STAGE_SOURCE - STAGE_HEADER,
        stream_bytes,
        staged as u64,
        low,
        high,
    ]);
    let trailer = words(&[
        STAGE_WORDS_MAGIC,
        STAGE_WORDS_TAIL,
        stream_bytes,
        staged as u64,
        low,
        high,
    ]);
    image[source - PAGE..source].copy_from_slice(&header);
    image[source..source + staged].copy_from_slice(&window.staged);
    image[source + staged..].copy_from_slice(&trailer);
    Ok(image)
}
