"""The installer's catalog.json: one record per fully pinned IPSW kind, made from ipsw/pins.bzl's IPSWS and STAGED, with
the kind's release assets hashed by switch_catalog_tool, which refuses a catalog the installer would refuse."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//install:release.bzl", "REPO")
load("//ipsw:defs.bzl", "APPLE_TRUST")
load("//ipsw:ipsw.bzl", "os_volume_uuid", "pinned_kinds")
load("//ipsw:pins.bzl", "IPSWS")
load(":staged.bzl", "STAGED")

FORMAT = 1

# Which prelink each XNU's kernel takes Apple's kexts by: //kernel:prelinked's select.
PRELINK = {
    "10063.141.1": "native",
    "12377.121.6": "fileset",
}

# boot/defs.bzl's rule for the /chosen/boot-manifest-hash slot.
_MANIFEST_HASH_FROM_IOS = 27

_FOURCC = {
    "device_tree": "dtre",
    "kernelcache": "krnl",
} | {role: fourcc for role, (_, fourcc) in APPLE_TRUST.items()}

_DISK_IMAGES = ("os", "system_cryptex", "app_cryptex")

_COPIED = (
    "member",
    "component",
    "offset",
    "stored_size",
    "size",
    "sha256",
    "content_sha256",
    "deflated",
    "manifest_digest",
)
_STAGED_KEYS = ("aea_raw_size", "container", "container_size", "container_sha256", "payload_size", "payload_sha256")
_RECORD_KEYS = (
    "build",
    "derived",
    "device_class",
    "ios",
    "members",
    "product",
    "size",
    "url",
    "version",
    "volumes",
    "xnu",
)

_DERIVED_KEYS = ("kernel_virt_base", "kernelcache_sha256", "quartzcore_metallib_sha256")

def _missing(kind):
    pins = IPSWS[kind]
    missing = [key for key in _RECORD_KEYS if key not in pins]
    missing += ["derived." + key for key in _DERIVED_KEYS if key not in pins.get("derived", {})]
    if "xnu" in pins and pins["xnu"] not in PRELINK:
        missing.append("install/catalog/catalog.bzl PRELINK[{}]".format(pins["xnu"]))
    for role, pin in pins.get("members", {}).items():
        merged = pin | STAGED.get(kind, {}).get(role, {})
        need = list(_COPIED)
        if pin.get("aea_key"):
            need.append("aea_raw_size")
        if role in _DISK_IMAGES:
            need.append("container")
            if merged.get("container") == "udif":
                need += ["container_size", "container_sha256"]
        if role in _FOURCC:
            need += ["payload_size"] + ([] if role == "kernelcache" else ["payload_sha256"])
        missing += ["{}.{}".format(role, key) for key in need if key not in merged]
    return missing

def catalog_kinds():
    """The pinned kinds with every pin a record needs, in pins.bzl's order.

    A kind `bazel run //ipsw:pin` has just added waits for its hand-pinned values and staged pins, and until then has
    no record, so the other kinds still load and analyze.

    Returns:
        The kinds.
    """
    return [kind for kind in pinned_kinds() if not _missing(kind)]

def _merged(kind, role, pin):
    staged = STAGED.get(kind, {}).get(role, {})
    both = [key for key in staged if key in pin]
    if both:
        fail("ipsw/pins.bzl now pins {} {}'s {}: remove it from install/catalog/staged.bzl".format(kind, role, both))
    merged = pin | staged
    unknown = [key for key in merged if key not in _COPIED + _STAGED_KEYS + ("aea_key",)]
    if unknown:
        fail("{} {} has pins the catalog doesn't carry: {}".format(kind, role, unknown))
    return merged

def _member(kind, role, pin, kernelcache_sha256):
    pin = _merged(kind, role, pin)
    member = {key: pin[key] for key in _COPIED}
    if pin.get("aea_key"):
        member["aea"] = {"key": pin["aea_key"], "raw_size": pin["aea_raw_size"]}
    if role in _DISK_IMAGES:
        if pin["container"] == "raw":
            size = member["aea"]["raw_size"] if "aea" in member else pin["size"]
            member["container"] = {"format": "raw", "sha256": pin["content_sha256"], "size": size}
        else:
            member["container"] = {
                "format": pin["container"],
                "sha256": pin["container_sha256"],
                "size": pin["container_size"],
            }
    if role in _FOURCC:
        member["im4p"] = {
            "fourcc": _FOURCC[role],
            "payload_sha256": kernelcache_sha256 if role == "kernelcache" else pin["payload_sha256"],
            "payload_size": pin["payload_size"],
        }
    return member

def catalog_record(kind):
    """The kind's catalog record without its assets (install/catalog/record.rs).

    Args:
        kind: a pinned IPSW kind.

    Returns:
        The record, a dict.
    """
    pins = IPSWS[kind]
    unknown = [key for key in pins if key not in _RECORD_KEYS]
    if unknown:
        fail("{} has pins the catalog doesn't carry: {}".format(kind, unknown))
    missing = _missing(kind)
    if missing:
        fail("{} lacks pins its record needs: {}".format(kind, missing))
    derived = dict(pins["derived"])
    kernelcache_sha256 = derived.pop("kernelcache_sha256")
    steps = {"prelink": PRELINK[pins["xnu"]]}
    if derived.get("launchd_catalog_sha256"):
        steps["launchd_catalog"] = "jobs"
    if derived.get("pretranslated"):
        steps["pretranslate"] = "a64x"
    if int(pins["ios"]) >= _MANIFEST_HASH_FROM_IOS:
        steps["boot_manifest_hash"] = "sha384"
    return {
        "assets": {},
        "build": pins["build"],
        "derived": derived,
        "device_class": pins["device_class"],
        "ios": int(pins["ios"]),
        "ipsw": {"size": pins["size"], "url": pins["url"]},
        "members": {role: _member(kind, role, pin, kernelcache_sha256) for role, pin in pins["members"].items()},
        "product": pins["product"],
        "steps": steps,
        "version": pins["version"],
        "volumes": pins["volumes"] | {"os_volume_uuid": os_volume_uuid(kind)},
        "xnu": pins["xnu"],
    }

def catalog_records(release, kinds = None):
    """The catalog of `kinds` (every kind of catalog_kinds() by default) with no assets.

    Args:
        release: the release the catalog's assets are published under, MODULE.bazel's version.
        kinds: the IPSW kinds.

    Returns:
        The catalog, a dict.
    """
    return {
        "builds": [catalog_record(kind) for kind in (kinds or catalog_kinds())],
        "format": FORMAT,
        "release": release,
    }

def boot_asset_name(release, build):
    return "{}-{}-boot-{}.tar".format(REPO.split("/")[1], release, build)

def overlay_asset_name(release, build):
    return "{}-{}-overlay-{}.tar".format(REPO.split("/")[1], release, build)

def bootstrap_asset_name(release, build):
    return "{}-{}-bootstrap-{}.tar".format(REPO.split("/")[1], release, build)

def gpu_asset_name(release, build):
    return "{}-{}-gpu-{}.tar".format(REPO.split("/")[1], release, build)

def seed_asset_name(release, build):
    return "{}-{}-seed-{}.tar".format(REPO.split("/")[1], release, build)

def _write_catalog(ctx, kinds_assets):
    release = ctx.attr.release
    records = ctx.actions.declare_file(ctx.label.name + "/records.json")
    ctx.actions.write(records, json.encode_indent(catalog_records(release, list(kinds_assets)), indent = "  ") + "\n")
    out = ctx.actions.declare_file(ctx.label.name + "/catalog.json")
    args = ctx.actions.args()
    args.add("--records", records)
    args.add("--out", out)
    args.add_all(["boot", "overlay", "bootstrap", "gpu", "seed"], before_each = "--require-role")
    published = []
    seen = {}
    inputs = [records]
    for kind, assets in kinds_assets.items():
        build = IPSWS[kind]["build"]
        for role, (name, tar) in sorted(assets.items()):
            args.add_all("--asset", [build, role, name, tar])
            if name in seen:
                if seen[name] != tar:
                    fail("asset {} maps to two different files: {} and {}".format(name, seen[name].path, tar.path))
            else:
                link = ctx.actions.declare_file("{}/{}".format(ctx.label.name, name))
                ctx.actions.symlink(output = link, target_file = tar)
                published.append(link)
                seen[name] = tar
                inputs.append(tar)
    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [args],
        inputs = inputs,
        outputs = [out],
        mnemonic = "SwitchCatalog",
        progress_message = "Writing %{label}'s catalog.json",
    )
    return [
        DefaultInfo(files = depset([out] + published)),
        OutputGroupInfo(catalog = depset([out])),
    ]

def _kinds_impl(_settings, attr):
    return {kind: {"//ipsw:kind": kind} for kind in attr.kinds}

_kinds = transition(implementation = _kinds_impl, inputs = [], outputs = ["//ipsw:kind"])

def _single(target):
    files = target[DefaultInfo].files.to_list()
    if len(files) != 1:
        fail("{} must be one file".format(target.label))
    return files[0]

_COMMON = {
    "release": attr.string(mandatory = True, doc = "MODULE.bazel's version: the release tag"),
    "_tool": attr.label(default = "//install/catalog:switch_catalog_tool", executable = True, cfg = "exec"),
}

def _kind_assets(release, kind, boot_tar, overlay_tar, bootstrap_tar, gpu_tar, seed_tar):
    build = IPSWS[kind]["build"]
    return {
        "boot": (boot_asset_name(release, build), boot_tar),
        "bootstrap": (bootstrap_asset_name(release, build), bootstrap_tar),
        "gpu": (gpu_asset_name(release, build), gpu_tar),
        "overlay": (overlay_asset_name(release, build), overlay_tar),
        "seed": (seed_asset_name(release, build), seed_tar),
    }

def _catalog_impl(ctx):
    return _write_catalog(ctx, {
        kind: _kind_assets(
            ctx.attr.release,
            kind,
            _single(ctx.split_attr.boot_assets[kind]),
            _single(ctx.split_attr.overlay_assets[kind]),
            _single(ctx.split_attr.bootstrap_assets[kind]),
            _single(ctx.split_attr.gpu_assets[kind]),
            _single(ctx.split_attr.seed_assets[kind]),
        )
        for kind in ctx.attr.kinds
    })

catalog = rule(
    implementation = _catalog_impl,
    doc = "catalog.json for every kind in `kinds`, each kind's boot, overlay, bootstrap, gpu, and seed assets, and " +
          "those assets under the names the release publishes them by.",
    attrs = _COMMON | {
        "boot_assets": attr.label(mandatory = True, cfg = _kinds, doc = "//boot:boot_assets"),
        "bootstrap_assets": attr.label(mandatory = True, cfg = _kinds, doc = "//bootstrap:payload"),
        "gpu_assets": attr.label(mandatory = True, cfg = _kinds, doc = "//install/image_gpu:gpu_assets"),
        "kinds": attr.string_list(mandatory = True),
        "overlay_assets": attr.label(mandatory = True, cfg = _kinds, doc = "//boot:overlay_assets"),
        "seed_assets": attr.label(mandatory = True, cfg = _kinds, doc = "//install/seed/assets:seed_assets"),
    },
)

def _selected_catalog_impl(ctx):
    kind = ctx.attr._kind[BuildSettingInfo].value
    return _write_catalog(ctx, {
        kind: _kind_assets(
            ctx.attr.release,
            kind,
            ctx.file.boot_assets,
            ctx.file.overlay_assets,
            ctx.file.bootstrap_assets,
            ctx.file.gpu_assets,
            ctx.file.seed_assets,
        ),
    })

selected_catalog = rule(
    implementation = _selected_catalog_impl,
    doc = "catalog.json for the selected kind alone, from its boot, overlay, bootstrap, gpu, and seed assets in this " +
          "configuration.",
    attrs = _COMMON | {
        "boot_assets": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:boot_assets"),
        "bootstrap_assets": attr.label(mandatory = True, allow_single_file = True, doc = "//bootstrap:payload"),
        "gpu_assets": attr.label(mandatory = True, allow_single_file = True, doc = "//install/image_gpu:gpu_assets"),
        "overlay_assets": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:overlay_assets"),
        "seed_assets": attr.label(mandatory = True, allow_single_file = True, doc = "//install/seed/assets:seed_assets"),
        "_kind": attr.label(default = "//ipsw:kind"),
    },
)
