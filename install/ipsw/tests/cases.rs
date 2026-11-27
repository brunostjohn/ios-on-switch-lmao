// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::PathBuf;

use switch_catalog::Catalog;
use switch_ipsw::Member;

pub struct Case {
    pub kind: String,
    pub role: String,
    pub member: Member,
    pub files: Vec<PathBuf>,
}

pub fn catalog() -> Catalog {
    let runfiles = runfiles::Runfiles::create().unwrap();
    let path = runfiles::rlocation!(runfiles, std::env::var("SWITCH_RECORDS").unwrap()).unwrap();
    Catalog::parse(&std::fs::read(path).unwrap()).unwrap()
}

/// `kind role rlocationpath...`, separated by `;`, from the environment variable `name`.
pub fn cases(name: &str) -> Vec<Case> {
    let catalog = catalog();
    let runfiles = runfiles::Runfiles::create().unwrap();
    let listed = std::env::var(name).unwrap();
    let cases: Vec<Case> = listed
        .split(';')
        .filter(|case| !case.trim().is_empty())
        .map(|case| {
            let mut fields = case.split_whitespace();
            let (kind, role) = (fields.next().unwrap().to_owned(), fields.next().unwrap().to_owned());
            let member = catalog.record(&kind).unwrap().members[&role].clone();
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

/// What the BUILD file must have listed, computed here from the catalog on its own.
pub fn expected(catalog: &Catalog, keep: impl Fn(&Member) -> bool) -> Vec<(String, String)> {
    let mut members: Vec<_> = catalog
        .builds
        .iter()
        .flat_map(|record| {
            record
                .members
                .iter()
                .map(move |(role, member)| (&record.build, role, member))
        })
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
