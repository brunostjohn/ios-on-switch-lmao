// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads the standard tree back through a mounted volume: each directory lists exactly the expected names, and every
//! file has its owner, mode and bytes. Every name is also looked up in its NFC and NFD forms, which the kernel finds
//! only through the stored name hash.

use std::collections::BTreeSet;
use std::ffi::OsString;
use std::fs;
use std::io;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

use unicode_normalization::UnicodeNormalization;

use crate::standard::{self, Spec};

pub fn run(mount: &Path, spec: &Spec) -> io::Result<bool> {
    let (dirs, files) = standard::listing(spec);
    let mut ok = true;
    let mut fail = |what: String| {
        println!("FAIL: {what}");
        ok = false;
    };
    let mut expected: Vec<(PathBuf, BTreeSet<OsString>)> = vec![(mount.into(), BTreeSet::new())];
    for dir in &dirs {
        expected[0].1.insert(dir.name.clone().into());
        let at = mount.join(&dir.name);
        let m = fs::metadata(&at)?;
        if !m.is_dir() || (m.uid(), m.gid(), m.mode() & 0o7777) != (dir.owner.0, dir.owner.1, 0o755) {
            fail(format!("{}: {}:{} {:o}", at.display(), m.uid(), m.gid(), m.mode()));
        }
        expected.push((at, BTreeSet::new()));
    }
    let mut lookups = 0;
    for file in &files {
        let dir = mount.join(&file.dir);
        if let Some((_, names)) = expected.iter_mut().find(|(d, _)| *d == dir) {
            names.insert(file.name.clone().into());
        }
        let at = dir.join(&file.name);
        let m = fs::metadata(&at)?;
        if !m.is_file() || (m.uid(), m.gid(), m.mode() & 0o7777) != (file.owner.0, file.owner.1, 0o644) {
            fail(format!("{}: {}:{} {:o}", at.display(), m.uid(), m.gid(), m.mode()));
        }
        if fs::read(&at)? != file.data {
            fail(format!("{}: contents", at.display()));
        }
        if !file.name.is_ascii() {
            for form in [file.name.nfc().collect::<String>(), file.name.nfd().collect()] {
                match fs::metadata(dir.join(&form)) {
                    Ok(found) if found.ino() == m.ino() => lookups += 1,
                    other => fail(format!("{form:?} looked up as {:?}", other.map(|f| f.ino()))),
                }
            }
        }
    }
    for (dir, names) in &expected {
        let listed: BTreeSet<OsString> = fs::read_dir(dir)?
            .map(|e| e.map(|e| e.file_name()))
            .collect::<Result<_, _>>()?;
        if listed != *names {
            let extra: Vec<_> = listed.difference(names).collect();
            let missing: Vec<_> = names.difference(&listed).collect();
            fail(format!("{} lists {extra:?} extra, {missing:?} missing", dir.display()));
        }
    }
    println!(
        "verified {} directories, {} files, {lookups} lookups of non-ASCII names by NFC and NFD",
        dirs.len() + 1,
        files.len()
    );
    Ok(ok)
}
