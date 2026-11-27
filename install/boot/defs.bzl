"""switch_boot's tests' inputs: the selected kind's catalog, boot assets, IPSW members and //boot:package, and every
pinned kind's records, as JSON of paths from the test's working directory."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _boot_case_impl(ctx):
    kind = ctx.attr._kind[BuildSettingInfo].value
    catalog = ctx.attr.catalog[OutputGroupInfo].catalog.to_list()[0]
    case = {
        "kind": kind,
        "catalog": catalog.short_path,
        "records": ctx.file.records.short_path,
        "assets": ctx.file.assets.short_path,
        "package": ctx.file.package.short_path,
        "members": {name: target.files.to_list()[0].short_path for name, target in ctx.attr.members.items()},
    }
    inputs = [catalog, ctx.file.records, ctx.file.assets, ctx.file.package]
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
    doc = "The selected kind's catalog, boot assets, IPSW members and //boot:package, with every pinned kind's record.",
    attrs = {
        "assets": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:boot_assets"),
        "catalog": attr.label(mandatory = True, doc = "//install/catalog:selected_catalog, of the same assets"),
        "records": attr.label(mandatory = True, allow_single_file = True, doc = "//install/catalog:records.json"),
        "package": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:package, the oracle"),
        "members": attr.string_keyed_label_dict(allow_files = True, doc = "switch_boot::Members field -> file"),
        "_kind": attr.label(default = "//ipsw:kind"),
    },
)
