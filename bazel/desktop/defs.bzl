"""desktop_binaries(): one binary built for each desktop platform (//bazel/platforms:desktop_*).

    desktop_binaries(name = "installer", binary = ":installer_bin")

declares :installer_<os>_<cpu> for each of the six platforms, and :installer, all six.
"""

DESKTOP_PLATFORMS = [
    os + "_" + cpu
    for os in ("linux", "windows", "macos")
    for cpu in ("x86_64", "arm64")
]

# The oldest macOS the installer runs on; the repository's own Apple targets keep their own.
MACOS_MINIMUM_OS = "13.0"

# apple_support links every binary with libc++ and Foundation; the linker drops whichever of them the binary never uses.
_MACOS_LINKOPTS = ["-Wl,-dead_strip_dylibs"]

def _desktop_transition_impl(settings, attr):
    linkopts = settings["//command_line_option:linkopt"]
    if attr.platform.name.startswith("desktop_macos_"):
        linkopts = linkopts + _MACOS_LINKOPTS
    return {
        "//command_line_option:linkopt": linkopts,
        "//command_line_option:macos_minimum_os": MACOS_MINIMUM_OS,
        "//command_line_option:platforms": str(attr.platform),
    }

_desktop_transition = transition(
    implementation = _desktop_transition_impl,
    inputs = ["//command_line_option:linkopt"],
    outputs = [
        "//command_line_option:linkopt",
        "//command_line_option:macos_minimum_os",
        "//command_line_option:platforms",
    ],
)

def _desktop_binary_impl(ctx):
    binary = ctx.attr.binary[0][DefaultInfo].files_to_run.executable
    out = ctx.actions.declare_file("{}/{}".format(ctx.label.name, binary.basename))
    ctx.actions.symlink(output = out, target_file = binary, is_executable = True)
    return [DefaultInfo(files = depset([out]), executable = out)]

desktop_binary = rule(
    implementation = _desktop_binary_impl,
    doc = "`binary`, built for `platform`.",
    attrs = {
        "binary": attr.label(mandatory = True, executable = True, cfg = _desktop_transition),
        "platform": attr.label(mandatory = True),
    },
    executable = True,
)

def desktop_binaries(name, binary, **kwargs):
    for platform in DESKTOP_PLATFORMS:
        desktop_binary(
            name = "{}_{}".format(name, platform),
            binary = binary,
            platform = "//bazel/platforms:desktop_" + platform,
            **kwargs
        )
    native.filegroup(
        name = name,
        srcs = ["{}_{}".format(name, platform) for platform in DESKTOP_PLATFORMS],
        **kwargs
    )
