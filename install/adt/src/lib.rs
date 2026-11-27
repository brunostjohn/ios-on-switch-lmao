// SPDX-License-Identifier: GPL-3.0-or-later
//! The boot ADT's last step, tools/package/adt_edit.py finish: the release's base ADT (//boot:adt_base) given the
//! identity of the iPad whose device tree Apple ships in the IPSW, then the IPSW kind's /chosen entries.

mod check;
mod chosen;
mod identity;
mod tree;

#[cfg(test)]
mod tests;

use tree::Node;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Input {
    Base,
    Apple,
}

impl std::fmt::Display for Input {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Input::Base => "the base ADT",
            Input::Apple => "Apple's device tree",
        })
    }
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0} is truncated at byte {1}")]
    Truncated(Input, usize),
    #[error("{0} has {1} bytes after its root node")]
    Trailing(Input, usize),
    #[error("{0} nests more than 64 levels")]
    TooDeep(Input),
    #[error("Apple's device tree has no single {0}")]
    NoSingleNode(String),
    #[error("Apple's device tree's {path} has no {name}")]
    NoProperty { path: String, name: String },
    #[error("Apple's /arm-io compatible names no SoC")]
    NoSoc,
    #[error("the base ADT's root has no compatible")]
    NoCompatible,
    #[error("the System volume UUID {0:?} is not ASCII")]
    VolumeUuid(String),
    #[error("a64x-cache-pretranslated: {0} bytes, not a list of CDHashes")]
    PretranslatedList(usize),
    #[error(transparent)]
    TrustModule(#[from] switch_trustcache::Error),
    #[error("switch-{0}-cdhashes: no CDHashes")]
    NoCdhashes(&'static str),
    #[error("switch-pretranslated-updates: {0}")]
    PretranslatedUpdates(String),
    #[error("the finished ADT: {0}")]
    Invalid(&'static str),
}

/// The trust modules of each role whose files the kernel authenticates by CDHash (adt_edit.py CDHASH_ROLES). A role
/// with no modules gets no list.
#[derive(Clone, Copy, Debug, Default)]
pub struct TrustModules<'a> {
    pub os_cryptex: &'a [&'a [u8]],
    pub app_cryptex: &'a [&'a [u8]],
    pub pretranslated: &'a [&'a [u8]],
}

impl<'a> TrustModules<'a> {
    fn roles(&self) -> [(&'static str, &'a [&'a [u8]]); 3] {
        [
            ("os-cryptex", self.os_cryptex),
            ("app-cryptex", self.app_cryptex),
            ("pretranslated", self.pretranslated),
        ]
    }
}

/// The IPSW kind's part of the ADT: what boot/defs.bzl passes adt_edit.py finish.
#[derive(Clone, Copy, Debug)]
pub struct Finish<'a> {
    /// The System volume's UUID (pins.bzl "volumes" os_volume_uuid).
    pub root_volume_uuid: &'a str,
    /// A zeroed /chosen/boot-manifest-hash for the layout step to fill: iOS 27 on, iOS 17 off.
    pub boot_manifest_hash: bool,
    /// iOS 17: the launchd catalogue's SHA-256 then its signature's.
    pub launchd_catalog_sha256: Option<&'a [u8; 64]>,
    /// iOS 27: the pretranslated dyld cache files' CDHashes.
    pub a64x_cache_pretranslated: Option<&'a [u8]>,
    /// iOS 27: JSON map of `{System path: CDHash hex}` for deploy-time pretranslated updates.
    pub pretranslated_updates: Option<&'a [u8]>,
    pub trust_modules: TrustModules<'a>,
}

/// The finished ADT, byte for byte what adt_edit.py finish makes of `base` and `apple`, Apple's device tree payload.
pub fn finish(base: &[u8], apple: &[u8], kind: &Finish) -> Result<Vec<u8>, Error> {
    let mut tree = Node::parse(base, Input::Base)?;
    let apple = Node::parse(apple, Input::Apple)?;
    identity::give(&mut tree, &apple)?;
    chosen::write(&mut tree, kind)?;
    let adt = tree.serialize();
    check::check(&tree, adt.len())?;
    Ok(adt)
}
