// SPDX-License-Identifier: GPL-3.0-or-later
//! The tree both macOS and our writer build in the Mac checks: numbered directories of small files, one directory of
//! long names, and one of names from every corner of Unicode that the 17.0 kernel accepts.

use switch_apfs_write::{Content, Entry, Owner, Tree};

const LONG_NAME: &str = "a-rather-long-file-name-that-makes-the-index-keys-much-bigger-than-the-numbered-ones-";

/// Each built from code points, so neither a shell nor a locale normalises them.
const UNICODE_NAMES: &[&[u32]] = &[
    &[0xD55C, 0xAE00, 0xAC01],
    &[0x1112, 0x1161, 0x11AB],
    &[0xE9, 0xF1, 0xFC, 0x1EC7],
    &[0x65, 0x301, 0x6E, 0x303],
    &[0x61, 0x308, 0x323, 0x301],
    &[0x1F600, 0x1F680],
    &[0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467],
    &[0x1F44D, 0x1F3FD],
    &[0x1F1EF, 0x1F1F5],
    &[0x2764, 0xFE0F],
    &[0x6F22, 0x5B57, 0x65E5, 0x672C],
    &[0x20000, 0x2A6D6],
    &[0xF900, 0xF91D],
    &[0x2F800, 0x2FA1D],
    &[0x2126, 0x212B, 0x212A],
    &[0x41, 0x2000, 0x2001, 0x42],
    &[0x386, 0x1F71, 0x385],
    &[0x915, 0x93C, 0x958],
    &[0xF73, 0xF75, 0xF81],
    &[0x1D15E, 0x1D160],
    &[0x10D40, 0x10D41],
    &[0x11380, 0x113B8, 0x113C2],
    &[0x7A, 0x897, 0x316],
    &[0x16D40, 0x16D63, 0x16D67],
    &[0x79, 0x1ACF, 0x316],
    &[0x323B0, 0x323B1],
    &[0x10940, 0x10941],
    &[0x1FAC8],
    &[0x41, 0x61, 0x130, 0x131, 0x3A3, 0x3C2],
    &[0xC5],
    &[0x41, 0x30A, 0x5F],
];

pub struct Spec {
    pub dirs: usize,
    pub files: usize,
}

pub struct Dir {
    pub name: String,
    pub owner: (u32, u32),
}

pub struct File {
    pub dir: String,
    pub name: String,
    pub owner: (u32, u32),
    pub data: Vec<u8>,
}

pub fn unicode_names() -> Vec<String> {
    UNICODE_NAMES
        .iter()
        .map(|codes| {
            codes
                .iter()
                .map(|&c| char::from_u32(c).expect("a scalar value"))
                .collect()
        })
        .collect()
}

fn owner(dir: usize) -> (u32, u32) {
    if dir.is_multiple_of(2) { (0, 0) } else { (501, 501) }
}

/// Every directory, all at the root, and every file.
pub fn listing(spec: &Spec) -> (Vec<Dir>, Vec<File>) {
    let mut dirs = Vec::new();
    let mut files = Vec::new();
    let mut add = |dir: &str, owner: (u32, u32), names: Vec<(String, Vec<u8>)>| {
        dirs.push(Dir {
            name: dir.into(),
            owner,
        });
        for (name, data) in names {
            files.push(File {
                dir: dir.into(),
                name,
                owner,
                data,
            });
        }
    };
    for d in 0..spec.dirs {
        let names = (0..spec.files).map(|f| (format!("file-{f}"), format!("{d} {f}\n").into_bytes()));
        add(&format!("d{d}"), owner(d), names.collect());
    }
    let long = (0..spec.files).map(|f| (format!("{LONG_NAME}{f}"), Vec::new()));
    add("long", (0, 0), long.collect());
    let names = unicode_names().into_iter().map(|n| (n.clone(), n.into_bytes()));
    add("names", (501, 501), names.collect());
    (dirs, files)
}

pub fn tree(spec: &Spec) -> Tree {
    let (dirs, files) = listing(spec);
    let entry = |name: &str, (uid, gid): (u32, u32), mode, content| Entry {
        name: name.into(),
        owner: Owner { uid, gid, mode },
        content,
    };
    let mut entries: Vec<Entry> = dirs
        .iter()
        .map(|dir| entry(&dir.name, dir.owner, 0o755, Content::Dir(Vec::new())))
        .collect();
    for file in files {
        let parent = entries
            .iter_mut()
            .find(|e| e.name == file.dir)
            .expect("parent listed first");
        let Content::Dir(children) = &mut parent.content else {
            unreachable!("only directories are listed as parents")
        };
        children.push(entry(&file.name, file.owner, 0o644, Content::File(file.data)));
    }
    Tree {
        root: Owner {
            uid: 0,
            gid: 0,
            mode: 0o755,
        },
        entries,
    }
}
