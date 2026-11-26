// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs::File;
use std::io;
use std::time::Instant;

fn env(name: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| panic!("{name} is not set"))
}

#[test]
fn the_member_decrypts_to_its_pinned_content() {
    let key = switch_aea::key_value(&env("SWITCH_AEA_KEY")).unwrap();
    let content: [u8; 32] = hex::decode(env("SWITCH_AEA_CONTENT_SHA256"))
        .unwrap()
        .try_into()
        .unwrap();
    let archive = File::open(env("SWITCH_AEA_ARCHIVE")).unwrap();
    let started = Instant::now();
    let written = switch_aea::decrypt(archive, &key, io::sink(), &content, |_| ()).unwrap();
    let seconds = started.elapsed().as_secs_f64();
    println!("{written} bytes, SHA-256 {}, in {seconds:.1} s", hex::encode(content));
}
