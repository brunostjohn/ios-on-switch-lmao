// SPDX-License-Identifier: GPL-3.0-or-later
use crate::{Assets, Error};

fn tar(entries: &[(&str, tar::EntryType, &[u8])]) -> Vec<u8> {
    let mut builder = tar::Builder::new(Vec::new());
    for (name, kind, data) in entries {
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(*kind);
        header.set_size(data.len() as u64);
        header.set_mode(0o644);
        let name_field = &mut header.as_old_mut().name;
        name_field[..name.len()].copy_from_slice(name.as_bytes());
        header.set_cksum();
        builder.append(&header, *data).unwrap();
    }
    builder.into_inner().unwrap()
}

fn refusal(entries: &[(&str, tar::EntryType, &[u8])]) -> String {
    match Assets::from_tar(tar(entries).as_slice()) {
        Err(Error::Assets(message)) => message,
        Err(other) => panic!("refused as {other}"),
        Ok(_) => panic!("accepted"),
    }
}

#[test]
fn members_are_named_by_their_path_in_the_tar() {
    let regular = tar::EntryType::Regular;
    let assets = Assets::from_tar(
        tar(&[
            ("./", tar::EntryType::Directory, b""),
            ("./boot.json", regular, b"{}"),
            ("kernel/sites/a.json", regular, b"[]"),
        ])
        .as_slice(),
    )
    .unwrap();
    assert_eq!(assets.get("boot.json"), Some(&b"{}"[..]));
    let sites: Vec<_> = assets.under("kernel/sites/").map(|(name, _)| name).collect();
    assert_eq!(sites, ["a.json"]);
}

#[test]
fn links_paths_outside_and_twins_are_refused() {
    let regular = tar::EntryType::Regular;
    assert!(refusal(&[("boot.json", tar::EntryType::Symlink, b"")]).contains("not a file"));
    assert!(refusal(&[("../boot.json", regular, b"")]).contains("outside"));
    assert!(refusal(&[("/boot.json", regular, b"")]).contains("outside"));
    assert!(refusal(&[("boot.json", regular, b""), ("./boot.json", regular, b"")]).contains("second member"));
}
