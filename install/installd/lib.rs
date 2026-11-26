// SPDX-License-Identifier: GPL-3.0-or-later
//! The overlay's `installd`, as `tools/install_policy/installd.py` makes `//drivers/SwitchInstallPolicy:installd`
//! byte for byte: Apple's, from the OS volume, with the install policy as a dependency, re-signed ad hoc with its own
//! identifier, flags, runtime version and entitlements. Its CDHash goes into the trust bundle, so into `STAGE_ID`.

mod inject;
mod macho;
mod slice;

use std::io::Cursor;

use switch_sign::{Embedded, Metadata, Signed, embedded_entitlements, sign_macho};

pub use inject::inject;
pub use slice::arm64_slice;

/// The dependency `installd` gains.
pub const LIBRARY: &str = "/usr/lib/libSwitchInstallPolicy.dylib";
/// Where `installd` is on the OS volume, and where the overlay puts the patched one.
pub const PATH: &str = "/usr/libexec/installd";
/// Where the OS volume names its build.
pub const SYSTEM_VERSION: &str = "/System/Library/CoreServices/SystemVersion.plist";

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("installd: {0}")]
    Refused(&'static str),
    #[error("SystemVersion.plist: {0}")]
    SystemVersion(#[from] plist::Error),
    #[error("OS volume build {found:?} differs from the selected IPSW pins ({expected})")]
    Build { found: Option<String>, expected: String },
    #[error("installd has no entitlement dictionary")]
    NoEntitlements,
    #[error("installd's entitlements: {0}")]
    Entitlements(String),
    #[error(transparent)]
    Sign(#[from] switch_sign::Error),
    #[error("installd pretranslate: {0}")]
    Pretranslate(String),
}

fn product_build(system_version: &[u8]) -> Result<Option<String>, Error> {
    let value = plist::Value::from_reader(Cursor::new(system_version))?;
    Ok(value
        .as_dictionary()
        .and_then(|d| d.get("ProductBuildVersion"))
        .and_then(plist::Value::as_string)
        .map(str::to_owned))
}

/// The entitlements as `plistlib.dumps(plistlib.loads(codesign --display --entitlements :-))` writes them.
fn entitlements_xml(macho: &[u8]) -> Result<Vec<u8>, Error> {
    let source = embedded_entitlements(macho)?.ok_or(Error::NoEntitlements)?;
    let value = plist::Value::from_reader(Cursor::new(source)).map_err(|e| Error::Entitlements(e.to_string()))?;
    if !value.as_dictionary().is_some_and(|d| !d.is_empty()) {
        return Err(Error::NoEntitlements);
    }
    switch_plistlib::to_xml(&value).map_err(|e| Error::Entitlements(e.to_string()))
}

/// `installd.py prepare_files`: the OS volume's `installd` for `build` (its `SystemVersion.plist` must say so),
/// patched, signed, and (when `pretranslate` is set, matching `--arm64e`) pretranslated.
pub fn prepare(installd: &[u8], system_version: &[u8], build: &str, pretranslate: bool) -> Result<Signed, Error> {
    let found = product_build(system_version)?;
    if found.as_deref() != Some(build) {
        return Err(Error::Build {
            found,
            expected: build.to_owned(),
        });
    }
    let xml = entitlements_xml(arm64_slice(installd)?)?;
    let patched = inject(installd)?;
    let options = Embedded {
        metadata: Metadata::Preserve,
        entitlements: Some(&xml),
    };
    let signed = sign_macho(&patched, &options)?;
    if !pretranslate {
        return Ok(signed);
    }
    let (bytes, slices) =
        switch_translate::translate_macho(PATH, signed.bytes).map_err(|e| Error::Pretranslate(e.to_string()))?;
    if slices.len() != 1 || slices[0].fallback {
        return Err(Error::Pretranslate("fallback or unexpected slice count".into()));
    }
    let cdhash = if slices[0].translated {
        slices[0].cdhash_out
    } else {
        signed.cdhash
    };
    Ok(Signed { bytes, cdhash })
}
