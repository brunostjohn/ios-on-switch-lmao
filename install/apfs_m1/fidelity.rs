// SPDX-License-Identifier: GPL-3.0-or-later
//! `fidelity`: what a mount can't show, compared record for record between a source subtree and its copy, paired
//! by path. Internal flags, protection class, write-generation counter, every xfield with its flags in order, the
//! directory record's hash, `date_added` and flags, every xattr's flags and stored form, and each stream's `j_dstream`
//! and extents (lengths, flags and crypto ids; holes as holes).
//!
//! Clone sharing is compared too: which inodes share a private id, each one's clone mapping when both volumes keep
//! them, and each data stream's reference count.
//!
//! What a copy changes on purpose is counted, not compared: object ids (parent, private id, stream and xattr ids,
//! physical blocks), a directory's subdirectory count under expanded records, for a file whose source has names
//! outside the subtree its link count and the name its inode records, the was-cloned flags set on a clone whose source
//! lacked them, and the mapping and reference count of a clone whose other files are outside the subtree.

use std::collections::BTreeMap;
use std::path::PathBuf;

use switch_apfs_format::fs::{INO_EXT_TYPE_NAME, InodeVal};
use switch_apfs_format::io::FileSource;
use switch_apfs_read::{Container, Entry, Fs, Step, Stream, XattrData};

use crate::parse::parse_uuid;
use crate::sharing::Sharing;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

#[derive(clap::Args)]
pub struct FidelityArgs {
    #[arg(long)]
    source: PathBuf,
    #[arg(long, default_value_t = 0)]
    source_offset: u64,
    #[arg(long)]
    copy: PathBuf,
    #[arg(long, value_parser = parse_uuid)]
    system_uuid: [u8; 16],
    #[arg(long, value_parser = parse_uuid)]
    data_uuid: [u8; 16],
    #[arg(long, default_value = "/private/var")]
    path: String,
}

#[derive(Default)]
struct Tally {
    compared: u64,
    expected: BTreeMap<&'static str, u64>,
    problems: BTreeMap<&'static str, Vec<String>>,
}

impl Tally {
    fn check<T: PartialEq + std::fmt::Debug>(&mut self, what: &'static str, path: &str, a: T, b: T) {
        if a != b {
            self.problems
                .entry(what)
                .or_default()
                .push(format!("{path}: {a:?} -> {b:?}"));
        }
    }

    fn expect(&mut self, what: &'static str) {
        *self.expected.entry(what).or_default() += 1;
    }
}

fn entries(fs: &Fs, path: &str) -> Result<BTreeMap<String, Entry>> {
    let mut out = BTreeMap::new();
    let base = path.trim_end_matches('/').len();
    fs.walk(path, &mut |e| {
        let rel = if e.drec.is_none() {
            "/".into()
        } else {
            e.path[base..].to_string()
        };
        out.insert(rel, e.clone());
        Ok(Step::Into)
    })?;
    Ok(out)
}

pub fn run(a: &FidelityArgs) -> Result<bool> {
    let src = FileSource::open(&a.source, a.source_offset)?;
    let copy = FileSource::open(&a.copy, 0)?;
    let (sc, cc) = (Container::open(&src)?, Container::open(&copy)?);
    let source = Fs::open(&src, sc.volume(a.system_uuid).ok_or("no System volume in the source")?)?;
    let data = Fs::open(&copy, cc.volume(a.data_uuid).ok_or("no Data volume in the copy")?)?;
    let (before, after) = (entries(&source, &a.path)?, entries(&data, "/")?);
    let sharing = [Sharing::read(&source, &before)?, Sharing::read(&data, &after)?];
    let mut t = Tally::default();
    for path in before.keys().filter(|p| !after.contains_key(*p)) {
        t.problems.entry("only in the source").or_default().push(path.clone());
    }
    for path in after.keys().filter(|p| !before.contains_key(*p)) {
        t.problems.entry("only in the copy").or_default().push(path.clone());
    }
    for (path, s) in before.iter().filter(|(p, _)| p.as_str() != "/") {
        if let Some(d) = after.get(path) {
            entry(&mut t, path, s, d, sharing[1].group(path).len() > 1);
            clones(&mut t, path, s, d, &sharing);
        }
    }
    println!(
        "{} entries compared record for record; expected differences: {:?}",
        t.compared, t.expected
    );
    for (what, items) in &t.problems {
        println!("DIFFERENT {what}: {}", items.len());
        for item in items.iter().take(10) {
            println!("  {item}");
        }
    }
    Ok(t.problems.is_empty())
}

fn entry(t: &mut Tally, path: &str, s: &Entry, d: &Entry, cloned: bool) {
    t.compared += 1;
    let (sv, dv) = (&s.inode.val, &d.inode.val);
    let flags = if cloned
        && dv.internal_flags.get() == sv.internal_flags.get() | INODE_WAS_CLONED_BOTH
        && sv.internal_flags.get() != dv.internal_flags.get()
    {
        t.expect("was-cloned flags set on a clone");
        dv.internal_flags.get()
    } else {
        sv.internal_flags.get()
    };
    let fields = |v: &InodeVal, flags: u64| {
        (
            flags,
            v.default_protection_class.get(),
            v.write_generation_counter.get(),
            v.bsd_flags.get(),
            (v.owner.get(), v.group.get(), v.mode.get()),
            (
                v.create_time.get(),
                v.mod_time.get(),
                v.change_time.get(),
                v.access_time.get(),
            ),
        )
    };
    t.check(
        "inode fields",
        path,
        fields(sv, flags),
        fields(dv, dv.internal_flags.get()),
    );
    let outside = !s.inode.is_dir() && sv.nchildren_or_nlink.get() != dv.nchildren_or_nlink.get();
    if outside {
        t.expect("links outside the subtree");
    } else {
        t.check(
            "children or links",
            path,
            sv.nchildren_or_nlink.get(),
            dv.nchildren_or_nlink.get(),
        );
    }
    if s.inode.is_dir() {
        if sv.uncompressed_size.get() != dv.uncompressed_size.get() {
            t.expect("subdirectory count");
        }
    } else {
        t.check(
            "uncompressed size",
            path,
            sv.uncompressed_size.get(),
            dv.uncompressed_size.get(),
        );
    }
    let xfields = |e: &Entry, keep_name: bool| -> Vec<(u8, u8, Vec<u8>)> {
        e.inode
            .xfields
            .iter()
            .map(|x| {
                let data = if x.kind == INO_EXT_TYPE_NAME && !keep_name {
                    Vec::new()
                } else {
                    x.data.clone()
                };
                (x.kind, x.flags, data)
            })
            .collect()
    };
    let same_name = s.inode.name() == d.inode.name();
    if !same_name && outside {
        t.expect("inode name of a link outside the subtree");
    }
    t.check(
        "inode xfields",
        path,
        xfields(s, same_name || !outside),
        xfields(d, same_name || !outside),
    );
    if let (Some(sd), Some(dd)) = (&s.drec, &d.drec) {
        let drec = |r: &switch_apfs_read::DirRec| (r.hash, r.val.date_added.get(), r.val.flags.get());
        t.check("directory record", path, drec(sd), drec(dd));
        let xf = |r: &switch_apfs_read::DirRec| r.xfields.iter().filter(|x| x.kind != 1).cloned().collect::<Vec<_>>();
        t.check("directory record xfields", path, xf(sd), xf(dd));
    }
    let xattrs = |e: &Entry| -> Vec<(String, u16, String)> {
        e.xattrs
            .iter()
            .map(|(x, s)| {
                let form = match (&x.data, s) {
                    (XattrData::Embedded(b), _) => format!("embedded {b:02x?}"),
                    (XattrData::Stream { dstream, .. }, s) => {
                        let d = &dstream.dstream;
                        format!(
                            "stream {} {} {} {} {} {}",
                            d.size.get(),
                            d.alloced_size.get(),
                            d.default_crypto_id.get(),
                            d.total_bytes_written.get(),
                            d.total_bytes_read.get(),
                            s.as_ref().map_or_else(String::new, shape)
                        )
                    }
                };
                (x.name.clone(), x.flags, form)
            })
            .collect()
    };
    t.check("xattrs", path, xattrs(s), xattrs(d));
    let data = |e: &Entry| e.data.as_ref().map(|s| (s.size, shape(s), s.refcnt.is_some()));
    t.check("data stream", path, data(s), data(d));
}

/// INODE_WAS_CLONED | INODE_WAS_EVER_CLONED, which macOS sets on every file of a clone.
const INODE_WAS_CLONED_BOTH: u64 = 0x410;

fn clones(t: &mut Tally, path: &str, s: &Entry, d: &Entry, sharing: &[Sharing; 2]) {
    let [ss, ds] = sharing;
    t.check("clone group", path, ss.group(path), ds.group(path));
    if let (Some(sm), Some(dm)) = (ss.mapping(path), ds.mapping(path)) {
        if sm != dm && ss.mapped_outside(path) {
            t.expect("clone mapping of a clone with files outside the subtree");
        } else {
            t.check("clone mapping", path, sm, dm);
        }
    }
    let refs = |e: &Entry| e.data.as_ref().and_then(|s| s.refcnt);
    let (sr, dr) = (refs(s), refs(d));
    if sr != dr && sr.is_some_and(|n| n as usize > ss.group(path).len()) {
        t.expect("reference count of a clone with files outside the subtree");
    } else {
        t.check("data stream references", path, sr, dr);
    }
}

/// A stream's extents without their blocks: adjacent ones of the same kind joined, since placement may split a run.
fn shape(s: &Stream) -> String {
    let mut extents = s.extents.clone();
    extents.sort_by_key(|e| e.logical_addr);
    let mut joined: Vec<(u64, u64, bool, u8, u64)> = Vec::new();
    for e in extents {
        let hole = e.phys_block_num == 0;
        match joined.last_mut() {
            Some(last)
                if last.0 + last.1 == e.logical_addr && (last.2, last.3, last.4) == (hole, e.flags, e.crypto_id) =>
            {
                last.1 += e.len;
            }
            _ => joined.push((e.logical_addr, e.len, hole, e.flags, e.crypto_id)),
        }
    }
    format!("{joined:?}")
}
