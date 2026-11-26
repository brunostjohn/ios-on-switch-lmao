// SPDX-License-Identifier: GPL-3.0-or-later
//! A partition run on a card image and everything a finished card must satisfy, and the same run cut at every write.

use switch_card::{Card, Device, LINUX_TYPE, SETUP_NAME};
use switch_card_testing::{Event, Recorder, SparseImage};
use switch_partition::{Error, FsKind, Plan, Report};

use crate::cuts::{self, first_difference};
use crate::exfat_reader;
use crate::oracle::{self, Files};
use crate::reader::{self, Listing};

/// The whole run, sized `ios_bytes` (tests take any multiple of 16 MiB).
pub fn partition<D: Device>(dev: &mut D, ios_bytes: u64) -> (Plan, Report) {
    let survey = switch_partition::survey(dev).unwrap();
    let plan = survey.plan(ios_bytes, 0).unwrap();
    let report = switch_partition::run(dev, &plan).unwrap();
    (plan, report)
}

/// What the installer does after a cut: resume a pending run, or start the run again if it never began.
pub fn recover(image: &mut SparseImage, ios_bytes: u64) {
    if switch_partition::resume(image).unwrap().is_some() {
        return;
    }
    match switch_partition::survey(image) {
        Err(Error::AlreadyPartitioned(_)) => {}
        Err(e) => panic!("survey after a cut: {e}"),
        Ok(survey) => {
            let plan = survey.plan(ios_bytes, 0).unwrap();
            switch_partition::run(image, &plan).unwrap();
        }
    }
}

/// The kernel's root before a run, which partition mode never changes.
pub fn roots(image: &mut SparseImage) -> u32 {
    oracle::root(image).map_or(0, |r| r.found)
}

/// A finished card: the journal gone, the tables as planned and whole, the kernel's rules satisfied, every file
/// where it was.
pub fn check_done(image: &mut SparseImage, plan: &Plan, before: &Listing, roots_before: u32, name: &str) {
    assert_eq!(switch_partition::pending(image).unwrap(), None, "{name}: journal left");
    let journal = image.read_bytes(switch_partition::journal::FIRST, switch_partition::journal::SECTORS);
    assert!(journal.iter().all(|&b| b == 0), "{name}: journal not zero");
    let card = Card::open(image).unwrap_or_else(|e| panic!("{name}: Card::open: {e}"));
    assert!(!card.torn(), "{name}: GPT copies differ");
    let fat = card.entries()[0];
    assert_eq!(
        (fat.first(), fat.sectors()),
        (plan.fat_first, plan.fat_new_sectors),
        "{name}: entry 0"
    );
    assert_eq!(
        card.boot_volume().unwrap(),
        (plan.fat_first, plan.fat_new_sectors),
        "{name}: MBR slot 0"
    );
    let new = card.entries()[plan.entry_index as usize];
    assert!(
        new.named(SETUP_NAME) && new.type_guid() == LINUX_TYPE,
        "{name}: new entry {new:?}"
    );
    assert_eq!(
        (new.first(), new.sectors()),
        (plan.entry_first, plan.entry_sectors),
        "{name}: new entry"
    );
    assert_eq!(plan.entry_first % 0x8000, 0, "{name}: new entry start alignment");
    assert_eq!(
        (plan.entry_first + plan.entry_sectors) % 0x8000,
        0,
        "{name}: new entry end alignment"
    );
    let target = card.target(None).unwrap();
    assert_eq!(target.index, plan.entry_index, "{name}: install's default target");
    let root = oracle::root(image).expect("the kernel reads the GPT");
    assert_eq!(root.found, roots_before, "{name}: kernel roots");
    let (first, count) = if root.unique {
        (root.first, root.count)
    } else {
        (new.first(), new.sectors())
    };
    let want_exfat = plan.fs == FsKind::Exfat;
    assert_eq!(
        oracle::files(image, first, count),
        Files::Volume {
            first: plan.fat_first,
            count: plan.fat_new_sectors,
            exfat: want_exfat
        },
        "{name}: the kernel's Files volume"
    );
    let boot = image.sector(plan.fat_first);
    match plan.fs {
        FsKind::Fat32 => {
            let total = u64::from(u32::from_le_bytes(boot[32..36].try_into().unwrap()));
            assert!(total <= plan.fat_new_sectors, "{name}: TotSec32 {total}");
            for copy in [0, 1] {
                assert!(
                    reader::list(image, plan.fat_first, copy) == *before,
                    "{name}: files through FAT {copy}"
                );
            }
        }
        FsKind::Exfat => {
            let vol_len = u64::from_le_bytes(boot[72..80].try_into().unwrap());
            let heap = u64::from(u32::from_le_bytes(boot[88..92].try_into().unwrap()));
            let clus = u32::from_le_bytes(boot[92..96].try_into().unwrap());
            assert!(vol_len <= plan.fat_new_sectors, "{name}: VolumeLength {vol_len}");
            assert_eq!(
                u64::from(clus),
                ((vol_len - heap) >> boot[109]).min(0xffff_fff5),
                "{name}: ClusterCount"
            );
            assert!(
                exfat_reader::list(image, plan.fat_first, true) == *before,
                "{name}: exFAT files"
            );
        }
    }
}

/// The run cut at every write `cuts::cuts` makes (`drops` for the epoch subsets), each cut checked before and after
/// recovery; then `nested` of the cuts that left a journal, spread over the run, have that recovery cut again at every
/// write and half-torn. The uncut run's card gets `check_done` and fsck. Returns the matrix's table.
pub fn matrix(name: &str, base: &SparseImage, ios_bytes: u64, drops: bool, nested: usize) -> String {
    let mut done = base.clone();
    let (plan, events) = {
        let mut rec = Recorder::new(&mut done);
        let (plan, _) = partition(&mut rec, ios_bytes);
        (plan, rec.events)
    };
    let mut start = base.clone();
    let before = match plan.fs {
        FsKind::Fat32 => reader::list(&mut start, crate::cards::FAT_FIRST, 0),
        FsKind::Exfat => exfat_reader::list(&mut start, crate::cards::FAT_FIRST, true),
    };
    let roots_before = roots(&mut start);
    check_done(&mut done, &plan, &before, roots_before, name);
    match plan.fs {
        FsKind::Fat32 => {
            crate::macos::fsck_fat32(&done, plan.fat_first, plan.fat_new_sectors, name);
        }
        FsKind::Exfat => {
            crate::macos::fsck_exfat(&done, plan.fat_first, plan.fat_new_sectors, name);
        }
    }
    let all = cuts::cuts(&events, drops);
    let check = |state: &SparseImage, label: &str| {
        let mut recovered = state.clone();
        recover(&mut recovered, ios_bytes);
        if let Some(lba) = first_difference(&recovered, &done) {
            panic!("{name}: {label}: recovered card differs from the uncut run at LBA {lba}");
        }
    };
    let rows: Vec<(bool, String)> = parallel(all.len(), |i| {
        let cut = &all[i];
        let mut state = cuts::state(base, &cut.landed);
        assert_eq!(roots(&mut state), roots_before, "{name}: {}: kernel roots", cut.label);
        let files = files_at_cut(&mut state, &plan, &cut.label, name);
        match plan.fs {
            FsKind::Fat32 => {
                for copy in [0, 1] {
                    assert!(
                        reader::list(&mut state, crate::cards::FAT_FIRST, copy) == before,
                        "{name}: {}: files through FAT {copy} before recovery",
                        cut.label
                    );
                }
            }
            FsKind::Exfat => {
                assert!(
                    exfat_reader::list(&mut state, crate::cards::FAT_FIRST, false) == before,
                    "{name}: {}: exFAT files before recovery",
                    cut.label
                );
            }
        }
        check(&state, &cut.label);
        (switch_partition::pending(&mut state).unwrap().is_some(), files)
    });
    let pending: Vec<usize> = (0..rows.len()).filter(|&i| rows[i].0).collect();
    let chosen: Vec<usize> = if nested == 0 || pending.is_empty() {
        Vec::new()
    } else {
        let stride = pending.len().div_ceil(nested);
        pending.iter().copied().step_by(stride).collect()
    };
    let inner: usize = parallel(chosen.len(), |j| {
        let cut = &all[chosen[j]];
        let state = cuts::state(base, &cut.landed);
        let mut resumed = state.clone();
        let events = {
            let mut rec = Recorder::new(&mut resumed);
            switch_partition::resume(&mut rec).unwrap();
            rec.events
        };
        let inner = cuts::cuts(&events, false);
        for c in &inner {
            let mut again = cuts::state(&state, &c.landed);
            let label = format!("{} then resume {}", cut.label, c.label);
            assert_eq!(roots(&mut again), roots_before, "{name}: {label}");
            files_at_cut(&mut again, &plan, &label, name);
            check(&again, &label);
        }
        inner.len()
    })
    .into_iter()
    .sum();
    let volume_kind = if plan.fs == FsKind::Exfat {
        "exFAT"
    } else {
        "either FAT"
    };
    let mut table = format!(
        "{name}: {} writes, {} barriers, {} cuts ({} leave a journal)\n  every cut: {roots_before} kernel root(s) as \
         before, every file intact through {volume_kind}, recovery ends byte-equal to the uncut run\n  {} of the cuts \
         leaving a journal had their resume cut again (prefixes and half-tears): {inner} more cuts, the same checks\n",
        cuts::writes(&events).len(),
        events.iter().filter(|e| **e == Event::Barrier).count(),
        all.len(),
        pending.len(),
        chosen.len(),
    );
    let mut tally: std::collections::BTreeMap<&str, usize> = std::collections::BTreeMap::new();
    for (_, files) in &rows {
        *tally.entry(files.as_str()).or_default() += 1;
    }
    for (files, n) in tally {
        table += &format!("  Files at {n} cuts: {files}\n");
    }
    if all.len() <= 40 {
        for (cut, (_, files)) in all.iter().zip(&rows) {
            table += &format!("    {:<36} Files {files}\n", cut.label);
        }
    }
    table
}

/// The kernel's Files volume at a cut, beside its unique root: offline (with the word its log gives) or the volume at
/// its old or its new length, nothing else.
fn files_at_cut(state: &mut SparseImage, plan: &Plan, label: &str, name: &str) -> String {
    let Some(root) = oracle::root(state).filter(|r| r.unique) else {
        return "not looked for (no root to boot)".into();
    };
    let want_exfat = plan.fs == FsKind::Exfat;
    match oracle::files(state, root.first, root.count) {
        Files::Offline(why) => format!("offline ({why})"),
        Files::Volume { first, count, exfat } => {
            let old_or_new = count == plan.fat_sectors || count == plan.fat_new_sectors;
            assert!(
                first == plan.fat_first && exfat == want_exfat && old_or_new,
                "{name}: {label}: Files took {first}+{count} (exfat={exfat})"
            );
            let which = if count == plan.fat_new_sectors { "new" } else { "old" };
            let kind = if want_exfat { "exFAT" } else { "FAT" };
            format!("the {kind} at its {which} length")
        }
    }
}

/// `f(0..n)` on every core, results in order.
fn parallel<T: Send>(n: usize, f: impl Fn(usize) -> T + Sync) -> Vec<T> {
    let next = std::sync::atomic::AtomicUsize::new(0);
    let out: Vec<std::sync::Mutex<Option<T>>> = (0..n).map(|_| std::sync::Mutex::new(None)).collect();
    let threads = std::thread::available_parallelism().map_or(4, |t| t.get()).min(12);
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    if i >= n {
                        break;
                    }
                    *out[i].lock().unwrap() = Some(f(i));
                }
            });
        }
    });
    out.into_iter().map(|m| m.into_inner().unwrap().unwrap()).collect()
}

pub fn report(name: &str, table: &str) {
    eprint!("{table}");
    if let Ok(dir) = std::env::var("TEST_UNDECLARED_OUTPUTS_DIR") {
        std::fs::write(format!("{dir}/{name}.txt"), table).unwrap();
    }
}
