"""switch_adt's tests' inputs: //boot:package's ADT finish step for the selected IPSW kind, and the base's provenance."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:defs.bzl", "PretranslatedPinsInfo")
load("//ipsw:ipsw.bzl", "ios_version", "os_volume_uuid", "pinned_kinds")
load("//release:ipsw_free.bzl", "ipsw_free")

def _named(files, basename):
    return [f for f in files if f.basename == basename]

def _finish_case_impl(ctx):
    kind = ctx.attr._kind[BuildSettingInfo].value
    pretranslated = ctx.attr.pretranslated[PretranslatedPinsInfo]

    # boot/defs.bzl's finish arguments: each role's trust modules, the manifest hash slot from iOS 27 on, the launchd
    # catalogue's digests (iOS 17) and the pretranslated cache list (iOS 27).
    roles = {
        "os_cryptex": [ctx.file.system_trust] + _named(pretranslated.trust, "system_cryptex.trustcache"),
        "app_cryptex": [ctx.file.app_trust] + _named(pretranslated.trust, "app_cryptex.trustcache"),
        "pretranslated": _named(pretranslated.trust, "os.trustcache"),
    }
    optional = {
        "launchd_catalog_sha256": _named(ctx.files.launchd_catalog, "launchd.sha256"),
        "a64x_cache_pretranslated": [pretranslated.cache_list] if pretranslated.cache_list else [],
        "pretranslated_updates": [pretranslated.updates] if pretranslated.updates else [],
    }
    case = {
        "kind": kind,
        "base": ctx.file.base.short_path,
        "apple": ctx.file.apple.short_path,
        "adt": ctx.file.adt.short_path,
        "root_volume_uuid": os_volume_uuid(kind),
        "boot_manifest_hash": ios_version(kind) >= 27,
        "pinned_volume_uuids": [os_volume_uuid(k) for k in pinned_kinds()],
    }
    for role, files in roles.items():
        case[role] = [f.short_path for f in files]
    for key, files in optional.items():
        case[key] = files[0].short_path if files else None
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(out, json.encode_indent(case, indent = "  ") + "\n")
    inputs = [ctx.file.base, ctx.file.apple, ctx.file.adt]
    for files in list(roles.values()) + list(optional.values()):
        inputs += files
    return [DefaultInfo(files = depset([out]), runfiles = ctx.runfiles(files = [out] + inputs))]

adt_finish_case = rule(
    implementation = _finish_case_impl,
    doc = "The selected IPSW kind's finish inputs and the ADT //boot:package made of them, as JSON of runfiles paths.",
    attrs = {
        "base": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:adt_base"),
        "adt": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:package's adt output group"),
        "apple": attr.label(mandatory = True, allow_single_file = True, doc = "Apple's device tree (//ipsw:device_tree)"),
        "system_trust": attr.label(mandatory = True, allow_single_file = True),
        "app_trust": attr.label(mandatory = True, allow_single_file = True),
        "launchd_catalog": attr.label(mandatory = True, allow_files = True, doc = "//boot:package's launchd_catalog output group"),
        "pretranslated": attr.label(mandatory = True, providers = [PretranslatedPinsInfo]),
        "_kind": attr.label(default = "//ipsw:kind"),
    },
)

def _base_passes_impl(ctx):
    env = analysistest.begin(ctx)
    files = analysistest.target_under_test(env)[DefaultInfo].files.to_list()
    asserts.equals(env, ["ios-base.adt"], [f.basename for f in files])
    return analysistest.end(env)

_base_passes_test = analysistest.make(_base_passes_impl)

def _adt_refused_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, "would ship Apple's bytes")
    asserts.expect_failure(env, "package_work/ios.adt")
    return analysistest.end(env)

_adt_refused_test = analysistest.make(_adt_refused_impl, expect_failure = True)

def adt_provenance_tests(name, base, adt):
    """ipsw_free passes the base and refuses the finished ADT, which read Apple's device tree.

    Args:
        name: the test_suite's name.
        base: //boot:adt_base.
        adt: //boot:package's adt output group.
    """
    ipsw_free(
        name = name + "_base_checked",
        src = base,
        tags = ["manual"],
    )
    ipsw_free(
        name = name + "_adt_checked",
        src = adt,
        tags = ["manual"],
    )
    _base_passes_test(
        name = name + "_passes_the_base",
        target_under_test = ":" + name + "_base_checked",
    )
    _adt_refused_test(
        name = name + "_refuses_the_adt",
        target_under_test = ":" + name + "_adt_checked",
    )
    native.test_suite(
        name = name,
        tests = [name + "_passes_the_base", name + "_refuses_the_adt"],
    )
