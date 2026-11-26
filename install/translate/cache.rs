// SPDX-License-Identifier: GPL-3.0-or-later
//! The SystemOS cryptex's dyld shared caches, the main one and DriverKit's: each planned, then every file translated
//! with its plan, the prebuilt loaders' cdhashes of on-disk binaries replaced, and re-signed (`arm64e dsc-apply`).

use std::ffi::c_char;
use std::sync::{Condvar, Mutex};
use std::time::Instant;

use rayon::prelude::*;
use sha2::{Digest, Sha256};
use switch_trustcache::Entry as TrustEntry;

use crate::buffer::{AnonBuffer, Buffer};
use crate::ffi::{self, CDHASH, CdhashPair};
use crate::manifest::Entry;
use crate::mmap::Mapped;
use crate::output::Output;
use crate::trust::Categories;
use crate::volume::{Backing, Source, Volume};
use crate::{Error, plan};

/// Each cache directory and the infix its plan's name takes after the role's.
pub const CACHES: [(&str, &str); 2] = [
    ("/System/Library/Caches/com.apple.dyld", ""),
    ("/System/DriverKit/System/Library/dyld", ".driverkit"),
];
const MAIN: &str = "dyld_shared_cache_arm64e";
const SUBCACHES: usize = 0x188;
const SUBCACHE_ENTRY: usize = 56;
const CODE_DIRECTORY: [u8; 4] = [0xfa, 0xde, 0x0c, 0x02];
/// Caps concurrent heap output buffers in `apply` so parallel threads do not stack gigabytes of dirty memory.
const APPLY_BUDGET: usize = 512 << 20;

#[derive(Default)]
pub struct Caches {
    /// Every cache file's cdhash after translation, in order: `a64x-cache-pretranslated.bin`.
    pub listed: Vec<[u8; CDHASH]>,
    pub trust: Vec<TrustEntry>,
    pub manifest: Vec<Entry>,
    /// Each plan's report, for the log.
    pub logs: Vec<String>,
}

pub struct Job<'a> {
    pub volume: &'a Volume<'a>,
    pub src: Source<'a>,
    pub backing: Option<Backing<'a>>,
    pub role: &'a str,
    /// Sorted by old cdhash.
    pub pairs: &'a [CdhashPair],
    pub apple: &'a Categories,
    pub out: &'a Output,
}

pub fn translate(job: &Job) -> Result<Caches, Error> {
    let mut caches = Caches::default();
    for (idx, (dir, infix)) in CACHES.into_iter().enumerate() {
        if job.volume.is_dir(dir)? {
            translate_dir(job, idx, dir, infix, &mut caches)?;
        }
    }
    Ok(caches)
}

fn translate_dir(job: &Job, dir_idx: usize, dir: &str, infix: &str, caches: &mut Caches) -> Result<(), Error> {
    let started = Instant::now();
    let (src, backing, sealed) = (job.src, job.backing, job.volume.sealed);
    // Declared before `main` and `files` so every mapping unmaps and deletes its temp file before `.cache-tmp` drops.
    let temp = job.out.temp_dir(&format!(".cache-tmp-{dir_idx}"))?;
    let main = job
        .volume
        .file(&format!("{dir}/{MAIN}"))?
        .ok_or_else(|| Error::Refused(format!("{dir}: no {MAIN}")))?
        .map_contents(src, backing, sealed, temp.path(), "0")?;
    let mut found = Vec::new();
    for suffix in subcache_suffixes(&main).map_err(|e| e.at(dir))? {
        if let Some(f) = job.volume.file(&format!("{dir}/{MAIN}{suffix}"))? {
            found.push((suffix, f));
        }
    }
    let total = found.len();
    let (compressed, plain): (Vec<_>, Vec<_>) =
        found.into_iter().enumerate().partition(|(_, (_, f))| f.is_compressed());
    let plain_mapped = plain
        .into_par_iter()
        .map(|(i, (suffix, f))| {
            let tag = (i + 1).to_string();
            Ok((i, suffix, f.map_contents(src, backing, sealed, temp.path(), &tag)?))
        })
        .collect::<Result<Vec<_>, Error>>()?;
    // Decompress large compressed subcaches one at a time so their transient heap buffers never overlap.
    let mut ordered: Vec<Option<(String, Mapped)>> = (0..total).map(|_| None).collect();
    for (i, suffix, mapped) in plain_mapped {
        ordered[i] = Some((suffix, mapped));
    }
    for (i, (suffix, f)) in compressed {
        let tag = (i + 1).to_string();
        let mapped = f.map_contents(src, backing, sealed, temp.path(), &tag)?;
        ordered[i] = Some((suffix, mapped));
    }
    let mut files: Vec<(String, Mapped)> = Vec::with_capacity(1 + total);
    files.push((String::new(), main));
    for slot in ordered {
        files.push(slot.expect("every subcache slot filled"));
    }
    let size: usize = files.iter().map(|(_, b)| b.len()).sum();
    caches.logs.push(format!(
        "{dir}: {} files, {size} bytes, mapped in {:.1?}",
        files.len(),
        started.elapsed()
    ));

    let started = Instant::now();
    let views: Vec<(&str, &[u8])> = files.iter().map(|(s, b)| (s.as_str(), &b[..])).collect();
    let plan = plan::make(dir, &views)?;
    drop(views);
    job.out.write(&format!("{}{infix}.a64xplan", job.role), &plan.bytes)?;
    caches.logs.push(plan.log);
    caches.logs.push(format!("{dir}: planned in {:.1?}", started.elapsed()));
    let plan = Buffer::from_vec(plan.bytes);
    let started = Instant::now();

    files.retain(|(suffix, _)| !suffix.ends_with(".symbols") && !suffix.ends_with(".atlas"));
    for (_, mapped) in &files {
        mapped.advise_dontneed();
    }
    let (pairs, out) = (job.pairs, job.out);
    let budget = Budget::new();
    let applied = files
        .into_par_iter()
        .map(|(suffix, bytes)| apply(pairs, out, &budget, &format!("{dir}/{MAIN}{suffix}"), &plan, &bytes))
        .collect::<Result<Vec<_>, Error>>()?;
    for (cdhash, changed) in applied {
        caches.listed.push(cdhash);
        if let Some((entry, original)) = changed {
            let (flags, category) = job.apple.get(&original).copied().unwrap_or((0, 0));
            caches.trust.push(TrustEntry {
                cdhash,
                flags,
                category,
            });
            caches.manifest.push(entry);
        }
    }
    caches.logs.push(format!("{dir}: applied in {:.1?}", started.elapsed()));
    Ok(())
}

type Applied = ([u8; CDHASH], Option<(Entry, [u8; CDHASH])>);

struct Budget {
    in_flight: Mutex<usize>,
    ready: Condvar,
}

struct Permit<'a> {
    budget: &'a Budget,
    bytes: usize,
}

impl Budget {
    fn new() -> Self {
        Self {
            in_flight: Mutex::new(0),
            ready: Condvar::new(),
        }
    }

    fn acquire(&self, bytes: usize) -> Permit<'_> {
        let mut used = self.in_flight.lock().unwrap_or_else(|p| p.into_inner());
        while *used > 0 && used.saturating_add(bytes) > APPLY_BUDGET {
            used = self.ready.wait(used).unwrap_or_else(|p| p.into_inner());
        }
        *used = used.saturating_add(bytes);
        Permit { budget: self, bytes }
    }
}

impl Drop for Permit<'_> {
    fn drop(&mut self) {
        let mut used = self.budget.in_flight.lock().unwrap_or_else(|p| p.into_inner());
        *used = used.saturating_sub(self.bytes);
        self.budget.ready.notify_all();
    }
}

fn apply(
    pairs: &[CdhashPair],
    out_dir: &Output,
    budget: &Budget,
    path: &str,
    plan: &[u8],
    input: &Mapped,
) -> Result<Applied, Error> {
    input.advise_sequential();
    let permit = budget.acquire(input.len());
    let mut out = AnonBuffer::zeroed(input.len())?;
    let mut cdhash = [0u8; CDHASH];
    let mut err = [0 as c_char; ffi::ERR_LEN];
    // SAFETY: each buffer is as long as its length, `cdhash` holds CDHASH bytes and `err` ERR_LEN.
    let rc = unsafe {
        ffi::switch_translate_dsc(
            plan.as_ptr(),
            plan.len(),
            input.as_ptr(),
            input.len(),
            pairs.as_ptr(),
            pairs.len(),
            out.as_mut_ptr(),
            cdhash.as_mut_ptr(),
            err.as_mut_ptr(),
        )
    };
    if rc != 0 {
        return Err(Error::Translate {
            path: path.to_owned(),
            message: ffi::message(&err),
        });
    }
    if out[..] == input[..] {
        return Ok((cdhash, None));
    }
    out_dir.write(&format!("files{path}"), &out)?;
    let sha256_out = hex::encode(Sha256::digest(&out[..]));
    drop(out);
    drop(permit);
    let entry = Entry {
        path: path.to_owned(),
        sha256_in: hex::encode(Sha256::digest(&input[..])),
        sha256_out,
        cdhash: vec![hex::encode(cdhash)],
    };
    Ok((cdhash, Some((entry, original_cdhash(input).map_err(|e| e.at(path))?))))
}

fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

/// The suffixes of the subcaches the main cache file names (`dyld_subcache_entry`: suffix at 24, 32 bytes).
fn subcache_suffixes(main: &[u8]) -> Result<Vec<String>, Error> {
    let bad = || Error::Refused("the main cache file's subcache table is cut short".into());
    let (off, count) = (
        u32_at(main, SUBCACHES).ok_or_else(bad)?,
        u32_at(main, SUBCACHES + 4).ok_or_else(bad)?,
    );
    (0..count as usize)
        .map(|i| {
            let at = off as usize + SUBCACHE_ENTRY * i;
            let raw = main.get(at + 24..at + SUBCACHE_ENTRY).ok_or_else(bad)?;
            let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
            String::from_utf8(raw[..end].to_vec()).map_err(|_| Error::Refused("a subcache suffix isn't UTF-8".into()))
        })
        .collect()
}

/// The cdhash of a cache file as Apple signed it: its first CodeDirectory at or after the signature.
fn original_cdhash(data: &[u8]) -> Result<[u8; CDHASH], Error> {
    let bad = || Error::Refused("no CodeDirectory in the cache file's signature".into());
    let sig = data
        .get(0x28..0x30)
        .map(|b| u64::from_le_bytes(b.try_into().expect("eight bytes")))
        .ok_or_else(bad)?;
    let from = usize::try_from(sig).map_err(|_| bad())?;
    let at = data
        .get(from..)
        .and_then(|rest| rest.windows(4).position(|w| w == CODE_DIRECTORY))
        .map(|p| from + p)
        .ok_or_else(bad)?;
    let len = data
        .get(at + 4..at + 8)
        .map(|b| u32::from_be_bytes(b.try_into().expect("four bytes")) as usize)
        .ok_or_else(bad)?;
    let cd = data.get(at..at + len).ok_or_else(bad)?;
    Ok(Sha256::digest(cd)[..CDHASH].try_into().expect("twenty bytes"))
}
