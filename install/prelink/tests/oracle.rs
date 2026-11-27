// SPDX-License-Identifier: GPL-3.0-or-later
//! The selected kind's prelink against Bazel's, from the same inputs: `//kernel:kernel` (the prelink's flags) and
//! the release path's (`release_prelink`, its prelink.json).

use sha2::{Digest, Sha256};
use switch_prelink::{Error, Inputs17, load_sites, prelink_17};

fn var(name: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| panic!("{name} is not set"))
}

fn paths(name: &str) -> Vec<String> {
    var(name)
        .split(' ')
        .filter(|p| !p.is_empty())
        .map(str::to_owned)
        .collect()
}

fn read(path: &str) -> Vec<u8> {
    std::fs::read(path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

fn input(name: &str) -> Vec<u8> {
    let found: Vec<String> = paths("SWITCH_PRELINK_INPUTS")
        .into_iter()
        .filter(|p| p.ends_with(name))
        .collect();
    let [path] = &found[..] else {
        panic!("SWITCH_PRELINK_INPUTS holds {} files named {name}", found.len());
    };
    read(path)
}

fn inputs() -> Inputs17 {
    let tables: Vec<Vec<u8>> = paths("SWITCH_PRELINK_SITES").iter().map(|p| read(p)).collect();
    let tables: Vec<&[u8]> = tables.iter().map(Vec::as_slice).collect();
    let sites = load_sites(&tables, &var("SWITCH_PRELINK_KIND")).unwrap();
    let inherited = read(&var("SWITCH_PRELINK_INHERITED"));
    Inputs17::new(
        &input("prelink.json"),
        &read(&var("SWITCH_PRELINK_PREVIOUS")),
        &inherited,
        sites,
    )
    .unwrap()
}

#[test]
fn prelink_17_equals_bazel() {
    let inputs = inputs();
    let kernelcache = read(&var("SWITCH_PRELINK_KERNELCACHE"));
    let prelinked = prelink_17(&input("kernel.macho"), &kernelcache, &inputs).unwrap();
    println!(
        "switch_prelink: {} bytes, sha256 {}",
        prelinked.len(),
        hex::encode(Sha256::digest(&prelinked))
    );
    for path in paths("SWITCH_PRELINK_EXPECTED") {
        let expected = read(&path);
        println!(
            "{path}: {} bytes, sha256 {}",
            expected.len(),
            hex::encode(Sha256::digest(&expected))
        );
        if prelinked != expected {
            let first = prelinked.iter().zip(&expected).position(|(a, b)| a != b);
            panic!(
                "{path}: differs at {first:?} (lengths {} and {})",
                prelinked.len(),
                expected.len()
            );
        }
    }
}

#[test]
fn another_kernelcache_is_refused() {
    let kernel = input("kernel.macho");
    match prelink_17(&kernel, &kernel, &inputs()) {
        Err(Error::Refused(message)) => assert!(message.contains("unchanged V20"), "{message}"),
        other => panic!("expected a refusal, got {:?}", other.map(|o| o.len())),
    }
}
