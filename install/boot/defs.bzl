"""switch_boot's tests' inputs: each pinned kind's record, and the selected kind's boot assets, IPSW members and
//boot:package, as JSON of paths from the test's working directory."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:ipsw.bzl", "ios_version", "os_volume_uuid", "pinned_kinds")
load("//ipsw:pins.bzl", "IPSWS")

# Which prelink each XNU's kernel takes Apple's kexts by (kernel/BUILD.bazel //kernel:prelinked).
_PRELINK = {
    "10063.141.1": "native",
    "12377.121.6": "fileset",
}

_IM4P_MEMBERS = ("device_tree", "static_trust_cache", "system_cryptex_trust_cache", "app_cryptex_trust_cache")

def boot_record(kind):
    """The kind's record as switch_boot reads it (install/boot/record.rs), all of it from ipsw/pins.bzl.

    Args:
        kind: a pinned IPSW kind.

    Returns:
        The record, a dict.
    """
    pins = IPSWS[kind]
    derived = pins["derived"]
    catalog = None
    if derived.get("launchd_catalog_sha256"):
        catalog = {
            "sha256": derived["launchd_catalog_sha256"],
            "signature_sha256": derived["launchd_catalog_signature_sha256"],
        }
    return {
        "build": pins["build"],
        "kernel_virt_base": derived["kernel_virt_base"],
        "os_volume_uuid": os_volume_uuid(kind),
        # boot/defs.bzl's rule for the manifest hash slot.
        "boot_manifest_hash": ios_version(kind) >= 27,
        "prelink": _PRELINK[pins["xnu"]],
        "kernelcache_sha256": derived["kernelcache_sha256"],
        "manifest_digests": {member: pins["members"][member]["manifest_digest"] for member in _IM4P_MEMBERS},
        "launchd_catalog": catalog,
    }

def _boot_case_impl(ctx):
    kind = ctx.attr._kind[BuildSettingInfo].value
    case = {
        "kind": kind,
        "records": {k: boot_record(k) for k in pinned_kinds()},
        "assets": ctx.file.assets.short_path,
        "package": ctx.file.package.short_path,
        "members": {name: target.files.to_list()[0].short_path for name, target in ctx.attr.members.items()},
    }
    inputs = [ctx.file.assets, ctx.file.package]
    for name, target in ctx.attr.members.items():
        files = target.files.to_list()
        if len(files) != 1:
            fail("member {} ({}) must be one file".format(name, target.label))
        inputs += files
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(out, json.encode_indent(case, indent = "  ") + "\n")
    return [DefaultInfo(files = depset([out]), runfiles = ctx.runfiles(files = [out] + inputs))]

boot_case = rule(
    implementation = _boot_case_impl,
    doc = "The selected kind's boot assets, IPSW members and //boot:package, with every pinned kind's record.",
    attrs = {
        "assets": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:boot_assets"),
        "package": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:package, the oracle"),
        "members": attr.string_keyed_label_dict(allow_files = True, doc = "switch_boot::Members field -> file"),
        "_kind": attr.label(default = "//ipsw:kind"),
    },
)
