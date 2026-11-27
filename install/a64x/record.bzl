"""a64x_cli_record: every call a fileset prelink makes to the arm64e CLI, with its inputs and outputs."""

load("//kernel/rules:prelink_fileset.bzl", "PrelinkFilesetInfo")

def _a64x_cli_record_impl(ctx):
    prelink = ctx.attr.prelinked[PrelinkFilesetInfo]
    record = ctx.actions.declare_directory(ctx.label.name)
    rebuilt = ctx.actions.declare_file(ctx.label.name + ".rebuilt.macho")
    bindings = ctx.actions.declare_file(ctx.label.name + ".bindings.json")
    recorder = ctx.actions.declare_file(ctx.label.name + ".record.sh")
    ctx.actions.expand_template(template = ctx.file._recorder, output = recorder, substitutions = {}, is_executable = True)
    args = ctx.actions.args()
    args.add(ctx.executable._arm64e)
    args.add(record.path)
    args.add(ctx.executable._prelink)
    args.add("--from-bindings", prelink.bindings)
    args.add("--kernel", prelink.kernel)
    args.add("--kernelcache", prelink.kernelcache)
    args.add("--arm64e", recorder)
    args.add_all(prelink.sites, before_each = "--sites")
    args.add("--expect", prelink.prelinked)
    args.add("--rebuilt", rebuilt)
    args.add("--out", bindings)
    ctx.actions.run_shell(
        command = """set -e
export SWITCH_A64X_CLI="$PWD/$1" SWITCH_A64X_RECORD="$PWD/$2"
/bin/mkdir -p "$SWITCH_A64X_RECORD"
shift 2
"$@"
""",
        arguments = [args],
        inputs = [prelink.bindings, prelink.kernel, prelink.kernelcache, prelink.prelinked] + prelink.sites,
        tools = [ctx.attr._arm64e[DefaultInfo].files_to_run, ctx.attr._prelink[DefaultInfo].files_to_run, recorder],
        outputs = [record, rebuilt, bindings],
        mnemonic = "A64xCliRecord",
        progress_message = "Recording %{label}'s arm64e CLI calls",
    )
    return [DefaultInfo(files = depset([record]))]

a64x_cli_record = rule(
    implementation = _a64x_cli_record_impl,
    doc = "The prelink from `prelinked`'s bindings, rerun with a recorder in front of the arm64e CLI: the record " +
          "directory holds one <call>-<command>/ per call, with its inputs, output files and stdout. The prelink " +
          "still refuses unless its kernel equals `prelinked` byte for byte. It holds Apple's code: test data only.",
    attrs = {
        "prelinked": attr.label(mandatory = True, providers = [PrelinkFilesetInfo], doc = "a kernel_prelink_fileset"),
        "_arm64e": attr.label(default = "//translate/arm64e", executable = True, cfg = "exec"),
        "_prelink": attr.label(default = "//translate/arm64e:prelink", executable = True, cfg = "exec"),
        "_recorder": attr.label(default = ":record.sh", allow_single_file = True),
    },
)
