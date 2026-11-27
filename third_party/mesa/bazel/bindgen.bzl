"""mesa_bindgen: bindgen genrule for NVK's Darwin :user builds (verbatim commands preserved) and the six desktop targets."""

load(":defs.bzl", "mesa_genrule")
load(":flags.bzl", "MESA_PRE_ARGS", "MESA_PRE_ARGS_LINUX", "MESA_PRE_ARGS_WINDOWS")

def _shell_flag(flag):
    if '"' in flag or "'" in flag or " " in flag:
        return "'" + flag.replace("'", "'\\''") + "'"
    return flag

def _join_flags(flags):
    return " ".join([_shell_flag(f) for f in flags])

_DARWIN_DEFINES = _join_flags(MESA_PRE_ARGS)
_LINUX_DEFINES = _join_flags(MESA_PRE_ARGS_LINUX)
_WINDOWS_DEFINES = _join_flags(MESA_PRE_ARGS_WINDOWS)

_BINDGEN_TOOLS = [
    "//third_party/mesa/bazel:bindgen",
    "//third_party/mesa/bazel:libclang",
    "//third_party/mesa/bazel:rustfmt",
    "@nvk_libclang//:lib/libclang.dylib",
    "@nvk_rust//:bin/rustfmt",
]

_WINDOWS_TOOLS = select({
    "//third_party/mesa/bazel:windows": ["//third_party/mesa/bazel:fix_windows_bindgen"],
    "//conditions:default": [],
})

_SYSROOT_SRCS = select({
    "//third_party/mesa/bazel:linux_x86_64": [
        "@desktop_musl//:x86_64",
        "@desktop_musl//:x86_64/SYSROOT",
    ],
    "//third_party/mesa/bazel:linux_aarch64": [
        "@desktop_musl//:aarch64",
        "@desktop_musl//:aarch64/SYSROOT",
    ],
    "//third_party/mesa/bazel:windows": [
        "@desktop_msvc//:headers",
        "@desktop_msvc//:SYSROOT",
    ],
    "//conditions:default": [],
})

def _desktop_cmd(header, out_rs, bindgen_args, clang_extra, defines, sysroot_setup, extra_clang_args):
    return (
        'LIBCLANG_DIR="$$PWD/$$(dirname $(execpath @nvk_libclang//:lib/libclang.dylib))"; ' +
        'RESOURCE_DIR="$$LIBCLANG_DIR/clang/23"; ' +
        sysroot_setup +
        'LIBCLANG_PATH="$$LIBCLANG_DIR" ' +
        'PATH="$$PWD/$$(dirname $(execpath @nvk_rust//:bin/rustfmt)):$$PATH" ' +
        'BINDGEN_EXTRA_CLANG_ARGS="' + extra_clang_args + '" ' +
        "$(execpath //third_party/mesa/bazel:bindgen) $(execpath " + header + ") " +
        "--output $(execpath " + out_rs + ") " +
        bindgen_args + " -- " +
        "-DNDEBUG " + defines + " " + clang_extra + " -std=c11"
    )

def mesa_bindgen(name, header, outs, out_rs, ios_cmd, macos_cmd, bindgen_args, clang_extra = ""):
    """Declares one Mesa bindgen genrule, keeping :ios and :host commands verbatim while supporting Linux/Windows.

    Args:
        name: target name.
        header: input C header path (package-relative).
        outs: genrule outputs list.
        out_rs: generated Rust output path (element of outs).
        ios_cmd: verbatim command string for //third_party/mesa/bazel:ios.
        macos_cmd: verbatim command string for Darwin (:host, macOS arm64/x86_64).
        bindgen_args: bindgen flags between `--output ...` and `--` (used for Linux and Windows).
        clang_extra: extra clang include/define flags after `--` (used for Linux and Windows).
    """
    macos_x86_cmd = _desktop_cmd(
        header = header,
        out_rs = out_rs,
        bindgen_args = bindgen_args,
        clang_extra = clang_extra,
        defines = _DARWIN_DEFINES,
        sysroot_setup = "",
        extra_clang_args = "$(BINDGEN_CLANG_ARGS) -resource-dir=$$RESOURCE_DIR",
    )
    linux_x86_cmd = _desktop_cmd(
        header = header,
        out_rs = out_rs,
        bindgen_args = bindgen_args,
        clang_extra = clang_extra,
        defines = _LINUX_DEFINES,
        sysroot_setup = 'SYSROOT="$$PWD/$$(dirname $(execpath @desktop_musl//:x86_64/SYSROOT))"; ',
        extra_clang_args = "$(BINDGEN_CLANG_ARGS) --sysroot=$$SYSROOT -resource-dir=$$RESOURCE_DIR",
    )
    linux_arm_cmd = _desktop_cmd(
        header = header,
        out_rs = out_rs,
        bindgen_args = bindgen_args,
        clang_extra = clang_extra,
        defines = _LINUX_DEFINES,
        sysroot_setup = 'SYSROOT="$$PWD/$$(dirname $(execpath @desktop_musl//:aarch64/SYSROOT))"; ',
        extra_clang_args = "$(BINDGEN_CLANG_ARGS) --sysroot=$$SYSROOT -resource-dir=$$RESOURCE_DIR",
    )
    windows_cmd = _desktop_cmd(
        header = header,
        out_rs = out_rs,
        bindgen_args = bindgen_args,
        clang_extra = clang_extra,
        defines = _WINDOWS_DEFINES,
        sysroot_setup = 'SYSROOT="$$PWD/$$(dirname $(execpath @desktop_msvc//:SYSROOT))"; ',
        extra_clang_args = (
            "$(BINDGEN_CLANG_ARGS) -resource-dir=$$RESOURCE_DIR " +
            "-isystem $$SYSROOT/crt/include " +
            "-isystem $$SYSROOT/sdk/include/ucrt " +
            "-isystem $$SYSROOT/sdk/include/um " +
            "-isystem $$SYSROOT/sdk/include/shared"
        ),
    ) + " && $(execpath //third_party/mesa/bazel:fix_windows_bindgen) $(execpath " + out_rs + ")"
    mesa_genrule(
        name = name,
        srcs = [
            header,
            "//third_party/mesa:header_files",
        ] + _SYSROOT_SRCS,
        outs = outs,
        cmd = select({
            "//third_party/mesa/bazel:ios": ios_cmd,
            "//third_party/mesa/bazel:macos_x86_64": macos_x86_cmd,
            "//third_party/mesa/bazel:linux_x86_64": linux_x86_cmd,
            "//third_party/mesa/bazel:linux_aarch64": linux_arm_cmd,
            "//third_party/mesa/bazel:windows": windows_cmd,
            "//conditions:default": macos_cmd,
        }),
        toolchains = ["//third_party/mesa/bazel:bindgen_vars"],
        tools = _BINDGEN_TOOLS + _WINDOWS_TOOLS,
    )
