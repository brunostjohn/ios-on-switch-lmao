// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io;

mod cases;

use cases::{cases, expected, listed};
use switch_ipsw::extract;

/// The large members that are not encrypted, extracted from Bazel's fetch of their stored bytes and checked against
/// their content pins as they stream, with nothing written to disk.
#[test]
fn large_members_match_their_pins() {
    let large: u64 = std::env::var("SWITCH_LARGE").unwrap().parse().unwrap();
    let cases = cases("SWITCH_LARGE_MEMBERS");
    let want = expected(&cases::catalog(), |m| {
        m.aea.is_none() && (!m.deflated || m.stored_size > large)
    });
    assert_eq!(listed(&cases), want);
    for case in cases {
        let started = std::time::Instant::now();
        let written = extract(File::open(&case.files[0]).unwrap(), &case.member, &mut io::sink())
            .unwrap_or_else(|e| panic!("{} {}: {e}", case.kind, case.role));
        let seconds = started.elapsed().as_secs_f64();
        println!(
            "{} {}: {} bytes, {} in {seconds:.1} s",
            case.kind,
            case.role,
            written.len,
            hex::encode(written.sha256)
        );
    }
}
