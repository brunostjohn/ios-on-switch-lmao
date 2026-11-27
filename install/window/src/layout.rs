// SPDX-License-Identifier: GPL-3.0-or-later
//! The kernel window's image (native_xnu_layout.py build_layout) as build_native_loader.py layout requests it: the
//! full-RAM root boot at loader_abi WINDOW_BASE, the trust cache bundle below the kernel, then the boot_args page, the
//! ADT and the Data seed.

use switch_loader_abi::{NATIVE_FULL_RAM_EXCLUSIONS, VIDEO, WINDOW_BASE, WINDOW_SIZE};
use switch_trustcache::check_bundle;

use crate::adt::{self, Adt};
use crate::kernel::{self, BLOCK, Kernel, PAGE, bounded_end, valid_window};
use crate::{Error, seed};

pub const XNU_PAGE: u64 = 0x4000;
pub const BOOT_ARGS_SIZE: u64 = 0x480;
pub const FULL_RAM_BANK: (u64, u64, u64) = (0x8000_0000, 0x1_0000_0000, 0xFFD0_0000);
const RAM_LEDGER_BYTES: usize = 1584;
const FULL_RAM_KERNEL_PA: u64 = 0xB207_C000;
const FRAMEBUFFER: u64 = 0xF5A0_0000;

/// What the checks and the loader's constants read of the layout (its manifest's fields).
pub struct Layout {
    pub image: Vec<u8>,
    pub kernel_phys_base: u64,
    pub virt_base: u64,
    pub window_end: u64,
    pub entry_pa: u64,
    pub boot_args_pa: u64,
    pub adt_pa: u64,
    pub adt_va: u64,
    pub adt_size: u64,
    pub used_end: u64,
    pub boot_virt: u64,
}

pub fn align(value: u64, boundary: u64) -> u64 {
    (value + boundary - 1) & boundary.wrapping_neg()
}

fn refused(why: impl Into<String>) -> Error {
    Error::Layout(why.into())
}

/// Where the kernel's lowest segment goes: above the bundle at its VA's offset modulo 2 MiB. The bundle's 16 KiB pages
/// end where the kernel starts: XNU's arm_vm_init makes them EXTRADATA, and a RELEASE kernel panics on a TrustCache
/// above its lowest segment.
pub fn kernel_phys_base(window_base: u64, kernel_base: u64, trustcache_bytes: u64) -> u64 {
    let floor = window_base + align(trustcache_bytes, XNU_PAGE);
    floor + (i128::from(kernel_base) - i128::from(floor)).rem_euclid(i128::from(BLOCK)) as u64
}

fn mapped_window() -> Result<(u64, u64, u64), Error> {
    valid_window(WINDOW_SIZE)?;
    let end = bounded_end(WINDOW_BASE, WINDOW_SIZE, "physical window")?;
    Ok((end, WINDOW_BASE & BLOCK.wrapping_neg(), align(end, BLOCK)))
}

fn check_reservations(mapped: (u64, u64)) -> Result<(), Error> {
    for (i, (name, start, size)) in NATIVE_FULL_RAM_EXCLUSIONS.iter().enumerate() {
        if name.is_empty() || NATIVE_FULL_RAM_EXCLUSIONS[..i].iter().any(|(n, _, _)| n == name) {
            return Err(refused("reservation names must be nonempty and unique"));
        }
        let end = bounded_end(*start, *size, &format!("reservation {name}"))?;
        if *start < mapped.1 && mapped.0 < end {
            return Err(refused(format!("block-rounded window overlaps reservation {name}")));
        }
    }
    Ok(())
}

fn check_video(mapped: (u64, u64)) -> Result<(), Error> {
    let [base, display, row, width, height, depth] = VIDEO;
    let native = [base, row, width, height, depth] == [FRAMEBUFFER, 2880, 720, 1280, 0x320] && display <= 1;
    if base == 0
        || base % 4 != 0
        || (!native && (display != 1 || depth != 32))
        || width == 0
        || height == 0
        || row < width * 4
        || row % 4 != 0
    {
        return Err(refused(
            "video requires 32bpp graphics or the native rotated profile, with valid width/height/stride",
        ));
    }
    let end = bounded_end(base, row * height, "framebuffer")?;
    if base < mapped.1 && mapped.0 < end {
        return Err(refused("block-rounded window overlaps framebuffer"));
    }
    Ok(())
}

fn check_inputs(adt: &Adt, kernel: &Kernel, command_line: &str, bundle: &[u8], seed: &[u8]) -> Result<(), Error> {
    if kernel.base > u64::MAX - WINDOW_SIZE {
        return Err(refused("virtual window overflows uint64"));
    }
    if adt.window_size != 4 << 30 {
        return Err(refused(
            "explicit window differs from ADT darwinboot-memory-size (default 256 MiB)",
        ));
    }
    if !command_line.is_ascii() || command_line.len() >= 1024 || command_line.contains('\0') {
        return Err(refused("command line must be at most 1023 bytes with no NUL"));
    }
    if adt.property("/chosen/memory-map", "RAMDisk").is_some() {
        return Err(refused(
            "RAMDisk input and ADT placeholder must both be present or absent",
        ));
    }
    for (key, data) in [("TrustCache", bundle), ("NativeDataSeed", seed)] {
        if adt.property("/chosen/memory-map", key).is_none() {
            return Err(refused(format!(
                "{key} input and ADT placeholder must both be present or absent"
            )));
        }
        if data.is_empty() || data.len() > 16 << 20 {
            return Err(refused(format!(
                "input must be a nonempty regular file <= {} bytes: {key}",
                16 << 20
            )));
        }
    }
    check_bundle(bundle)?;
    seed::check(seed)
}

fn boot_args(layout: &Layout, command_line: &str) -> Vec<u8> {
    let (boot_phys, boot_size, boot_actual) = FULL_RAM_BANK;
    let mut args = vec![0u8; BOOT_ARGS_SIZE as usize];
    args[..4].copy_from_slice(&[2, 0, 2, 0]);
    let words = [layout.boot_virt, boot_phys, boot_size, layout.used_end]
        .into_iter()
        .chain(VIDEO);
    for (i, word) in words.enumerate() {
        args[8 + 8 * i..16 + 8 * i].copy_from_slice(&word.to_le_bytes());
    }
    args[0x60..0x68].copy_from_slice(&layout.adt_va.to_le_bytes());
    args[0x68..0x6C].copy_from_slice(&(layout.adt_size as u32).to_le_bytes());
    args[0x6C..0x6C + command_line.len()].copy_from_slice(command_line.as_bytes());
    args[0x478..].copy_from_slice(&boot_actual.to_le_bytes());
    args
}

struct Image {
    data: Vec<u8>,
    ranges: Vec<(u64, u64)>,
}

impl Image {
    fn place(&mut self, address: u64, data: &[u8], memory_size: u64) -> Result<(), Error> {
        let offset = address
            .checked_sub(WINDOW_BASE)
            .ok_or_else(|| refused("internal layout bounds failure"))?;
        if data.len() as u64 > memory_size || offset + memory_size > self.data.len() as u64 {
            return Err(refused("internal layout bounds failure"));
        }
        if self
            .ranges
            .iter()
            .any(|(start, end)| address < *end && *start < address + memory_size)
        {
            return Err(refused("internal layout overlap"));
        }
        self.data[offset as usize..offset as usize + data.len()].copy_from_slice(data);
        self.ranges.push((address, address + memory_size));
        Ok(())
    }
}

fn put_placeholder(tree: &mut [u8], adt: &Adt, key: &str, address: u64, size: u64) {
    let (offset, _) = adt
        .property("/chosen/memory-map", key)
        .expect("checked by check_inputs");
    tree[offset..offset + 8].copy_from_slice(&address.to_le_bytes());
    tree[offset + 8..offset + 16].copy_from_slice(&size.to_le_bytes());
}

/// The window's image for `adt`, which already holds every value but the sidecars' addresses.
pub fn build(
    kernel_image: &[u8],
    adt_bytes: &[u8],
    command_line: &str,
    bundle: &[u8],
    seed: &[u8],
) -> Result<Layout, Error> {
    let (window_end, mapped_start, mapped_end) = mapped_window()?;
    check_reservations((mapped_start, mapped_end))?;
    let (kernel, adt) = (kernel::parse(kernel_image)?, adt::parse(adt_bytes)?);
    check_video((mapped_start, mapped_end))?;
    check_inputs(&adt, &kernel, command_line, bundle, seed)?;

    let bundle_span = align(bundle.len() as u64, XNU_PAGE);
    let kernel_pa = kernel_phys_base(WINDOW_BASE, kernel.base, bundle.len() as u64);
    let trustcache_pa = kernel_pa - bundle_span;
    let args_pa = align(kernel_pa + kernel.end - kernel.base, XNU_PAGE);
    let tree_pa = args_pa + PAGE;
    let seed_pa = align(tree_pa + adt_bytes.len() as u64, PAGE);
    let seed_span = align(seed.len() as u64, PAGE);
    let used_end = align(seed_pa + seed_span, XNU_PAGE);
    if used_end - WINDOW_BASE > WINDOW_SIZE - BLOCK {
        return Err(refused("layout exceeds window or leaves less than 2 MiB slack"));
    }
    if kernel_pa != FULL_RAM_KERNEL_PA {
        return Err(refused(format!(
            "full RAM puts the kernel at 0xB207C000 (boot_layout.h SWITCH_KERNEL_PHYS_BASE), not {kernel_pa:#x}: the \
             trust caches must fit the {:#x} bytes below it",
            FULL_RAM_KERNEL_PA - WINDOW_BASE
        )));
    }
    if adt.property("/chosen", "switch-ram-ledger").map(|(_, n)| n) != Some(RAM_LEDGER_BYTES) {
        return Err(refused(
            "full RAM requires the measured native profile and runtime ledger",
        ));
    }

    let mut tree = adt_bytes.to_vec();
    put_placeholder(&mut tree, &adt, "TrustCache", trustcache_pa, bundle.len() as u64);
    put_placeholder(&mut tree, &adt, "NativeDataSeed", seed_pa, seed.len() as u64);
    let mut layout = Layout {
        image: Vec::new(),
        kernel_phys_base: kernel_pa,
        virt_base: kernel.base,
        window_end,
        entry_pa: kernel_pa + kernel.entry - kernel.base,
        boot_args_pa: args_pa,
        adt_pa: tree_pa,
        adt_va: kernel.base + tree_pa - kernel_pa,
        adt_size: tree.len() as u64,
        used_end,
        boot_virt: kernel.base - kernel_pa + FULL_RAM_BANK.0,
    };
    let mut image = Image {
        data: vec![0; (used_end - WINDOW_BASE) as usize],
        ranges: Vec::new(),
    };
    for segment in &kernel.segments {
        let file = &kernel_image[segment.fileoff as usize..(segment.fileoff + segment.filesize) as usize];
        image.place(kernel_pa + segment.vmaddr - kernel.base, file, segment.vmsize)?;
    }
    image.place(args_pa, &boot_args(&layout, command_line), PAGE)?;
    image.place(tree_pa, &tree, tree.len() as u64)?;
    image.place(trustcache_pa, bundle, bundle.len() as u64)?;
    image.place(seed_pa, seed, seed_span)?;
    layout.image = image.data;
    Ok(layout)
}
