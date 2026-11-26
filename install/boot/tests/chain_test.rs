// SPDX-License-Identifier: GPL-3.0-or-later
//! The installer's whole boot path for the selected kind: the release's boot assets and the IPSW's members, as the
//! zip holds them, through switch_boot, against every file of //boot:package's tree.

mod case;

use std::time::Instant;

use case::{Case, read};
use sha2::{Digest, Sha256};
use switch_boot::boot_files;
use switch_catalog::Build;

const INSTALLD: &str = "overlay/usr/libexec/installd";
const CATALOGUE: [&str; 2] = [
    "overlay/System/Library/xpc/launchd.plist",
    "overlay/System/Library/xpc/launchd.plist.sig",
];

/// What the release's other assets put in the tree: the overlay but the files made from Apple's, and the bootstrap
/// archive.
fn from_other_assets(path: &str, build: &Build) -> bool {
    let ours = path == INSTALLD || (build.steps.launchd_catalog && CATALOGUE.contains(&path));
    !ours && (path.starts_with("overlay/") || path == "bootstrap/payload.tar")
}

fn max_rss() -> u64 {
    let mut usage = std::mem::MaybeUninit::<libc::rusage>::zeroed();
    // SAFETY: getrusage fills the struct it is given.
    let usage = unsafe {
        libc::getrusage(libc::RUSAGE_SELF, usage.as_mut_ptr());
        usage.assume_init()
    };
    let rss = usage.ru_maxrss as u64;
    if cfg!(target_os = "macos") { rss } else { rss * 1024 }
}

const MIB: f64 = (1 << 20) as f64;

#[test]
fn from_the_ipsw_and_the_boot_assets_to_the_package() {
    let case = Case::load();
    let kind = &case.kind;
    let build = case.build();
    let start = Instant::now();
    let assets = case.assets();
    let files = case.member_files();
    let read_in = start.elapsed();
    let rss_inputs = max_rss();
    let package = boot_files(&build, &assets, &files.members()).unwrap();
    let made_in = start.elapsed() - read_in;
    let rss_peak = max_rss();
    println!(
        "{kind}: read the assets (checked against the catalog) and members {:.2} s, boot_files {:.2} s; peak RSS \
         {:.0} MiB with the inputs read, {:.0} MiB at the end",
        read_in.as_secs_f64(),
        made_in.as_secs_f64(),
        rss_inputs as f64 / MIB,
        rss_peak as f64 / MIB
    );

    let paths = case.package_paths();
    for (path, data) in &package.files {
        assert!(paths.contains(path), "{kind}: {path} is not in //boot:package");
        let expected = read(&case.package().join(path));
        println!(
            "{kind}: {path} {} ({} B)",
            hex::encode(Sha256::digest(data)),
            data.len()
        );
        assert!(*data == expected, "{kind}: {path} differs from //boot:package's");
    }
    let others: Vec<&String> = paths.iter().filter(|p| !package.files.contains_key(*p)).collect();
    let unmade: Vec<&&String> = others.iter().filter(|p| !from_other_assets(p, &build)).collect();
    assert!(
        unmade.is_empty(),
        "{kind}: //boot:package has files switch_boot does not make: {unmade:?}"
    );
    println!(
        "{kind}: {} files equal to //boot:package's, {} from the release's other assets \
         (overlay/, bootstrap/payload.tar); installd {}",
        package.files.len(),
        others.len(),
        hex::encode(package.installd_cdhash)
    );
}
