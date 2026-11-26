// SPDX-License-Identifier: GPL-3.0-or-later
use std::io::Cursor;
use std::process::ExitCode;

use switch_sign::{Detached, Embedded, Metadata, RUNTIME, Signed, embedded_entitlements, sign_detached, sign_macho};

const USAGE: &str = "usage: switch_sign_tool detached <identifier> <file> <signature>
       switch_sign_tool resign <executable> <out> [<entitlements.plist>]

resign keeps the identifier, flags and runtime version. Without an entitlements file it keeps the executable's own
entitlements, written again as plistlib writes them (as tools/install_policy/installd.py does).";

fn read(path: &str) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("{path}: {e}"))
}

fn own_entitlements(input: &[u8]) -> Result<Vec<u8>, String> {
    let xml = embedded_entitlements(input)
        .map_err(|e| e.to_string())?
        .ok_or("the executable has no entitlements")?;
    let value = plist::Value::from_reader(Cursor::new(xml)).map_err(|e| e.to_string())?;
    switch_plistlib::to_xml(&value).map_err(|e| e.to_string())
}

fn run(args: &[String]) -> Result<Signed, String> {
    let (signed, out) = match args {
        [mode, identifier, file, signature] if mode == "detached" => {
            let options = Detached {
                identifier,
                flags: RUNTIME,
                page_size: 4096,
            };
            (sign_detached(&read(file)?, &options), signature)
        }
        [mode, input, out, rest @ ..] if mode == "resign" && rest.len() <= 1 => {
            let executable = read(input)?;
            let entitlements = match rest.first() {
                Some(path) => read(path)?,
                None => own_entitlements(&executable)?,
            };
            let options = Embedded {
                metadata: Metadata::Preserve,
                entitlements: Some(&entitlements),
            };
            (sign_macho(&executable, &options), out)
        }
        _ => return Err(USAGE.into()),
    };
    let signed = signed.map_err(|e| e.to_string())?;
    std::fs::write(out, &signed.bytes).map_err(|e| format!("{out}: {e}"))?;
    Ok(signed)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(signed) => {
            println!(
                "{}",
                signed.cdhash.iter().map(|b| format!("{b:02x}")).collect::<String>()
            );
            ExitCode::SUCCESS
        }
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}
