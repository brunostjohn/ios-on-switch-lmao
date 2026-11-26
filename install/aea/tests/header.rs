// SPDX-License-Identifier: GPL-3.0-or-later
//! Each encrypted member's headers, read from Apple's URL with its catalog key: the archive decrypts to the size the
//! catalog pins.

use switch_catalog::Catalog;
use switch_fetch::{HttpFile, Url};

#[test]
fn every_archive_decrypts_to_its_pinned_size() {
    let path = std::env::var("SWITCH_RECORDS").expect("SWITCH_RECORDS is set by the BUILD file");
    let catalog = Catalog::parse(&std::fs::read(path).unwrap()).unwrap();
    let mut archives = 0;
    for record in &catalog.builds {
        let url = Url::parse(&record.ipsw.url).unwrap();
        for (role, member) in &record.members {
            let Some(aea) = &member.aea else { continue };
            let client = reqwest::blocking::Client::new();
            let mut file = HttpFile::new(client, url.clone(), member.offset, member.stored_size, 64 << 10);
            let key = switch_aea::key_value(&aea.key).unwrap();
            let index =
                switch_aea::Index::read(&mut file, &key).unwrap_or_else(|e| panic!("{} {role}: {e}", record.build));
            assert_eq!(index.raw_size(), aea.raw_size, "{} {role}", record.build);
            println!(
                "{} {role}: {} clusters, {} bytes decrypted",
                record.build,
                index.clusters(),
                aea.raw_size
            );
            archives += 1;
        }
    }
    assert!(archives > 0, "the catalog has no encrypted member");
}
