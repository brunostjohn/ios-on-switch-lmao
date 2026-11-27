// SPDX-License-Identifier: GPL-3.0-or-later
//! `native_prelink.plan_original`: the original kernelcache's closure, extents and pointer sites.

use std::collections::{BTreeSet, HashMap};

use serde::Deserialize;

use super::{Original, slid};
use crate::error::{Result, refused};
use crate::info::{self, Dict, Value};
use crate::macho::{self, Image, MH_EXECUTE, MH_KEXT_BUNDLE, ORDINARY};

/// The original kernelcache, its prelink info read.
pub(crate) struct Kernelcache<'a> {
    pub data: &'a [u8],
    pub image: Image,
    pub entries: Vec<Dict>,
    index: HashMap<String, usize>,
    kaslr_offsets: Vec<u8>,
}

impl<'a> Kernelcache<'a> {
    pub fn read(data: &'a [u8]) -> Result<Self> {
        let image = Image::parse(data, 0, Some(ORDINARY))?;
        if image.kind != MH_EXECUTE {
            return refused("Expected MH_EXECUTE kernel images");
        }
        let segment = image.segment("__PRELINK_INFO")?;
        let at = image.offset_of(segment.va, segment.filesize, data.len())?;
        let Value::Dict(mut metadata) = info::unserialize(&data[at..at + segment.filesize as usize])? else {
            return refused("Prelink info is not a dictionary");
        };
        let Some(Value::Array(list)) = metadata.remove("_PrelinkInfoDictionary") else {
            return refused("Prelink info has no _PrelinkInfoDictionary");
        };
        let Some(Value::Data(kaslr_offsets)) = metadata.remove("_PrelinkLinkKASLROffsets") else {
            return refused("Prelink info has no _PrelinkLinkKASLROffsets");
        };
        let (mut entries, mut index) = (Vec::with_capacity(list.len()), HashMap::new());
        for entry in list {
            let Value::Dict(entry) = entry else {
                return refused("A prelink info entry that is not a dictionary");
            };
            if index
                .insert(entry.string("CFBundleIdentifier")?.to_owned(), entries.len())
                .is_some()
            {
                return refused("Duplicate prelink identifier");
            }
            entries.push(entry);
        }
        Ok(Kernelcache {
            data,
            image,
            entries,
            index,
            kaslr_offsets,
        })
    }

    pub fn get(&self, identifier: &str) -> Option<&Dict> {
        self.index.get(identifier).map(|&i| &self.entries[i])
    }

    pub fn entry(&self, identifier: &str) -> Result<&Dict> {
        self.get(identifier)
            .map_or_else(|| refused(format!("Missing declared dependency {identifier}")), Ok)
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Extent {
    pub module: String,
    pub segment: String,
    pub va: u64,
    pub size: u64,
}

pub(crate) enum Target {
    Component(u64),
    Kernel { target: u64, names: Vec<String> },
    Cut { component: String, dependency: String },
}

pub(crate) struct Binding {
    pub new_site: u64,
    pub target: Target,
}

/// What `plan_original` finds: the sorted closure, its extents and its pointers, and the selected modules in
/// identifier order.
pub(crate) struct Closure {
    pub components: Vec<String>,
    pub codeless: Vec<String>,
    pub interfaces: Vec<String>,
    pub extents: Vec<Extent>,
    pub bindings: Vec<Binding>,
    pub modules: Vec<(String, Image)>,
}

struct Owned {
    start: u64,
    end: u64,
    module: String,
    segment: String,
}

pub(crate) fn plan_original(cache: &Kernelcache, original: &Original) -> Result<Closure> {
    if original.slide.rem_euclid(0x4000) != 0 {
        return refused("Rebasing must keep the segments on whole VM pages (16 KiB)");
    }
    for (component, dependency) in original.cuts.keys() {
        if !cache
            .get(component)
            .and_then(|e| e.dict("OSBundleLibraries"))
            .is_some_and(|d| d.contains(dependency))
        {
            return refused(format!("Cut {component} -> {dependency}: not a declared dependency"));
        }
    }
    let (selected, interfaces) = closure(cache, original)?;
    let (owned, mut modules) = component_segments(cache)?;
    let codeless: BTreeSet<&String> = selected.iter().filter(|id| !modules.contains_key(*id)).collect();
    check_codeless(cache, &codeless)?;
    let owners = Owners::new(owned)?;
    let mut binder = Binder {
        owners: &owners,
        selected: &selected,
        original,
        symbols: macho::exports(cache.data, &cache.image)?,
        bindings: Vec::new(),
        cut_sites: HashMap::new(),
    };
    let base = cache.image.segment("__PRELINK_TEXT")?.va;
    for offset in pointer_offsets(&cache.kaslr_offsets)? {
        binder.bind_site(cache, base.checked_add(offset.into()))?;
    }
    for ((component, dependency), cut) in &original.cuts {
        let found = binder
            .cut_sites
            .get(&(component.as_str(), dependency.as_str()))
            .copied()
            .unwrap_or(0);
        if selected.contains(component) && i128::from(found) != cut.sites {
            return refused(format!(
                "Cut {component} -> {dependency}: {found} pointer sites, the allow-list says {}",
                cut.sites
            ));
        }
    }
    let bindings = binder.bindings;
    let extents = rebased_extents(&owners, &selected, original.slide)?;
    let components: Vec<String> = selected.iter().filter(|id| !codeless.contains(id)).cloned().collect();
    let modules = components
        .iter()
        .map(|id| (id.clone(), modules.remove(id).unwrap()))
        .collect();
    Ok(Closure {
        components,
        codeless: codeless.into_iter().cloned().collect(),
        interfaces: interfaces.into_iter().collect(),
        extents,
        bindings,
        modules,
    })
}

fn closure(cache: &Kernelcache, original: &Original) -> Result<(BTreeSet<String>, BTreeSet<String>)> {
    let (mut selected, mut interfaces) = (BTreeSet::new(), BTreeSet::new());
    let mut pending: Vec<String> = original.roots.clone();
    while let Some(identifier) = pending.pop() {
        if selected.contains(&identifier) || interfaces.contains(&identifier) {
            continue;
        }
        let entry = cache.entry(&identifier)?;
        if entry.is_true("OSKernelResource") {
            interfaces.insert(identifier);
            continue;
        }
        if let Some(libraries) = entry.get("OSBundleLibraries").filter(|v| v.truthy()) {
            let Value::Dict(libraries) = libraries else {
                return refused(format!("{identifier}: OSBundleLibraries is not a dictionary"));
            };
            let edge = |d: &str| original.cuts.contains_key(&(identifier.clone(), d.to_owned()));
            pending.extend(libraries.keys().filter(|d| !edge(d)).map(str::to_owned));
        }
        selected.insert(identifier);
    }
    Ok((selected, interfaces))
}

fn component_segments(cache: &Kernelcache) -> Result<(Vec<Owned>, HashMap<String, Image>)> {
    let (mut owned, mut modules) = (Vec::new(), HashMap::new());
    for entry in &cache.entries {
        if entry.is_true("OSKernelResource") {
            continue;
        }
        let Some(va) = entry.get("_PrelinkExecutableLoadAddr") else {
            continue;
        };
        if !entry.get("_PrelinkExecutableSize").is_some_and(Value::truthy) {
            continue;
        }
        let header = cache.image.offset_of(address(va)?, 32, cache.data.len())?;
        let module = Image::parse(cache.data, header, Some(ORDINARY))?;
        if module.kind != MH_KEXT_BUNDLE {
            return refused("Expected MH_KEXT_BUNDLE component");
        }
        let identifier = entry.string("CFBundleIdentifier")?;
        for segment in module.segments.iter().filter(|s| s.size != 0 && s.name != "__LINKEDIT") {
            let Some(end) = segment.va.checked_add(segment.size) else {
                return refused("Segment outside 64-bit address space");
            };
            owned.push(Owned {
                start: segment.va,
                end,
                module: identifier.to_owned(),
                segment: segment.name.clone(),
            });
        }
        modules.insert(identifier.to_owned(), module);
    }
    Ok((owned, modules))
}

/// An address held in the prelink info, which Python compares as an integer.
pub(crate) fn address(value: &Value) -> Result<u64> {
    match value {
        Value::Integer(v) => u64::try_from(*v).or_else(|_| refused(format!("Ambiguous or unbacked address {v:#x}"))),
        _ => refused("A prelink info address that is not an integer"),
    }
}

fn check_codeless(cache: &Kernelcache, codeless: &BTreeSet<&String>) -> Result<()> {
    const EXECUTABLE_KEYS: [&str; 5] = [
        "CFBundleExecutable",
        "_PrelinkExecutableLoadAddr",
        "_PrelinkExecutableSourceAddr",
        "_PrelinkExecutableSize",
        "_PrelinkKmodInfo",
    ];
    for identifier in codeless {
        let entry = cache.entry(identifier)?;
        let personalities = entry.dict("IOKitPersonalities").is_some_and(|p| !p.0.is_empty());
        if EXECUTABLE_KEYS.iter().any(|k| entry.contains(k)) || !personalities {
            return refused(format!(
                "Codeless component must contain personalities and no executable fields: {identifier}"
            ));
        }
    }
    Ok(())
}

fn pointer_offsets(packed: &[u8]) -> Result<Vec<u32>> {
    let words: Vec<u32> = packed
        .chunks(4)
        .map(|c| u32::from_le_bytes(c.try_into().unwrap_or([0xff; 4])))
        .collect();
    let Some((&count, offsets)) = words.split_first() else {
        return refused("Invalid pointer-table extent");
    };
    if packed.len() != 4 + count as usize * 4 {
        return refused("Invalid pointer-table extent");
    }
    if offsets.iter().collect::<BTreeSet<_>>().len() != offsets.len() {
        return refused("Duplicate pointer relocation site");
    }
    Ok(offsets.to_vec())
}

fn rebased_extents(owners: &Owners, selected: &BTreeSet<String>, slide: i64) -> Result<Vec<Extent>> {
    let mut extents = Vec::new();
    for owned in owners.owned.iter().filter(|o| selected.contains(&o.module)) {
        if slid(owned.start, slide).is_err() || i128::from(owned.end) + i128::from(slide) > 1 << 64 {
            return refused("Rebased segment outside 64-bit address space");
        }
        extents.push(Extent {
            module: owned.module.clone(),
            segment: owned.segment.clone(),
            va: slid(owned.start, slide)?,
            size: owned.end - owned.start,
        });
    }
    Ok(extents)
}

struct Owners {
    owned: Vec<Owned>,
}

impl Owners {
    fn new(mut owned: Vec<Owned>) -> Result<Self> {
        owned.sort_by(|a, b| (a.start, a.end, &a.module).cmp(&(b.start, b.end, &b.module)));
        for pair in owned.windows(2) {
            if pair[0].end > pair[1].start {
                return refused(format!(
                    "Overlapping component extents: {} and {}",
                    pair[0].module, pair[1].module
                ));
            }
        }
        Ok(Owners { owned })
    }

    fn owner(&self, address: Option<u64>) -> Option<usize> {
        let address = address?;
        let index = self.owned.partition_point(|o| o.start <= address).checked_sub(1)?;
        (address < self.owned[index].end).then_some(index)
    }
}

struct Binder<'a> {
    owners: &'a Owners,
    selected: &'a BTreeSet<String>,
    original: &'a Original<'a>,
    symbols: HashMap<u64, Vec<String>>,
    bindings: Vec<Binding>,
    cut_sites: HashMap<(&'a str, &'a str), u64>,
}

impl<'a> Binder<'a> {
    /// A site past 2^64 is owned by no component, so it is passed over as one outside them is.
    fn bind_site(&mut self, cache: &Kernelcache, site: Option<u64>) -> Result<()> {
        let Some(source) = self.owners.owner(site) else {
            return Ok(());
        };
        let module = &self.owners.owned[source].module;
        if !self.selected.contains(module) {
            return Ok(());
        }
        let site = site.unwrap();
        if self.owners.owner(site.checked_add(7)) != Some(source) {
            return refused("Pointer spans component extent");
        }
        let target = macho::u64_at(cache.data, cache.image.offset_of(site, 8, cache.data.len())?)?;
        self.bind(site, target, source)
    }

    fn bind(&mut self, site: u64, target: u64, source: usize) -> Result<()> {
        let owners = self.owners;
        let module = owners.owned[source].module.as_str();
        let destination = owners.owner(Some(target)).map(|i| owners.owned[i].module.as_str());
        let target = match destination {
            Some(dependency)
                if !self.selected.contains(dependency)
                    && self
                        .original
                        .cuts
                        .contains_key(&(module.to_owned(), dependency.to_owned())) =>
            {
                *self.cut_sites.entry((module, dependency)).or_default() += 1;
                Target::Cut {
                    component: module.to_owned(),
                    dependency: dependency.to_owned(),
                }
            }
            Some(dependency) if !self.selected.contains(dependency) => {
                return refused(format!("Undeclared pointer dependency {dependency} from {module}"));
            }
            Some(_) => match slid(target, self.original.slide) {
                Ok(replacement) => Target::Component(replacement),
                Err(_) => return refused("Rebased pointer outside 64-bit address space"),
            },
            None => Target::Kernel {
                target,
                names: self.symbols.get(&target).cloned().unwrap_or_default(),
            },
        };
        let Ok(new_site) = slid(site, self.original.slide) else {
            return refused("Rebased pointer outside 64-bit address space");
        };
        self.bindings.push(Binding { new_site, target });
        Ok(())
    }
}
