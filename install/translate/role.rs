// SPDX-License-Identifier: GPL-3.0-or-later
//! One role's delta: translate_tree.py's main() over a volume read in process.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::time::Instant;

use rayon::prelude::*;
use sha2::{Digest, Sha256};
use switch_trustcache::{Entry as TrustEntry, trust_module};

use crate::cache::{self, CACHES};
use crate::edits::Edit;
use crate::ffi::{CDHASH, CdhashPair};
use crate::manifest::{Entry, Manifest};
use crate::output::Output;
use crate::trust::{self, Pins};
use crate::volume::{File, Source, Volume};
use crate::{Error, macho};

const CACHE_LIST: &str = "a64x-cache-pretranslated.bin";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Os,
    AppCryptex,
    SystemCryptex,
}

impl Role {
    pub fn name(self) -> &'static str {
        match self {
            Role::Os => "os",
            Role::AppCryptex => "app_cryptex",
            Role::SystemCryptex => "system_cryptex",
        }
    }

    pub fn parse(name: &str) -> Option<Self> {
        [Role::Os, Role::AppCryptex, Role::SystemCryptex]
            .into_iter()
            .find(|r| r.name() == name)
    }
}

pub struct Inputs<'a> {
    pub role: Role,
    /// The decoded member: the OS image or the cryptex.
    pub source: Source<'a>,
    /// The OS image's System volume, from the IPSW's pins; a cryptex has one volume.
    pub volume: Option<[u8; 16]>,
    /// Apple's trust cache modules (IM4P payloads): the static one, then the SystemOS cryptex's for that role.
    pub apple_trust: &'a [Vec<u8>],
    /// The checked byte edits, by volume path.
    pub edits: &'a BTreeMap<String, Vec<Edit>>,
    /// The other roles' cdhash pairs, which the SystemOS cryptex's prebuilt loaders take: the OS's, then AppOS's.
    pub pairs: &'a [CdhashPair],
    /// The translator version the manifest records, as the release publishes it.
    pub translator: &'a str,
    pub pins: &'a Pins,
}

pub struct Delta {
    pub dir: PathBuf,
    /// This role's (old, new) cdhash of each translated slice.
    pub pairs: Vec<CdhashPair>,
    pub files: usize,
    /// Counts, timings and each plan's report.
    pub log: Vec<String>,
}

/// The role's delta in `out`, which must not exist yet: the translated files under `files/`, the manifest, the trust
/// module, the cdhash pairs and, for the SystemOS cryptex, each cache's plan and the cache list. Nothing is left in
/// `out` unless the pins accept the result.
pub fn translate(inputs: &Inputs, out: &Path) -> Result<Delta, Error> {
    let output = Output::create(out)?;
    let made = run(inputs, &output)?;
    let dir = output.commit()?;
    Ok(Delta { dir, ..made })
}

struct Translated {
    entry: Entry,
    slices: Vec<macho::Slice>,
}

/// A Mach-O candidate's outcome: None for a file that isn't one.
type Outcome = Option<(String, Option<Translated>)>;

fn run(inputs: &Inputs, output: &Output) -> Result<Delta, Error> {
    let role = inputs.role.name();
    let mut log = Vec::new();
    let start = Instant::now();
    let volume = Volume::open(inputs.source, inputs.volume)?;
    let skip: Vec<&str> = CACHES.iter().map(|(dir, _)| *dir).collect();
    let mut files = volume.files(&skip)?;
    files.sort_by(|a, b| a.path.split('/').cmp(b.path.split('/')));
    log.push(format!(
        "{role}: {} files walked in {:.1?}",
        files.len(),
        start.elapsed()
    ));

    let apple = trust::categories(inputs.apple_trust)?;
    let sealed = volume.sealed;
    let started = Instant::now();
    let outcomes = files
        .par_iter()
        .map(|f| translate_file(inputs, sealed, f, output))
        .collect::<Result<Vec<Outcome>, Error>>()?;
    let machos: BTreeSet<&str> = outcomes.iter().flatten().map(|(p, _)| p.as_str()).collect();
    if let Some(missing) = inputs.edits.keys().find(|p| !machos.contains(p.as_str())) {
        return Err(Error::Refused(format!(
            "{role}: edit target {missing} not found in the volume"
        )));
    }
    let candidates = machos.len();

    let mut manifest = Manifest {
        role,
        translator: inputs.translator,
        files: Vec::new(),
    };
    let mut pairs = Vec::new();
    let mut trust = Vec::new();
    let mut no_category = 0;
    for t in outcomes.into_iter().flatten().filter_map(|(_, t)| t) {
        for s in &t.slices {
            pairs.push(CdhashPair {
                old: s.cdhash_in,
                now: s.cdhash_out,
            });
            let (flags, category) = apple.get(&s.cdhash_in).copied().unwrap_or_else(|| {
                no_category += 1;
                (0, 0)
            });
            trust.push(TrustEntry {
                cdhash: s.cdhash_out,
                flags,
                category,
            });
        }
        manifest.files.push(t.entry);
    }
    log.push(format!(
        "{role}: {} Mach-O files, {} translated in {:.1?}; {no_category} binaries not in Apple's trust caches",
        candidates,
        manifest.files.len(),
        started.elapsed()
    ));
    output.write(&format!("{role}.cdhash-pairs"), pair_lines(&pairs).as_bytes())?;

    let mut cache_list = Vec::new();
    if inputs.role == Role::SystemCryptex {
        let started = Instant::now();
        let mut all: Vec<CdhashPair> = pairs.iter().chain(inputs.pairs).copied().collect();
        all.sort();
        all.dedup();
        if let Some(w) = all.windows(2).find(|w| w[0].old == w[1].old) {
            return Err(Error::Refused(format!(
                "{} has two translations",
                hex::encode(w[0].old)
            )));
        }
        let job = cache::Job {
            volume: &volume,
            src: inputs.source,
            role,
            pairs: &all,
            apple: &apple,
            out: output,
        };
        let caches = cache::translate(&job)?;
        cache_list = caches.listed.concat();
        manifest.files.extend(caches.manifest);
        trust.extend(caches.trust);
        log.extend(caches.logs);
        log.push(format!("{role}: dyld caches in {:.1?}", started.elapsed()));
        if !cache_list.is_empty() {
            output.write(CACHE_LIST, &cache_list)?;
        }
    }

    let module = trust_module(trust, None)?;
    output.write(&format!("{role}.manifest.json"), manifest.to_json().as_bytes())?;
    output.write(&format!("{role}.trustcache"), &module)?;
    inputs.pins.check(&format!("{role}.trustcache"), &module)?;
    if inputs.role == Role::SystemCryptex {
        inputs.pins.check(CACHE_LIST, &cache_list)?;
    }
    log.push(format!("{role}: done in {:.1?}", start.elapsed()));
    Ok(Delta {
        dir: PathBuf::new(),
        pairs,
        files: manifest.files.len(),
        log,
    })
}

fn translate_file(inputs: &Inputs, sealed: bool, f: &File, output: &Output) -> Result<Outcome, Error> {
    let Some(bytes) = f.macho_contents(inputs.source, sealed)? else {
        return Ok(None);
    };
    let edits = inputs.edits.get(&f.path).map_or(&[][..], Vec::as_slice);
    let t = macho::translate(&f.path, &bytes, edits)?;
    #[cfg(switch_translate_perturb)]
    let t = crate::perturb::translated(t)?;
    let slices: Vec<macho::Slice> = t.translated().copied().collect();
    if slices.is_empty() {
        if !edits.is_empty() {
            return Err(Error::Refused(format!("{}: edited, but not translated", f.path)));
        }
        return Ok(Some((f.path.clone(), None)));
    }
    let entry = Entry {
        path: f.path.clone(),
        sha256_in: hex::encode(Sha256::digest(&bytes[..])),
        sha256_out: hex::encode(Sha256::digest(&t.bytes[..])),
        cdhash: slices.iter().map(|s| hex::encode(s.cdhash_out)).collect(),
    };
    output.write(&entry.file(), &t.bytes)?;
    Ok(Some((f.path.clone(), Some(Translated { entry, slices }))))
}

/// `<role>.cdhash-pairs`: "OLD NEW" in hex, a line each.
pub fn pair_lines(pairs: &[CdhashPair]) -> String {
    pairs
        .iter()
        .map(|p| format!("{} {}\n", hex::encode(p.old), hex::encode(p.now)))
        .collect()
}

/// Reads `<role>.cdhash-pairs`.
pub fn parse_pairs(text: &str) -> Result<Vec<CdhashPair>, Error> {
    let bad = |line: &str| Error::Refused(format!("a cdhash pair line {line:?}"));
    text.lines()
        .map(|line| {
            let mut words = line.split_whitespace();
            let (Some(old), Some(now), None) = (words.next(), words.next(), words.next()) else {
                return Err(bad(line));
            };
            let cdhash = |h: &str| -> Option<[u8; CDHASH]> { hex::decode(h).ok()?.try_into().ok() };
            Ok(CdhashPair {
                old: cdhash(old).ok_or_else(|| bad(line))?,
                now: cdhash(now).ok_or_else(|| bad(line))?,
            })
        })
        .collect()
}
