// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use serde::Serialize;
use sha2::{Digest, Sha256};
use switch_seed_crawl::{Function, KERNEL, parse_functions_for_build};

use crate::derive;
use crate::journal::{RenderKey, Stage, kernel_links, parse_kernel, parse_render, parse_specialization};
use crate::{PlanError, Result};

pub const DERIVED_MARKER: &str = "# derived";
pub const MAX_PLAN_BUCKETS: usize = 4096;
const APPLICATION_PREFIX: &str = "lib=/Applications/";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Depth {
    Corpus,
    Wide,
    Deep,
}

impl Depth {
    pub fn parse(s: &str) -> Result<Self> {
        match s {
            "corpus" => Ok(Self::Corpus),
            "wide" => Ok(Self::Wide),
            "deep" => Ok(Self::Deep),
            _ => Err(PlanError::Invalid(format!("unknown depth {s:?}"))),
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Corpus => "corpus",
            Self::Wide => "wide",
            Self::Deep => "deep",
        }
    }
}

fn collect_files_sorted(dir: &Path, out: &mut Vec<PathBuf>) -> Result<()> {
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if path.is_dir() {
            collect_files_sorted(&path, out)?;
        } else if path.is_file() {
            out.push(path);
        }
    }
    Ok(())
}

pub type LibraryIndex = (BTreeMap<String, Vec<Function>>, BTreeMap<String, String>);
pub type BucketPlan = (Vec<Vec<String>>, BTreeMap<(String, char), usize>);

pub fn index_libraries(root: &Path, data_files: &[PathBuf], build: Option<&str>) -> Result<LibraryIndex> {
    let mut root_files = Vec::new();
    collect_files_sorted(root, &mut root_files)?;
    root_files.sort();

    let mut index = BTreeMap::new();
    let mut by_sha = BTreeMap::new();
    for path in root_files {
        let content = fs::read(&path)?;
        let digest = hex::encode(Sha256::digest(&content));
        by_sha.insert(digest, path.to_string_lossy().into_owned());
        if let Some(found) = parse_functions_for_build(&content, build)?
            && !found.is_empty()
        {
            let rel = path
                .strip_prefix(root)
                .map_err(|e| PlanError::Invalid(e.to_string()))?
                .to_string_lossy()
                .replace('\\', "/");
            index.insert(format!("/{rel}"), found);
        }
    }
    for path in data_files {
        let content = fs::read(path)?;
        let digest = hex::encode(Sha256::digest(&content));
        by_sha.insert(digest.clone(), path.to_string_lossy().into_owned());
        if let Some(found) = parse_functions_for_build(&content, build)?
            && !found.is_empty()
        {
            index.insert(format!("data:{digest}"), found);
        }
    }
    Ok((index, by_sha))
}

pub fn lines_for_depth(
    depth: Depth,
    corpus: &[String],
    index: &BTreeMap<String, Vec<Function>>,
) -> Vec<(&'static str, Vec<String>)> {
    let corpus_lines: Vec<String> = corpus
        .iter()
        .filter(|line| matches!(line.get(..2), Some("S " | "R " | "C " | "D ")))
        .cloned()
        .collect();
    let mut tiers = vec![("corpus", corpus_lines)];
    if depth == Depth::Corpus {
        return tiers;
    }
    let renders: Vec<_> = corpus.iter().filter_map(|l| parse_render(l)).collect();
    let non_d: Vec<&str> = corpus
        .iter()
        .filter(|l| !l.starts_with("D "))
        .map(String::as_str)
        .collect();
    tiers.push(("kernels", derive::kernels(index, non_d)));
    tiers.push((
        "targets",
        derive::targets(&renders).into_iter().map(|r| r.text()).collect(),
    ));
    if depth == Depth::Deep {
        tiers.push((
            "neighbours",
            derive::neighbours(&renders).into_iter().map(|r| r.text()).collect(),
        ));
    }
    tiers
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
enum LineIdentity {
    D(String),
    R(RenderKey),
    C(String, Stage, Vec<(String, Stage)>),
    S(Option<Stage>),
}

fn line_identity(line: &str) -> LineIdentity {
    if line.starts_with("D ") {
        return LineIdentity::D(line.to_owned());
    }
    if let Some(render) = parse_render(line) {
        return LineIdentity::R(render.key());
    }
    if let Some((fields, kernel)) = parse_kernel(line) {
        return LineIdentity::C(fields, kernel, kernel_links(line));
    }
    LineIdentity::S(parse_specialization(line))
}

fn line_program(line: &str) -> String {
    if let Some(render) = parse_render(line) {
        let mut parts = vec![
            render.vertex.text(),
            render.fragment.as_ref().map_or_else(|| "-".to_owned(), Stage::text),
        ];
        for (tag, stage) in &render.linked {
            parts.push(format!("{tag} {}", stage.text()));
        }
        return parts.join(" | ");
    }
    if let Some((_, kernel)) = parse_kernel(line) {
        let mut parts = vec![kernel.text()];
        for (tag, stage) in kernel_links(line) {
            parts.push(format!("{tag} {}", stage.text()));
        }
        return parts.join(" | ");
    }
    parse_specialization(line).map_or_else(|| line.to_owned(), |stage| stage.text())
}

pub fn bucket_lines(tiers: &[(&str, Vec<String>)], count: usize, apps: usize) -> Result<BucketPlan> {
    if count == 0 || count > MAX_PLAN_BUCKETS || apps > MAX_PLAN_BUCKETS {
        return Err(PlanError::Invalid(format!(
            "invalid bucket counts: count={count}, apps={apps} (must be 1..={MAX_PLAN_BUCKETS} and 0..={MAX_PLAN_BUCKETS})"
        )));
    }
    let total = count
        .checked_add(apps)
        .filter(|&n| n <= MAX_PLAN_BUCKETS * 2)
        .ok_or_else(|| PlanError::Invalid("bucket count overflow".into()))?;
    let mut observed: Vec<Vec<String>> = vec![Vec::new(); total];
    let mut derived: Vec<Vec<String>> = vec![Vec::new(); total];
    let mut seen: HashSet<LineIdentity> = HashSet::new();
    let mut kept: BTreeMap<(String, char), usize> = BTreeMap::new();

    for (tier, lines) in tiers {
        for line in lines {
            let identity = line_identity(line);
            if !seen.insert(identity) {
                continue;
            }
            let kind_char = line.chars().next().unwrap_or(' ');
            *kept.entry(((*tier).to_owned(), kind_char)).or_insert(0) += 1;
            let program = line_program(line);
            let digest = Sha256::digest(program.as_bytes());
            let number = u32::from_le_bytes(digest[..4].try_into().expect("4 bytes")) as usize;
            let idx = if apps > 0 && program.contains(APPLICATION_PREFIX) {
                count + (number % apps)
            } else {
                number % count
            };
            if *tier == "corpus" {
                observed[idx].push(line.clone());
            } else {
                derived[idx].push(line.clone());
            }
        }
    }

    let buckets = observed
        .into_iter()
        .zip(derived)
        .map(|(mut obs, der)| {
            obs.push(DERIVED_MARKER.to_owned());
            obs.extend(der);
            obs
        })
        .collect();
    Ok((buckets, kept))
}

#[derive(Clone, Debug)]
pub struct PlanConfig {
    pub build: Option<String>,
    pub corpus: PathBuf,
    pub root: PathBuf,
    pub data_files: Vec<PathBuf>,
    pub data_dirs: Vec<PathBuf>,
    pub qc: PathBuf,
    pub qc_sha256: String,
    pub depth: Depth,
    pub buckets_dir: PathBuf,
    pub count: usize,
    pub store: PathBuf,
    pub report: PathBuf,
    pub input_reports: Vec<PathBuf>,
    pub apps: usize,
    pub base_root: Option<PathBuf>,
    pub base_data_dirs: Vec<PathBuf>,
    pub base_store: Option<PathBuf>,
}

#[derive(Clone, Debug, Serialize)]
pub struct PlanReport {
    pub depth: String,
    pub libraries: usize,
    pub kernels: usize,
    pub lines: BTreeMap<String, usize>,
    pub buckets: Vec<usize>,
    pub inputs: Vec<serde_json::Value>,
}

fn metallibs_in_dirs(dirs: &[PathBuf]) -> Result<Vec<PathBuf>> {
    let mut found = Vec::new();
    for dir in dirs {
        if !dir.exists() {
            continue;
        }
        for entry in fs::read_dir(dir)? {
            let path = entry?.path();
            if path.is_file() && path.extension().and_then(|e| e.to_str()) == Some("metallib") {
                found.push(path);
            }
        }
    }
    found.sort();
    Ok(found)
}

fn format_store_json(by_sha: &BTreeMap<String, String>) -> Result<String> {
    if by_sha.is_empty() {
        return Ok("{}\n".to_owned());
    }
    let mut out = String::from("{\n");
    for (idx, (k, v)) in by_sha.iter().enumerate() {
        if idx > 0 {
            out.push_str(",\n");
        }
        out.push_str(&serde_json::to_string(k)?);
        out.push_str(": ");
        out.push_str(&serde_json::to_string(v)?);
    }
    out.push_str("\n}\n");
    Ok(out)
}

pub fn run_plan(cfg: &PlanConfig) -> Result<PlanReport> {
    let qc_bytes = fs::read(&cfg.qc)?;
    let qc_digest = hex::encode(Sha256::digest(&qc_bytes));
    if qc_digest != cfg.qc_sha256 {
        return Err(PlanError::Invalid(format!(
            "{} is not the IPSW kind's QuartzCore default.metallib ({})",
            cfg.qc.display(),
            cfg.qc_sha256
        )));
    }
    let mut data = cfg.data_files.clone();
    data.extend(metallibs_in_dirs(&cfg.data_dirs)?);
    let (index, by_sha) = index_libraries(&cfg.root, &data, cfg.build.as_deref())?;
    let corpus_text = fs::read_to_string(&cfg.corpus)?;
    let corpus: Vec<String> = corpus_text.lines().map(str::to_owned).collect();
    let tiers = lines_for_depth(cfg.depth, &corpus, &index);
    let (buckets, kept) = bucket_lines(&tiers, cfg.count, cfg.apps)?;

    fs::create_dir_all(&cfg.buckets_dir)?;
    for (i, lines) in buckets.iter().enumerate() {
        let mut text = String::new();
        for line in lines {
            text.push_str(line);
            text.push('\n');
        }
        fs::write(cfg.buckets_dir.join(format!("bucket-{i:03}.log")), text)?;
    }
    if let Some(parent) = cfg.store.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(&cfg.store, format_store_json(&by_sha)?)?;

    if let (Some(base_store), Some(base_root)) = (&cfg.base_store, &cfg.base_root) {
        let mut base_data = cfg.data_files.clone();
        base_data.extend(metallibs_in_dirs(&cfg.base_data_dirs)?);
        let (_, base_sha) = index_libraries(base_root, &base_data, cfg.build.as_deref())?;
        if let Some(parent) = base_store.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(base_store, format_store_json(&base_sha)?)?;
    }

    let mut inputs = Vec::with_capacity(cfg.input_reports.len());
    for path in &cfg.input_reports {
        inputs.push(serde_json::from_str(&fs::read_to_string(path)?)?);
    }
    let report = PlanReport {
        depth: cfg.depth.as_str().to_owned(),
        libraries: index.len(),
        kernels: index
            .values()
            .flat_map(|fs| fs.iter())
            .filter(|f| f.kind == KERNEL)
            .count(),
        lines: kept
            .into_iter()
            .map(|((tier, kind), n)| (format!("{tier}.{kind}"), n))
            .collect(),
        buckets: buckets.iter().map(|b| b.len().saturating_sub(1)).collect(),
        inputs,
    };
    let mut buf = Vec::new();
    let formatter = serde_json::ser::PrettyFormatter::with_indent(b" ");
    let mut ser = serde_json::Serializer::with_formatter(&mut buf, formatter);
    report.serialize(&mut ser)?;
    buf.push(b'\n');
    if let Some(parent) = cfg.report.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(&cfg.report, buf)?;
    Ok(report)
}
