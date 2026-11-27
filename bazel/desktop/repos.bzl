"""The downloads the desktop toolchains (bazel/desktop) add to Rust 1.98.0 and @llvm, each pinned by sha256: the
`desktop_toolchains` module extension in MODULE.bazel.

@desktop_rust_std  rust-std 1.98.0 for the targets the build's own Rust toolchain repositories don't carry.
@desktop_musl      musl's headers and empty stub archives (libpthread/m/rt/dl), plus prebuilt static libc++ headers
                   and archives (libc++.a, libc++abi.a) from Alpine v3.20. rust-std supplies libc.a, libunwind.a and
                   the crt objects (self-contained linking).
@desktop_msvc      the MSVC CRT and the Windows SDK for x86_64 and aarch64, which xwin fetches from Microsoft's
                   servers. Microsoft's Visual Studio and Windows SDK licences are accepted for every build machine.
"""

RUST_STD_VERSION = "1.98.0"

# channel-rust-1.98.0.toml's xz_hash values (rules_rust 0.75.0's known_shas.bzl).
RUST_STD = {
    "aarch64-pc-windows-msvc": "df28cbf39697dfbfac0633814b56c1ca6f07fd32a70ab2074cb095f3871ae338",
    "aarch64-unknown-linux-musl": "36f82e96e86ec8a02d0a0a66574f4b7be086771cec41bb8e9d40ba5cbd64c93a",
    "x86_64-apple-darwin": "8923fa9d0e0407b8a492e59d568e7aceff75949e1eee2e3e1b60e174373890cb",
    "x86_64-pc-windows-msvc": "81d123f834111ebeaf8b1eed0e93b0b66ea4bcf4cc1d3836d9a0314d4ea94a73",
    "x86_64-unknown-linux-musl": "1a76f782db2d540e1cd16ea47829b323f4a8f4dda64bca4b23be189109c510f8",
}

# Alpine 3.20 is end of life, so its repository no longer changes. musl 1.2.5 is the musl the targets' rust-std
# bundles; libc++ 17.0.6 (Apache-2.0 WITH LLVM-exception) is built against it.
MUSL_DEV = {
    "aarch64": "54c60103a486d0d54263289fd0bf89b490514e04b74d5228c346c78089d587e4",
    "x86_64": "e9cca1709f3a740976f22cc27b825261de062d97e7de2d08c41243017c389338",
}
_MUSL_DEV_URL = "https://dl-cdn.alpinelinux.org/alpine/v3.20/main/{arch}/musl-dev-1.2.5-r4.apk"

LIBCXX_DEV = {
    "aarch64": "bb10ca836332848b0e53ee5fa3c649a960865c00dd6f539140b0b17c1014cbd7",
    "x86_64": "d2431d30c342361796825652cc89d627816d5f1f125829b885d8858bf22c5f80",
}
_LIBCXX_DEV_URL = "https://dl-cdn.alpinelinux.org/alpine/v3.20/main/{arch}/libc++-dev-17.0.6-r1.apk"

LIBCXX_STATIC = {
    "aarch64": "5b42cbd768fc4dfe86c595e7be471b4eed97cce1323d1a9f83f135acbdedac82",
    "x86_64": "9b326f13d966d25d911fd8127aeb463175238bf0e372d3ef53301ca68a5aaa69",
}
_LIBCXX_STATIC_URL = "https://dl-cdn.alpinelinux.org/alpine/v3.20/main/{arch}/libc++-static-17.0.6-r1.apk"

# musl puts pthread, m, rt and dl inside libc.a and ships 8-byte empty archives for them. libc++.a and libc++abi.a
# record `pthread` in SHT_LLVM_DEPENDENT_LIBRARIES, so rust-lld looks for libpthread.a on -L; extracting only these
# stubs satisfies that without shadowing rust-std's self-contained libc.a or crt objects.
_MUSL_STUB_LIBS = [
    "usr/lib/libdl.a",
    "usr/lib/libm.a",
    "usr/lib/libpthread.a",
    "usr/lib/librt.a",
]

# xwin 0.10.0 as Homebrew builds it: xwin's own releases stopped shipping macOS binaries after 0.6.5.
XWIN = struct(
    url = "https://ghcr.io/v2/homebrew/core/xwin/blobs/sha256:bede9a47eb9d801ef267bb4f6f6348a200132785da7a2684daeac45ba8a34957",
    sha256 = "bede9a47eb9d801ef267bb4f6f6348a200132785da7a2684daeac45ba8a34957",
    binary = "xwin/0.10.0/bin/xwin",
)

# Visual Studio 17.14.45's channel manifest (https://aka.ms/vs/17/release/channel on 25 Nov 2026) and the package
# manifest it names. xwin checks every package against the package manifest's sha256s, but not the package manifest
# itself: the sha256 the channel gives for it never matches what Microsoft serves (xwin's src/manifest.rs). So it is
# pinned here and put where xwin's cache looks for it, under the name xwin gives it.
VS_CHANNEL = struct(
    url = "https://download.visualstudio.microsoft.com/download/pr/95995f57-8355-4ff5-8d90-a7e1669fb3b3/a788dfa3e6f50a33f7dee8396fd40d2946c69a776c0cfa5208f56d461fd1a825/VisualStudio.17.Release.chman",
    sha256 = "44a3e7dc44486c945abdc1fae97e7e22de4d05e80f03b00e9302ba4b72c3e0e3",
    packages_url = "https://download.visualstudio.microsoft.com/download/pr/95995f57-8355-4ff5-8d90-a7e1669fb3b3/da49c304df0fff4e75c20849171840a06840e845ce814104d709dfb5cd461668/VisualStudio.vsman",
    packages_sha256 = "d1d8109899558618035724949673f69f8c1b5704f542a2e76f31d87e425d5710",
    packages_cache_name = "pkg_manifest_da49c304df0fff4e75c20849171840a06840e845ce814104d709dfb5cd461668.vsman",
    crt_version = "14.44.17.14",
    sdk_version = "10.0.26100",
)

_ALL_FILES = """\
package(default_visibility = ["//visibility:public"])
"""

def _rust_std_impl(rctx):
    build = [
        'load("@rules_rust//rust:toolchain.bzl", "rust_stdlib_filegroup")',
        _ALL_FILES,
    ]
    for triple, sha256 in RUST_STD.items():
        archive = "rust-std-{}-{}".format(RUST_STD_VERSION, triple)
        rctx.download_and_extract(
            url = "https://static.rust-lang.org/dist/{}.tar.xz".format(archive),
            sha256 = sha256,
            stripPrefix = "{}/rust-std-{}".format(archive, triple),
        )
        build.append("""rust_stdlib_filegroup(
    name = "{triple}",
    srcs = glob(["lib/rustlib/{triple}/lib/**"]),
)
""".format(triple = triple))
    rctx.file("BUILD.bazel", "\n".join(build))
    return rctx.repo_metadata(reproducible = True)

_rust_std = repository_rule(implementation = _rust_std_impl)

def _musl_impl(rctx):
    build = [_ALL_FILES]
    for arch, sha256 in MUSL_DEV.items():
        rctx.file(arch + "/SYSROOT", "")
        for name, url, digest, members in [
            ("musl", _MUSL_DEV_URL, sha256, ["usr/include"] + _MUSL_STUB_LIBS),
            ("libcxx-dev", _LIBCXX_DEV_URL, LIBCXX_DEV[arch], ["usr/include/c++/v1"]),
            ("libcxx-static", _LIBCXX_STATIC_URL, LIBCXX_STATIC[arch], ["usr/lib/libc++.a", "usr/lib/libc++abi.a"]),
        ]:
            apk = "{}-{}.apk".format(arch, name)
            rctx.download(url = url.format(arch = arch), output = apk, sha256 = digest)
            result = rctx.execute(["/usr/bin/tar", "-xzf", apk, "-C", arch] + members)
            if result.return_code:
                fail("extracting {} ({}): {}".format(name, arch, result.stderr))
            rctx.delete(apk)
        build.append("""exports_files(["{arch}/SYSROOT"])

filegroup(
    name = "{arch}",
    srcs = glob([
        "{arch}/usr/include/**",
        "{arch}/usr/lib/*.a",
    ]),
)

filegroup(
    name = "{arch}_cxx_libs",
    srcs = [
        "{arch}/usr/lib/libc++.a",
        "{arch}/usr/lib/libc++abi.a",
    ],
)
""".format(arch = arch))
    rctx.file("BUILD.bazel", "\n".join(build))
    return rctx.repo_metadata(reproducible = True)

_musl = repository_rule(implementation = _musl_impl)

def _msvc_impl(rctx):
    rctx.download_and_extract(url = XWIN.url, sha256 = XWIN.sha256, output = "xwin", type = "tar.gz", headers = {
        # ghcr's anonymous token, the one Homebrew sends.
        "Authorization": "Bearer QQ==",
    })
    rctx.download(url = VS_CHANNEL.url, output = "channel.chman", sha256 = VS_CHANNEL.sha256)
    rctx.download(
        url = VS_CHANNEL.packages_url,
        output = "cache/dl/" + VS_CHANNEL.packages_cache_name,
        sha256 = VS_CHANNEL.packages_sha256,
    )
    result = rctx.execute([
        "xwin/" + XWIN.binary,
        "--accept-license",
        "--log-level=warn",
        "--manifest=channel.chman",
        "--arch=x86_64,aarch64",
        "--variant=desktop",
        "--crt-version=" + VS_CHANNEL.crt_version,
        "--sdk-version=" + VS_CHANNEL.sdk_version,
        "--cache-dir=cache",
        "splat",
        # The build runs on case-insensitive APFS, where xwin's case-fixing symlinks collide with their targets.
        "--disable-symlinks",
        "--output=.",
    ], timeout = 1800, quiet = False)
    if result.return_code:
        fail("xwin splat: " + result.stderr)

    # xwin leaves a `10.0.26100 -> .` link in sdk/include and sdk/lib even with --disable-symlinks, and a glob follows
    # it forever. rm, not rctx.delete, so only the link goes.
    result = rctx.execute(["/bin/rm", "sdk/include/" + VS_CHANNEL.sdk_version, "sdk/lib/" + VS_CHANNEL.sdk_version])
    if result.return_code:
        fail("removing xwin's version links: " + result.stderr)
    for path in ["cache", "xwin", "channel.chman", "sdk/include/winrt", "sdk/include/cppwinrt"] + [
        "crt/lib/{}/enclave".format(arch)
        for arch in ("x86_64", "aarch64")
    ]:
        rctx.delete(path)
    rctx.file("SYSROOT", "")
    rctx.file("BUILD.bazel", _ALL_FILES + """
exports_files(["SYSROOT"])

filegroup(
    name = "headers",
    srcs = glob(["crt/include/**", "sdk/include/**"]),
)
""" + "".join(["""
filegroup(
    name = "{arch}_libs",
    srcs = glob(["crt/lib/{arch}/**", "sdk/lib/um/{arch}/**", "sdk/lib/ucrt/{arch}/**"]),
)
""".format(arch = arch) for arch in ("x86_64", "aarch64")]))
    return rctx.repo_metadata(reproducible = True)

_msvc = repository_rule(implementation = _msvc_impl)

def _desktop_toolchains_impl(module_ctx):
    _rust_std(name = "desktop_rust_std")
    _musl(name = "desktop_musl")
    _msvc(name = "desktop_msvc")
    return module_ctx.extension_metadata(reproducible = True)

desktop_toolchains = module_extension(implementation = _desktop_toolchains_impl)
