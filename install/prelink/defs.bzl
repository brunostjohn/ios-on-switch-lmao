"""kernel_input(): one file of a multi-file target by basename, for rules that take a single file."""

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
