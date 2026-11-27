// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_launchd against the build's Python (oracle.py): plistlib's binary round trip and launchd_catalog's edit
//! must give the same bytes, and refuse the same inputs.

use switch_launchd::bplist::Plist;
use switch_launchd::{Jobs, add_bootstrap_jobs};

struct Case {
    kind: u8,
    pieces: Vec<Vec<u8>>,
    expected: Option<Vec<u8>>,
}

fn piece(data: &[u8], at: &mut usize) -> Vec<u8> {
    let length = u32::from_be_bytes(data[*at..*at + 4].try_into().unwrap()) as usize;
    *at += 4 + length;
    data[*at - length..*at].to_vec()
}

fn cases() -> Vec<Case> {
    let path = std::env::var("ORACLE_CASES").expect("ORACLE_CASES is set by the BUILD file");
    let data = std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"));
    let mut at = 0;
    let mut cases = Vec::new();
    while at < data.len() {
        let (kind, refused) = (data[at], data[at + 1] == 1);
        at += 2;
        let count = if kind == 0 { 2 } else { 5 };
        let mut pieces: Vec<Vec<u8>> = (0..count).map(|_| piece(&data, &mut at)).collect();
        let expected = pieces.pop().filter(|_| !refused);
        cases.push(Case { kind, pieces, expected });
    }
    cases
}

fn round_trip(source: &[u8]) -> Result<Vec<u8>, String> {
    let plist = Plist::from_binary(source).map_err(|e| e.to_string())?;
    plist.to_binary().map_err(|e| e.to_string())
}

fn edit(pieces: &[Vec<u8>]) -> Result<Vec<u8>, String> {
    let jobs = Jobs::parse(&pieces[1], &pieces[2], &pieces[3]).map_err(|e| e.to_string())?;
    add_bootstrap_jobs(&pieces[0], &jobs).map_err(|e| e.to_string())
}

fn hex(bytes: &[u8]) -> String {
    let shown: String = bytes.iter().take(160).map(|b| format!("{b:02x}")).collect();
    if bytes.len() > 160 {
        format!("{shown}… ({} B)", bytes.len())
    } else {
        shown
    }
}

#[test]
fn matches_plistlib_and_launchd_catalog() {
    let cases = cases();
    let mut counts = [[0usize; 2]; 2];
    let mut failures = Vec::new();
    for (index, case) in cases.iter().enumerate() {
        let ours = if case.kind == 0 {
            round_trip(&case.pieces[0])
        } else {
            edit(&case.pieces)
        };
        counts[usize::from(case.kind)][usize::from(case.expected.is_none())] += 1;
        match (&case.expected, ours) {
            (Some(expected), Ok(ours)) if *expected == ours => {}
            (None, Err(_)) => {}
            (expected, ours) => failures.push(format!(
                "case {index} (kind {}): source {}\n  python {}\n  ours   {}",
                case.kind,
                hex(&case.pieces[0]),
                expected.as_deref().map_or("refused".into(), hex),
                ours.map_or_else(|e| format!("refused: {e}"), |o| hex(&o)),
            )),
        }
    }
    let [[trips, trips_refused], [edits, edits_refused]] = counts;
    println!(
        "round trips: {trips} equal, {trips_refused} refused by both; edits: {edits} equal, {edits_refused} refused by both"
    );
    assert!(
        failures.is_empty(),
        "{} of {} cases differ:\n{}",
        failures.len(),
        cases.len(),
        failures.join("\n")
    );
    assert!(trips > 500 && trips_refused > 1000 && edits > 5 && edits_refused > 10);
}
