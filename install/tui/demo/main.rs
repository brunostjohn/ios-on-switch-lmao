// SPDX-License-Identifier: GPL-3.0-or-later
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use clap::{Parser, ValueEnum};
use switch_tui::{
    BuildOption, CancelToken, CardOption, CardStateAfterError, DoneSummary, ErrorSummary, FinalStatus, Mode,
    PendingJournal, ProgressEvent, RelocationEstimate, Selection, SizeConstraints, SlotStatus, StepDescriptor,
    StepStatus, WizardConfig, run_terminal,
};

const GIB: u64 = 1024 * 1024 * 1024;
const MIB: u64 = 1024 * 1024;

#[derive(Clone, Copy, ValueEnum)]
enum CliMode {
    Install,
    Partition,
    PartitionAndInstall,
}

impl From<CliMode> for Mode {
    fn from(value: CliMode) -> Self {
        match value {
            CliMode::Install => Mode::Install,
            CliMode::Partition => Mode::Partition,
            CliMode::PartitionAndInstall => Mode::PartitionAndInstall,
        }
    }
}

#[derive(Parser)]
#[command(
    name = "switch_tui_demo",
    about = "Interactive demo of the Switch iPadOS installer TUI"
)]
struct Args {
    #[arg(long, value_enum)]
    mode: Option<CliMode>,
    #[arg(long)]
    simulate_error: bool,
    #[arg(long)]
    pending_journal: bool,
}

fn demo_builds() -> Vec<BuildOption> {
    vec![
        BuildOption {
            id: "24B119".to_owned(),
            label: "iPadOS 27.2 (24B119)".to_owned(),
            product: "iPad12,1".to_owned(),
            release_tag: "v0.2.0".to_owned(),
            size_summary: "~7.8 GiB download · min 16 GiB partition".to_owned(),
            min_partition_bytes: Some(16 * GIB),
            unsupported_reason: None,
        },
        BuildOption {
            id: "21H461".to_owned(),
            label: "iPadOS 17.7.11 (21H461)".to_owned(),
            product: "iPad7,1".to_owned(),
            release_tag: "v0.2.0".to_owned(),
            size_summary: "~6.1 GiB download · min 16 GiB partition".to_owned(),
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
            unsupported_reason: Some("Requires a newer installer (unknown step kind 'apfs_v3')".to_owned()),
        },
    ]
}

fn demo_cards(pending_journal: bool) -> Vec<CardOption> {
    vec![
        CardOption {
            id: "disk4".to_owned(),
            name: "SanDisk Extreme PRO 1TB".to_owned(),
            capacity_bytes: 954 * GIB,
            layout_summary: "GPT + hybrid MBR · 820 GiB FAT32 · 64 GiB iOS · 32 GiB emuMMC · 38 GiB L4T".to_owned(),
            slot: SlotStatus::ExistingIos {
                size_bytes: 64 * GIB,
                partition_index: Some(10),
            },
            pending_journal: pending_journal.then(|| PendingJournal {
                stage: "Relocating FAT32 tail clusters (62% done)".to_owned(),
                detail: "Carving 64 GiB iOS-setup partition from FAT32 tail".to_owned(),
            }),
            confirm_token: "disk4".to_owned(),
            confirm_case_sensitive: true,
            size_constraints: Some(SizeConstraints {
                min_bytes: 16 * GIB,
                max_bytes: 340 * GIB,
                default_bytes: None,
                total_card_bytes: 954 * GIB,
                fat_total_bytes: 820 * GIB,
                fat_used_bytes: 410 * GIB,
                other_partitions_bytes: 70 * GIB,
                relocate_estimate: RelocationEstimate::Samples(vec![
                    (16 * GIB, 512 * MIB),
                    (64 * GIB, 4 * GIB + 256 * MIB),
                    (128 * GIB, 14 * GIB),
                    (340 * GIB, 68 * GIB),
                ]),
            }),
            preserved_items: Vec::new(),
            overwritten_items: Vec::new(),
            unusable_reason: None,
        },
        CardOption {
            id: "disk5".to_owned(),
            name: "Samsung PRO Plus 256GB".to_owned(),
            capacity_bytes: 238 * GIB,
            layout_summary: "MBR-only Hekate · 206 GiB FAT32 · 32 GiB emuMMC".to_owned(),
            slot: SlotStatus::None,
            pending_journal: None,
            confirm_token: "disk5".to_owned(),
            confirm_case_sensitive: true,
            size_constraints: Some(SizeConstraints {
                min_bytes: 16 * GIB,
                max_bytes: 112 * GIB,
                default_bytes: None,
                total_card_bytes: 238 * GIB,
                fat_total_bytes: 206 * GIB,
                fat_used_bytes: 88 * GIB,
                other_partitions_bytes: 32 * GIB,
                relocate_estimate: RelocationEstimate::Proportional,
            }),
            preserved_items: Vec::new(),
            overwritten_items: Vec::new(),
            unusable_reason: None,
        },
    ]
}

fn run_simulated_work(
    selection: Selection,
    cancel: CancelToken,
    simulate_error: bool,
) -> (Vec<StepDescriptor>, mpsc::Receiver<ProgressEvent>) {
    let steps = vec![
        StepDescriptor::new("download", "Download release & IPSW"),
        StepDescriptor::new("build", "Assemble APFS image & shader seed"),
        StepDescriptor::new("write", "Write image to microSD card"),
        StepDescriptor::new("boot", "Update switchroot/ios boot entry"),
    ];
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        let _ = tx.send(ProgressEvent::Log(format!(
            "Starting {} on {}",
            selection.mode.label(),
            selection.card_id
        )));

        let _ = tx.send(ProgressEvent::StepStarted {
            step_id: "download".to_owned(),
            irreversible: false,
        });
        let total = 200 * MIB;
        for step in 1..=10 {
            if cancel.is_cancelled() {
                let _ = tx.send(ProgressEvent::Cancelled {
                    message: "Cancelled before touching the microSD card.".to_owned(),
                    log_path: Some("/tmp/switch-installer.log".to_owned()),
                });
                return;
            }
            let _ = tx.send(ProgressEvent::StepProgress {
                step_id: "download".to_owned(),
                done_bytes: step * 20 * MIB,
                total_bytes: Some(total),
                bytes_per_sec: Some(45.0 * MIB as f64),
                detail: Some("Fetching IPSW payload".to_owned()),
            });
            thread::sleep(Duration::from_millis(80));
        }
        let _ = tx.send(ProgressEvent::StepFinished {
            step_id: "download".to_owned(),
            status: StepStatus::Done,
            elapsed: None,
        });

        let _ = tx.send(ProgressEvent::StepStarted {
            step_id: "build".to_owned(),
            irreversible: false,
        });
        let _ = tx.send(ProgressEvent::StepProgress {
            step_id: "build".to_owned(),
            done_bytes: 0,
            total_bytes: None,
            bytes_per_sec: None,
            detail: Some("Sealing cryptex volumes".to_owned()),
        });
        thread::sleep(Duration::from_millis(250));
        let _ = tx.send(ProgressEvent::StepFinished {
            step_id: "build".to_owned(),
            status: StepStatus::Done,
            elapsed: None,
        });

        if simulate_error {
            let _ = tx.send(ProgressEvent::StepStarted {
                step_id: "write".to_owned(),
                irreversible: true,
            });
            let _ = tx.send(ProgressEvent::Log("I/O error writing sector 0x0040_0000".to_owned()));
            let _ = tx.send(ProgressEvent::StepFinished {
                step_id: "write".to_owned(),
                status: StepStatus::Failed,
                elapsed: None,
            });
            let _ = tx.send(ProgressEvent::Failed(ErrorSummary {
                title: "Write failed".to_owned(),
                message: "Simulated I/O error while streaming APFS container.".to_owned(),
                log_path: Some("/tmp/switch-installer.log".to_owned()),
                card_state: CardStateAfterError::Resumable {
                    detail: "Partition journal intact; re-run to resume safely.".to_owned(),
                },
                failed_step: Some("Write image to microSD card".to_owned()),
            }));
            return;
        }

        let _ = tx.send(ProgressEvent::StepStarted {
            step_id: "write".to_owned(),
            irreversible: true,
        });
        for step in 1..=8 {
            let _ = tx.send(ProgressEvent::StepProgress {
                step_id: "write".to_owned(),
                done_bytes: step * 16 * MIB,
                total_bytes: Some(128 * MIB),
                bytes_per_sec: Some(64.0 * MIB as f64),
                detail: Some("Streaming APFS blocks".to_owned()),
            });
            thread::sleep(Duration::from_millis(80));
        }
        let _ = tx.send(ProgressEvent::StepFinished {
            step_id: "write".to_owned(),
            status: StepStatus::Done,
            elapsed: None,
        });

        let _ = tx.send(ProgressEvent::StepStarted {
            step_id: "boot".to_owned(),
            irreversible: true,
        });
        let _ = tx.send(ProgressEvent::Log(
            "Wrote switchroot/ios/boot.bin and ios.ini".to_owned(),
        ));
        let _ = tx.send(ProgressEvent::StepFinished {
            step_id: "boot".to_owned(),
            status: StepStatus::Done,
            elapsed: None,
        });

        let _ = tx.send(ProgressEvent::Finished(DoneSummary {
            mode: selection.mode,
            title: "iPadOS installation complete".to_owned(),
            card_label: selection.card_id,
            ejected: true,
            details: vec!["Preserved Horizon OS, emuMMC, and bootargs.txt.".to_owned()],
            next_steps: Vec::new(),
            log_path: Some("/tmp/switch-installer.log".to_owned()),
        }));
    });
    (steps, rx)
}

fn main() {
    let args = Args::parse();
    let pending = args.pending_journal;
    let simulate_error = args.simulate_error;
    let config = WizardConfig {
        title: "iPadOS Switch Installer (Demo)".to_owned(),
        initial_mode: args.mode.map(Mode::from),
        builds: demo_builds(),
        cards: demo_cards(pending),
    };
    match run_terminal(
        config,
        move || Ok(demo_cards(pending)),
        move |sel, cancel| run_simulated_work(sel, cancel, simulate_error),
    ) {
        Ok(FinalStatus::CancelledInWizard) => println!("Cancelled."),
        Ok(FinalStatus::Done(summary)) => println!("Done: {}", summary.title),
        Ok(FinalStatus::Error(err)) => {
            eprintln!("Error: {} — {}", err.title, err.message);
            std::process::exit(1);
        }
        Err(err) => {
            eprintln!("TUI error: {err}");
            std::process::exit(1);
        }
    }
}
