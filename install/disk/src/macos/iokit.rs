// SPDX-License-Identifier: GPL-3.0-or-later
use std::ffi::{CStr, CString, c_char};
use std::time::Duration;

use objc2_core_foundation::{CFBoolean, CFDictionary, CFMutableDictionary, CFNumber, CFRetained, CFString};
use objc2_io_kit::{
    IOBSDNameMatching, IOIteratorNext, IOObjectConformsTo, IOObjectRelease, IORegistryEntryCreateCFProperty,
    IORegistryEntryGetName, IORegistryEntryGetParentEntry, IORegistryEntryGetRegistryEntryID,
    IOServiceGetMatchingService, IOServiceGetMatchingServices, IOServiceMatching, io_object_t, kIOMainPortDefault,
};

use crate::error::{Error, Result};

type Name = [c_char; 128];

#[repr(C)]
struct MachTimespec {
    tv_sec: u32,
    tv_nsec: i32,
}

unsafe extern "C-unwind" {
    fn IOServiceWaitQuiet(service: io_object_t, wait: *const MachTimespec) -> libc::kern_return_t;
}

/// One IOMedia object: a whole disk, a partition, a synthesized APFS container or an APFS volume.
#[derive(Clone, Debug)]
pub struct Media {
    pub id: u64,
    pub bsd: String,
    pub name: String,
    pub size: u64,
    pub block: u32,
    pub content: String,
    pub base: u64,
    pub number: u32,
    pub writable: bool,
    /// A GPT partition's unique GUID or an APFS volume's UUID; it survives a re-probe that renumbers BSD names.
    pub uuid: String,
    /// IOMedia registry IDs above this one, nearest first. An APFS volume's run through its synthesized container
    /// to the partition and the physical disk holding it.
    pub ancestors: Vec<u64>,
}

impl Media {
    pub fn is_on(&self, root: u64) -> bool {
        self.id == root || self.ancestors.contains(&root)
    }

    /// What names this media across a re-probe: its UUID, or for an MBR partition, which has none, where it lies.
    pub fn identity(&self) -> String {
        if self.uuid.is_empty() {
            format!("{}@{}+{}:{}", self.content, self.base, self.size, self.ancestors.len())
        } else {
            self.uuid.clone()
        }
    }
}

struct Object(io_object_t);

impl Drop for Object {
    fn drop(&mut self) {
        IOObjectRelease(self.0);
    }
}

pub fn all_media() -> Result<Vec<Media>> {
    // SAFETY: a NUL-terminated class name.
    let matching =
        unsafe { IOServiceMatching(c"IOMedia".as_ptr()) }.ok_or_else(|| Error::os("IOKit", "IOServiceMatching"))?;
    let mut iterator = 0;
    // SAFETY: the dictionary is consumed; iterator receives a handle we release.
    let kr = unsafe { IOServiceGetMatchingServices(kIOMainPortDefault, Some(upcast(matching)), &mut iterator) };
    if kr != 0 {
        return Err(Error::os("IOKit: list IOMedia", format!("kern_return {kr:#x}")));
    }
    let iterator = Object(iterator);
    let mut media = Vec::new();
    loop {
        let entry = IOIteratorNext(iterator.0);
        if entry == 0 {
            break;
        }
        let entry = Object(entry);
        if let Some(m) = describe(&entry) {
            media.push(m);
        }
    }
    Ok(media)
}

pub fn media_by_bsd(bsd: &str) -> Option<Media> {
    let entry = service_by_bsd(bsd)?;
    describe(&entry)
}

/// Waits until IOKit has finished matching drivers below the disk: after an exclusive open closes, the partition
/// scheme, the APFS container and its volumes are probed again, and a provider stays busy while any child is.
pub fn wait_quiet(bsd: &str, timeout: Duration) -> Result<()> {
    let entry = service_by_bsd(bsd).ok_or_else(|| Error::Changed(format!("/dev/{bsd}")))?;
    let wait = MachTimespec {
        tv_sec: timeout.as_secs() as u32,
        tv_nsec: timeout.subsec_nanos() as i32,
    };
    // SAFETY: a live service handle and a valid timespec.
    match unsafe { IOServiceWaitQuiet(entry.0, &wait) } {
        0 => Ok(()),
        _ => Err(Error::Timeout {
            what: format!("macOS to probe /dev/{bsd} again"),
            waited: timeout,
        }),
    }
}

fn service_by_bsd(bsd: &str) -> Option<Object> {
    let name = CString::new(bsd).ok()?;
    // SAFETY: a NUL-terminated BSD name; the dictionary is consumed by the lookup.
    let matching = unsafe { IOBSDNameMatching(kIOMainPortDefault, 0, name.as_ptr()) }?;
    let entry = unsafe { IOServiceGetMatchingService(kIOMainPortDefault, Some(upcast(matching))) };
    (entry != 0).then_some(Object(entry))
}

fn upcast(dictionary: CFRetained<CFMutableDictionary>) -> CFRetained<CFDictionary> {
    // SAFETY: a CFMutableDictionary is a CFDictionary.
    unsafe { CFRetained::cast_unchecked(dictionary) }
}

fn describe(entry: &Object) -> Option<Media> {
    let bsd = string(entry, "BSD Name")?;
    Some(Media {
        id: registry_id(entry.0)?,
        name: name(entry.0),
        size: number(entry, "Size").unwrap_or(0),
        block: number(entry, "Preferred Block Size").unwrap_or(512) as u32,
        content: string(entry, "Content").unwrap_or_default(),
        base: number(entry, "Base").unwrap_or(0),
        number: number(entry, "Partition ID").unwrap_or(0) as u32,
        writable: boolean(entry, "Writable"),
        uuid: string(entry, "UUID").unwrap_or_default(),
        ancestors: media_ancestors(entry.0),
        bsd,
    })
}

fn media_ancestors(entry: io_object_t) -> Vec<u64> {
    let mut plane = name_buffer("IOService");
    let mut class = name_buffer("IOMedia");
    let mut ancestors = Vec::new();
    let mut current: Option<Object> = None;
    loop {
        let child = current.as_ref().map_or(entry, |o| o.0);
        let mut parent = 0;
        // SAFETY: a live registry entry and a NUL-terminated plane name.
        if unsafe { IORegistryEntryGetParentEntry(child, &mut plane, &mut parent) } != 0 || parent == 0 {
            return ancestors;
        }
        let parent = Object(parent);
        // SAFETY: as above, with a NUL-terminated class name.
        if unsafe { IOObjectConformsTo(parent.0, &mut class) }
            && let Some(id) = registry_id(parent.0)
        {
            ancestors.push(id);
        }
        current = Some(parent);
    }
}

fn registry_id(entry: io_object_t) -> Option<u64> {
    let mut id = 0;
    // SAFETY: a live registry entry.
    (unsafe { IORegistryEntryGetRegistryEntryID(entry, &mut id) } == 0).then_some(id)
}

fn name(entry: io_object_t) -> String {
    let mut buffer: Name = [0; 128];
    // SAFETY: IOKit writes at most 128 bytes, NUL included.
    if unsafe { IORegistryEntryGetName(entry, &mut buffer) } != 0 {
        return String::new();
    }
    // SAFETY: NUL-terminated by IOKit within the buffer.
    unsafe { CStr::from_ptr(buffer.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

fn name_buffer(text: &str) -> Name {
    let mut buffer: Name = [0; 128];
    for (slot, byte) in buffer.iter_mut().zip(text.bytes()) {
        *slot = byte as c_char;
    }
    buffer
}

fn property(entry: &Object, key: &'static str) -> Option<CFRetained<objc2_core_foundation::CFType>> {
    let key = CFString::from_static_str(key);
    // SAFETY: a live registry entry and a CFString key.
    unsafe { IORegistryEntryCreateCFProperty(entry.0, Some(&key), None, 0) }
}

fn string(entry: &Object, key: &'static str) -> Option<String> {
    property(entry, key)?.downcast_ref::<CFString>().map(|s| s.to_string())
}

fn number(entry: &Object, key: &'static str) -> Option<u64> {
    property(entry, key)?
        .downcast_ref::<CFNumber>()?
        .as_i64()
        .map(|n| n as u64)
}

fn boolean(entry: &Object, key: &'static str) -> bool {
    property(entry, key)
        .and_then(|p| p.downcast_ref::<CFBoolean>().map(|b| b.as_bool()))
        .unwrap_or(false)
}
