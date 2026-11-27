// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::BTreeMap;

use sha1::{Digest, Sha1};

use crate::blob::{self, SuperBlob, slot};
use crate::code_directory::{self, ADHOC, Builder, ExecSegment};
use crate::{Error, Signed};

/// `codesign -s - -i <identifier> [-o <flags>] --timestamp=none --pagesize <page_size> --digest-algorithm sha256
/// --detached <signature> <file>` for a file that is not a Mach-O.
pub struct Detached<'a> {
    pub identifier: &'a str,
    pub flags: u32,
    pub page_size: u32,
}

pub fn sign_detached(data: &[u8], options: &Detached) -> Result<Signed, Error> {
    const MACHO_MAGICS: [[u8; 4]; 6] = [
        [0xfe, 0xed, 0xfa, 0xce],
        [0xce, 0xfa, 0xed, 0xfe],
        [0xfe, 0xed, 0xfa, 0xcf],
        [0xcf, 0xfa, 0xed, 0xfe],
        [0xca, 0xfe, 0xba, 0xbe],
        [0xca, 0xfe, 0xba, 0xbf],
    ];
    if data.len() >= 4 && MACHO_MAGICS.iter().any(|magic| data[..4] == *magic) {
        return Err(Error::Unsupported("a detached signature of a Mach-O"));
    }
    if !options.page_size.is_power_of_two() || !(0x1000..=0x1_0000).contains(&options.page_size) {
        return Err(Error::Unsupported(
            "a page size that is not a power of two from 4 KiB to 64 KiB",
        ));
    }
    if options.identifier.is_empty() {
        return Err(Error::Unsupported("an empty identifier"));
    }
    let code_limit = u32::try_from(data.len()).map_err(|_| Error::Unsupported("a file over 4 GiB"))?;
    let requirements = blob::empty_requirements();
    let mut cd = Builder {
        identifier: options.identifier,
        flags: options.flags | ADHOC,
        page_shift: options.page_size.trailing_zeros() as u8,
        code_limit,
        exec_segment: ExecSegment::default(),
        runtime: 0,
        special: BTreeMap::new(),
    };
    cd.add_special(slot::REQUIREMENTS, &requirements);
    let cd = cd.build(data);
    let cdhash = code_directory::cdhash(&cd);
    let mut signature = SuperBlob::default();
    signature.insert(slot::CODE_DIRECTORY, cd);
    signature.insert(slot::REQUIREMENTS, requirements);
    signature.insert(slot::SIGNATURE, blob::wrap(blob::WRAPPER, &[]));
    signature.insert(slot::IDENTIFICATION, blob::wrap(blob::WRAPPER, &Sha1::digest(data)));
    Ok(Signed {
        bytes: signature.build(),
        cdhash,
    })
}
