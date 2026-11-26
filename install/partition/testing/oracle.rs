// SPDX-License-Identifier: GPL-3.0-or-later
//! The kernel's root rule and shared-volume selection over a card image, through //install/card:oracle, which compiles
//! them from the kernel's own sources.

use std::ffi::{CStr, c_char, c_int, c_void};

use switch_card::Device;

type ReadFn = extern "C" fn(*mut c_void, u64, *mut u8) -> c_int;

#[repr(C)]
#[derive(Default)]
struct RootOut {
    first: u64,
    count: u64,
    found: u32,
    unique: u8,
}

#[repr(C)]
#[derive(Default)]
struct FilesOut {
    first: u64,
    count: u64,
    exfat: u8,
}

unsafe extern "C" {
    fn switch_card_oracle_root(read: ReadFn, context: *mut c_void, out: *mut RootOut) -> c_int;
    fn switch_card_oracle_files(
        read: ReadFn,
        context: *mut c_void,
        capacity: u64,
        root_first: u64,
        root_sectors: u64,
        out: *mut FilesOut,
    ) -> c_int;
    fn switch_card_oracle_files_reason(status: c_int) -> *const c_char;
}

extern "C" fn read<D: Device>(context: *mut c_void, lba: u64, out: *mut u8) -> c_int {
    // SAFETY: `context` is the `&mut D` the calls below pass, live for the call; the kernel's callers pass 512 bytes.
    let (dev, sector) = unsafe { (&mut *context.cast::<D>(), std::slice::from_raw_parts_mut(out, 512)) };
    if lba < dev.sector_count() && dev.read_sectors(lba, sector).is_ok() {
        0
    } else {
        -1
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Root {
    pub found: u32,
    pub unique: bool,
    pub first: u64,
    pub count: u64,
}

/// What setup() finds: None when it refuses the primary header before counting roots.
pub fn root<D: Device>(dev: &mut D) -> Option<Root> {
    let mut out = RootOut::default();
    // SAFETY: `read::<D>` gets back the `dev` pointer passed here, and `out` outlives the call.
    let code = unsafe { switch_card_oracle_root(read::<D>, (dev as *mut D).cast(), &mut out) };
    (code == 0).then_some(Root {
        found: out.found,
        unique: out.unique != 0,
        first: out.first,
        count: out.count,
    })
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Files {
    Volume { first: u64, count: u64, exfat: bool },
    Offline(&'static str),
}

/// The shared volume the kernel brings online beside a root at `root_first..+root_sectors`, or its log's word.
pub fn files<D: Device>(dev: &mut D, root_first: u64, root_sectors: u64) -> Files {
    let capacity = dev.sector_count();
    let mut out = FilesOut::default();
    // SAFETY: as for `root`.
    let status = unsafe {
        switch_card_oracle_files(
            read::<D>,
            (dev as *mut D).cast(),
            capacity,
            root_first,
            root_sectors,
            &mut out,
        )
    };
    if status == 0 {
        return Files::Volume {
            first: out.first,
            count: out.count,
            exfat: out.exfat != 0,
        };
    }
    // SAFETY: switchSdFilesReason returns a static string for every status.
    Files::Offline(
        unsafe { CStr::from_ptr(switch_card_oracle_files_reason(status)) }
            .to_str()
            .unwrap(),
    )
}
