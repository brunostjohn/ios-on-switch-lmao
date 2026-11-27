// SPDX-License-Identifier: GPL-3.0-or-later
use std::ffi::c_int;
use std::fs::{File, OpenOptions};
use std::io::{self, Read};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::process::{Command, Stdio};

use crate::Privilege;
use crate::error::{Error, Result};
use crate::raw::Geometry;

const DKIOCSYNCHRONIZE: libc::c_ulong = 0x8018_6416;
const DKIOCGETBLOCKSIZE: libc::c_ulong = 0x4004_6418;
const DKIOCGETBLOCKCOUNT: libc::c_ulong = 0x4008_6419;
const DKIOCGETPHYSICALBLOCKSIZE: libc::c_ulong = 0x4004_644d;

#[repr(C)]
#[derive(Default)]
struct DkSynchronize {
    offset: u64,
    length: u64,
    options: u32,
    reserved: [u8; 4],
}

/// O_EXLOCK on a disk node is an exclusive IOMedia open: while it is held, IOKit tears down the partitions and the
/// APFS container above it, and any other open of the disk fails with EBUSY.
const FLAGS: c_int = libc::O_RDWR | libc::O_EXLOCK | libc::O_NONBLOCK;

/// Opens the raw node directly when this user may (root, or an image the user attached), otherwise through
/// authopen(1), which asks for an administrator's password and hands back the descriptor.
pub fn open_raw(raw: &Path, privilege: Privilege) -> Result<File> {
    if privilege == Privilege::Least {
        match OpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(FLAGS | libc::O_CLOEXEC)
            .open(raw)
        {
            Ok(file) => return uncached(file, raw),
            Err(e) if matches!(e.raw_os_error(), Some(libc::EACCES | libc::EPERM)) => {}
            Err(e) => return Err(open_error(raw, e)),
        }
    }
    let file = authopen(raw)?;
    uncached(file, raw)
}

fn uncached(file: File, raw: &Path) -> Result<File> {
    // SAFETY: fcntl on a descriptor we own.
    if unsafe { libc::fcntl(file.as_raw_fd(), libc::F_NOCACHE, 1) } == -1 {
        return Err(Error::io(
            format!("F_NOCACHE on {}", raw.display()),
            io::Error::last_os_error(),
        ));
    }
    Ok(file)
}

fn open_error(raw: &Path, e: io::Error) -> Error {
    if e.raw_os_error() == Some(libc::EBUSY) {
        Error::Busy {
            volume: raw.display().to_string(),
            detail: "another program has the disk open".into(),
        }
    } else {
        Error::io(format!("open {}", raw.display()), e)
    }
}

fn authopen(raw: &Path) -> Result<File> {
    let (ours, theirs) = UnixStream::pair().map_err(|e| Error::io("socketpair for authopen", e))?;
    let mut child = Command::new("/usr/libexec/authopen")
        .args(["-stdoutpipe", "-o", &FLAGS.to_string()])
        .arg(raw)
        .stdin(Stdio::null())
        .stdout(Stdio::from(OwnedFd::from(theirs)))
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| Error::io("start /usr/libexec/authopen", e))?;
    let received = receive_fd(&ours);
    let mut stderr = String::new();
    if let Some(mut pipe) = child.stderr.take() {
        let _ = pipe.read_to_string(&mut stderr);
    }
    let status = child.wait().map_err(|e| Error::io("wait for authopen", e))?;
    match received {
        Ok(Some(fd)) => Ok(File::from(fd)),
        Ok(None) if stderr.contains("Resource busy") => Err(open_error(raw, io::Error::from_raw_os_error(libc::EBUSY))),
        Ok(None) => Err(Error::NotAuthorized {
            path: raw.display().to_string(),
            detail: format!("authopen {status}: {}", stderr.trim()),
        }),
        Err(e) => Err(Error::io("receive the descriptor from authopen", e)),
    }
}

/// Reads one SCM_RIGHTS descriptor, or `None` when authopen exits without sending one.
fn receive_fd(socket: &UnixStream) -> io::Result<Option<OwnedFd>> {
    let mut data = [0u8; 64];
    let mut iov = libc::iovec {
        iov_base: data.as_mut_ptr().cast(),
        iov_len: data.len(),
    };
    let mut control = [0u64; 8];
    // SAFETY: a zeroed msghdr is valid; the pointers set below outlive recvmsg.
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.as_mut_ptr().cast();
    msg.msg_controllen = size_of_val(&control) as libc::socklen_t;
    // SAFETY: msg describes buffers we own.
    if unsafe { libc::recvmsg(socket.as_raw_fd(), &mut msg, 0) } < 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: walking the control buffer recvmsg filled, with the libc macros.
    unsafe {
        let mut header = libc::CMSG_FIRSTHDR(&msg);
        while !header.is_null() {
            if (*header).cmsg_level == libc::SOL_SOCKET && (*header).cmsg_type == libc::SCM_RIGHTS {
                let fd = std::ptr::read_unaligned(libc::CMSG_DATA(header).cast::<c_int>());
                libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC);
                return Ok(Some(OwnedFd::from_raw_fd(fd)));
            }
            header = libc::CMSG_NXTHDR(&msg, header);
        }
    }
    Ok(None)
}

pub fn geometry(file: &File, raw: &Path) -> Result<Geometry> {
    let logical: u32 =
        ioctl_read(file, DKIOCGETBLOCKSIZE).map_err(|e| Error::io(format!("block size of {}", raw.display()), e))?;
    let count: u64 =
        ioctl_read(file, DKIOCGETBLOCKCOUNT).map_err(|e| Error::io(format!("block count of {}", raw.display()), e))?;
    let physical: u32 = ioctl_read(file, DKIOCGETPHYSICALBLOCKSIZE).unwrap_or(logical);
    Ok(Geometry {
        size: count * u64::from(logical),
        logical,
        physical,
    })
}

fn ioctl_read<T: Default>(file: &File, request: libc::c_ulong) -> io::Result<T> {
    let mut value = T::default();
    // SAFETY: each request writes exactly one T, as sys/disk.h declares it.
    if unsafe { libc::ioctl(file.as_raw_fd(), request, &mut value as *mut T) } == -1 {
        return Err(io::Error::last_os_error());
    }
    Ok(value)
}

/// SYNCHRONIZE CACHE for the whole device, as DKIOCSYNCHRONIZE with a zero range means.
pub fn flush(file: &File) -> io::Result<()> {
    let mut range = DkSynchronize::default();
    // SAFETY: the request reads one dk_synchronize_t.
    if unsafe { libc::ioctl(file.as_raw_fd(), DKIOCSYNCHRONIZE, &mut range as *mut DkSynchronize) } == -1 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}
