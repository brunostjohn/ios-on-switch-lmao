// SPDX-License-Identifier: GPL-3.0-or-later
//! What the cacheable-copy root-boot loader needs of a layout (build_native_loader.py window_checks), and the
//! loader's constants it gives.

use switch_loader_abi::{
    BL33_BASE, BOOT_MANIFEST_HASH_BYTES, EXPORTER_RUN, KERNEL_PHYS_BASE, NATIVE_FULL_RAM_EXCLUSIONS, SD_REFERENCE,
    SD_REFERENCE_BYTES, STAGE_END, STAGE_SOURCE, WINDOW_BASE, WINDOW_BYTES_MAX, WINDOW_SIZE,
};

use crate::adt::Adt;
use crate::chosen::{self, InWindow};
use crate::constants::Constants;
use crate::layout::{BOOT_ARGS_SIZE, FULL_RAM_BANK, Layout, XNU_PAGE};
use crate::{Error, Kind};

/// The EMC rate the kernel's SwitchPMIC sets (1600 MHz), which the loader sets before its copy (boot/loader/emc.h).
const EMC_KHZ: u64 = 1_600_000;
const KERNEL_ALIGN: u64 = 0x20_0000;

pub(crate) fn require(condition: bool, why: &str) -> Result<(), Error> {
    if condition {
        Ok(())
    } else {
        Err(Error::Window(why.to_owned()))
    }
}

fn check_layout(m: &Layout) -> Result<(), Error> {
    let len = m.image.len() as u64;
    require(
        0 < len && len <= WINDOW_BYTES_MAX && len % 8 == 0,
        "image exceeds the bounded window",
    )?;
    require(
        m.kernel_phys_base == KERNEL_PHYS_BASE,
        "the kernel is not at boot_layout.h SWITCH_KERNEL_PHYS_BASE",
    )?;
    require(
        m.used_end == WINDOW_BASE + len
            && m.window_end == WINDOW_BASE + WINDOW_SIZE
            && m.virt_base.checked_add(WINDOW_SIZE).is_some()
            && WINDOW_BASE <= m.kernel_phys_base
            && m.kernel_phys_base % KERNEL_ALIGN == m.virt_base % KERNEL_ALIGN
            && m.boot_args_pa % XNU_PAGE == 0
            && m.adt_pa == m.boot_args_pa + 0x1000
            && m.adt_size > 0
            && m.adt_pa + m.adt_size <= m.used_end
            && m.boot_args_pa + BOOT_ARGS_SIZE <= m.used_end
            && m.entry_pa % 4 == 0
            && len <= WINDOW_SIZE - KERNEL_ALIGN,
        "inconsistent fixed-layout relationships",
    )?;
    require(STAGE_SOURCE + len <= WINDOW_BASE, "staging intersects destination")?;
    for (name, address) in [
        ("entry_pa", m.entry_pa),
        ("boot_args_pa", m.boot_args_pa),
        ("adt_pa", m.adt_pa),
    ] {
        require(
            (WINDOW_BASE..WINDOW_BASE + len).contains(&address),
            &format!("layout address outside used extent: {name}"),
        )?;
    }
    Ok(())
}

fn check_sidecars(m: &Layout, adt: &Adt, sources: [(&str, &[u8]); 2]) -> Result<(), Error> {
    let start = (m.adt_pa - WINDOW_BASE) as usize;
    let mut spans: Vec<(u64, u64)> = Vec::new();
    for (key, source) in sources {
        let (pos, length) = adt.property("/chosen/memory-map", key).unwrap_or((0, 0));
        require(length == 16, &format!("root_boot sidecar placeholder missing: {key}"))?;
        let slot = &m.image[start + pos..start + pos + 16];
        let address = u64::from_le_bytes(slot[..8].try_into().expect("eight bytes"));
        let size = u64::from_le_bytes(slot[8..].try_into().expect("eight bytes"));
        let within = if key == "TrustCache" {
            address % XNU_PAGE == 0
                && WINDOW_BASE <= address
                && address + size <= m.kernel_phys_base
                && m.kernel_phys_base < address + size + XNU_PAGE
        } else {
            address % 0x1000 == 0 && m.adt_pa + m.adt_size <= address && address <= m.used_end
        };
        require(
            within && size == source.len() as u64 && 0 < size && size <= 16 << 20 && size <= m.used_end - address,
            &format!("root_boot sidecar bounds changed: {key}"),
        )?;
        require(
            !spans.iter().any(|(b, e)| address < *e && *b < address + size),
            "root_boot sidecars overlap",
        )?;
        spans.push((address, address + size));
        let at = (address - WINDOW_BASE) as usize;
        require(
            m.image[at..at + size as usize] == *source,
            &format!("root_boot sidecar contents changed: {key}"),
        )?;
    }
    Ok(())
}

fn check_command_line(m: &Layout, command_line: &str) -> Result<(), Error> {
    let at = (m.boot_args_pa - WINDOW_BASE + 0x6C) as usize;
    let mut expected = command_line.as_bytes().to_vec();
    expected.resize(1024, 0);
    require(
        command_line.len() < 1024 && m.image[at..at + 1024] == expected,
        "the boot argument manifest does not match the staged boot arguments",
    )?;
    require(
        !command_line
            .split_whitespace()
            .any(|w| w.starts_with("rd=") || w.starts_with("boot-uuid=")),
        "root_boot cannot use an alternate root selection",
    )
}

fn check_exporter(window_bytes: u64) -> Result<(), Error> {
    let (run, size) = EXPORTER_RUN;
    let (_, start, capacity) = NATIVE_FULL_RAM_EXCLUSIONS
        .iter()
        .find(|r| r.0 == "staged-kernel-capacity")
        .expect("in loader_abi");
    require(
        *start <= run
            && run + size <= start + capacity
            && (run + size <= BL33_BASE || run >= STAGE_END)
            && (run + size <= WINDOW_BASE || WINDOW_BASE + window_bytes <= run),
        "the firmware exporter overlaps the kernel window or leaves its reserved range",
    )
}

fn check_native_profile(m: &Layout, kind: &Kind) -> Result<(), Error> {
    if m.virt_base != kind.virt_base {
        return Err(Error::Window(format!(
            "the prelinked image's lowest segment {:#x} is not {}'s kernel_virt_base {:#x} (ipsw/pins.bzl), which the \
             kernel derives its boot_args virtBase from",
            m.virt_base, kind.name, kind.virt_base
        )));
    }
    require(
        WINDOW_BASE & !0x1F_FFFF == 0xB200_0000 && (m.window_end + 0x1F_FFFF) & !0x1F_FFFF == 0xE220_0000,
        "native ownership envelope changed",
    )?;
    let len = m.image.len() as u64;
    require(
        len <= WINDOW_BYTES_MAX,
        &format!("native staging or display contract mismatch (window {len:#x} bytes)"),
    )?;
    require(
        len % 0x1000 == 0,
        "native cacheable copy requires a page-aligned payload",
    )
}

/// Check the layout `m` of `adt` (the ADT before its sidecars' addresses) and return the loader's constants, the
/// staged ones unknown.
pub fn window_checks(
    m: &Layout,
    adt_bytes: &[u8],
    inputs: &crate::Inputs,
    manifest_hash: Option<&[u8; BOOT_MANIFEST_HASH_BYTES as usize]>,
) -> Result<Constants, Error> {
    check_layout(m)?;
    let adt = crate::adt::parse(adt_bytes)?;
    let (ram_offset, ram_bytes) = adt.property("/chosen", "switch-ram-ledger").unwrap_or((0, 0));
    require(
        ram_bytes == 1584 && adt_bytes[ram_offset..ram_offset + ram_bytes].iter().all(|b| *b == 0),
        "full RAM requires zeroed runtime ledger slot",
    )?;
    require(
        m.boot_virt == m.virt_base - m.kernel_phys_base + FULL_RAM_BANK.0,
        "full RAM boot bases/span differ from measured bank profile",
    )?;
    let in_window = InWindow {
        image: &m.image,
        start: (m.adt_pa - WINDOW_BASE) as usize,
    };
    let mut sd_reference = SD_REFERENCE.to_le_bytes().to_vec();
    sd_reference.extend_from_slice(&SD_REFERENCE_BYTES.to_le_bytes());
    let (sd_offset, size) = adt.property("/chosen", "switch-sd-reference").unwrap_or((0, 0));
    require(
        size == 16
            && adt_bytes[sd_offset..sd_offset + 16] == *sd_reference
            && in_window.at(sd_offset, 16) == sd_reference,
        "missing or changed fixed SD reference property",
    )?;
    let slots = chosen::check(&adt, adt_bytes, &in_window, inputs.kind.root_volume_uuid, manifest_hash)?;
    check_sidecars(
        m,
        &adt,
        [
            ("NativeDataSeed", inputs.data_seed),
            ("TrustCache", inputs.trust_bundle),
        ],
    )?;
    check_command_line(m, inputs.command_line)?;
    check_exporter(m.image.len() as u64)?;
    check_native_profile(m, &inputs.kind)?;
    let (boot_phys, boot_size, boot_actual) = FULL_RAM_BANK;
    Ok(Constants::new(&[
        ("SOURCE", STAGE_SOURCE),
        ("DEST", WINDOW_BASE),
        ("BYTES", m.image.len() as u64),
        ("STAMPS_ADT_OFFSET", slots.stamps as u64),
        ("SEED_ADT_OFFSET", slots.seed as u64),
        ("SEED_SOURCE_ADT_OFFSET", slots.seed_source as u64),
        ("EMC_KHZ", EMC_KHZ),
        ("ARGS", m.boot_args_pa),
        ("ENTRY", m.entry_pa),
        ("CRC", 0),
        ("VERIFY_KERNEL", 0),
        ("VIRT", m.virt_base),
        ("ADT_PA", m.adt_pa),
        ("BOOT_PHYS", boot_phys),
        ("BOOT_VIRT", m.boot_virt),
        ("BOOT_SIZE", boot_size),
        ("BOOT_ACTUAL", boot_actual),
        ("FULL_RAM", 1),
        ("RAM_ADT_OFFSET", ram_offset as u64),
        ("WINDOW", WINDOW_SIZE),
        ("TOP", m.used_end),
        ("ADT_VA", m.adt_va),
        ("ADT_BYTES", m.adt_size),
        ("NATIVE_GATES", 1),
        ("SD_REFERENCE", 1),
        ("SD_CARD", 1),
        ("SD_REFERENCE_ADT_OFFSET", sd_offset as u64),
        ("FAST_COPY", 1),
    ]))
}
