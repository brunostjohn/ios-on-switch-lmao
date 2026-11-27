"""kernel_input(): one file of a multi-file target by basename, for rules that take a single file.
fileset_pins(): a fileset prelink's pins, as the release publishes them (switch_prelink_tool pins).
"""

load("//ipsw:ipsw.bzl", "IpswInfo")

def _kernel_input_impl(ctx):
    found = [f for f in ctx.files.inputs if f.basename == ctx.attr.file]
    if len(found) != 1:
        fail("{} holds {} files named {}".format(ctx.attr.inputs.label, len(found), ctx.attr.file))
    return [DefaultInfo(files = depset(found))]

kernel_input = rule(
    implementation = _kernel_input_impl,
    attrs = {
        "file": attr.string(mandatory = True),
        "inputs": attr.label(mandatory = True, allow_files = True),
    },
)

def _fileset_pins_impl(ctx):
    out = ctx.actions.declare_file(ctx.attr.out)
    args = ctx.actions.args()
    args.add("pins")
    args.add(ctx.attr._ipsw_kind[IpswInfo].kind)
    args.add(ctx.file.prelinked)
    args.add(out)
    args.add_all(ctx.files.sites)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [ctx.file.prelinked] + ctx.files.sites,
        outputs = [out],
        mnemonic = "FilesetPrelinkPins",
        progress_message = "Pinning %{label}'s prelinked kernel and site tables",
    )
    return [DefaultInfo(files = depset([out]))]

fileset_pins = rule(
    implementation = _fileset_pins_impl,
    attrs = {
        "out": attr.string(mandatory = True),
        "prelinked": attr.label(mandatory = True, allow_single_file = True, doc = "the fileset prelink's kernel"),
        "sites": attr.label(default = "//kits/SwitchABI:sites", allow_files = [".json"], doc = "the kind's site tables"),
        "_ipsw_kind": attr.label(default = "//ipsw:current", providers = [IpswInfo]),
        "_tool": attr.label(default = "//install/prelink:switch_prelink_tool", executable = True, cfg = "exec"),
    },
)
