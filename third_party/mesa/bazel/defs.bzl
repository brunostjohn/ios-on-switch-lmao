"""Mesa's NVK build in Bazel, the way meson builds it (third_party/mesa/meson.build and its subdirectories):

  mesa_cc_library   one meson static_library's objects: cc_library with exactly meson's compile flags for that target
                    (mesa_args), one action per source, so an edit recompiles that file only
  mesa_platform     the transition into the configuration NVK builds in: the iOS or macOS user platform and Rust 1.96.1
  cargo_tool        bindgen and cbindgen (host tools, from their own Cargo.lock)

Paths: Mesa's sources are third_party/mesa/<path>; what meson writes to its build directory, Bazel writes to
$(BINDIR)/third_party/mesa/<the same path> (every generator's outputs are declared in the package of the meson.build
that makes them), so meson's include directories translate one for one. Strings that reach the binary keep meson's
spelling: __FILE__ of a source is ../mesa/<path> and of a generated file <path> (-fmacro-prefix-map), as in the
meson build directory next to the source tree that main's dylibs were built in.
"""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//kernel/rules:common.bzl", "XCODE_EXECUTION")
load(
    ":flags.bzl",
    "MESA_CPP_ARGS",
    "MESA_CPP_ARGS_LINUX",
    "MESA_CPP_ARGS_WINDOWS",
    "MESA_C_ARGS",
    "MESA_C_ARGS_LINUX",
    "MESA_C_ARGS_WINDOWS",
    "MESA_PRE_ARGS",
    "MESA_PRE_ARGS_LINUX",
    "MESA_PRE_ARGS_WINDOWS",
)

MESA = "third_party/mesa"

# Targets tied to NVK's Mach-O configuration (:user platforms, Rust 1.96.1).
MESA_USER_ONLY = ["//bazel/platforms:user"]

# Mesa's C/genrule/rlib targets build in NVK's configuration (:user) and on desktop/host platforms, never in the kernel.
# Every Mesa internal target is also tagged `manual` so host `bazel build //...` skips them unless depended on.
MESA_ONLY = select({
    "//bazel/platforms:kernel": ["@platforms//:incompatible"],
    "//conditions:default": [],
})

# Mesa's own files in a package: never ios-on-switch-lmao's to format or lint (lint_files(dirs = [], exclude = ...)).
MESA_LINT_EXCLUDE = [
    "*.c",
    "*.cc",
    "*.cpp",
    "*.h",
    "*.hpp",
    "*.inc",
    "*.json",
    "*.m",
    "*.md",
    "*.mm",
    "*.py",
    "*.rs",
    "*.sh",
    "*.toml",
    ".clang-format",
    ".clang-tidy",
]

# What the generators' Python imports beyond the standard library.
MESA_PY_DEPS = [
    "@switch_pypi//mako",
    "@switch_pypi//pyyaml",
]

# meson's buildtype and b_ndebug arguments, which it puts after a target's include directories and visibility: the
# debug profile is today's NVK (buildtype=release, b_ndebug=true, no LTO; //bazel:profile); release adds full LTO
# only for NVK's Darwin :user builds (desktop links clang C archives with rust-lld, which rejects foreign LTO bitcode).
_HEAD = {
    "c": ["-DNDEBUG", "-Wall", "-Winvalid-pch", "-std=c11", "-O3"],
    "cxx": ["-DNDEBUG", "-Wall", "-Winvalid-pch", "-std=c++17", "-O3"],
}

_HEAD_WINDOWS = {
    "c": ["/DNDEBUG", "/std:c11", "/O2"],
    "cxx": ["/DNDEBUG", "/std:c++17", "/O2"],
}

_PROFILE = select({
    "//third_party/mesa/bazel:darwin_user_release": ["-flto"],
    "//conditions:default": [],
})

# The iOS cross file's c_args (after the project's c_args).
_CROSS_C = select({
    "//third_party/mesa/bazel:ios": ["-Wno-error=incompatible-function-pointer-types"],
    "//conditions:default": [],
})

# __FILE__ as meson's build directory spelled it.
_PREFIX_MAPS = [
    "-fmacro-prefix-map={}/=../mesa/".format(MESA),
    "-fmacro-prefix-map=$(BINDIR)/{}/=".format(MESA),
]

_PREFIX_MAPS_WINDOWS = [
    "/clang:-fmacro-prefix-map={}/=../mesa/".format(MESA),
    "/clang:-fmacro-prefix-map=$(BINDIR)/{}/=".format(MESA),
]

def _shell(flag):
    """copts are Bourne-tokenized: quote a flag that carries quotes (-DPACKAGE_VERSION="27.0.0-devel")."""
    if '"' in flag or "'" in flag or " " in flag:
        return "'" + flag.replace("'", "'\\''") + "'"
    return flag

def _include(directory, prefix = "-I"):
    """The -I (or /I on Windows) for one of meson's include directories.

    Args:
        directory: "src/util" a source directory, "gen:src/util" a build one ("gen:" the build root), "." Mesa's
            root, "execroot:" the execution root.
        prefix: include flag prefix ("-I" or "/I").

    Returns:
        The flag.
    """
    if directory == "execroot:":
        return prefix + "."
    if directory.startswith("gen:"):
        path = directory[len("gen:"):]
        return "{}$(BINDIR)/{}{}".format(prefix, MESA, "/" + path if path else "")
    return "{}{}{}".format(prefix, MESA, "" if directory == "." else "/" + directory)

def _win_extra(flags):
    """Filters a target's extra compile args for clang-cl (drops GCC/Darwin-only warning and platform flags)."""
    out = []
    for f in flags:
        if f == "-DNVK_HAS_DARWIN":
            continue
        if f.startswith("-D") or f.startswith("/"):
            out.append(_shell(f))
    return out

def _linux_extra(flags):
    """Filters a target's extra compile args for Linux musl (drops Darwin-only platform defines)."""
    return [_shell(f) for f in flags if f != "-DNVK_HAS_DARWIN"]

def mesa_args(lang, include_dirs, hidden = True, pre = True, proj = True, extra = []):
    """One meson target's compile arguments for one language, in meson's order.

    Args:
        lang: "c" or "cxx".
        include_dirs: the target's include directories in meson's order (see _include; not `includes`, which
            buildifier would sort).
        hidden: gnu_symbol_visibility: 'hidden' (-fvisibility=hidden).
        pre: the project's pre_args (MESA_PRE_ARGS).
        proj: the project's c_args/cpp_args (MESA_C_ARGS / MESA_CPP_ARGS) and, for C on iOS, the cross file's c_args.
        extra: the target's own arguments (c_args/cpp_args, its dependencies' compile_args), or a dict of them by
            platform ("ios", "macos", optional "linux", "windows") where builds differ.

    Returns:
        The arguments, a list (or a list with select()s).
    """
    unix_head = [_include(d) for d in include_dirs]
    if hidden:
        unix_head.append("-fvisibility=hidden")
    win_head = [_include(d, "/I") for d in include_dirs]
    args = select({
        "//third_party/mesa/bazel:windows": [_shell(f) for f in win_head + _HEAD_WINDOWS[lang]],
        "//conditions:default": [_shell(f) for f in unix_head + _HEAD[lang]],
    }) + _PROFILE
    if pre:
        args = args + select({
            "//third_party/mesa/bazel:linux": [_shell(f) for f in MESA_PRE_ARGS_LINUX],
            "//third_party/mesa/bazel:windows": [_shell(f) for f in MESA_PRE_ARGS_WINDOWS],
            "//conditions:default": [_shell(f) for f in MESA_PRE_ARGS],
        })
    if proj:
        args = args + select({
            "//third_party/mesa/bazel:linux": MESA_C_ARGS_LINUX if lang == "c" else MESA_CPP_ARGS_LINUX,
            "//third_party/mesa/bazel:windows": MESA_C_ARGS_WINDOWS if lang == "c" else MESA_CPP_ARGS_WINDOWS,
            "//conditions:default": MESA_C_ARGS if lang == "c" else MESA_CPP_ARGS,
        })
        if lang == "c":
            args = args + _CROSS_C
    if type(extra) == "dict":
        extra_sel = {
            "//third_party/mesa/bazel:ios": [_shell(f) for f in extra["ios"]],
            "//conditions:default": [_shell(f) for f in extra["macos"]],
        }
        if "linux" in extra:
            extra_sel["//third_party/mesa/bazel:linux"] = [_shell(f) for f in extra["linux"]]
        if "windows" in extra:
            extra_sel["//third_party/mesa/bazel:windows"] = [_shell(f) for f in extra["windows"]]
        extra = select(extra_sel)
    else:
        extra = select({
            "//third_party/mesa/bazel:windows": _win_extra(extra),
            "//third_party/mesa/bazel:linux": _linux_extra(extra),
            "//conditions:default": [_shell(f) for f in extra],
        })
    prefix_maps = select({
        "//third_party/mesa/bazel:windows": _PREFIX_MAPS_WINDOWS,
        "//conditions:default": _PREFIX_MAPS,
    })
    return args + extra + prefix_maps

def mesa_cc_library(name, sources, c_args = [], cpp_args = [], deps = [], tags = [], **kwargs):
    """One of meson's static_library targets: its objects, one action each, and its archive.

    Every Mesa header is an input, pruned per object by its .d file: a header edit recompiles only the objects that
    include it.

    Args:
        name: meson's library name (the archive is lib<name>.a).
        sources: its sources in meson's archive member order, which is link order (`sources`, not `srcs`, which
            buildifier would sort).
        c_args: mesa_args("c", ...).
        cpp_args: mesa_args("cxx", ...).
        deps: headers of its own package that //third_party/mesa:headers leaves out.
        tags: extra Bazel tags.
        **kwargs: other cc_library attributes.
    """
    cc_library(
        name = name,
        srcs = sources,
        conlyopts = c_args,
        cxxopts = cpp_args,
        deps = ["//third_party/mesa:headers"] + deps,
        linkstatic = True,
        # Mesa's own code: never ios-on-switch-lmao's to lint (clang-tidy skips no-lint targets).
        tags = ["no-lint", "manual"] + tags,
        target_compatible_with = MESA_ONLY,
        **kwargs
    )

def mesa_genrule(tags = [], **kwargs):
    """A generator of one of Mesa's meson.build files (genrule).

    Args:
        tags: extra Bazel tags.
        **kwargs: the genrule's attributes.
    """
    native.genrule(tags = ["manual"] + tags, target_compatible_with = MESA_ONLY, **kwargs)

def _mesa_file_impl(ctx):
    out = ctx.outputs.out
    ctx.actions.run(
        executable = "/bin/cp",
        arguments = [ctx.file.src.path, out.path],
        inputs = [ctx.file.src],
        outputs = [out],
        mnemonic = "MesaFile",
        progress_message = "Copying %{input} to %{output}",
    )
    return [DefaultInfo(files = depset([out]))]

_mesa_file = rule(
    implementation = _mesa_file_impl,
    doc = "`src` copied to `out` in this package's output directory (cacheable, unlike skylib's copy_file).",
    attrs = {
        "out": attr.output(mandatory = True),
        "src": attr.label(mandatory = True, allow_single_file = True),
    },
)

def mesa_file(tags = [], **kwargs):
    """A file put where meson's build directory has it (see _mesa_file).

    Args:
        tags: extra Bazel tags.
        **kwargs: name, src, out.
    """
    _mesa_file(tags = ["manual"] + tags, target_compatible_with = MESA_ONLY, **kwargs)

# --- The configuration NVK builds in -----------------------------------------------------------------------------

_PLATFORMS = "//command_line_option:platforms"
_RUST = "//bazel/rust:toolchain_version"

MESA_PLATFORMS = {
    "ios": "//bazel/platforms:ios_arm64",
    "macos": "//bazel/platforms:macos_user_arm64",
}

def _mesa_platform_impl(_settings, attr):
    return {_PLATFORMS: [str(Label(MESA_PLATFORMS[attr.platform]))], _RUST: "1.96.1"}

mesa_platform = transition(implementation = _mesa_platform_impl, inputs = [], outputs = [_PLATFORMS, _RUST])

def _mesa_files_impl(ctx):
    files = depset(transitive = [t[DefaultInfo].files for t in ctx.attr.srcs])
    return [DefaultInfo(files = files, runfiles = ctx.runfiles(transitive_files = files))]

mesa_files = rule(
    implementation = _mesa_files_impl,
    doc = "`srcs` built for `platform` (ios or macos) in NVK's configuration: its files.",
    attrs = {
        "platform": attr.string(mandatory = True, values = MESA_PLATFORMS.keys()),
        "srcs": attr.label_list(cfg = mesa_platform, allow_files = True),
    },
)

def _mesa_make_variables_impl(ctx):
    return [platform_common.TemplateVariableInfo(ctx.attr.variables)]

mesa_make_variables = rule(
    implementation = _mesa_make_variables_impl,
    doc = "Make variables for genrules (toolchains = [...]), chosen by the configuration's platform.",
    attrs = {"variables": attr.string_dict()},
)

# --- Host tools ----------------------------------------------------------------------------------------------------

def _cargo_tool_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name)
    rustc = ctx.file._rustc
    cargo = ctx.file._cargo
    src = ctx.file.manifest.dirname
    vendor = src.rpartition("/")[0] + "/vendor"
    ctx.actions.run_shell(
        command = """\
set -e
here=$PWD
export CARGO_HOME=$here/{out}.cargo-home RUSTC=$here/{rustc} PATH=/usr/bin:/bin
export DEVELOPER_DIR=$(sed -n 's/^DEVELOPER_DIR=//p' {xcode_env})
cd {src}
$here/{cargo} build --release --offline --locked --bin {bin} --target-dir $here/{out}.target \\
  --config 'source.crates-io.replace-with="vendored"' --config "source.vendored.directory=\\"$here/{vendor}\\""
cp $here/{out}.target/release/{bin} $here/{out}
rm -rf $here/{out}.target $here/{out}.cargo-home
""".format(
            out = out.path,
            rustc = rustc.path,
            cargo = cargo.path,
            src = src,
            vendor = vendor,
            bin = ctx.attr.bin,
            xcode_env = ctx.file._xcode_env.path,
        ),
        inputs = ctx.files.srcs + ctx.files._rust + [ctx.file._xcode_env, ctx.file._xcode_identity],
        outputs = [out],
        mnemonic = "CargoTool",
        progress_message = "Building {} with cargo (offline, its own Cargo.lock)".format(ctx.attr.bin),
        execution_requirements = XCODE_EXECUTION,
    )
    return [DefaultInfo(files = depset([out]), executable = out)]

cargo_tool = rule(
    implementation = _cargo_tool_impl,
    doc = "A crates.io binary built by Rust 1.96.1's cargo from a vendored source repository (repos.bzl).",
    executable = True,
    attrs = {
        "bin": attr.string(mandatory = True),
        "manifest": attr.label(mandatory = True, allow_single_file = True, doc = "its src/Cargo.toml"),
        "srcs": attr.label(mandatory = True, doc = "the repository's :all"),
        "_cargo": attr.label(default = "@nvk_rust//:bin/cargo", allow_single_file = True),
        "_rust": attr.label(default = "@nvk_rust//:all"),
        "_rustc": attr.label(default = "@nvk_rust//:bin/rustc", allow_single_file = True),
        "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
)
