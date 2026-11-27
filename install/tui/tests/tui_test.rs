// SPDX-License-Identifier: GPL-3.0-or-later
mod progress;
mod render;
mod wizard;

use ratatui::Terminal;
use ratatui::backend::TestBackend;
use switch_tui::{
    App, BuildOption, CardOption, Mode, PendingJournal, RelocationEstimate, SizeConstraints, SlotStatus, WizardConfig,
};

pub const MIB: u64 = 1024 * 1024;
pub const GIB: u64 = 1024 * MIB;

pub fn sample_builds() -> Vec<BuildOption> {
    vec![
        BuildOption {
            id: "24B119".to_owned(),
            label: "iPadOS 27.2 (24B119)".to_owned(),
            product: "iPad12,1".to_owned(),
            release_tag: "v0.2.0".to_owned(),
            size_summary: "~7.8 GiB download · min 16 GiB".to_owned(),
            min_partition_bytes: Some(16 * GIB),
            unsupported_reason: None,
        },
        BuildOption {
            id: "21H461".to_owned(),
            label: "iPadOS 17.7.11 (21H461)".to_owned(),
            product: "iPad7,1".to_owned(),
            release_tag: "v0.2.0".to_owned(),
            size_summary: "~6.1 GiB download · min 16 GiB".to_owned(),
            min_partition_bytes: Some(16 * GIB),
            unsupported_reason: None,
        },
        BuildOption {
            id: "25A999".to_owned(),
            label: "iPadOS 28.0 (25A999)".to_owned(),
            product: "iPad14,1".to_owned(),
            release_tag: "v0.3.0".to_owned(),
            size_summary: "~8.4 GiB download".to_owned(),
            min_partition_bytes: None,
            unsupported_reason: Some("unknown step kind 'apfs_v3'".to_owned()),
        },
    ]
}

pub fn sample_constraints() -> SizeConstraints {
    SizeConstraints {
        min_bytes: 20 * GIB,
        max_bytes: 200 * GIB,
        default_bytes: None,
        total_card_bytes: 954 * GIB,
        fat_total_bytes: 820 * GIB,
        fat_used_bytes: 380 * GIB,
        other_partitions_bytes: 70 * GIB,
        relocate_estimate: RelocationEstimate::Samples(vec![
            (20 * GIB, 512 * MIB),
            (64 * GIB, 4 * GIB),
            (200 * GIB, 40 * GIB),
        ]),
    }
}

pub fn sample_cards() -> Vec<CardOption> {
    vec![
        CardOption {
            id: "disk4".to_owned(),
            name: "SanDisk Extreme PRO 1TB".to_owned(),
            capacity_bytes: 954 * GIB,
            layout_summary: "GPT + hybrid MBR · 820 GiB FAT32 · 64 GiB iOS · 70 GiB other".to_owned(),
            slot: SlotStatus::ExistingIos {
                size_bytes: 64 * GIB,
                partition_index: Some(10),
            },
            pending_journal: None,
            confirm_token: "disk4".to_owned(),
            confirm_case_sensitive: true,
            size_constraints: Some(sample_constraints()),
            preserved_items: Vec::new(),
            overwritten_items: Vec::new(),
            unusable_reason: None,
        },
        CardOption {
            id: r"\\.\PhysicalDrive2".to_owned(),
            name: "Samsung PRO Plus 256GB".to_owned(),
            capacity_bytes: 238 * GIB,
            layout_summary: "MBR-only · 206 GiB FAT32 · 32 GiB emuMMC".to_owned(),
            slot: SlotStatus::ExistingSetup {
                size_bytes: 48 * GIB,
                partition_index: Some(2),
            },
            pending_journal: None,
            confirm_token: r"\\.\PhysicalDrive2".to_owned(),
            confirm_case_sensitive: false,
            size_constraints: Some(SizeConstraints {
                min_bytes: 16 * GIB,
                max_bytes: 48 * GIB,
                default_bytes: None,
                total_card_bytes: 238 * GIB,
                fat_total_bytes: 206 * GIB,
                fat_used_bytes: 90 * GIB,
                other_partitions_bytes: 32 * GIB,
                relocate_estimate: RelocationEstimate::Proportional,
            }),
            preserved_items: Vec::new(),
            overwritten_items: Vec::new(),
            unusable_reason: None,
        },
    ]
}

pub fn card_with_pending_journal() -> CardOption {
    CardOption {
        id: "/dev/sdb".to_owned(),
        name: "Lexar PLAY 512GB".to_owned(),
        capacity_bytes: 476 * GIB,
        layout_summary: "GPT · 412 GiB FAT32 · interrupted carve".to_owned(),
        slot: SlotStatus::ExistingSetup {
            size_bytes: 64 * GIB,
            partition_index: Some(1),
        },
        pending_journal: Some(PendingJournal {
            stage: "Relocating FAT32 tail clusters".to_owned(),
            detail: "Journal at LBA 32752-32767 has 18 MiB left to move".to_owned(),
        }),
        confirm_token: "/dev/sdb".to_owned(),
        confirm_case_sensitive: true,
        size_constraints: Some(sample_constraints()),
        preserved_items: Vec::new(),
        overwritten_items: Vec::new(),
        unusable_reason: None,
    }
}

pub fn sample_config(initial_mode: Option<Mode>) -> WizardConfig {
    WizardConfig {
        title: "iPadOS Switch Installer".to_owned(),
        initial_mode,
        builds: sample_builds(),
        cards: sample_cards(),
    }
}

pub fn render_to_string(app: &App, width: u16, height: u16) -> String {
    let backend = TestBackend::new(width, height);
    let mut terminal = Terminal::new(backend).expect("create TestBackend terminal");
    terminal
        .draw(|frame| switch_tui::render(frame, app))
        .expect("render frame");
    let buf = terminal.backend().buffer();
    let mut out = String::with_capacity((width as usize + 1) * height as usize);
    for y in 0..height {
        for x in 0..width {
            out.push_str(buf[(x, y)].symbol());
        }
        out.push('\n');
    }
    out
}
