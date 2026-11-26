// SPDX-License-Identifier: GPL-3.0-or-later
use std::ffi::{CStr, c_char};
use std::io;
use std::path::PathBuf;

#[derive(Clone, Debug)]
pub struct Mount {
    pub bsd: String,
    pub mount_point: PathBuf,
    pub file_system: String,
}

/// Every mounted file system backed by a `/dev/disk*` node.
pub fn mounts() -> io::Result<Vec<Mount>> {
    // SAFETY: a null buffer asks only for the count.
    let count = unsafe { libc::getfsstat(std::ptr::null_mut(), 0, libc::MNT_NOWAIT) };
    if count < 0 {
        return Err(io::Error::last_os_error());
    }
    // Room for mounts made between the two calls.
    let mut stats: Vec<libc::statfs> = Vec::with_capacity(count as usize + 8);
    let bytes = (stats.capacity() * size_of::<libc::statfs>()) as libc::c_int;
    // SAFETY: the buffer holds `capacity` entries and getfsstat fills at most `bytes` of it.
    let filled = unsafe { libc::getfsstat(stats.as_mut_ptr(), bytes, libc::MNT_NOWAIT) };
    if filled < 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: getfsstat initialised `filled` entries.
    unsafe { stats.set_len(filled as usize) };
    Ok(stats
        .iter()
        .filter_map(|s| {
            let from = text(&s.f_mntfromname);
            let bsd = from.strip_prefix("/dev/")?.to_owned();
            Some(Mount {
                bsd,
                mount_point: PathBuf::from(text(&s.f_mntonname)),
                file_system: text(&s.f_fstypename),
            })
        })
        .collect())
}

fn text(field: &[c_char]) -> String {
    // SAFETY: the kernel NUL-terminates these fixed-size fields.
    unsafe { CStr::from_ptr(field.as_ptr()) }.to_string_lossy().into_owned()
}
