// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::path::PathBuf;

use switch_ipsw::{Ipsw, Member, parse_pins};

pub struct Case {
    pub kind: String,
    pub role: String,
    pub member: Member,
    pub files: Vec<PathBuf>,
}

pub fn pins() -> BTreeMap<String, Ipsw> {
    let runfiles = runfiles::Runfiles::create().unwrap();
    let path = runfiles::rlocation!(runfiles, std::env::var("SWITCH_PINS").unwrap()).unwrap();
    parse_pins(&std::fs::read_to_string(path).unwrap()).unwrap()
}

/// `kind role rlocationpath...`, separated by `;`, from the environment variable `name`.
pub fn cases(name: &str) -> Vec<Case> {
    let pins = pins();
    let runfiles = runfiles::Runfiles::create().unwrap();
    let listed = std::env::var(name).unwrap();
    let cases: Vec<Case> = listed
        .split(';')
        .filter(|case| !case.trim().is_empty())
        .map(|case| {
            let mut fields = case.split_whitespace();
            let (kind, role) = (fields.next().unwrap().to_owned(), fields.next().unwrap().to_owned());
            let member = pins[&kind].members[&role].clone();
            let files = fields
                .map(|path| runfiles::rlocation!(runfiles, path).unwrap())
                .collect();
            Case {
                kind,
                role,
                member,
                files,
            }
        })
        .collect();
    assert!(!cases.is_empty(), "{name} lists no members");
    cases
}

/// What the BUILD file must have listed, computed here from the pins on their own.
pub fn expected(pins: &BTreeMap<String, Ipsw>, keep: impl Fn(&Member) -> bool) -> Vec<(String, String)> {
    let mut members: Vec<_> = pins
        .iter()
        .flat_map(|(kind, ipsw)| ipsw.members.iter().map(move |(role, member)| (kind, role, member)))
        .filter(|(_, _, member)| keep(member))
        .map(|(kind, role, _)| (kind.clone(), role.clone()))
        .collect();
    members.sort();
    members
}

pub fn listed(cases: &[Case]) -> Vec<(String, String)> {
    let mut members: Vec<_> = cases
        .iter()
        .map(|case| (case.kind.clone(), case.role.clone()))
        .collect();
    members.sort();
    members
}
