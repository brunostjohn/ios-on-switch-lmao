// SPDX-License-Identifier: GPL-3.0-or-later
//! One install (mark, then finish) on a card, cut short after every write it makes: at each cut the kernel's rules and
//! `Card::open` must read the card the same way, and a rerun from there must end on the uncut run's tables.

use std::fmt::Write as _;

use switch_card::{Card, Device, ENTRY_BYTES, SETUP_NAME};
use switch_card_testing::{Event, Recorder, SparseImage};

use crate::ffi::{self, Files};

pub const NEED: u64 = 1 << 30;

pub struct Scenario {
    pub name: &'static str,
    pub image: SparseImage,
    pub choice: Option<u32>,
    /// The shared FAT volume the kernel must find whenever the tables are whole: (first sector, sectors).
    pub fat: (u64, u64),
}

/// Runs the scenario's matrix and returns its table, one row per cut.
pub fn run(s: &Scenario) -> String {
    let mut done = s.image.clone();
    let (target, mark_writes, writes) = {
        let mut rec = Recorder::new(&mut done);
        let mut card = Card::open(&mut rec).unwrap();
        let plan = card.plan(s.choice, NEED).unwrap();
        let mark_writes = card.mark(&mut rec, &plan).unwrap();
        card.finish(&mut rec, &plan).unwrap();
        (plan.target, mark_writes, writes_with_barriers(&rec.events))
    };
    for (lba, _) in &writes {
        assert!(
            table_sector(&s.image, *lba),
            "{}: wrote LBA {lba}, outside the GPT",
            s.name
        );
    }
    let mut table = format!(
        "{}: target entry {}, {} writes ({} mark)\n",
        s.name,
        target.index,
        writes.len(),
        mark_writes
    );
    writeln!(table, "  cut  step    last write           kernel roots  torn   Files").unwrap();
    for k in 0..=writes.len() {
        let mut state = s.image.clone();
        for (lba, data) in &writes[..k] {
            state.set(*lba, data);
        }
        let kernel = ffi::root(&mut state).expect("setup() takes the primary header");
        assert!(kernel.found <= 1, "{}: cut {k}: {} roots", s.name, kernel.found);
        let card = Card::open(&mut state).unwrap_or_else(|e| panic!("{}: cut {k}: {e}", s.name));
        let ours = card.roots();
        assert_eq!(ours.len() as u32, kernel.found, "{}: cut {k}", s.name);
        if let [root] = ours[..] {
            let e = card.entries()[root as usize];
            assert_eq!(
                (e.first(), e.sectors()),
                (kernel.first, kernel.count),
                "{}: cut {k}",
                s.name
            );
        }
        check_stable(&s.image, &state, k);
        let (first, count) = if kernel.unique {
            (kernel.first, kernel.count)
        } else {
            (target.first, target.sectors)
        };
        let files = ffi::files(&mut state, first, count);
        if !card.torn() {
            assert_eq!(
                files,
                Files::Volume {
                    first: s.fat.0,
                    count: s.fat.1,
                    exfat: false
                },
                "{}: cut {k}",
                s.name
            );
        }
        resume(s, &state, k, mark_writes, writes.len(), &done);
        let step = if k == 0 {
            "before"
        } else if k <= mark_writes {
            "mark"
        } else {
            "finish"
        };
        let last = if k == 0 {
            "-".to_string()
        } else {
            describe(&s.image, writes[k - 1].0)
        };
        let roots = match ours[..] {
            [] => "none".to_string(),
            [r] => format!("entry {r}"),
            _ => unreachable!(),
        };
        let files = match files {
            Files::Volume { first, .. } => format!("FAT at {first}"),
            Files::Offline(why) => format!("offline ({why})"),
        };
        writeln!(
            table,
            "  {k:>3}  {step:<6}  {last:<20} {roots:<13} {:<6} {files}",
            card.torn()
        )
        .unwrap();
    }
    table
}

fn writes_with_barriers(events: &[Event]) -> Vec<(u64, Vec<u8>)> {
    events
        .chunks(2)
        .map(|pair| match pair {
            [Event::Write(lba, data), Event::Barrier] => (*lba, data.clone()),
            other => panic!("a write not followed by a barrier: {other:?}"),
        })
        .collect()
}

/// A rerun of the cut step and every step after it, from `state`, as the engine resumes: same choice, same image.
fn resume(s: &Scenario, state: &SparseImage, k: usize, mark_writes: usize, total: usize, done: &SparseImage) {
    let mut image = state.clone();
    let mut card = Card::open(&mut image).unwrap();
    let plan = card.plan(s.choice, NEED).unwrap();
    let t = plan.target.index;
    if card.entries()[t as usize].named(SETUP_NAME) {
        assert_eq!(
            card.target(None).unwrap().index,
            t,
            "{}: cut {k}: default choice after the mark",
            s.name
        );
    }
    let mut rewritten = 0;
    if k <= mark_writes {
        rewritten += card.mark(&mut image, &plan).unwrap();
        if k == mark_writes {
            assert_eq!(rewritten, 0, "{}: a finished mark rewrites nothing", s.name);
        }
    }
    rewritten += card.finish(&mut image, &plan).unwrap();
    if k == total {
        assert_eq!(rewritten, 0, "{}: a finished install rewrites nothing", s.name);
    }
    for lba in done.data.keys().chain(image.data.keys()) {
        assert_eq!(
            image.sector(*lba),
            done.sector(*lba),
            "{}: cut {k}: LBA {lba} after the rerun",
            s.name
        );
    }
}

fn header_lbas(image: &SparseImage) -> [(u64, u64); 2] {
    [1, image.sectors - 1].map(|lba| (lba, u64::from_le_bytes(image.sector(lba)[72..80].try_into().unwrap())))
}

/// LBA 0 as it was; both headers changed only in their CRCs; entry 0 (the FAT) as it was; every entry changed only in
/// its type and name.
fn check_stable(before: &SparseImage, now: &SparseImage, k: usize) {
    assert_eq!(before.sector(0), now.sector(0), "cut {k}: LBA 0");
    for (header, array) in header_lbas(before) {
        let (old, new) = (before.sector(header), now.sector(header));
        for (i, (a, b)) in old.iter().zip(new.iter()).enumerate() {
            assert!(
                a == b || (16..20).contains(&i) || (88..92).contains(&i),
                "cut {k}: LBA {header} byte {i}"
            );
        }
        let size = u32::from_le_bytes(old[84..88].try_into().unwrap()) as usize;
        let count = u32::from_le_bytes(old[80..84].try_into().unwrap()) as usize;
        let sectors = (count * size).div_ceil(512) as u64;
        let (a, b) = (before.read_bytes(array, sectors), now.read_bytes(array, sectors));
        assert_eq!(a[..ENTRY_BYTES], b[..ENTRY_BYTES], "cut {k}: entry 0 at LBA {array}");
        for (e, (x, y)) in a.chunks(size).zip(b.chunks(size)).enumerate() {
            assert!(
                x[16..56] == y[16..56] && x[ENTRY_BYTES..] == y[ENTRY_BYTES..],
                "cut {k}: entry {e} beyond type+name"
            );
        }
        assert_eq!(
            a[count * size..],
            b[count * size..],
            "cut {k}: array padding at LBA {array}"
        );
    }
}

fn table_sector(image: &SparseImage, lba: u64) -> bool {
    header_lbas(image).iter().any(|&(header, array)| {
        let h = image.sector(header);
        let bytes = u64::from(u32::from_le_bytes(h[80..84].try_into().unwrap()))
            * u64::from(u32::from_le_bytes(h[84..88].try_into().unwrap()));
        lba == header || (array..array + bytes.div_ceil(512)).contains(&lba)
    })
}

fn describe(image: &SparseImage, lba: u64) -> String {
    let [(_, primary), (_, backup)] = header_lbas(image);
    match lba {
        1 => "primary header".to_string(),
        l if l == image.sector_count() - 1 => "backup header".to_string(),
        l if l >= backup => format!("backup array +{}", l - backup),
        l => format!("primary array +{}", l - primary),
    }
}
