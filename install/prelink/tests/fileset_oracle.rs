// SPDX-License-Identifier: GPL-3.0-or-later
//! The fileset prelink against `//kernel:kernel_12377`, from the inputs a user's computer has: the unprelinked
//! kernel, the decompressed kernelcache, the bindings export, the pins and the kind's site tables. Then the inputs
//! it must refuse.

use sha2::{Digest, Sha256};
use switch_prelink::{Error, FilesetInputs, prelink_fileset};

fn var(name: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| panic!("{name} is not set"))
}

fn read(name: &str) -> Vec<u8> {
    let path = var(name);
    std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

fn tables() -> Vec<Vec<u8>> {
    var("SWITCH_PRELINK_SITES")
        .split(' ')
        .filter(|p| !p.is_empty())
        .map(|p| std::fs::read(p).unwrap_or_else(|e| panic!("{p}: {e}")))
        .collect()
}

fn inputs(bindings: &[u8], pins: &[u8], tables: &[Vec<u8>]) -> Result<FilesetInputs, Error> {
    let tables: Vec<&[u8]> = tables.iter().map(Vec::as_slice).collect();
    FilesetInputs::new(bindings, pins, &tables)
}

fn real_inputs() -> FilesetInputs {
    inputs(
        &read("SWITCH_PRELINK_BINDINGS"),
        &read("SWITCH_PRELINK_PINS"),
        &tables(),
    )
    .unwrap()
}

fn sha256(bytes: &[u8]) -> String {
    hex::encode(Sha256::digest(bytes))
}

/// `text` with the first digit after `key` changed.
fn change_digit_after(text: &[u8], key: &str) -> Vec<u8> {
    let mut text = text.to_vec();
    let start = text.windows(key.len()).position(|w| w == key.as_bytes()).expect(key) + key.len();
    let digit = start + text[start..].iter().position(u8::is_ascii_hexdigit).unwrap();
    text[digit] = if text[digit] == b'0' { b'1' } else { b'0' };
    text
}

fn refusal<T>(result: Result<T, Error>) -> String {
    match result {
        Err(Error::Refused(message)) => message,
        Err(other) => panic!("expected a refusal, got {other}"),
        Ok(_) => panic!("expected a refusal, got a result"),
    }
}

#[test]
fn prelink_fileset_equals_bazel() {
    let kernelcache = read("SWITCH_PRELINK_KERNELCACHE");
    let prelinked = prelink_fileset(&read("SWITCH_PRELINK_KERNEL"), &kernelcache, &real_inputs()).unwrap();
    let expected = read("SWITCH_PRELINK_EXPECTED");
    println!(
        "switch_prelink: {} bytes, sha256 {}",
        prelinked.len(),
        sha256(&prelinked)
    );
    println!("kernel_12377:   {} bytes, sha256 {}", expected.len(), sha256(&expected));
    assert!(prelinked == expected, "the prelink differs from //kernel:kernel_12377");
}

#[test]
fn another_kernelcache_is_refused() {
    let mut kernelcache = read("SWITCH_PRELINK_KERNELCACHE");
    kernelcache[0x4000] ^= 1;
    let message = refusal(prelink_fileset(
        &read("SWITCH_PRELINK_KERNEL"),
        &kernelcache,
        &real_inputs(),
    ));
    assert!(message.contains("made for the kernelcache_sha256"), "{message}");
}

#[test]
fn another_kernel_is_refused() {
    let kernel = read("SWITCH_PRELINK_EXPECTED");
    let message = refusal(prelink_fileset(
        &kernel,
        &read("SWITCH_PRELINK_KERNELCACHE"),
        &real_inputs(),
    ));
    assert!(message.contains("made for the kernel_sha256"), "{message}");
}

#[test]
fn another_export_is_refused() {
    let (bindings, pins) = (read("SWITCH_PRELINK_BINDINGS"), read("SWITCH_PRELINK_PINS"));
    let wrong_kernel = inputs(&change_digit_after(&bindings, "\"kernel_sha256\""), &pins, &tables()).unwrap();
    let message = refusal(prelink_fileset(
        &read("SWITCH_PRELINK_KERNEL"),
        &read("SWITCH_PRELINK_KERNELCACHE"),
        &wrong_kernel,
    ));
    assert!(message.contains("made for the kernel_sha256"), "{message}");
    let format_2 = String::from_utf8(bindings)
        .unwrap()
        .replacen("\"format\": 1", "\"format\": 2", 1);
    let message = refusal(inputs(format_2.as_bytes(), &pins, &tables()));
    assert!(message.contains("format 2, this prelink reads format 1"), "{message}");
}

#[test]
fn unpinned_site_tables_are_refused() {
    let (bindings, pins) = (read("SWITCH_PRELINK_BINDINGS"), read("SWITCH_PRELINK_PINS"));
    let mut changed = tables();
    changed[0].push(b'\n');
    let message = refusal(inputs(&bindings, &pins, &changed));
    assert!(message.contains("the release pins"), "{message}");
    let message = refusal(inputs(&bindings, &pins, &tables()[1..]));
    assert!(message.contains("not given"), "{message}");
}

#[test]
fn a_kernel_other_than_the_pinned_one_is_refused() {
    let pins = change_digit_after(&read("SWITCH_PRELINK_PINS"), "\"prelinked_sha256\"");
    let inputs = inputs(&read("SWITCH_PRELINK_BINDINGS"), &pins, &tables()).unwrap();
    let message = refusal(prelink_fileset(
        &read("SWITCH_PRELINK_KERNEL"),
        &read("SWITCH_PRELINK_KERNELCACHE"),
        &inputs,
    ));
    assert!(message.contains("the release's is"), "{message}");
}
