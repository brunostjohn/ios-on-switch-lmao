// SPDX-License-Identifier: GPL-3.0-or-later
//! Whether this installer can run a record: every step, member role, container format and asset role it names must be
//! one this installer knows. A record it can't run leaves the catalog's other builds usable.

use std::collections::BTreeMap;
use std::fmt;

use crate::record::{Asset, ContainerPins, Member, Record};
use crate::{Error, Sha256};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Role {
    Os,
    SystemCryptex,
    AppCryptex,
    Kernelcache,
    DeviceTree,
    StaticTrustCache,
    SystemCryptexTrustCache,
    AppCryptexTrustCache,
}

impl Role {
    pub const ALL: [Role; 8] = [
        Role::Os,
        Role::SystemCryptex,
        Role::AppCryptex,
        Role::Kernelcache,
        Role::DeviceTree,
        Role::StaticTrustCache,
        Role::SystemCryptexTrustCache,
        Role::AppCryptexTrustCache,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Role::Os => "os",
            Role::SystemCryptex => "system_cryptex",
            Role::AppCryptex => "app_cryptex",
            Role::Kernelcache => "kernelcache",
            Role::DeviceTree => "device_tree",
            Role::StaticTrustCache => "static_trust_cache",
            Role::SystemCryptexTrustCache => "system_cryptex_trust_cache",
            Role::AppCryptexTrustCache => "app_cryptex_trust_cache",
        }
    }

    pub fn from_name(name: &str) -> Option<Role> {
        Role::ALL.into_iter().find(|role| role.name() == name)
    }

    pub fn is_disk_image(self) -> bool {
        matches!(self, Role::Os | Role::SystemCryptex | Role::AppCryptex)
    }
}

/// How a disk image member holds its APFS container: a UDIF image of a disk holding it, or the bare container, which
/// the catalog calls `raw`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Container {
    Udif,
    Apfs,
}

impl Container {
    pub fn from_name(name: &str) -> Option<Container> {
        match name {
            "raw" => Some(Container::Apfs),
            "udif" => Some(Container::Udif),
            _ => None,
        }
    }
}

/// How Apple's kexts get into the kernel: from a prelinked kernelcache (iOS 17), or from a fileset one through CI's
/// bindings export (iOS 26 on).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Prelink {
    Native,
    Fileset,
}

/// The steps a build runs, beyond those every build does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Steps {
    pub prelink: Prelink,
    /// Apple's launchd catalogue gets the project's jobs, signed again.
    pub launchd_catalog: bool,
    /// Apple's arm64e code is translated, and the boot carries the trust modules and cache list of that run.
    pub pretranslate: bool,
    /// `/chosen/boot-manifest-hash` holds the SHA-384 of the kernel and the trust bundle.
    pub boot_manifest_hash: bool,
}

pub const ASSET_ROLES: [&str; 1] = ["boot"];
pub const KNOWN_ASSET_ROLES: [&str; 5] = ["boot", "overlay", "bootstrap", "gpu", "seed"];

/// Apple's launchd catalogue and its detached signature, as the OS volume holds them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LaunchdPins {
    pub sha256: Sha256,
    pub signature_sha256: Sha256,
}

/// A record this installer can run.
#[derive(Clone, Debug)]
pub struct Build {
    pub record: Record,
    pub steps: Steps,
    members: BTreeMap<Role, Member>,
}

impl Build {
    pub fn build(&self) -> &str {
        &self.record.build
    }

    pub fn member(&self, role: Role) -> &Member {
        &self.members[&role]
    }

    /// A disk image member's container format and pins.
    pub fn container(&self, role: Role) -> Option<(Container, &ContainerPins)> {
        let pins = self.member(role).container.as_ref()?;
        Some((Container::from_name(&pins.format)?, pins))
    }

    /// The catalogue's pins, for a build that runs the launchd_catalog step.
    pub fn launchd_catalog(&self) -> Option<LaunchdPins> {
        if !self.steps.launchd_catalog {
            return None;
        }
        let derived = &self.record.derived;
        Some(LaunchdPins {
            sha256: derived.launchd_catalog_sha256?,
            signature_sha256: derived.launchd_catalog_signature_sha256?,
        })
    }

    pub fn asset(&self, role: &str) -> Result<&Asset, Error> {
        self.record.assets.get(role).ok_or_else(|| Error::NoAsset {
            build: self.record.build.clone(),
            role: role.to_owned(),
        })
    }
}

/// Why this installer can't run a record.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Unsupported {
    pub build: String,
    pub reasons: Vec<String>,
}

impl fmt::Display for Unsupported {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "this installer can't install {}: {}",
            self.build,
            self.reasons.join("; ")
        )
    }
}

impl std::error::Error for Unsupported {}

fn newer(what: String) -> String {
    format!("{what}, which a newer installer may run")
}

fn steps(record: &Record, reasons: &mut Vec<String>) -> Option<Steps> {
    let mut prelink = None;
    let (mut launchd_catalog, mut pretranslate, mut boot_manifest_hash) = (false, false, false);
    for (name, kind) in &record.steps {
        match (name.as_str(), kind.as_str()) {
            ("prelink", "native") => prelink = Some(Prelink::Native),
            ("prelink", "fileset") => prelink = Some(Prelink::Fileset),
            ("launchd_catalog", "jobs") => launchd_catalog = true,
            ("pretranslate", "a64x") => pretranslate = true,
            ("boot_manifest_hash", "sha384") => boot_manifest_hash = true,
            ("prelink" | "launchd_catalog" | "pretranslate" | "boot_manifest_hash", _) => {
                reasons.push(newer(format!("the {name} step's kind {kind}")))
            }
            _ => reasons.push(newer(format!("the {name} step"))),
        }
    }
    let derived = &record.derived;
    let catalogue_pins = derived.launchd_catalog_sha256.is_some() && derived.launchd_catalog_signature_sha256.is_some();
    if launchd_catalog != catalogue_pins {
        reasons.push("the launchd_catalog step and the catalogue's two pins don't go together".into());
    }
    if pretranslate == derived.pretranslated.is_empty() {
        reasons.push("the pretranslate step and the pretranslated pins don't go together".into());
    }
    if prelink.is_none() && !record.steps.contains_key("prelink") {
        reasons.push("it has no prelink step".into());
    }
    Some(Steps {
        prelink: prelink?,
        launchd_catalog,
        pretranslate,
        boot_manifest_hash,
    })
}

fn member_shape(role: Role, member: &Member, reasons: &mut Vec<String>) {
    match (&member.container, role.is_disk_image()) {
        (Some(pins), true) if Container::from_name(&pins.format).is_none() => {
            reasons.push(newer(format!(
                "the {} member's container format {}",
                role.name(),
                pins.format
            )));
        }
        (Some(_), true) | (None, false) => {}
        (None, true) => reasons.push(format!("its {} member has no container pins", role.name())),
        (Some(_), false) => reasons.push(format!("its {} member is no disk image", role.name())),
    }
    let is_im4p = !role.is_disk_image();
    match &member.im4p {
        Some(_) if !is_im4p => reasons.push(format!("its {} member is no IM4P", role.name())),
        Some(im4p) if im4p.fourcc.len() != 4 || !im4p.fourcc.is_ascii() => {
            reasons.push(format!(
                "its {} member's IM4P type {:?} is no fourcc",
                role.name(),
                im4p.fourcc
            ));
        }
        None if is_im4p => reasons.push(format!("its {} member has no IM4P pins", role.name())),
        _ => {}
    }
}

impl Record {
    pub fn plan(&self) -> Result<Build, Unsupported> {
        let mut reasons = Vec::new();
        let steps = steps(self, &mut reasons);
        let mut members = BTreeMap::new();
        for (name, member) in &self.members {
            match Role::from_name(name) {
                Some(role) => {
                    member_shape(role, member, &mut reasons);
                    members.insert(role, member.clone());
                }
                None => reasons.push(newer(format!("the {name} member"))),
            }
        }
        for role in Role::ALL.iter().filter(|role| !members.contains_key(role)) {
            reasons.push(format!("it has no {} member", role.name()));
        }
        for role in self
            .assets
            .keys()
            .filter(|role| !KNOWN_ASSET_ROLES.contains(&role.as_str()))
        {
            reasons.push(newer(format!("the {role} asset")));
        }
        match steps {
            Some(steps) if reasons.is_empty() => Ok(Build {
                record: self.clone(),
                steps,
                members,
            }),
            _ => Err(Unsupported {
                build: self.build.clone(),
                reasons,
            }),
        }
    }
}
