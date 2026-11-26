// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_window_tool: the trust bundle, kernel window and bl33.bin from files, timing each step on stderr.

use std::path::PathBuf;
use std::time::Instant;

use clap::Parser;
use switch_window::{Boot, Bundle, Inputs, Kind, bl33, compress, join, layout, trust_bundle};

#[derive(Parser)]
struct Args {
    #[arg(long)]
    kernel: PathBuf,
    /// The finished ADT, its boot manifest hash slot (iOS 27) still zero.
    #[arg(long)]
    adt: PathBuf,
    #[arg(long)]
    static_trust: PathBuf,
    /// Every other module the bundle merges, the overlay's without installd.
    #[arg(long)]
    module: Vec<PathBuf>,
    /// installd's CDHash, in hex.
    #[arg(long)]
    installd_cdhash: String,
    #[arg(long)]
    data_seed: PathBuf,
    /// A file holding the built-in boot-args line.
    #[arg(long)]
    boot_args: PathBuf,
    #[arg(long)]
    kind: String,
    #[arg(long, value_parser = parse_hex)]
    virt_base: u64,
    #[arg(long)]
    root_volume_uuid: String,
    #[arg(long)]
    boot_manifest_hash: bool,
    #[arg(long)]
    u_boot: PathBuf,
    #[arg(long)]
    script: PathBuf,
    #[arg(long)]
    loader: PathBuf,
    #[arg(long)]
    exporter: PathBuf,
    #[arg(long)]
    dtb: PathBuf,
    #[arg(long)]
    loader_constants: PathBuf,
    #[arg(long)]
    out: PathBuf,
    #[arg(long)]
    constants: Option<PathBuf>,
}

fn parse_hex(value: &str) -> Result<u64, std::num::ParseIntError> {
    u64::from_str_radix(value.trim_start_matches("0x"), 16)
}

fn read(path: &PathBuf) -> Vec<u8> {
    std::fs::read(path).unwrap_or_else(|e| fail(format!("{}: {e}", path.display())))
}

fn fail(message: impl std::fmt::Display) -> ! {
    eprintln!("switch_window_tool: {message}");
    std::process::exit(1)
}

fn main() {
    let args = Args::parse();
    let installd = hex::decode(&args.installd_cdhash)
        .ok()
        .and_then(|bytes| bytes.try_into().ok())
        .unwrap_or_else(|| fail("--installd-cdhash is not 20 bytes of hex"));
    let modules: Vec<Vec<u8>> = args.module.iter().map(read).collect();
    let modules: Vec<&[u8]> = modules.iter().map(Vec::as_slice).collect();
    let static_module = read(&args.static_trust);
    let (kernel, adt, seed) = (read(&args.kernel), read(&args.adt), read(&args.data_seed));
    let boot_args = String::from_utf8(read(&args.boot_args)).unwrap_or_else(|e| fail(e));
    let boot_files = [
        &args.u_boot,
        &args.script,
        &args.loader,
        &args.exporter,
        &args.dtb,
        &args.loader_constants,
    ]
    .map(read);

    let total = Instant::now();
    let mut step = Instant::now();
    let mut lap = |name: &str| {
        eprintln!("{name}: {:.3} s", step.elapsed().as_secs_f64());
        step = Instant::now();
    };
    let trust = trust_bundle(&Bundle {
        static_module: &static_module,
        modules: &modules,
        installd,
    })
    .unwrap_or_else(|e| fail(e));
    lap("trust bundle");
    let inputs = Inputs {
        kernel: &kernel,
        adt: &adt,
        trust_bundle: &trust,
        data_seed: &seed,
        command_line: boot_args.trim_end_matches('\n'),
        kind: Kind {
            name: &args.kind,
            virt_base: args.virt_base,
            root_volume_uuid: &args.root_volume_uuid,
            boot_manifest_hash: args.boot_manifest_hash,
        },
    };
    let laid = layout(&inputs).unwrap_or_else(|e| fail(e));
    lap("layout and checks");
    let compressed = compress(&laid).unwrap_or_else(|e| fail(e));
    lap(&format!(
        "LZ4, {} blocks on {} threads",
        laid.blocks.len(),
        rayon::current_num_threads()
    ));
    let window = join(&laid, &compressed).unwrap_or_else(|e| fail(e));
    lap("join");
    let [u_boot, script, loader, exporter, dtb, loader_constants] = &boot_files;
    let boot = Boot {
        u_boot,
        script,
        loader,
        exporter,
        dtb,
        loader_constants,
    };
    let image = bl33(&window, &boot).unwrap_or_else(|e| fail(e));
    lap("bl33.bin");
    eprintln!("total: {:.3} s", total.elapsed().as_secs_f64());
    std::fs::write(&args.out, image).unwrap_or_else(|e| fail(e));
    if let Some(path) = &args.constants {
        std::fs::write(path, window.constants.json()).unwrap_or_else(|e| fail(e));
    }
}
