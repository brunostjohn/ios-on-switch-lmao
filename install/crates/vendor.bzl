"""@installer_vendor: every crate in install/crates/Cargo.lock as a cargo directory source, for :deny_test.

cargo-deny reads the crate graph from `cargo metadata`, which loads the manifest of every package in the lockfile,
whichever platform it is for. Only what that needs, and the licence texts, are exposed as :vendor.
"""

load("//gpu:deps.bzl", "lock_packages")

_CRATES_IO = "registry+https://github.com/rust-lang/crates.io-index"

_BUILD = """\
package(default_visibility = ["//visibility:public"])

exports_files(["crates.txt"])

filegroup(
    name = "vendor",
    srcs = glob(["vendor/*/" + f for f in [
        ".cargo-checksum.json",
        "COPYING*",
        "Cargo.toml",
        "LICENCE*",
        "LICENSE*",
        "UNLICENSE",
        "build.rs",
        "src/lib.rs",
        "src/main.rs",
    ]], allow_empty = True),
)
"""

def _installer_vendor_impl(rctx):
    crates = []
    for package in lock_packages(rctx.read(rctx.attr.cargo_lockfile)):
        if package.get("source") != _CRATES_IO:
            continue
        name, version, checksum = package["name"], package["version"], package["checksum"]
        directory = "vendor/{}-{}".format(name, version)
        rctx.download_and_extract(
            url = "https://static.crates.io/crates/{0}/{0}-{1}.crate".format(name, version),
            sha256 = checksum,
            stripPrefix = "{}-{}".format(name, version),
            output = directory,
            type = "tar.gz",
        )
        rctx.file(directory + "/.cargo-checksum.json", json.encode({"files": {}, "package": checksum}))
        crates.append("{} {}".format(name, version))
    rctx.file("crates.txt", "\n".join(crates) + "\n")
    rctx.file("BUILD.bazel", _BUILD)
    return rctx.repo_metadata(reproducible = True)

installer_vendor = repository_rule(
    implementation = _installer_vendor_impl,
    attrs = {"cargo_lockfile": attr.label(mandatory = True, allow_single_file = True)},
)
