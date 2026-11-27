// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::Path;
use std::process::ExitCode;

use switch_launchd::{Jobs, Pins, build};

const USAGE: &str = "usage: switch_launchd_tool <launchd.plist> <launchd.plist.sig> <catalog sha256> <signature sha256>
                           <bootstrap job> <bootstrap-apps job> <sdwake job> <out dir>

Writes launchd.plist, launchd.plist.sig, launchd.trustcache and launchd.sha256 into <out dir>, as
tools/package/launchd_catalog.py does, and prints the catalogue's CDHash.";

fn read(path: &str) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("{path}: {e}"))
}

fn run(args: &[String]) -> Result<String, String> {
    let [
        catalog,
        signature,
        catalog_sha256,
        signature_sha256,
        job,
        apps,
        sdwake,
        out,
    ] = args
    else {
        return Err(USAGE.into());
    };
    let jobs = Jobs::parse(&read(job)?, &read(apps)?, &read(sdwake)?).map_err(|e| e.to_string())?;
    let pins = Pins {
        catalog_sha256,
        signature_sha256,
    };
    let made = build(&read(catalog)?, &read(signature)?, &pins, &jobs).map_err(|e| e.to_string())?;
    let out = Path::new(out);
    std::fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    for (name, bytes) in [
        ("launchd.plist", &made.plist[..]),
        ("launchd.plist.sig", &made.signature[..]),
        ("launchd.trustcache", &made.trust[..]),
        ("launchd.sha256", &made.digest[..]),
    ] {
        let path = out.join(name);
        std::fs::write(&path, bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    }
    Ok(made.cdhash.iter().map(|b| format!("{b:02x}")).collect())
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
