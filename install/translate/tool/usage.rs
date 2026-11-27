// SPDX-License-Identifier: GPL-3.0-or-later

/// The process's peak resident set so far.
#[cfg(unix)]
pub fn peak_rss() -> String {
    let mut usage = std::mem::MaybeUninit::<libc::rusage>::zeroed();
    // SAFETY: getrusage fills the struct it is given; zeroed is a valid rusage either way.
    let usage = unsafe {
        libc::getrusage(libc::RUSAGE_SELF, usage.as_mut_ptr());
        usage.assume_init()
    };
    let max = u64::try_from(usage.ru_maxrss).unwrap_or(0);
    let bytes = if cfg!(target_os = "macos") { max } else { max * 1024 };
    format!("{} MiB", bytes >> 20)
}

#[cfg(not(unix))]
pub fn peak_rss() -> String {
    "not measured on this platform".into()
}
