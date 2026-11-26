"""gpu_assets(): IPSW-free format-2 GPU stage tar from //gpu:base and //kernel:gpu_kext_digest."""

def _gpu_assets_impl(ctx):
    dirs = [f for f in ctx.files.base if f.is_directory]
    if len(dirs) != 1:
        fail("{} must produce one directory".format(ctx.attr.base.label))
    base = dirs[0]
    out = ctx.actions.declare_file(ctx.attr.name + ".tar")
    args = ctx.actions.args()
    args.add("--base", base.path)
    args.add("--kext-digest", ctx.file.kext_digest)
    args.add("--out", out)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = [base, ctx.file.kext_digest],
        outputs = [out],
        mnemonic = "SwitchGpuAsset",
        progress_message = "Packing %{label} format-2 GPU release asset",
    )
    return [DefaultInfo(files = depset([out]))]

gpu_assets = rule(
    implementation = _gpu_assets_impl,
    doc = "Deterministic format-2 GPU stage tar (gpu.stage with # kext + ngpu/ bundle files), free of IPSW bytes.",
    attrs = {
        "base": attr.label(mandatory = True, doc = "//gpu:base"),
        "kext_digest": attr.label(
            default = "//kernel:gpu_kext_digest",
            allow_single_file = True,
            doc = "GPU kext build identity for the manifest's # kext header",
        ),
        "_tool": attr.label(
            default = "//install/image_gpu:pack_gpu_asset",
            executable = True,
            cfg = "exec",
        ),
    },
)
