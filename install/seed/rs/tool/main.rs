// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};
use switch_seed_crawl::{Crawler, format_inventory_json, format_names_json, index_names, resolve_corpus};
use switch_seed_link::{LinkConfig, link_buckets, stamp_pack_file};
use switch_seed_plan::{
    DEFAULT_BUCKET_BYTES, Depth, IDENTITY_BYTES, PlanConfig, check_tools_match_bundle, layout_seed_tree,
    pack_native_seed, parse_seedpack_list, restamp_buckets, run_plan, write_seed_pack,
};

#[derive(Parser)]
#[command(name = "switch_seed_tool")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    Crawl {
        #[arg(long = "image", required = true)]
        images: Vec<PathBuf>,
        #[arg(long)]
        out: PathBuf,
        #[arg(long)]
        inventory: PathBuf,
    },
    Names {
        #[arg(long)]
        build: String,
        root: PathBuf,
        out: PathBuf,
        devices: Vec<String>,
    },
    Resolve {
        names_json: PathBuf,
        corpus: PathBuf,
        out: PathBuf,
    },
    Plan {
        #[arg(long)]
        corpus: PathBuf,
        #[arg(long)]
        root: PathBuf,
        #[arg(long = "data")]
        data_files: Vec<PathBuf>,
        #[arg(long = "data-dir")]
        data_dirs: Vec<PathBuf>,
        #[arg(long)]
        qc: PathBuf,
        #[arg(long = "qc-sha256")]
        qc_sha256: String,
        #[arg(long)]
        depth: String,
        #[arg(long)]
        buckets: PathBuf,
        #[arg(long)]
        count: usize,
        #[arg(long)]
        store: PathBuf,
        #[arg(long)]
        report: PathBuf,
        #[arg(long = "input-report")]
        input_reports: Vec<PathBuf>,
        #[arg(long, default_value_t = 0)]
        apps: usize,
        #[arg(long = "base-root")]
        base_root: Option<PathBuf>,
        #[arg(long = "base-data-dir")]
        base_data_dirs: Vec<PathBuf>,
        #[arg(long = "base-store")]
        base_store: Option<PathBuf>,
    },
    Seedpack {
        directory: PathBuf,
        name_bytes: u32,
        identity_hex: String,
        list: PathBuf,
    },
    Restamp {
        stack: String,
        out: PathBuf,
        #[arg(required = true)]
        buckets: Vec<PathBuf>,
    },
    PackNative {
        #[arg(long)]
        seed: PathBuf,
        #[arg(long)]
        out: PathBuf,
    },
    MergeCheck {
        #[arg(long = "tools-manifest")]
        tools_manifest: PathBuf,
        #[arg(long = "base", required = true)]
        base: Vec<PathBuf>,
        #[arg(long)]
        out: PathBuf,
    },
    MergeTree {
        #[arg(long)]
        seed: Option<PathBuf>,
        #[arg(long)]
        raw: Option<PathBuf>,
        #[arg(long)]
        native: Option<PathBuf>,
        #[arg(long)]
        out: PathBuf,
        #[arg(long)]
        size: PathBuf,
    },
    Link {
        #[arg(long)]
        out: PathBuf,
        #[arg(long = "context-log")]
        context_log: Option<PathBuf>,
        #[arg(long = "draw-context")]
        draw_context: Option<PathBuf>,
        #[arg(long)]
        archives: Option<PathBuf>,
        #[arg(long = "corpus-report")]
        corpus_report: Option<PathBuf>,
        #[arg(long)]
        stack: Option<String>,
        #[arg(long = "debug-outputs", default_value_t = false)]
        debug_outputs: bool,
        #[arg(required = true)]
        buckets: Vec<PathBuf>,
    },
    Stamp {
        pack: PathBuf,
        #[arg(long)]
        stack: String,
    },
}

fn parse_stack(hex_str: &str) -> Result<u64, String> {
    u64::from_str_radix(hex_str.trim(), 16).map_err(|e| format!("invalid stack hex {hex_str:?}: {e}"))
}

fn run(cli: Cli) -> Result<(), String> {
    match cli.command {
        Command::Crawl { images, out, inventory } => {
            let mut crawler = Crawler::new(&out).map_err(|e| e.to_string())?;
            for img in &images {
                crawler.crawl_path(img).map_err(|e| e.to_string())?;
            }
            let inv = crawler.inventory();
            let json = format_inventory_json(&inv).map_err(|e| e.to_string())?;
            if let Some(parent) = inventory.parent() {
                fs::create_dir_all(parent).map_err(|e| e.to_string())?;
            }
            fs::write(&inventory, json).map_err(|e| e.to_string())?;
            let msl_total: usize = inv.msl_sources.values().sum();
            println!(
                "crawl: {} library files, {} embedded ({} in CPU binaries), {} unreadable, {msl_total} Metal sources",
                inv.files,
                inv.embedded,
                inv.embedded_seeded,
                inv.unreadable.len()
            );
        }
        Command::Names {
            build,
            root,
            out,
            devices,
        } => {
            let dev_opt = (!devices.is_empty()).then_some(devices.as_slice());
            let table = index_names(&build, &root, dev_opt).map_err(|e| e.to_string())?;
            let json = format_names_json(&table).map_err(|e| e.to_string())?;
            if let Some(parent) = out.parent() {
                fs::create_dir_all(parent).map_err(|e| e.to_string())?;
            }
            fs::write(&out, json).map_err(|e| e.to_string())?;
            println!("{}: {} function names", out.display(), table.len());
        }
        Command::Resolve {
            names_json,
            corpus,
            out,
        } => {
            let table: BTreeMap<String, Vec<String>> =
                serde_json::from_str(&fs::read_to_string(&names_json).map_err(|e| e.to_string())?)
                    .map_err(|e| e.to_string())?;
            let corpus_text = fs::read_to_string(&corpus).map_err(|e| e.to_string())?;
            let res = resolve_corpus(&table, &corpus_text);
            if let Some(parent) = out.parent() {
                fs::create_dir_all(parent).map_err(|e| e.to_string())?;
            }
            fs::write(&out, res.text).map_err(|e| e.to_string())?;
        }
        Command::Plan {
            corpus,
            root,
            data_files,
            data_dirs,
            qc,
            qc_sha256,
            depth,
            buckets,
            count,
            store,
            report,
            input_reports,
            apps,
            base_root,
            base_data_dirs,
            base_store,
        } => {
            let cfg = PlanConfig {
                build: None,
                corpus,
                root,
                data_files,
                data_dirs,
                qc,
                qc_sha256,
                depth: Depth::parse(&depth).map_err(|e| e.to_string())?,
                buckets_dir: buckets,
                count,
                store,
                report,
                input_reports,
                apps,
                base_root,
                base_data_dirs,
                base_store,
            };
            let rep = run_plan(&cfg).map_err(|e| e.to_string())?;
            println!(
                "plan: {}",
                serde_json::to_string(&rep.lines).map_err(|e| e.to_string())?
            );
        }
        Command::Seedpack {
            directory,
            name_bytes,
            identity_hex,
            list,
        } => {
            let id_vec = hex::decode(&identity_hex).map_err(|e| format!("bad identity hex: {e}"))?;
            let identity: [u8; IDENTITY_BYTES] = id_vec
                .try_into()
                .map_err(|_| format!("identity must be {} hex chars", IDENTITY_BYTES * 2))?;
            let text = fs::read_to_string(&list).map_err(|e| e.to_string())?;
            let entries = parse_seedpack_list(&text).map_err(|e| e.to_string())?;
            write_seed_pack(&directory, name_bytes, &identity, entries, DEFAULT_BUCKET_BYTES)
                .map_err(|e| e.to_string())?;
        }
        Command::Restamp { stack, out, buckets } => {
            restamp_buckets(&stack, &out, &buckets).map_err(|e| e.to_string())?;
        }
        Command::PackNative { seed, out } => {
            pack_native_seed(&seed, &out).map_err(|e| e.to_string())?;
        }
        Command::MergeCheck {
            tools_manifest,
            base,
            out,
        } => {
            let msg = check_tools_match_bundle(&tools_manifest, &base).map_err(|e| e.to_string())?;
            fs::write(&out, msg).map_err(|e| e.to_string())?;
        }
        Command::MergeTree {
            seed,
            raw,
            native,
            out,
            size,
        } => {
            let stats = layout_seed_tree(seed.as_deref(), raw.as_deref(), native.as_deref(), &out)
                .map_err(|e| e.to_string())?;
            let json = serde_json::to_string(&stats).map_err(|e| e.to_string())? + "\n";
            fs::write(&size, json).map_err(|e| e.to_string())?;
        }
        Command::Link {
            out,
            context_log,
            draw_context,
            archives,
            corpus_report,
            stack,
            debug_outputs,
            buckets,
        } => {
            let stack_u64 = stack.as_deref().map(parse_stack).transpose()?;
            let cfg = LinkConfig {
                buckets,
                out_dir: out,
                context_log,
                draw_context,
                archives,
                corpus_report,
                stack: stack_u64,
                write_debug_outputs: debug_outputs,
            };
            let result = link_buckets(&cfg).map_err(|e| e.to_string())?;
            print!("{}", result.report);
        }
        Command::Stamp { pack, stack } => {
            let stack_u64 = parse_stack(&stack)?;
            stamp_pack_file(&pack, stack_u64).map_err(|e| e.to_string())?;
            println!("raw pack: stack {stack}");
        }
    }
    Ok(())
}

fn main() -> ExitCode {
    match run(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("switch_seed_tool: {err}");
            ExitCode::FAILURE
        }
    }
}
