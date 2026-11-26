// SPDX-License-Identifier: GPL-3.0-or-later
use std::process::ExitCode;

const USAGE: &str = "usage: switch_installd_tool <installd> <SystemVersion.plist> <build> <out>

Writes the OS volume's installd patched and re-signed as the overlay carries it, and prints its CDHash.";

fn run(args: &[String]) -> Result<String, String> {
    let [installd, system_version, build, out] = args else {
        return Err(USAGE.into());
    };
    let read = |path: &str| std::fs::read(path).map_err(|e| format!("{path}: {e}"));
    let signed =
        switch_installd::prepare(&read(installd)?, &read(system_version)?, build).map_err(|e| e.to_string())?;
    std::fs::write(out, &signed.bytes).map_err(|e| format!("{out}: {e}"))?;
    Ok(signed.cdhash.iter().map(|b| format!("{b:02x}")).collect())
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(cdhash) => {
            println!("{cdhash}");
            ExitCode::SUCCESS
        }
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}
