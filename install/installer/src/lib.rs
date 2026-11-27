// SPDX-License-Identifier: GPL-3.0-or-later
pub mod cli;
pub mod convert;
pub mod discover;
pub mod runner;

use std::io::{IsTerminal, stdin, stdout};
use std::process::ExitCode;

use clap::Parser;
use switch_catalog::Catalog;
use switch_engine::{Mode, RunRequest, estimate_min_partition_bytes, load_catalog, select_build};
use switch_tui::{format_bytes, format_gib};

pub use cli::{Cli, CliMode, GuardError, parse_size_bytes, validate_non_interactive};
pub use convert::{
    card_error_state_to_tui, catalog_to_build_options, disk_confirm_token, disk_to_card_option, engine_event_to_tui,
    image_to_card_option, mode_from_tui, mode_to_tui,
};
pub use discover::{DiscoveredCard, discover_cards, discover_image_card, match_card_selector};

pub fn run_from_env() -> ExitCode {
    let cli = Cli::parse();
    let is_tty = stdout().is_terminal() && stdin().is_terminal();
    match execute(&cli, is_tty) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(msg) => {
            eprintln!("error: {msg}");
            ExitCode::FAILURE
        }
    }
}

pub fn execute(cli: &Cli, is_tty: bool) -> Result<bool, String> {
    if cli.list_cards {
        return print_cards(cli);
    }

    let need_catalog =
        cli.list_builds || cli.should_use_tui(is_tty) || cli.mode.map(Mode::from).is_none_or(Mode::needs_build);
    let catalog = if need_catalog {
        match load_catalog(
            cli.catalog.as_deref(),
            cli.catalog_url.as_deref(),
            cli.release_tag.as_deref(),
        ) {
            Ok(c) => Some(c),
            Err(err) => {
                if cli.list_builds || cli.mode.map(Mode::from).is_some_and(Mode::needs_build) {
                    return Err(format!("failed to load catalog: {err}"));
                }
                eprintln!("warning: could not load release catalog ({err}); continuing without build list");
                None
            }
        }
    } else {
        None
    };

    if cli.list_builds {
        let cat = catalog.as_ref().ok_or("no catalog loaded")?;
        print_builds(cat);
        return Ok(true);
    }

    if cli.should_use_tui(is_tty) {
        let cards = match &cli.image {
            Some(path) => vec![discover_image_card(path, 0)?],
            None => discover_cards(!cli.no_eject, cli.force_unmount)?,
        };
        return runner::run_tui_session(cli, catalog, cards);
    }

    run_non_interactive(cli, catalog.as_ref())
}

pub fn run_non_interactive(cli: &Cli, catalog: Option<&Catalog>) -> Result<bool, String> {
    let discovered = resolve_target_card(cli)?;
    let expected_token = discovered
        .as_ref()
        .map(|c| c.option.effective_confirm_token().to_owned());
    let case_sensitive = discovered.as_ref().is_none_or(|c| c.option.confirm_case_sensitive);

    let mode = validate_non_interactive(cli, expected_token.as_deref(), case_sensitive).map_err(|e| e.to_string())?;
    let card = discovered.ok_or_else(|| "target card or image not found".to_owned())?;

    if cli.dry_run {
        let plan_text = format_dry_run(mode, cli, catalog, &card)?;
        print!("{plan_text}");
        return Ok(true);
    }

    let mut request = RunRequest::new(mode, card.target);
    request.build_id = cli.build.clone();
    request.partition_index = cli.partition;
    request.ios_size_bytes = cli.ios_size;
    request.resume_journal = cli.resume;
    request.work_dir = cli.cache_dir.clone();
    request.log_path = cli.log_file.clone();
    request.local = cli.local_inputs();

    match runner::run_cli_session(&request, catalog) {
        Ok(summary) => {
            println!(
                "Completed {:?} on {} (ejected={})",
                summary.mode, summary.card_label, summary.ejected
            );
            if let Some(build) = &summary.build {
                println!("Build: {build}");
            }
            if let (Some(idx), Some(bytes)) = (summary.partition_index, summary.partition_bytes) {
                println!("Partition {idx}: {}", format_bytes(bytes));
            }
            if let Some(log) = &summary.log_path {
                println!("Log: {}", log.display());
            }
            Ok(true)
        }
        Err(failure) => {
            eprintln!("error: {}", failure.error);
            if let Some(log) = &failure.log_path {
                eprintln!("log: {}", log.display());
            }
            Ok(false)
        }
    }
}

pub fn format_dry_run(
    mode: Mode,
    cli: &Cli,
    catalog: Option<&Catalog>,
    card: &DiscoveredCard,
) -> Result<String, String> {
    let mut lines = Vec::new();
    lines.push(format!("Dry run: {:?}", mode));
    lines.push(format!(
        "Target: {} ({}, {})",
        card.option.id,
        card.option.name,
        format_bytes(card.option.capacity_bytes)
    ));
    lines.push(format!("Layout: {}", card.option.layout_summary));
    lines.push(format!("Slot: {}", card.option.slot.badge()));
    if let Some(j) = &card.option.pending_journal {
        lines.push(format!("Pending journal: {} — {}", j.stage, j.detail));
    }
    if mode.needs_build() {
        let cat = catalog.ok_or("catalog is required for install/partition+install")?;
        let build = select_build(cat, cli.build.as_deref()).map_err(|e| e.to_string())?;
        lines.push(format!(
            "Build: {} (iPadOS {}, {}, min partition {})",
            build.build(),
            build.record.version,
            build.record.product,
            format_gib(estimate_min_partition_bytes(&build))
        ));
    }
    if mode.needs_partition() {
        if let Some(size) = cli.ios_size {
            lines.push(format!("Requested iOS size: {}", format_gib(size)));
        } else if let Some(sc) = &card.option.size_constraints {
            let def = sc.default_bytes.unwrap_or(sc.min_bytes);
            lines.push(format!(
                "Default iOS size: {} (range {} .. {})",
                format_gib(def),
                format_gib(sc.min_bytes),
                format_gib(sc.max_bytes)
            ));
        }
    }
    lines.push("No writes performed (--dry-run).".to_owned());
    Ok(lines.join("\n") + "\n")
}

fn resolve_target_card(cli: &Cli) -> Result<Option<DiscoveredCard>, String> {
    if let Some(path) = &cli.image {
        return Ok(Some(discover_image_card(path, 0)?));
    }
    let Some(selector) = cli.card.as_deref() else {
        return Ok(None);
    };
    let cards = discover_cards(!cli.no_eject, cli.force_unmount)?;
    let found = match_card_selector(&cards, selector)
        .cloned()
        .ok_or_else(|| format!("removable card {selector:?} not found (use --list-cards)"))?;
    Ok(Some(found))
}

fn print_cards(cli: &Cli) -> Result<bool, String> {
    let cards = match &cli.image {
        Some(path) => vec![discover_image_card(path, 0)?],
        None => discover_cards(!cli.no_eject, cli.force_unmount)?,
    };
    if cards.is_empty() {
        println!("No removable microSD cards found.");
        return Ok(true);
    }
    for card in &cards {
        println!(
            "{}\t{}\t{}\t[{}]\t{}",
            card.option.id,
            card.option.name,
            format_bytes(card.option.capacity_bytes),
            card.option.slot.badge(),
            card.option.layout_summary
        );
    }
    Ok(true)
}

fn print_builds(catalog: &Catalog) {
    println!("Release: {}", catalog.release);
    for build in catalog_to_build_options(catalog) {
        match &build.unsupported_reason {
            None => println!(
                "{}  {}  [{}]  {}",
                build.id, build.label, build.product, build.size_summary
            ),
            Some(reason) => println!("{}  {}  [UNSUPPORTED: {reason}]", build.id, build.label),
        }
    }
}
