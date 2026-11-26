// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;
use std::io::Cursor;

use crate::blob::{self, Blobs, SuperBlob, slot};
use crate::code_directory::{self, ADHOC, Builder, ESTIMATE_VERSION, ExecSegment, Existing, LINKER_SIGNED};
use crate::entitlements::Entitlements;
use crate::macho::{Image, Version};
use crate::{Error, RUNTIME, Signed};

const MH_EXECUTE: u32 = 2;
const CPU_TYPE_ARM64: u32 = 0x0100_000c;
const CPU_TYPE_ARM64_32: u32 = 0x0200_000c;
const PLATFORM_MACOS: u32 = 1;
const PLATFORM_IOS: u32 = 2;
const PLATFORM_TVOS: u32 = 3;
const PLATFORM_WATCHOS: u32 = 4;
const EXEC_SEGMENT_MAIN_BINARY: u64 = 1;

pub enum Metadata<'a> {
    /// `-i <identifier> [-o <flags>]`; flags 0 is no `-o`.
    Given { identifier: &'a str, flags: u32 },
    /// `--preserve-metadata=identifier,flags,runtime`, from the file's existing ad hoc or real signature.
    Preserve,
}

/// `codesign --force -s - --timestamp=none [--entitlements <xml>] [--generate-entitlement-der]` on a thin
/// executable. `codesign` always generates the DER entitlements, so the flag changes nothing.
pub struct Embedded<'a> {
    pub metadata: Metadata<'a>,
    pub entitlements: Option<&'a [u8]>,
}

pub fn sign_macho(input: &[u8], options: &Embedded) -> Result<Signed, Error> {
    let image = Image::parse(input)?;
    if image.filetype != MH_EXECUTE {
        return Err(Error::Unsupported("a Mach-O that is not an executable"));
    }
    if needs_sha1(image.version) {
        return Err(Error::Unsupported(
            "a deployment target that needs SHA-1 CodeDirectories as well",
        ));
    }
    let existing = existing_metadata(input, &image)?;
    let info_plist = match image.info_plist {
        Some((offset, size)) => Some(input.get(offset..offset + size).ok_or(Error::MachO("__info_plist"))?),
        None => None,
    };
    let (identifier, flags, runtime) = match (&options.metadata, &existing) {
        (Metadata::Given { identifier, flags }, _) => (identifier.to_string(), *flags, 0),
        (Metadata::Preserve, Some(old)) => (old.identifier.clone(), old.flags & !ADHOC, old.runtime),
        (Metadata::Preserve, None) => return Err(Error::Unsupported("preserving metadata the file does not have")),
    };
    let flags_given = matches!(options.metadata, Metadata::Given { flags, .. } if flags != 0);
    if !flags_given && info_plist.is_some_and(sets_flags) {
        return Err(Error::Unsupported("an embedded Info.plist with CSFlags"));
    }
    if identifier.is_empty() {
        return Err(Error::Unsupported("an empty identifier"));
    }
    let entitlements = options.entitlements.map(Entitlements::parse).transpose()?;

    let mut blobs = SuperBlob::default();
    let requirements = blob::empty_requirements();
    let mut cd = Builder {
        identifier: &identifier,
        flags: flags | ADHOC,
        page_shift: page_shift(&image),
        code_limit: 0,
        exec_segment: exec_segment(&image),
        runtime: runtime_version(flags, runtime, image.version),
        special: BTreeMap::new(),
    };
    if let Some(info) = info_plist {
        cd.add_special(slot::INFO, info);
    }
    cd.add_special(slot::REQUIREMENTS, &requirements);
    blobs.insert(slot::REQUIREMENTS, requirements);
    if let Some(entitlements) = &entitlements {
        let (xml, der) = (entitlements.xml_blob(), entitlements.der_blob()?);
        cd.add_special(slot::ENTITLEMENTS, &xml);
        cd.add_special(slot::ENTITLEMENTS_DER, &der);
        blobs.insert(slot::ENTITLEMENTS, xml);
        blobs.insert(slot::ENTITLEMENTS_DER, der);
        cd.exec_segment.flags |= entitlements.exec_segment_flags();
    }
    blobs.insert(slot::SIGNATURE, blob::wrap(blob::WRAPPER, &[]));

    // The space is sized before the file is laid out: over the old code limit, with a version 2.5 CodeDirectory.
    cd.code_limit = image.signature.map_or(input.len() as u32, |s| s.dataoff);
    let estimate = blobs.len() + 8 + cd.size(ESTIMATE_VERSION);
    let space = u32::try_from(estimate.next_multiple_of(16)).map_err(|_| Error::Unsupported("signature size"))?;
    let (mut out, dataoff) = image.allocate(input, space)?;

    cd.code_limit = dataoff;
    let cd = cd.build(&out[..dataoff as usize]);
    let cdhash = code_directory::cdhash(&cd);
    blobs.insert(slot::CODE_DIRECTORY, cd);
    let signature = blobs.build();
    out[dataoff as usize..dataoff as usize + signature.len()].copy_from_slice(&signature);
    Ok(Signed { bytes: out, cdhash })
}

/// The XML entitlements an executable is signed with, as its signature holds them.
pub fn embedded_entitlements(macho: &[u8]) -> Result<Option<&[u8]>, Error> {
    let image = Image::parse(macho)?;
    let Some(blobs) = existing_blobs(macho, &image)? else {
        return Ok(None);
    };
    Ok(blobs.get(slot::ENTITLEMENTS).map(|blob| &blob[8..]))
}

fn existing_blobs<'a>(input: &'a [u8], image: &Image) -> Result<Option<Blobs<'a>>, Error> {
    let Some(signature) = image.signature else {
        return Ok(None);
    };
    let (start, size) = (signature.dataoff as usize, signature.datasize as usize);
    let data = input
        .get(start..start + size)
        .ok_or(Error::Signature("signature past the end of the file"))?;
    Blobs::parse(data).map(Some)
}

// codesign keeps metadata only from a signature that is not the linker's.
fn existing_metadata(input: &[u8], image: &Image) -> Result<Option<Existing>, Error> {
    let Some(blobs) = existing_blobs(input, image)? else {
        return Ok(None);
    };
    if blobs.slots().any(|s| slot::ALTERNATE_CODE_DIRECTORIES.contains(&s)) {
        return Err(Error::Unsupported(
            "an existing signature with alternate CodeDirectories",
        ));
    }
    let cd = blobs
        .get(slot::CODE_DIRECTORY)
        .ok_or(Error::Signature("no CodeDirectory"))?;
    let existing = Existing::parse(cd)?;
    Ok((existing.flags & LINKER_SIGNED == 0).then_some(existing))
}

fn needs_sha1(version: Option<Version>) -> bool {
    let Some(version) = version else { return false };
    let first_sha256_only = match version.platform {
        PLATFORM_MACOS => 0x000a_0b04,
        PLATFORM_IOS | PLATFORM_TVOS => 0x000b_0000,
        PLATFORM_WATCHOS => 0x000c_0000,
        _ => return false,
    };
    version.min_os < first_sha256_only
}

// Measured on macOS 27's codesign: 16 KiB page hashes for arm64, except tvOS and iOS targets older than 16.
fn page_shift(image: &Image) -> u8 {
    let arm64 = matches!(image.cputype, CPU_TYPE_ARM64 | CPU_TYPE_ARM64_32);
    let small_pages = match image.version {
        Some(Version {
            platform: PLATFORM_TVOS,
            ..
        }) => true,
        Some(Version {
            platform: PLATFORM_IOS,
            min_os,
            ..
        }) => min_os < 0x10_0000,
        _ => false,
    };
    if arm64 && !small_pages { 14 } else { 12 }
}

fn exec_segment(image: &Image) -> ExecSegment {
    match image.text {
        Some((base, limit)) if image.platform() != 0 => ExecSegment {
            base,
            limit,
            flags: EXEC_SEGMENT_MAIN_BINARY,
        },
        _ => ExecSegment::default(),
    }
}

fn runtime_version(flags: u32, preserved: u32, version: Option<Version>) -> u32 {
    match (flags & RUNTIME != 0, preserved) {
        (false, _) => 0,
        (true, 0) => version.map_or(0, |v| v.sdk),
        (true, preserved) => preserved,
    }
}

fn sets_flags(info_plist: &[u8]) -> bool {
    match plist::Value::from_reader(Cursor::new(info_plist)) {
        Ok(plist::Value::Dictionary(dict)) => dict.contains_key("CSFlags"),
        _ => false,
    }
}
