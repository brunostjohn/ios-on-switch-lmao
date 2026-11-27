// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io;
use std::time::Instant;

use switch_catalog::Catalog;

fn env(name: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| panic!("{name} is not set"))
}

#[test]
fn the_member_decrypts_to_its_pinned_content() {
    let catalog = Catalog::parse(&std::fs::read(env("SWITCH_RECORDS")).unwrap()).unwrap();
    let (build, role) = (env("SWITCH_AEA_BUILD"), env("SWITCH_AEA_ROLE"));
    let member = &catalog.record(&build).unwrap().members[&role];
    let archive = File::open(env("SWITCH_AEA_ARCHIVE")).unwrap();
    let started = Instant::now();
    let written = switch_aea::decrypt_member(archive, member, io::sink(), |_| ()).unwrap();
    let seconds = started.elapsed().as_secs_f64();
    println!(
        "{build} {role}: {written} bytes, SHA-256 {}, in {seconds:.1} s",
        member.content_sha256
    );
}
