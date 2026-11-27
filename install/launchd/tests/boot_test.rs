// SPDX-License-Identifier: GPL-3.0-or-later
//! The selected IPSW's launchd catalogue made here from the build's own inputs, against `//boot:package`'s
//! `launchd_catalog` output group: all four files must be the same bytes. A catalogue or signature off its pin is
//! refused.

use std::path::Path;

use sha2::{Digest, Sha256};
use switch_launchd::{Error, Jobs, Pins, build};

fn var(name: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| panic!("{name} is set by the BUILD file"))
}

fn read(path: &str) -> Vec<u8> {
    std::fs::read(path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

struct Inputs {
    catalog: Vec<u8>,
    signature: Vec<u8>,
    jobs: Jobs,
    catalog_sha256: String,
    signature_sha256: String,
}

impl Inputs {
    fn load() -> Inputs {
        let jobs = var("SWITCH_LAUNCHD_JOBS");
        let jobs: Vec<Vec<u8>> = jobs.split(' ').map(read).collect();
        Inputs {
            catalog: read(&var("SWITCH_LAUNCHD_CATALOG")),
            signature: read(&var("SWITCH_LAUNCHD_SIGNATURE")),
            jobs: Jobs::parse(&jobs[0], &jobs[1], &jobs[2]).unwrap(),
            catalog_sha256: var("SWITCH_LAUNCHD_CATALOG_SHA256"),
            signature_sha256: var("SWITCH_LAUNCHD_SIGNATURE_SHA256"),
        }
    }

    fn pins(&self) -> Pins<'_> {
        Pins {
            catalog_sha256: &self.catalog_sha256,
            signature_sha256: &self.signature_sha256,
        }
    }
}

fn sha256(data: &[u8]) -> String {
    Sha256::digest(data).iter().map(|b| format!("{b:02x}")).collect()
}

#[test]
fn matches_the_boot_package() {
    let inputs = Inputs::load();
    let made = build(&inputs.catalog, &inputs.signature, &inputs.pins(), &inputs.jobs).unwrap();
    let expected = var("SWITCH_LAUNCHD_EXPECTED");
    let expected: Vec<&str> = expected.split(' ').collect();
    assert_eq!(expected.len(), 4, "the launchd_catalog output group: {expected:?}");
    for (name, ours) in [
        ("launchd.plist", &made.plist[..]),
        ("launchd.plist.sig", &made.signature[..]),
        ("launchd.trustcache", &made.trust[..]),
        ("launchd.sha256", &made.digest[..]),
    ] {
        let path = expected
            .iter()
            .find(|path| Path::new(path).file_name().is_some_and(|file| file == name))
            .unwrap_or_else(|| panic!("{name} is not in the launchd_catalog output group"));
        let theirs = read(path);
        println!(
            "{name}: build {} ours {} ({} B)",
            sha256(&theirs),
            sha256(ours),
            ours.len()
        );
        assert!(theirs == ours, "{name} differs from the boot package's");
    }
    println!(
        "cdhash {}",
        made.cdhash.iter().map(|b| format!("{b:02x}")).collect::<String>()
    );
}

#[test]
fn refuses_a_catalogue_or_signature_off_its_pins() {
    let inputs = Inputs::load();
    let mut catalog = inputs.catalog.clone();
    *catalog.last_mut().unwrap() ^= 1;
    let mut signature = inputs.signature.clone();
    signature[0] ^= 1;
    for (catalog, signature) in [(&catalog, &inputs.signature), (&inputs.catalog, &signature)] {
        let refused = build(catalog, signature, &inputs.pins(), &inputs.jobs);
        assert!(
            matches!(refused, Err(Error::Pins)),
            "a changed input was not refused: {:?}",
            refused.err()
        );
    }
}
