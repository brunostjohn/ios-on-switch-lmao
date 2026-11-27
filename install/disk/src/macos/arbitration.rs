// SPDX-License-Identifier: GPL-3.0-or-later
//! A DiskArbitration session that refuses every mount on one physical disk while it lives, and runs unmount, mount
//! and eject requests to completion. Its callbacks run on a private dispatch queue; callers wait on a condition
//! variable with a deadline.
use std::collections::HashMap;
use std::ffi::{CStr, CString, c_void};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use dispatch2::{DispatchQueue, DispatchRetained};
use objc2_core_foundation::{CFRetained, CFString};
use objc2_disk_arbitration::{
    DADisk, DADissenter, DARegisterDiskAppearedCallback, DARegisterDiskMountApprovalCallback, DASession,
    DAUnregisterApprovalCallback, DAUnregisterCallback, kDADiskEjectOptionDefault, kDADiskMountOptionDefault,
    kDADiskUnmountOptionDefault, kDADiskUnmountOptionForce, kDAReturnBusy, kDAReturnExclusiveAccess,
};

use super::iokit;
use crate::error::{Error, Result};

pub const REQUEST_TIMEOUT: Duration = Duration::from_secs(60);

pub struct Arbiter {
    session: CFRetained<DASession>,
    queue: DispatchRetained<DispatchQueue>,
    state: Arc<State>,
}

struct State {
    root: u64,
    refusing: AtomicBool,
    events: Mutex<Events>,
    changed: Condvar,
}

#[derive(Default)]
struct Events {
    appeared: Vec<String>,
    refused: Vec<String>,
    finished: HashMap<String, std::result::Result<(), Dissent>>,
}

#[derive(Clone, Debug)]
struct Dissent {
    status: i32,
    detail: String,
}

impl Arbiter {
    /// Starts refusing mounts of anything on the physical disk with IOKit registry ID `root`.
    pub fn hold(root: u64) -> Result<Self> {
        // SAFETY: DASessionCreate with the default allocator.
        let session = unsafe { DASession::new(None) }.ok_or_else(|| Error::os("DiskArbitration", "no session"))?;
        let queue = DispatchQueue::new("com.brunostjohn.switch.disk", None);
        let state = Arc::new(State {
            root,
            refusing: AtomicBool::new(true),
            events: Mutex::new(Events::default()),
            changed: Condvar::new(),
        });
        let arbiter = Arbiter { session, queue, state };
        // SAFETY: the context outlives the registration (Drop unregisters before the Arc goes) and the callbacks
        // match DiskArbitration's signatures.
        unsafe {
            DARegisterDiskMountApprovalCallback(&arbiter.session, None, Some(approve_mount), arbiter.context());
            DARegisterDiskAppearedCallback(&arbiter.session, None, Some(appeared), arbiter.context());
            arbiter.session.set_dispatch_queue(Some(&arbiter.queue));
        }
        Ok(arbiter)
    }

    /// Ends the refusal. DiskArbitration does not retry a refused mount by itself.
    pub fn stop_refusing(&self) {
        self.state.refusing.store(false, Ordering::SeqCst);
    }

    #[cfg(all(test, feature = "device-tests"))]
    pub fn refused(&self) -> Vec<String> {
        self.state.lock().refused.clone()
    }

    pub fn unmount(&self, bsd: &str, force: bool) -> Result<()> {
        let options = if force {
            kDADiskUnmountOptionForce
        } else {
            kDADiskUnmountOptionDefault
        };
        let disk = self.disk(bsd)?;
        // SAFETY: `finished` matches DADiskUnmountCallback and the context outlives the request.
        self.request(bsd, "unmount", || unsafe {
            disk.unmount(options, Some(finished), self.context())
        })
    }

    pub fn mount(&self, bsd: &str) -> Result<()> {
        let disk = self.disk(bsd)?;
        // SAFETY: as for unmount.
        self.request(bsd, "mount", || unsafe {
            disk.mount(None, kDADiskMountOptionDefault, Some(finished), self.context())
        })
    }

    pub fn eject(&self, bsd: &str) -> Result<()> {
        let disk = self.disk(bsd)?;
        // SAFETY: as for unmount.
        self.request(bsd, "eject", || unsafe {
            disk.eject(kDADiskEjectOptionDefault, Some(finished), self.context())
        })
    }

    /// A marker for [`Arbiter::wait_appeared`]: appearances before it don't count.
    pub fn mark(&self) -> usize {
        self.state.lock().appeared.len()
    }

    /// Waits until DiskArbitration has announced each of `names` after `mark`: it has probed them and finished
    /// deciding whether to mount them.
    pub fn wait_appeared(&self, names: &[String], mark: usize, timeout: Duration) -> Result<()> {
        let deadline = Instant::now() + timeout;
        let mut events = self.state.lock();
        loop {
            let seen = &events.appeared[mark.min(events.appeared.len())..];
            let missing: Vec<&String> = names.iter().filter(|n| !seen.contains(n)).collect();
            if missing.is_empty() {
                return Ok(());
            }
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Err(Error::Timeout {
                    what: format!("macOS to announce {missing:?}"),
                    waited: timeout,
                });
            }
            events = self
                .state
                .changed
                .wait_timeout(events, left)
                .map(|(e, _)| e)
                .unwrap_or_else(|e| e.into_inner().0);
        }
    }

    fn request(&self, bsd: &str, op: &str, send: impl FnOnce()) -> Result<()> {
        self.state.lock().finished.remove(bsd);
        send();
        let deadline = Instant::now() + REQUEST_TIMEOUT;
        let mut events = self.state.lock();
        loop {
            if let Some(result) = events.finished.remove(bsd) {
                return result.map_err(|d| dissent_error(bsd, op, d));
            }
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Err(Error::Timeout {
                    what: format!("macOS to {op} /dev/{bsd}"),
                    waited: REQUEST_TIMEOUT,
                });
            }
            events = self
                .state
                .changed
                .wait_timeout(events, left)
                .map(|(e, _)| e)
                .unwrap_or_else(|e| e.into_inner().0);
        }
    }

    fn disk(&self, bsd: &str) -> Result<CFRetained<DADisk>> {
        let name = CString::new(bsd).map_err(|_| Error::Changed(bsd.to_owned()))?;
        let ptr = NonNull::new(name.as_ptr().cast_mut()).expect("CString is never null");
        // SAFETY: a NUL-terminated BSD name that lives across the call.
        unsafe { DADisk::from_bsd_name(None, &self.session, ptr) }.ok_or_else(|| Error::Changed(format!("/dev/{bsd}")))
    }

    fn context(&self) -> *mut c_void {
        Arc::as_ptr(&self.state).cast_mut().cast()
    }
}

impl Drop for Arbiter {
    fn drop(&mut self) {
        let approve = NonNull::new(approve_mount as *mut c_void).expect("function pointer");
        let appear = NonNull::new(appeared as *mut c_void).expect("function pointer");
        // SAFETY: these are the registrations made in hold, with the same context.
        unsafe {
            DAUnregisterApprovalCallback(&self.session, approve, self.context());
            DAUnregisterCallback(&self.session, appear, self.context());
            self.session.set_dispatch_queue(None);
        }
        // A callback already queued still holds the context: let it finish before the state goes.
        self.queue.exec_sync(|| {});
    }
}

impl State {
    fn lock(&self) -> MutexGuard<'_, Events> {
        self.events.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn note(&self, record: impl FnOnce(&mut Events)) {
        record(&mut self.lock());
        self.changed.notify_all();
    }
}

fn dissent_error(bsd: &str, op: &str, dissent: Dissent) -> Error {
    if dissent.status == kDAReturnBusy {
        Error::Busy {
            volume: format!("/dev/{bsd}"),
            detail: dissent.detail,
        }
    } else {
        Error::os(format!("{op} /dev/{bsd}"), dissent.detail)
    }
}

/// # Safety
/// `context` is the `State` an `Arbiter` registered; DiskArbitration calls this on the arbiter's queue.
unsafe fn state<'a>(context: *mut c_void) -> &'a State {
    unsafe { &*context.cast::<State>() }
}

fn bsd_name(disk: NonNull<DADisk>) -> String {
    // SAFETY: DiskArbitration passes a live disk; its BSD name is a C string or null.
    let name = unsafe { disk.as_ref().bsd_name() };
    if name.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(name) }.to_string_lossy().into_owned()
}

unsafe extern "C-unwind" fn approve_mount(disk: NonNull<DADisk>, context: *mut c_void) -> *const DADissenter {
    let state = unsafe { state(context) };
    if !state.refusing.load(Ordering::SeqCst) {
        return std::ptr::null();
    }
    let bsd = bsd_name(disk);
    if !iokit::media_by_bsd(&bsd).is_some_and(|m| m.is_on(state.root)) {
        return std::ptr::null();
    }
    state.note(|e| e.refused.push(bsd));
    let reason = CFString::from_static_str("the iPadOS installer is writing this disk");
    // SAFETY: DiskArbitration releases the dissenter it is handed.
    let dissenter = unsafe { DADissenter::new(None, kDAReturnExclusiveAccess, Some(&reason)) };
    CFRetained::into_raw(dissenter).as_ptr()
}

unsafe extern "C-unwind" fn appeared(disk: NonNull<DADisk>, context: *mut c_void) {
    let state = unsafe { state(context) };
    let bsd = bsd_name(disk);
    state.note(|e| e.appeared.push(bsd));
}

unsafe extern "C-unwind" fn finished(disk: NonNull<DADisk>, dissenter: *const DADissenter, context: *mut c_void) {
    let state = unsafe { state(context) };
    let bsd = bsd_name(disk);
    // SAFETY: a dissenter, when there is one, lives for the duration of the callback.
    let result = match unsafe { dissenter.as_ref() } {
        None => Ok(()),
        Some(d) => {
            let status = unsafe { d.status() };
            let detail = unsafe { d.status_string() }
                .map(|s| s.to_string())
                .unwrap_or_else(|| format!("{status:#x}"));
            Err(Dissent { status, detail })
        }
    };
    state.note(|e| {
        e.finished.insert(bsd, result);
    });
}
