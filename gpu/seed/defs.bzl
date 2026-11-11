"""The shader cache seed: what the host tools (tools.bzl) make from the IPSW's Metal libraries on the Mac that composes
the image.

  shader_crawl   the IPSW kind's crawled libraries (crawl.bzl crawls them once, in a repository) and their names
  metal_seed     resolve the corpus, plan the buckets, one precompile action per bucket, join their indexes
  seed_raw_pack  the raw GM20B path's pipeline pack, from Core Animation's corpus lines
  seed_tree      the seed and the raw pack laid out as the Data volume holds them (the image builder's)

Every output is named below the IPSW kind (21H461/, 24B119/): a build of the other kind writes its own files, so
switching --config=ios27 on and off reuses both instead of overwriting one with the other.
"""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:ipsw.bzl", "IpswInfo")
load(":crawl.bzl", "QUARTZCORE")
load(":native.bzl", "DARWIN", "bucket_resources", "named", "native_pack", "native_records", "three")
load(":tools.bzl", "seed_tool_files")

_IPSW = {"ipsw": attr.label(default = "//ipsw:current", providers = [IpswInfo])}

def _repo_root(files):
    return files[0].owner.workspace_root

CrawlInfo = provider(
    doc = "A crawl of the IPSW's images: the library tree (root/, data/) and the QuartzCore library in it.",
    fields = {
        "files": "depset: every file of the tree",
        "root": "the tree's directory, which holds root/ and data/",
        "qc": "QuartzCore's default.metallib",
    },
)

SeedInfo = provider(
    doc = "The packed seed: the buckets' packs (packs/<NNN> in order) and the join's directory.",
    fields = {
        "packs": "one pack per bucket",
        "meta": "SEED, MANIFEST.json, the index and the merged driver records' pack",
        "native": "the native records and analyses as the device reads them, packed (native.bzl)",
    },
)

def _shader_crawl_impl(ctx):
    qc = ctx.file.qc
    if not qc.path.endswith("/" + QUARTZCORE):
        fail("qc must be the tree's " + QUARTZCORE)
    root = qc.path.removesuffix("/" + QUARTZCORE)
    files = depset(ctx.files.tree)
    sources = []
    if ctx.files.applications or ctx.files.embedded:
        tree = ctx.actions.declare_directory(named(ctx, "_inputs"))
        report = ctx.actions.declare_file(named(ctx, "_inputs.json"))
        args = ctx.actions.args()
        args.add("--crawl", root)
        args.add_all(ctx.files.applications, before_each = "--application")
        args.add_all(ctx.files.embedded, before_each = "--embedded")
        args.add("--ios-major", ctx.attr.ipsw[IpswInfo].version.split(".")[0])
        args.add("--ipsw", ctx.attr.ipsw[IpswInfo].build)
        args.add("--out", tree.path)
        args.add("--report", report)
        ctx.actions.run(
            executable = ctx.executable._inputs,
            arguments = [args],
            inputs = depset(ctx.files.applications + ctx.files.embedded, transitive = [files]),
            outputs = [tree, report],
            mnemonic = "MetalSeedInputs",
            progress_message = "Adding this OS build's application and embedded shaders",
        )
        root = tree.path
        files = depset([tree, qc])
        sources = [report]
    names = ctx.actions.declare_file(named(ctx, ".names.json"))
    ctx.actions.run(
        executable = ctx.executable._libraries,
        arguments = ["names", root + "/root", names.path],
        inputs = files,
        outputs = [names],
        mnemonic = "M2vLibraryNames",
        progress_message = "Indexing the crawled libraries' function names",
    )
    return [
        DefaultInfo(files = depset([ctx.file.inventory] + sources, transitive = [files])),
        CrawlInfo(files = files, root = root, qc = qc),
        OutputGroupInfo(names = depset([names]), inventory = depset([ctx.file.inventory]), sources = depset(sources)),
    ]

shader_crawl = rule(
    implementation = _shader_crawl_impl,
    doc = "The IPSW kind's crawled Metal libraries (@ipsw//:crawl_*, crawl.bzl), and the names of their functions.",
    attrs = _IPSW | {
        "tree": attr.label(mandatory = True, allow_files = True, doc = "every file below the tree (root/, data/)"),
        "qc": attr.label(mandatory = True, allow_single_file = True, doc = "the tree's QuartzCore default.metallib"),
        "inventory": attr.label(mandatory = True, allow_single_file = [".json"], doc = "the crawl's inventory.json"),
        "applications": attr.label_list(allow_files = [".ipa"]),
        "embedded": attr.label_list(allow_files = [".h"]),
        "_inputs": attr.label(default = "//tools/gpu/shaders:inputs", executable = True, cfg = "exec"),
        "_libraries": attr.label(default = "//drivers/TegraGM20BMetal:libraries", executable = True, cfg = "exec"),
    },
)

def _plan(ctx, crawl, corpus):
    count = ctx.attr.buckets
    input_reports = ctx.attr.crawl[OutputGroupInfo].sources.to_list()
    buckets = [ctx.actions.declare_file(named(ctx, "_plan/bucket-{}.log".format(three(i)))) for i in range(count)]
    store = ctx.actions.declare_file(named(ctx, "_plan/store.json"))
    report = ctx.actions.declare_file(named(ctx, "_plan/plan.json"))
    args = ctx.actions.args()
    args.add("--corpus", corpus)
    args.add("--root", crawl.root + "/root")
    args.add("--data-dir", crawl.root + "/data")
    args.add_all(ctx.files.device_libraries, before_each = "--data")
    args.add("--qc", crawl.qc)
    args.add("--qc-sha256", ctx.attr.ipsw[IpswInfo].derived["quartzcore_metallib_sha256"])
    args.add("--depth", ctx.attr.depth[BuildSettingInfo].value)
    args.add("--buckets", buckets[0].dirname)
    args.add("--count", count)
    args.add("--store", store)
    args.add("--report", report)
    args.add_all(input_reports, before_each = "--input-report")
    ctx.actions.run(
        executable = ctx.executable._plan,
        arguments = [args],
        inputs = depset([corpus] + ctx.files.device_libraries + input_reports, transitive = [crawl.files]),
        outputs = buckets + [store, report],
        mnemonic = "MetalSeedPlan",
        progress_message = "Planning the shader seed's buckets",
    )
    return buckets, store, report

def _precompile_args(ctx, tools, crawl, store, lines, out):
    args = ctx.actions.args()
    args.add("--precompile", tools.precompile)
    args.add("--mock", tools.mock)
    args.add("--nvk", tools.twin)
    args.add("--root", crawl.root + "/root")
    args.add("--store", store)
    args.add_all(ctx.files.embedded, before_each = "--embedded")
    args.add("--qc", crawl.qc)
    args.add("--lines", lines)
    args.add("--out", out)
    return args

def _bucket(ctx, number, tools, crawl, store, lines, inputs):
    """One bucket's compile; returns its pack, index and report."""
    out = ctx.actions.declare_file(named(ctx, "_buckets/{}.pack".format(three(number))))
    index = ctx.actions.declare_file(named(ctx, "_buckets/{}.index".format(three(number))))
    report = ctx.actions.declare_file(named(ctx, "_buckets/{}.json".format(three(number))))
    outputs = [out, index, report]
    args = _precompile_args(ctx, tools, crawl, store, lines, out)
    if ctx.attr.depth[BuildSettingInfo].value == "deep":
        args.add("--guess-dispatch")
    args.add("--index", index)
    args.add("--report", report)

    # What NAK and NVK make of a bucket's programs is a function of NVK's compiler identity, which the twin's .source
    # names (gpu/nvk: the hash NVK puts in its pipeline cache UUID): the action is keyed on that, and the twin, whose
    # bytes every NVK edit changes, is an input it reports unused, so an edit outside the compiler reruns no bucket.
    unused = None
    if tools.nvk_identity:
        unused = ctx.actions.declare_file(named(ctx, "_buckets/{}.unused".format(three(number))))
        args.add("--unused-list", unused)
        args.add("--unused", tools.twin)
        outputs.append(unused)
    ctx.actions.run(
        executable = ctx.executable._precompile,
        arguments = [args],
        inputs = inputs,
        outputs = outputs,
        unused_inputs_list = unused,
        mnemonic = "MetalSeedBucket",
        progress_message = "Precompiling shader seed bucket {} of {}".format(number + 1, ctx.attr.buckets),
        execution_requirements = DARWIN,
        resource_set = bucket_resources,
    )
    return out, index, report

def _metal_seed_impl(ctx):
    crawl = ctx.attr.crawl[CrawlInfo]
    names = ctx.attr.crawl[OutputGroupInfo].names.to_list()[0]
    inventory = ctx.attr.crawl[OutputGroupInfo].inventory.to_list()[0]
    tools = seed_tool_files(ctx.attr.tools)
    corpus = ctx.actions.declare_file(named(ctx, "_plan/corpus.log"))
    ctx.actions.run(
        executable = ctx.executable._libraries,
        arguments = ["resolve", names.path, ctx.file.corpus.path, corpus.path],
        inputs = [names, ctx.file.corpus],
        outputs = [corpus],
        mnemonic = "MetalSeedCorpus",
        progress_message = "Resolving the shader corpus's libraries",
    )
    lines, store, report = _plan(ctx, crawl, corpus)
    native_stamped, native_checked = native_records(ctx, tools, crawl, store, lines)

    # A bucket's compile reads its lines, the libraries and the three tools that make code (the precompile tool with
    # the frontend and m2v in it, its mock IOKit, the NVK twin): never the plugin, the corpus or the rest of the tools.
    tool_files = [store, tools.precompile, tools.mock, tools.twin] + ([tools.nvk_identity] if tools.nvk_identity else [])
    compile_inputs = depset(tool_files + ctx.files.device_libraries + ctx.files.embedded, transitive = [crawl.files])
    packs, indexes, reports = [], [], []
    for i, bucket_lines in enumerate(lines):
        out, index, bucket_report = _bucket(
            ctx,
            i,
            tools,
            crawl,
            store,
            bucket_lines,
            depset([bucket_lines], transitive = [compile_inputs]),
        )
        packs.append(out)
        indexes.append(index)
        reports.append(bucket_report)

    meta = ctx.actions.declare_directory(named(ctx, "_meta"))
    args = ctx.actions.args()
    args.add("join")
    args.add_all(indexes, before_each = "--index")
    args.add_all(reports, before_each = "--report")
    if ctx.files.records:
        args.add("--records", _repo_root(ctx.files.records) + "/records")
    if ctx.files.replies:
        args.add("--replies", _repo_root(ctx.files.replies) + "/msl")
        args.add_all(ctx.files.replies, before_each = "--reply-input")
    if ctx.attr._profile[BuildSettingInfo].value == "release":
        args.add("--strict-replies")
    args.add("--ipsw", ctx.attr.ipsw[IpswInfo].build)
    args.add("--plan", report)
    args.add("--inventory", inventory)
    args.add("--out", meta.path)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = depset(indexes + reports + [report, inventory] + ctx.files.records + ctx.files.replies),
        outputs = [meta],
        mnemonic = "MetalSeedJoin",
        progress_message = "Joining the shader seed's bucket indexes",
    )
    native_packed = native_pack(ctx, native_stamped, meta)

    # The tools must be the GPU bundle's, or no key of the seed would ever be hit: checked beside the build, which
    # neither waits for it nor reruns anything when only the bundle changes.
    checked = ctx.actions.declare_file(named(ctx, ".tools-match"))
    args = ctx.actions.args()
    args.add("check")
    args.add("--tools-manifest", tools.manifest)
    args.add("--twin", tools.twin)
    args.add_all(ctx.files.base, before_each = "--base", expand_directories = False)
    args.add("--out", checked)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = [tools.manifest, tools.twin] + ctx.files.base,
        outputs = [checked],
        mnemonic = "MetalSeedCheck",
        progress_message = "Checking the shader tools are the GPU bundle's",
    )

    verify = ctx.actions.declare_file(named(ctx, "_verify/misses.pack"))
    verify_index = ctx.actions.declare_file(named(ctx, "_verify/misses.index"))
    verify_report = ctx.actions.declare_file(named(ctx, "_verify/report.json"))
    args = _precompile_args(ctx, tools, crawl, store, corpus, verify)
    args.add("--index", verify_index)
    args.add("--report", verify_report)
    args.add("--seed", meta.path)
    args.add_all(packs, before_each = "--seed-pack")
    ctx.actions.run(
        executable = ctx.executable._precompile,
        arguments = [args],
        inputs = depset([corpus, meta] + packs, transitive = [compile_inputs]),
        outputs = [verify, verify_index, verify_report],
        mnemonic = "MetalSeedVerify",
        progress_message = "Replaying the shader corpus over the seed",
        execution_requirements = DARWIN,
    )
    return [
        DefaultInfo(files = depset([meta, native_packed] + packs)),
        SeedInfo(packs = packs, meta = meta, native = native_packed),
        OutputGroupInfo(
            plan = depset([corpus, store, report] + lines),
            buckets = depset(indexes + reports),
            verify = depset([verify_report]),
            native = depset([native_stamped, native_checked]),
            _validation = depset([checked, native_checked]),
        ),
    ]

metal_seed = rule(
    implementation = _metal_seed_impl,
    doc = "The m2v shader cache seed the device reads beneath every process's own cache.",
    attrs = _IPSW | {
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo], doc = "the IPSW's libraries (shader_crawl)"),
        "device_libraries": attr.label(allow_files = True, doc = "libraries only the device made (@gpu_device_data)"),
        "records": attr.label(allow_files = True, doc = "the device's driver records (@gpu_device_data)"),
        "replies": attr.label(allow_files = True, doc = "the device's Metal compiler replies (@gpu_device_data)"),
        "embedded": attr.label_list(allow_files = [".h"], doc = "C arrays of metallibs the frontend makes libraries of"),
        "tools": attr.label(mandatory = True, doc = "the host tools (seed_tools, or files with MANIFEST.json)"),
        "base": attr.label(mandatory = True, doc = "the GPU bundle the seed is for (//gpu:base or the release's)"),
        "depth": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "buckets": attr.int(default = 256, doc = "a corpus line costs its bucket's compile: a 256th of the seed"),
        "_libraries": attr.label(default = "//drivers/TegraGM20BMetal:libraries", executable = True, cfg = "exec"),
        "_plan": attr.label(default = "//tools/gpu/shaders:plan", executable = True, cfg = "exec"),
        "_precompile": attr.label(default = "//tools/gpu/shaders:precompile", executable = True, cfg = "exec"),
        "_native_seed": attr.label(default = "//tools/gpu/shaders:native_seed", executable = True, cfg = "exec"),
        "_seedpack": attr.label(default = "//kits/SwitchSeedPack:seedpack", executable = True, cfg = "exec"),
        "_profile": attr.label(default = "//bazel:profile", providers = [BuildSettingInfo]),
        "_merge": attr.label(default = "//tools/gpu/shaders:merge", executable = True, cfg = "exec"),
    },
)

# rawpack.py's BUCKETS: the split writes this many bucket corpora, each made by its own action.
_RAW_BUCKETS = 16

# The generator's modules each step runs (rawpack.py imports or runs them by path, beside it).
_RAW_SPLIT = ["rawpack.py", "ca/caset.py", "ca/caspec.py", "ca/caspec27.py"]
_RAW_PROBE = ["rawpack.py"]
_RAW_LINK = ["rawpack.py", "directlink.py", "mkpack.py", "kernels.py", "layouts.py", "packfile.py", "probelog.py", "splits.py"]

def _generator_files(ctx, names):
    here = ctx.file.generator.dirname + "/"
    files = [f for f in ctx.files.srcs if f.path.removeprefix(here) in names]
    if len(files) != len(names):
        fail("the raw pack generator's srcs lack some of " + ", ".join(names))
    return files

def _seed_raw_pack_impl(ctx):
    tools = seed_tool_files(ctx.attr.tools)
    crawl = ctx.attr.crawl[CrawlInfo]
    qc = [crawl.qc]
    lines = ctx.actions.declare_file(named(ctx, "_work/corpus.log"))
    corpus_report = ctx.actions.declare_file(named(ctx, "_work/corpus-report.txt"))
    kind = ctx.attr.ipsw[IpswInfo].kind
    archives = [f for f in ctx.files.archives if f.basename == kind + ".tsv"]
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = ["corpus", "--corpus", ctx.file.corpus.path, "--out", lines.path, "--report", corpus_report.path] +
                    ["--build", ctx.attr.ipsw[IpswInfo].build] + ["--archives=" + f.path for f in archives],
        inputs = [ctx.file.corpus] + archives,
        outputs = [lines, corpus_report],
        mnemonic = "MetalRawCorpus",
        progress_message = "Picking the raw pack's corpus lines",
    )

    # QuartzCore's tables (ca/qc-<build>.json) make Core Animation's states from their names.
    tables = [f for f in ctx.files.srcs if f.dirname.endswith("/ca") and f.extension == "json"]
    corpora = [ctx.actions.declare_file(named(ctx, "_work/split/bucket{}.log".format(i))) for i in range(_RAW_BUCKETS)]
    args = ctx.actions.args()
    names = [f for f in ctx.files.ca_names if f.basename == kind + ".txt"]
    args.add("split")
    args.add("--generator", ctx.file.generator)
    args.add("--qc", crawl.qc)
    args.add("--corpus", lines)
    args.add_all(names, before_each = "--ca-names")
    args.add_all("--out", corpora)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = depset([lines] + qc + tables + names + _generator_files(ctx, _RAW_SPLIT)),
        outputs = corpora,
        mnemonic = "MetalRawSplit",
        progress_message = "Splitting the raw pack's states into buckets",
    )

    probe_inputs = depset([tools.twin] + tools.raw + ctx.files.device_libraries +
                          _generator_files(ctx, _RAW_PROBE), transitive = [crawl.files])
    buckets = []
    for i, bucket_corpus in enumerate(corpora):
        # rawpack.py knows a bucket by its directory's name (bucket0 also draws the raw path's own states).
        bucket = ctx.actions.declare_directory(named(ctx, "_work/bucket{}".format(i)))
        args = ctx.actions.args()
        args.add("probe")
        args.add("--generator", ctx.file.generator)
        args.add("--raw", tools.raw_dir)
        args.add("--nvk", tools.twin)
        args.add("--qc", crawl.qc)
        args.add("--data-dir", crawl.root + "/data")
        args.add_all(ctx.files.device_libraries, before_each = "--data")
        args.add("--lines", bucket_corpus)
        args.add("--out", bucket.path)
        ctx.actions.run(
            executable = ctx.executable._tool,
            arguments = [args],
            inputs = depset([bucket_corpus], transitive = [probe_inputs]),
            outputs = [bucket],
            mnemonic = "MetalRawProbe",
            progress_message = "Making the raw pack's bucket {} of {} directly".format(i + 1, _RAW_BUCKETS),
            execution_requirements = DARWIN,
        )
        buckets.append(bucket)

    linked = ctx.actions.declare_directory(named(ctx, "_work/linked"))
    args = ctx.actions.args()
    args.add("link")
    args.add("--generator", ctx.file.generator)
    args.add("--out", linked.path)
    args.add("--corpus-report", corpus_report)
    args.add_all(buckets, expand_directories = False)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = depset([corpus_report] + buckets + _generator_files(ctx, _RAW_LINK)),
        outputs = [linked],
        mnemonic = "MetalRawLink",
        progress_message = "Linking the raw GM20B path's pipeline pack",
    )

    out = ctx.actions.declare_directory(named(ctx, ""))
    args = ctx.actions.args()
    args.add("stamp")
    args.add("--generator", ctx.file.generator)
    args.add("--pack", linked.path + "/pack.bin")
    args.add("--manifest", tools.manifest)
    args.add("--out", out.path)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [linked, tools.manifest, ctx.file.generator],
        outputs = [out],
        mnemonic = "MetalRawStamp",
        progress_message = "Stamping the raw pack with the tools' stack key",
    )
    return [
        DefaultInfo(files = depset([out])),
        # report: the link's pack and report, and each bucket's states (test/directcheck makes the pack's records from them)
        OutputGroupInfo(report = depset([linked] + corpora), buckets = depset(buckets)),
    ]

seed_raw_pack = rule(
    implementation = _seed_raw_pack_impl,
    doc = "The raw GM20B path's pipeline pack: Core Animation's corpus states made directly per bucket, linked, stamped.",
    attrs = _IPSW | {
        "tools": attr.label(mandatory = True),
        "crawl": attr.label(mandatory = True, providers = [CrawlInfo]),
        "device_libraries": attr.label(allow_files = True, doc = "device-compiled libraries used by native corpus states"),
        "corpus": attr.label(mandatory = True, allow_single_file = True),
        "archives": attr.label_list(
            allow_files = [".tsv"],
            doc = "the archive inventories (precompile/archives/<build>.tsv): the IPSW kind's, where there is one",
        ),
        "ca_names": attr.label_list(
            allow_files = [".txt"],
            doc = "Core Animation states devices named missing (precompile/ca-names/<build>.txt): the IPSW kind's",
        ),
        "generator": attr.label(mandatory = True, allow_single_file = True, doc = "raw/pack/rawpack.py"),
        "srcs": attr.label_list(allow_files = True, doc = "the generator's modules and QuartzCore tables (raw/pack)"),
        "_tool": attr.label(default = "//tools/gpu:rawpack", executable = True, cfg = "exec"),
    },
)

def _seed_tree_impl(ctx):
    seed = ctx.attr.seed[SeedInfo]
    out = ctx.actions.declare_directory(named(ctx, ""))
    size = ctx.actions.declare_file(named(ctx, ".size.json"))
    args = ctx.actions.args()
    args.add("tree")
    args.add("--seed", seed.meta.path)
    args.add("--native", seed.native.path)
    args.add_all(seed.packs, before_each = "--pack")
    if ctx.file.raw:
        args.add("--raw", ctx.file.raw.path)
    args.add("--out", out.path)
    args.add("--size", size)
    ctx.actions.run(
        executable = ctx.executable._merge,
        arguments = [args],
        inputs = [seed.meta, seed.native] + seed.packs + ctx.files.raw,
        outputs = [out, size],
        mnemonic = "MetalSeedTree",
        progress_message = "Laying out the shader seed for the Data volume",
    )
    return [DefaultInfo(files = depset([out])), OutputGroupInfo(size = depset([size]))]

seed_tree = rule(
    implementation = _seed_tree_impl,
    doc = "ngpu/addons/TegraGM20BMetal.m2v-seed/shared and TegraGM20BMetal.raw, for the Data volume's root.",
    attrs = _IPSW | {
        "seed": attr.label(mandatory = True, providers = [SeedInfo]),
        "raw": attr.label(allow_single_file = True),
        "_merge": attr.label(default = "//tools/gpu/shaders:merge", executable = True, cfg = "exec"),
    },
)
