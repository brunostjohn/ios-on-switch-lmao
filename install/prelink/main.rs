// SPDX-License-Identifier: GPL-3.0-or-later
use std::process::ExitCode;

use switch_prelink::{Inputs17, load_sites, prelink_17};

const USAGE: &str = "usage: switch_prelink_tool v17 <kernel.macho> <kernelcache> <prelink.json> <dma-prelink.json> \
<dma-kernel.json> <kind> <out> [<sites.json>...]";

fn read(path: &str) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("{path}: {e}"))
}

fn run(args: &[String]) -> Result<usize, String> {
    let [
        mode,
        kernel,
        kernelcache,
        options,
        previous,
        inherited,
        kind,
        out,
        tables @ ..,
    ] = args
    else {
        return Err(USAGE.into());
    };
    if mode != "v17" {
        return Err(USAGE.into());
    }
    let tables = tables.iter().map(|path| read(path)).collect::<Result<Vec<_>, _>>()?;
    let tables: Vec<&[u8]> = tables.iter().map(Vec::as_slice).collect();
    let sites = load_sites(&tables, kind).map_err(|e| e.to_string())?;
    let inputs =
        Inputs17::new(&read(options)?, &read(previous)?, &read(inherited)?, sites).map_err(|e| e.to_string())?;
    let prelinked = prelink_17(&read(kernel)?, &read(kernelcache)?, &inputs).map_err(|e| e.to_string())?;
    std::fs::write(out, &prelinked).map_err(|e| format!("{out}: {e}"))?;
    Ok(prelinked.len())
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(length) => {
            println!("{length} bytes");
            ExitCode::SUCCESS
        }
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}
