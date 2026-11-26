// SPDX-License-Identifier: GPL-3.0-or-later
use std::process::ExitCode;

use switch_prelink::{FilesetInputs, Inputs17, fileset_pins, load_sites, prelink_17, prelink_fileset};

const USAGE: &str = "usage: switch_prelink_tool v17 <kernel.macho> <kernelcache> <prelink.json> <dma-prelink.json> \
<dma-kernel.json> <kind> <out> [<sites.json>...]
       switch_prelink_tool fileset <kernel.macho> <kernelcache> <prelink-bindings.json> <pins.json> <out> \
[<sites.json>...]
       switch_prelink_tool pins <build> <prelinked kernel.macho> <out> [<sites.json>...]";

fn read(path: &str) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("{path}: {e}"))
}

fn read_all(paths: &[String]) -> Result<Vec<Vec<u8>>, String> {
    paths.iter().map(|path| read(path)).collect()
}

fn write(path: &str, bytes: &[u8]) -> Result<usize, String> {
    std::fs::write(path, bytes).map_err(|e| format!("{path}: {e}"))?;
    Ok(bytes.len())
}

fn v17(args: &[String]) -> Result<usize, String> {
    let [
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
    let tables = read_all(tables)?;
    let tables: Vec<&[u8]> = tables.iter().map(Vec::as_slice).collect();
    let sites = load_sites(&tables, kind).map_err(|e| e.to_string())?;
    let inputs =
        Inputs17::new(&read(options)?, &read(previous)?, &read(inherited)?, sites).map_err(|e| e.to_string())?;
    let prelinked = prelink_17(&read(kernel)?, &read(kernelcache)?, &inputs).map_err(|e| e.to_string())?;
    write(out, &prelinked)
}

fn fileset(args: &[String]) -> Result<usize, String> {
    let [kernel, kernelcache, bindings, pins, out, tables @ ..] = args else {
        return Err(USAGE.into());
    };
    let tables = read_all(tables)?;
    let tables: Vec<&[u8]> = tables.iter().map(Vec::as_slice).collect();
    let inputs = FilesetInputs::new(&read(bindings)?, &read(pins)?, &tables).map_err(|e| e.to_string())?;
    let prelinked = prelink_fileset(&read(kernel)?, &read(kernelcache)?, &inputs).map_err(|e| e.to_string())?;
    write(out, &prelinked)
}

fn pins(args: &[String]) -> Result<usize, String> {
    let [build, prelinked, out, tables @ ..] = args else {
        return Err(USAGE.into());
    };
    let tables = read_all(tables)?;
    let tables: Vec<&[u8]> = tables.iter().map(Vec::as_slice).collect();
    let pins = fileset_pins(build, &read(prelinked)?, &tables).map_err(|e| e.to_string())?;
    write(out, &pins)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.split_first() {
        Some((mode, rest)) if mode == "v17" => v17(rest),
        Some((mode, rest)) if mode == "fileset" => fileset(rest),
        Some((mode, rest)) if mode == "pins" => pins(rest),
        _ => Err(USAGE.into()),
    };
    match result {
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
