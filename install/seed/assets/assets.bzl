"""seed_assets(): IPSW-free shader seed release asset tar for the selected IPSW kind."""

load("//gpu/seed:tools.bzl", "raw_tool_files", "seed_tool_files")
load("//ipsw:ipsw.bzl", "IpswInfo")

_SPLIT_BUCKETS = 17

def _seed_assets_impl(ctx):
    ipsw = ctx.attr.ipsw[IpswInfo]
    kind = ipsw.kind
    build = ipsw.build
    qc_sha256 = ipsw.derived.get("quartzcore_metallib_sha256", "")
    if not qc_sha256:
        fail("{} has no quartzcore_metallib_sha256 in ipsw/pins.bzl".format(build))

    tools = seed_tool_files(ctx.attr.tools)
    own_metallib, context_log = raw_tool_files(tools, ["own.metallib", "context.log"])

    archives = [f for f in ctx.files.archives if f.basename == kind + ".tsv"]
    ca_names = [f for f in ctx.files.ca_names if f.basename == kind + ".txt"]
    app_states = [f for f in ctx.files.app_states if f.basename == kind + ".log"]

    corpora = [
        ctx.actions.declare_file("{}/{}/split/bucket{}.log".format(ctx.attr.name, kind, i))
        for i in range(_SPLIT_BUCKETS)
    ]
    corpus_report = ctx.actions.declare_file("{}/{}/split/corpus-report.txt".format(ctx.attr.name, kind))

    split_args = ctx.actions.args()
    split_args.add("--corpus", ctx.file.corpus)
    split_args.add("--build", build)
    split_args.add("--qc-sha256", qc_sha256)
    split_args.add("--ca-preferences", ctx.file._ca_preferences)
    split_args.add_all(archives, before_each = "--archives")
    split_args.add_all(ca_names, before_each = "--ca-names")
    split_args.add_all(app_states, before_each = "--app-states")
    split_args.add("--report", corpus_report)
    split_args.add_all("--out", corpora)
    ctx.actions.run(
        executable = ctx.executable._split_tool,
        arguments = [split_args],
        inputs = [ctx.file.corpus, ctx.file._ca_preferences] + archives + ca_names + app_states,
        outputs = corpora + [corpus_report],
        mnemonic = "SwitchSeedSplitIpswFree",
        progress_message = "Splitting %s pack corpora without IPSW bytes" % build,
    )

    app_lib_inputs = []
    app_lib_pairs = []
    for target, guest_paths_csv in ctx.attr.app_libraries.items():
        files = target[DefaultInfo].files.to_list()
        if len(files) != 1:
            fail("app_libraries target {} must produce one metallib".format(target.label))
        app_lib_inputs.append(files[0])
        for raw_guest in guest_paths_csv.split(","):
            guest_path = raw_guest.strip()
            if not guest_path:
                fail("empty guest path for {}".format(target.label))
            app_lib_pairs.append((guest_path, files[0]))
    app_lib_pairs = sorted(app_lib_pairs, key = lambda pair: pair[0])

    out = ctx.actions.declare_file(ctx.attr.name + ".tar")
    pack_args = ctx.actions.args()
    pack_args.add("--build", build)
    pack_args.add("--kind", kind)
    pack_args.add("--manifest", tools.manifest)
    pack_args.add("--qc-sha256", qc_sha256)
    pack_args.add("--own-metallib", own_metallib)
    pack_args.add("--context-log", context_log)
    pack_args.add("--draw-context", ctx.file.draw_context)
    pack_args.add("--corpus", ctx.file.corpus)
    pack_args.add_all(archives, before_each = "--archives")
    pack_args.add_all(ctx.files.embedded, before_each = "--embedded")
    for guest_path, lib_file in app_lib_pairs:
        pack_args.add("--app-library")
        pack_args.add(lib_file, format = guest_path + "=%s")
    pack_args.add("--corpus-report", corpus_report)
    pack_args.add_all("--split-bucket", corpora)
    pack_args.add("--out", out)
    ctx.actions.run(
        executable = ctx.executable._pack_tool,
        arguments = [pack_args],
        inputs = [
            tools.manifest,
            own_metallib,
            context_log,
            ctx.file.draw_context,
            ctx.file.corpus,
            corpus_report,
        ] + archives + ctx.files.embedded + app_lib_inputs + corpora,
        outputs = [out],
        mnemonic = "SwitchSeedAsset",
        progress_message = "Packing %s IPSW-free shader seed release asset" % build,
    )
    return [
        DefaultInfo(files = depset([out])),
        OutputGroupInfo(split = depset(corpora + [corpus_report])),
    ]

seed_assets = rule(
    implementation = _seed_assets_impl,
    doc = "Deterministic USTAR tar of IPSW-free shader seed inputs and pre-split pack bucket corpora.",
    attrs = {
        "ipsw": attr.label(default = "//ipsw:current", providers = [IpswInfo]),
        "tools": attr.label(mandatory = True, doc = "//gpu/seed:local_tools (manifest, own.metallib, context.log)"),
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "draw_context": attr.label(mandatory = True, allow_single_file = True),
        "archives": attr.label_list(allow_files = [".tsv"]),
        "ca_names": attr.label_list(allow_files = [".txt"]),
        "app_states": attr.label_list(allow_files = [".log"]),
        "app_libraries": attr.label_keyed_string_dict(
            allow_files = [".metallib"],
            doc = "IPSW-free app metallib targets mapped to comma-separated guest root-relative paths.",
        ),
        "embedded": attr.label_list(allow_files = [".h"]),
        "_ca_preferences": attr.label(
            default = "//tools/gpu:stage_format.py",
            allow_single_file = True,
        ),
        "_split_tool": attr.label(
            default = "//install/seed/assets:split_ipsw_free",
            executable = True,
            cfg = "exec",
        ),
        "_pack_tool": attr.label(
            default = "//install/seed/assets:pack_seed_asset",
            executable = True,
            cfg = "exec",
        ),
    },
)
