"""The `arm64e` CLI as //install:delta_<role> ran it, and a role's byte-equality test against that delta."""

load("@rules_shell//shell:sh_test.bzl", "sh_test")

# The tests that read 24B119's members and its pretranslate pins.
IOS27_ONLY = select({
    "//ipsw:ios27": [],
    "//conditions:default": ["@platforms//:incompatible"],
})

def _exec_file_impl(ctx):
    tool = ctx.executable.tool
    return [DefaultInfo(files = depset([tool]), runfiles = ctx.runfiles(files = [tool]))]

exec_file = rule(
    implementation = _exec_file_impl,
    doc = "An executable built for the exec platform, as a file: what translate_tree hashed as the translator version.",
    attrs = {"tool": attr.label(mandatory = True, executable = True, cfg = "exec")},
)

def delta_test(name, role, image, trustcaches, edits = None, pairs = []):
    """switch_translate_tool's delta for `role` against //install:delta_<role>, file for file.

    It reads the IPSW's large members, which only a build host has, so it is manual.
    """
    args = [
        "$(rootpath :switch_translate_tool)",
        "$(rootpath //install:delta_%s)" % role,
        "--role",
        role,
        "--image",
        "$(rootpath %s)" % image,
        "--ipsw-pins",
        "$(rootpath //install:ipsw_pins)",
        "--pins",
        "$(rootpath //ipsw:pretranslated_pins)",
        "--translator-of",
        "$(rootpath :arm64e_exec)",
    ]
    for t in trustcaches:
        args += ["--apple-trustcache", "$(rootpath %s)" % t]
    if edits:
        args += ["--edits", "$(rootpath %s)" % edits]
    for p in pairs:
        args += ["--cdhash-pairs", "$(rootpath //install:delta_%s)/%s.cdhash-pairs" % (p, p)]
    sh_test(
        name = name,
        size = "enormous",
        srcs = ["delta_test.sh"],
        args = args,
        data = [
            image,
            ":arm64e_exec",
            ":switch_translate_tool",
            "//install:delta_%s" % role,
            "//install:ipsw_pins",
            "//ipsw:pretranslated_pins",
        ] + trustcaches + ([edits] if edits else []) + ["//install:delta_%s" % p for p in pairs],
        tags = [
            "external",
            "manual",
        ],
        target_compatible_with = IOS27_ONLY,
    )
