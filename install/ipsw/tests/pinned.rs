// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;

mod cases;

use cases::{cases, expected, listed};
use switch_ipsw::extract;

fn large() -> u64 {
    std::env::var("SWITCH_LARGE").unwrap().parse().unwrap()
}

/// Every small deflated member of every kind, inflated from the stored bytes Bazel fetched, against //ipsw's own
/// inflated output.
#[test]
fn small_members_match_the_build() {
    let cases = cases("SWITCH_SMALL_MEMBERS");
    let want = expected(&cases::pins(), |m| {
        m.deflated && m.aea_key.is_none() && m.stored_size <= large()
    });
    assert_eq!(listed(&cases), want);
    for case in cases {
        let [stored, built] = &case.files[..] else {
            panic!("{} {}: stored and built files", case.kind, case.role)
        };
        let mut out = Vec::new();
        let written = extract(File::open(stored).unwrap(), &case.member, &mut out)
            .unwrap_or_else(|e| panic!("{} {}: {e}", case.kind, case.role));
        assert_eq!(written.sha256, case.member.content_sha256);
        assert!(
            out == std::fs::read(built).unwrap(),
            "{} {}: differs from {}",
            case.kind,
            case.role,
            built.display()
        );
    }
}
