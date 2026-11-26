// SPDX-License-Identifier: GPL-3.0-or-later
//! udisks2 over D-Bus, for a user who isn't root: polkit decides, and asks for a password through the session's
//! agent when its policy says so.
//!
//! Each call runs on a thread of its own inside zbus's `block_on`. zbus panics or deadlocks when `block_on` runs
//! inside another runtime's task, and the installer calls this crate from Tokio.
use std::collections::HashMap;
use std::future::Future;
use std::os::fd::OwnedFd;

use udisks2::Client;
use udisks2::zbus::{self, OwnedObjectPath, Variant};

use crate::error::{Error, Result};

pub fn unmount(device: &str, force: bool) -> Result<()> {
    let device = device.to_owned();
    run(format!("unmount {device}"), async move {
        let client = Client::new().await?;
        let path = resolve(&client, &device).await?;
        let filesystem = object(&client, path).filesystem().await?;
        filesystem
            .unmount(HashMap::from([("force", Variant::from(force))]))
            .await
    })
}

pub fn open(device: &str, flags: i32) -> Result<OwnedFd> {
    let device = device.to_owned();
    run(format!("open {device} through udisks"), async move {
        let client = Client::new().await?;
        let path = resolve(&client, &device).await?;
        let block = object(&client, path).block().await?;
        let fd = block
            .open_device("rw", HashMap::from([("flags", Variant::from(flags))]))
            .await?;
        Ok(OwnedFd::from(fd))
    })
}

pub fn rescan(device: &str) -> Result<()> {
    let device = device.to_owned();
    run(format!("rescan {device}"), async move {
        let client = Client::new().await?;
        let path = resolve(&client, &device).await?;
        let block = object(&client, path).block().await?;
        block.rescan(HashMap::new()).await
    })
}

pub fn eject(device: &str) -> Result<()> {
    let device = device.to_owned();
    run(format!("eject {device}"), async move {
        let client = Client::new().await?;
        let path = resolve(&client, &device).await?;
        let block = object(&client, path).block().await?;
        let drive = object(&client, block.drive().await?).drive().await?;
        drive.eject(HashMap::new()).await
    })
}

fn object(client: &Client, path: OwnedObjectPath) -> udisks2::Object {
    let Ok(object) = client.object(path);
    object
}

async fn resolve(client: &Client, device: &str) -> udisks2::Result<OwnedObjectPath> {
    let spec = HashMap::from([("path", Variant::from(device))]);
    let found = client.manager().resolve_device(spec, HashMap::new()).await?;
    let missing = || udisks2::Error::Zbus(zbus::Error::Failure(format!("udisks has no device {device}")));
    found.into_iter().next().ok_or_else(missing)
}

fn run<T, F>(what: String, call: F) -> Result<T>
where
    T: Send + 'static,
    F: Future<Output = udisks2::Result<T>> + Send + 'static,
{
    let joined = std::thread::spawn(move || zbus::block_on(call)).join();
    let result = joined.map_err(|_| Error::os(what.clone(), "the D-Bus thread panicked"))?;
    result.map_err(|e| translate(what, e))
}

fn translate(what: String, e: udisks2::Error) -> Error {
    match e {
        udisks2::Error::DeviceBusy => Error::Busy {
            volume: what,
            detail: "a program has files open on it".into(),
        },
        udisks2::Error::NotAuthorized
        | udisks2::Error::NotAuthorizedCanObtain
        | udisks2::Error::NotAuthorizedDismissed => Error::NotAuthorized {
            path: what,
            detail: format!("polkit: {e:?}"),
        },
        other => Error::os(what, format!("udisks: {other:?}")),
    }
}
