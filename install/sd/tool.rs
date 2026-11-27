// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_sd_tool <card image> [<boot package dir>]
//!
//! Prints what switch_card reads on the image (the roots, the install targets, the boot volume) and, given a boot
//! package (switchroot/ios/, bootloader/ini/ios.ini, bootloader/res/icon_ios_hue.bmp), puts it on the boot volume.

use std::fs::OpenOptions;
use std::path::Path;
use std::process::ExitCode;

use switch_card::{Card, StreamDevice};
use switch_sd::{BOOT_DIR, BootFile, ICON, INI, install};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (image, package) = match args.as_slice() {
        [image] => (image, None),
        [image, package] => (image, Some(Path::new(package))),
        _ => {
            eprintln!("usage: switch_sd_tool <card image> [<boot package dir>]");
            return ExitCode::from(2);
        }
    };
    match run(image, package) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("switch_sd_tool: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run(image: &str, package: Option<&Path>) -> Result<(), Box<dyn std::error::Error>> {
    let file = OpenOptions::new().read(true).write(package.is_some()).open(image)?;
    let mut dev = StreamDevice::file(file, 512)?;
    let card = Card::open(&mut dev)?;
    println!(
        "roots: {:?}{}",
        card.roots(),
        if card.torn() { " (GPT copies mid-write)" } else { "" }
    );
    for t in card.targets() {
        let e = card.entries()[t.index as usize];
        println!(
            "entry {}: {:?} {:?}, {} sectors from {}",
            t.index,
            e.name(),
            t.role,
            t.sectors,
            t.first
        );
    }
    let (first, sectors) = card.boot_volume()?;
    println!("boot volume: {sectors} sectors from {first}");
    let Some(package) = package else { return Ok(()) };
    let mut files: Vec<(String, Vec<u8>)> = Vec::new();
    for entry in std::fs::read_dir(package.join(BOOT_DIR))? {
        let entry = entry?;
        if entry.file_type()?.is_file() {
            let name = entry
                .file_name()
                .into_string()
                .map_err(|n| format!("{n:?}: not UTF-8"))?;
            files.push((format!("{BOOT_DIR}/{name}"), std::fs::read(entry.path())?));
        }
    }
    for path in [INI, ICON] {
        if package.join(path).is_file() {
            files.push((path.to_string(), std::fs::read(package.join(path))?));
        }
    }
    let boot: Vec<BootFile> = files.iter().map(|(path, data)| BootFile { path, data }).collect();
    let summary = install(&mut dev, first, sectors, &boot)?;
    println!(
        "written {:?}\nunchanged {:?}\nremoved {:?}",
        summary.written, summary.unchanged, summary.removed
    );
    Ok(())
}
