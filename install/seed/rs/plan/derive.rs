// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, BTreeSet};

use switch_seed_crawl::{Constant, Function, KERNEL};

use crate::journal::{
    Render, Stage, compute_line, constant_key, constant_type_str, ordered, parse_kernel, parse_specialization,
};

pub const NEIGHBOURS: usize = 32;
pub const KERNEL_VARIANTS: usize = 4;
const SHARED_RATIO: f64 = 0.9;
const BOOL_TYPE: u8 = 53;
const PLAIN_FIELDS: &str = "form=f opt=0";

fn spelled(constant: &Constant, by_index: bool) -> String {
    if by_index {
        format!("#{}", constant.index)
    } else {
        constant.name.clone()
    }
}

#[derive(Default)]
struct ObservedLibrary {
    kernels: BTreeSet<String>,
    pipelines: BTreeSet<String>,
    fields: Vec<(String, usize)>,
    made: Vec<Vec<String>>,
    values: BTreeMap<String, BTreeSet<String>>,
}

impl ObservedLibrary {
    fn see(&mut self, fields: Option<&str>, kernel: &Stage) {
        self.kernels.insert(kernel.name.clone());
        if let Some(f) = fields {
            self.pipelines.insert(kernel.name.clone());
            if let Some(entry) = self.fields.iter_mut().find(|(k, _)| k == f) {
                entry.1 += 1;
            } else {
                self.fields.push((f.to_owned(), 1));
            }
        }
        self.made.push(kernel.constants.clone());
        for token in &kernel.constants {
            self.values
                .entry(constant_key(token).to_owned())
                .or_default()
                .insert(token.clone());
        }
    }

    fn common_fields(&self) -> &str {
        // Ties break by first insertion order, matching Python's Counter.most_common(1).
        let mut best: Option<(&str, usize)> = None;
        for (fields, count) in &self.fields {
            if best.is_none_or(|(_, best_n)| *count > best_n) {
                best = Some((fields.as_str(), *count));
            }
        }
        best.map_or(PLAIN_FIELDS, |(f, _)| f)
    }

    fn by_index(&self) -> bool {
        let mut total = 0usize;
        let mut indexed = 0usize;
        for made in &self.made {
            for token in made {
                total += 1;
                if constant_key(token).starts_with('#') {
                    indexed += 1;
                }
            }
        }
        indexed * 2 > total
    }

    fn shared(&self) -> Vec<String> {
        if self.made.len() < 3 {
            return Vec::new();
        }
        let mut counts: BTreeMap<&str, usize> = BTreeMap::new();
        for made in &self.made {
            let unique: BTreeSet<&str> = made.iter().map(String::as_str).collect();
            for token in unique {
                *counts.entry(token).or_insert(0) += 1;
            }
        }
        let threshold = SHARED_RATIO * (self.made.len() as f64);
        counts
            .into_iter()
            .filter(|(_, n)| (*n as f64) >= threshold)
            .map(|(t, _)| t.to_owned())
            .collect()
    }

    fn variants(&self, function: &Function) -> Vec<Vec<String>> {
        let by_index = self.by_index();
        let shared = self.shared();
        let mut choices: Vec<Vec<String>> = Vec::new();
        for constant in function.required_constants() {
            let key = spelled(constant, by_index);
            let want_type = constant.data_type.to_string();
            let mut seen: Vec<String> = self
                .values
                .get(&key)
                .map(|set| {
                    set.iter()
                        .filter(|t| constant_type_str(t) == Some(want_type.as_str()))
                        .cloned()
                        .collect()
                })
                .unwrap_or_default();
            if seen.is_empty() && constant.data_type == BOOL_TYPE {
                seen.push(format!("{key}:{BOOL_TYPE}:00"));
                seen.push(format!("{key}:{BOOL_TYPE}:01"));
            }
            if seen.is_empty() {
                return Vec::new();
            }
            choices.push(seen);
        }

        let mut combos: Vec<Vec<String>> = vec![Vec::new()];
        for options in &choices {
            let mut next = Vec::new();
            'outer: for prefix in &combos {
                for opt in options {
                    let mut row = prefix.clone();
                    row.push(opt.clone());
                    next.push(row);
                    if next.len() >= KERNEL_VARIANTS {
                        break 'outer;
                    }
                }
            }
            combos = next;
        }
        combos.truncate(KERNEL_VARIANTS);

        let mut out = Vec::with_capacity(combos.len());
        for combo in combos {
            let mut given: BTreeMap<String, String> = BTreeMap::new();
            for token in &shared {
                given.insert(constant_key(token).to_owned(), token.clone());
            }
            for token in combo {
                given.insert(constant_key(&token).to_owned(), token);
            }
            let values: Vec<String> = given.into_values().collect();
            out.push(ordered(&values));
        }
        out
    }
}

fn observed<'a>(
    index: &BTreeMap<String, Vec<Function>>,
    corpus: impl IntoIterator<Item = &'a str>,
) -> (BTreeMap<String, ObservedLibrary>, Vec<Stage>) {
    let mut kinds: BTreeMap<(&str, &str), u8> = BTreeMap::new();
    for (lib, funcs) in index {
        for f in funcs {
            kinds.insert((lib.as_str(), f.name.as_str()), f.kind);
        }
    }
    let mut libraries: BTreeMap<String, ObservedLibrary> = BTreeMap::new();
    let mut specialized = Vec::new();
    for line in corpus {
        if let Some((fields, stage)) = parse_kernel(line)
            && let Some(lib) = &stage.lib
        {
            libraries.entry(lib.clone()).or_default().see(Some(&fields), &stage);
            continue;
        }
        if let Some(stage) = parse_specialization(line)
            && let Some(lib) = &stage.lib
            && kinds.get(&(lib.as_str(), stage.name.as_str())) == Some(&KERNEL)
        {
            specialized.push(stage);
        }
    }
    for stage in &specialized {
        if let Some(lib) = &stage.lib {
            libraries.entry(lib.clone()).or_default().see(None, stage);
        }
    }
    (libraries, specialized)
}

pub fn kernels<'a>(index: &BTreeMap<String, Vec<Function>>, corpus: impl IntoIterator<Item = &'a str>) -> Vec<String> {
    let (libraries, specialized) = observed(index, corpus);
    let mut out = Vec::new();
    for stage in &specialized {
        if let Some(lib) = &stage.lib
            && let Some(obs) = libraries.get(lib)
            && !obs.pipelines.contains(&stage.name)
        {
            out.push(compute_line(obs.common_fields(), stage));
        }
    }
    for (lib, funcs) in index {
        let seen = libraries.get(lib);
        for func in funcs {
            if func.kind != KERNEL || seen.is_some_and(|s| s.kernels.contains(&func.name)) {
                continue;
            }
            let stage = Stage::new(func.name.clone(), Some(lib.clone()));
            let Some(obs) = seen else {
                if func.required_constants().is_empty() {
                    out.push(compute_line(PLAIN_FIELDS, &stage));
                }
                continue;
            };
            let variants = if !func.required_constants().is_empty() || !obs.shared().is_empty() {
                obs.variants(func)
            } else {
                vec![Vec::new()]
            };
            let fields = obs.common_fields();
            for variant in variants {
                let s = if variant.is_empty() {
                    stage.clone()
                } else {
                    stage.specialized(&variant)
                };
                out.push(compute_line(fields, &s));
            }
        }
    }
    out
}

type TargetKey<'a> = (Option<&'a str>, &'a str, usize);

pub fn targets(renders: &[Render]) -> Vec<Render> {
    let mut seen: BTreeMap<TargetKey<'_>, BTreeSet<Vec<String>>> = BTreeMap::new();
    for r in renders {
        let frag_name = r.fragment.as_ref().map_or("-", |f| f.name.as_str());
        seen.entry((r.vertex.lib.as_deref(), frag_name, r.colors()))
            .or_default()
            .insert(r.target.clone());
    }
    let mut out = Vec::new();
    for r in renders {
        let frag_name = r.fragment.as_ref().map_or("-", |f| f.name.as_str());
        if let Some(target_set) = seen.get(&(r.vertex.lib.as_deref(), frag_name, r.colors())) {
            for target in target_set {
                if target != &r.target {
                    out.push(Render {
                        target: target.clone(),
                        layout: r.layout.clone(),
                        vertex: r.vertex.clone(),
                        fragment: r.fragment.clone(),
                        label: r.label.clone(),
                        linked: r.linked.clone(),
                    });
                }
            }
        }
    }
    out
}

fn stage_with_constant(stage: Option<&Stage>, key: &str, token: &str) -> Option<Stage> {
    let stage = stage?;
    if !stage.constants.iter().any(|t| constant_key(t) == key) {
        return Some(stage.clone());
    }
    Some(Stage {
        name: stage.name.clone(),
        lib: stage.lib.clone(),
        spec: true,
        constants: stage
            .constants
            .iter()
            .map(|t| {
                if constant_key(t) == key {
                    token.to_owned()
                } else {
                    t.clone()
                }
            })
            .collect(),
        specialized_name: stage.specialized_name.clone(),
    })
}

pub fn neighbours(renders: &[Render]) -> Vec<Render> {
    let mut values: BTreeMap<(&str, Option<&str>, &str), BTreeSet<&str>> = BTreeMap::new();
    for r in renders {
        for stage in std::iter::once(&r.vertex).chain(r.fragment.iter()) {
            for token in &stage.constants {
                values
                    .entry((stage.name.as_str(), stage.lib.as_deref(), constant_key(token)))
                    .or_default()
                    .insert(token.as_str());
            }
        }
    }
    let mut out = Vec::new();
    for r in renders {
        let mut made = 0usize;
        let mut keys: BTreeSet<&str> = BTreeSet::new();
        for stage in std::iter::once(&r.vertex).chain(r.fragment.iter()) {
            for token in &stage.constants {
                keys.insert(constant_key(token));
            }
        }
        let r_key = r.key();
        for key in keys {
            let mut pool: BTreeSet<&str> = BTreeSet::new();
            for stage in std::iter::once(&r.vertex).chain(r.fragment.iter()) {
                if let Some(vs) = values.get(&(stage.name.as_str(), stage.lib.as_deref(), key)) {
                    pool.extend(vs.iter().copied());
                }
            }
            for token in pool {
                let derived = Render {
                    target: r.target.clone(),
                    layout: r.layout.clone(),
                    vertex: stage_with_constant(Some(&r.vertex), key, token).expect("vertex present"),
                    fragment: stage_with_constant(r.fragment.as_ref(), key, token),
                    label: r.label.clone(),
                    linked: r.linked.clone(),
                };
                if derived.key() != r_key && made < NEIGHBOURS {
                    out.push(derived);
                    made += 1;
                }
            }
        }
    }
    out
}
