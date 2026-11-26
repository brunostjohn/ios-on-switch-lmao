// SPDX-License-Identifier: GPL-3.0-or-later
//! `arm64e kext`, `fips` and `fips-mac` (translate/arm64e/tools) over switch_a64x: the same arguments, files and
//! output, so either can stand in for the other.
use std::fmt::Write as _;
use std::process::ExitCode;

use switch_a64x::{CLASSES, Kext, class_name, fips, translate_kexts};

const USAGE: &str = "usage: switch_a64x_tool kext BLOB BLOB_VA JOBS STUBS_VA OUT_BLOB OUT_STUBS
       switch_a64x_tool fips HEADER...
       switch_a64x_tool fips-mac ORIGINAL TRANSLATED MAC";

fn read(path: &str) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("{path}: {e}"))
}

fn write(path: &str, bytes: &[u8]) -> Result<(), String> {
    std::fs::write(path, bytes).map_err(|e| format!("{path}: {e}"))
}

/// strtoull with base 0.
fn number(s: &str) -> Result<u64, String> {
    let parsed = match s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        Some(hex) => u64::from_str_radix(hex, 16),
        None if s.len() > 1 && s.starts_with('0') => u64::from_str_radix(&s[1..], 8),
        None => s.parse(),
    };
    parsed.map_err(|e| format!("{s}: {e}"))
}

fn hex(s: &str) -> Result<u64, String> {
    u64::from_str_radix(s.trim_start_matches("0x"), 16).map_err(|e| format!("{s}: {e}"))
}

/// C's `%#llx`: no prefix for zero.
fn alt_hex(v: u64) -> String {
    if v == 0 { "0".into() } else { format!("{v:#x}") }
}

struct Job {
    id: String,
    sections: Vec<[u64; 2]>,
    starts: Vec<u64>,
}

fn current<'a>(job: &'a mut Option<Job>, line: &str) -> Result<&'a mut Job, String> {
    job.as_mut().ok_or_else(|| format!("{line}: before any K line"))
}

fn jobs(text: &str) -> Result<Vec<Job>, String> {
    let mut done = Vec::new();
    let mut job = None::<Job>;
    for line in text.lines() {
        let mut fields = line.get(2..).unwrap_or("").split_whitespace();
        match line.as_bytes().first() {
            Some(b'K') => {
                job = Some(Job {
                    id: fields.next().unwrap_or("").chars().take(159).collect(),
                    sections: Vec::new(),
                    starts: Vec::new(),
                })
            }
            Some(b'R') => {
                let range = [hex(fields.next().unwrap_or(""))?, hex(fields.next().unwrap_or(""))?];
                current(&mut job, line)?.sections.push(range);
            }
            Some(b'F') => current(&mut job, line)?.starts.push(hex(fields.next().unwrap_or(""))?),
            Some(b'E') => done.push(job.take().ok_or_else(|| "E before any K line".to_string())?),
            _ => {}
        }
    }
    Ok(done)
}

fn kext(args: &[String]) -> Result<String, String> {
    let [blob, blob_va, jobs_path, stubs_va, out_blob, out_stubs] = args else {
        return Err(USAGE.into());
    };
    let original = read(blob)?;
    let jobs = jobs(&String::from_utf8_lossy(&read(jobs_path)?))?;
    let kexts: Vec<Kext> = jobs
        .iter()
        .map(|j| Kext {
            sections: &j.sections,
            starts: &j.starts,
        })
        .collect();
    let out = translate_kexts(&original, number(blob_va)?, number(stubs_va)?, &kexts).map_err(|e| e.to_string())?;
    let mut stdout = String::new();
    for (job, r) in jobs.iter().zip(&out.kexts) {
        for island in &r.islands {
            writeln!(stdout, "I {:x} {}", island.va, island.words).unwrap();
        }
        let (id, words, changed, first, end) = (&job.id, r.words, r.changed, r.stub_first, r.stub_end);
        write!(
            stdout,
            "S {id} {words} {changed} {first} {end} islands={}",
            r.island_words
        )
        .unwrap();
        for c in 1..CLASSES {
            let (rw, stub, left) = (r.stats.rewrite1[c], r.stats.stubs[c], r.stats.left[c]);
            if rw != 0 || stub != 0 || left != 0 {
                write!(stdout, " {}={rw}/{stub}/{left}", class_name(c)).unwrap();
            }
        }
        stdout.push('\n');
    }
    write(out_blob, &out.text)?;
    write(out_stubs, &out.stubs)?;
    Ok(stdout)
}

fn fips_find(headers: &[String]) -> Result<String, String> {
    if headers.is_empty() {
        return Err(USAGE.into());
    }
    let mut stdout = String::new();
    for path in headers {
        match fips::find(&read(path)?) {
            Some(f) => writeln!(
                stdout,
                "{} {} {}",
                alt_hex(f.text_vmaddr),
                alt_hex(f.text_size),
                alt_hex(f.mac_vmaddr)
            )
            .unwrap(),
            None => stdout.push_str("-\n"),
        }
    }
    Ok(stdout)
}

fn fips_mac(args: &[String]) -> Result<String, String> {
    let [original, translated, was] = args else {
        return Err(USAGE.into());
    };
    let (original, translated) = (read(original)?, read(translated)?);
    let mut mac = [0u8; fips::MAC_LEN];
    if original.len() != translated.len() || was.len() != 2 * mac.len() || !was.is_ascii() {
        return Err(USAGE.into());
    }
    for (i, byte) in mac.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&was[2 * i..2 * i + 2], 16).map_err(|_| USAGE.to_string())?;
    }
    fips::rewrite(&original, &translated, &mut mac);
    let mut stdout: String = mac.iter().map(|b| format!("{b:02x}")).collect();
    stdout.push('\n');
    Ok(stdout)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.split_first() {
        Some((command, rest)) if command == "kext" => kext(rest),
        Some((command, rest)) if command == "fips" => fips_find(rest),
        Some((command, rest)) if command == "fips-mac" => fips_mac(rest),
        _ => Err(USAGE.into()),
    };
    match result {
        Ok(stdout) => {
            print!("{stdout}");
            ExitCode::SUCCESS
        }
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}
