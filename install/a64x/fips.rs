// SPDX-License-Identifier: GPL-3.0-or-later
use crate::ffi;

pub const MAC_LEN: usize = 32;

/// Where corecrypto's FIPS integrity check reads (translate/arm64e/macho/fips.h): the text it MACs and the MAC it
/// expects, as addresses and as offsets from the Mach-O header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fips {
    pub text_vmaddr: u64,
    pub text_fileoff: u64,
    pub text_size: u64,
    pub mac_vmaddr: u64,
    pub mac_fileoff: u64,
}

/// `arm64e fips`: `header` is a `mach_header_64` and its load commands. `None` for an image without the check.
pub fn find(header: &[u8]) -> Option<Fips> {
    let mut raw = ffi::Fips::default();
    if unsafe { ffi::a64x_fips_find(header.as_ptr(), header.len(), &mut raw) } == 0 {
        return None;
    }
    Some(Fips {
        text_vmaddr: raw.text_vmaddr,
        text_fileoff: raw.text_fileoff,
        text_size: raw.text_size,
        mac_vmaddr: raw.mac_vmaddr,
        mac_fileoff: raw.mac_fileoff,
    })
}

/// `arm64e fips-mac`: `mac` is the image's MAC. If it is that of `original`, it becomes that of `translated` (true);
/// a stale one stays (false), as Apple ships some.
pub fn rewrite(original: &[u8], translated: &[u8], mac: &mut [u8; MAC_LEN]) -> bool {
    assert_eq!(
        original.len(),
        translated.len(),
        "the original and translated texts differ in size"
    );
    unsafe { ffi::a64x_fips_rewrite(original.as_ptr(), translated.as_ptr(), original.len(), mac.as_mut_ptr()) != 0 }
}
