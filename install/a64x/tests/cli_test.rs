// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_a64x_tool against the arm64e CLI: every call the iOS 27 prelink made (record.bzl), rerun with the same
//! arguments, must give the same stdout and the same files.
use std::path::{Path, PathBuf};
use std::process::Command;

fn env_path(name: &str) -> PathBuf {
    PathBuf::from(std::env::var(name).unwrap_or_else(|_| panic!("{name} is not set")))
}

fn read(path: &Path) -> Vec<u8> {
    std::fs::read(path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn run(tool: &Path, call: &Path, args: &[&str]) -> Vec<u8> {
    let out = Command::new(tool).args(args).output().unwrap();
    assert!(
        out.status.success(),
        "{}: {}",
        call.display(),
        String::from_utf8_lossy(&out.stderr)
    );
    out.stdout
}

fn same(call: &Path, what: &str, ours: &[u8], theirs: &[u8]) {
    let at = ours.iter().zip(theirs).position(|(a, b)| a != b);
    assert!(
        ours == theirs,
        "{}: {what} differs (ours {} bytes, the CLI's {}, first difference at {at:?})",
        call.display(),
        ours.len(),
        theirs.len()
    );
}

fn kext(tool: &Path, call: &Path, scratch: &Path) {
    let vas = String::from_utf8(read(&call.join("vas"))).unwrap();
    let [blob_va, stubs_va] = vas.lines().collect::<Vec<_>>()[..] else {
        panic!("{}: vas", call.display());
    };
    let (blob, stubs) = (scratch.join("out_blob"), scratch.join("out_stubs"));
    let p = |path: &Path| path.to_str().unwrap().to_owned();
    let stdout = run(
        tool,
        call,
        &[
            "kext",
            &p(&call.join("blob")),
            blob_va,
            &p(&call.join("jobs")),
            stubs_va,
            &p(&blob),
            &p(&stubs),
        ],
    );
    same(call, "stdout", &stdout, &read(&call.join("stdout")));
    same(call, "text", &read(&blob), &read(&call.join("out_blob")));
    same(call, "stubs", &read(&stubs), &read(&call.join("out_stubs")));
    println!(
        "{}: {} kexts, {} bytes of text, {} stub bytes",
        call.display(),
        String::from_utf8_lossy(&stdout)
            .lines()
            .filter(|l| l.starts_with("S "))
            .count(),
        read(&blob).len(),
        read(&stubs).len()
    );
}

fn fips(tool: &Path, call: &Path) {
    let mut headers: Vec<(usize, PathBuf)> = std::fs::read_dir(call)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter_map(|p| Some((p.file_name()?.to_str()?.strip_prefix("header.")?.parse().ok()?, p)))
        .collect();
    headers.sort();
    let mut args = vec!["fips".to_owned()];
    args.extend(headers.iter().map(|(_, p)| p.to_str().unwrap().to_owned()));
    let args: Vec<&str> = args.iter().map(String::as_str).collect();
    let stdout = run(tool, call, &args);
    same(call, "stdout", &stdout, &read(&call.join("stdout")));
    let found = String::from_utf8_lossy(&stdout).lines().filter(|l| *l != "-").count();
    println!(
        "{}: {} headers, {found} with the FIPS check",
        call.display(),
        headers.len()
    );
}

fn fips_mac(tool: &Path, call: &Path) {
    let mac = String::from_utf8(read(&call.join("mac"))).unwrap();
    let original = call.join("original");
    let translated = call.join("translated");
    let args = [
        "fips-mac",
        original.to_str().unwrap(),
        translated.to_str().unwrap(),
        mac.trim_end(),
    ];
    let stdout = run(tool, call, &args);
    same(call, "stdout", &stdout, &read(&call.join("stdout")));
    println!(
        "{}: MAC {} -> {}",
        call.display(),
        mac.trim_end(),
        String::from_utf8_lossy(&stdout).trim_end()
    );
}

#[test]
fn the_tool_gives_the_cli_s_bytes_for_every_prelink_call() {
    let tool = env_path("A64X_TOOL");
    let record = env_path("A64X_RECORD");
    let scratch = env_path("TEST_TMPDIR");
    let mut calls: Vec<PathBuf> = std::fs::read_dir(&record).unwrap().map(|e| e.unwrap().path()).collect();
    calls.sort();
    let mut counts = [0usize; 3];
    for call in &calls {
        let name = call.file_name().unwrap().to_str().unwrap();
        match name.split_once('-').map(|(_, command)| command) {
            Some("kext") => {
                kext(&tool, call, &scratch);
                counts[0] += 1;
            }
            Some("fips") => {
                fips(&tool, call);
                counts[1] += 1;
            }
            Some("fips-mac") => {
                fips_mac(&tool, call);
                counts[2] += 1;
            }
            _ => panic!("{name}: a call this test does not know"),
        }
    }
    println!("calls: kext {}, fips {}, fips-mac {}", counts[0], counts[1], counts[2]);
    assert!(
        counts[0] == 1 && counts[1] == 1 && counts[2] >= 1,
        "the prelink calls kext and fips once each, and fips-mac for corecrypto: {counts:?}"
    );
}
