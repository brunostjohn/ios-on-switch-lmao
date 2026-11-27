"""boot_assets(): the selected IPSW kind's boot assets, every file switch_boot makes the boot entry from besides the
IPSW's, under the member names install/boot/assets.rs reads, as rules_pkg files (a pkg_tar packs them)."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@rules_pkg//pkg:providers.bzl", "PackageFilesInfo")
load("//ipsw:pins.bzl", "IPSWS")

# The bundle's own boot.json, which switch_boot reads first: the format and the build it is for.
FORMAT = 1

# The members //boot:package makes, each from (output group, its file of that name).
_PACKAGE = {
    "adt/ios-base.adt": ("adt_base", "ios-base.adt"),
    "trust/overlay-base.trustcache": ("overlay_trust_base", "overlay-base.trustcache"),
    "window/boot-args.txt": ("stage", "boot-args.txt"),
    "bl33/u-boot.bin": ("stage", "u-boot.bin"),
    "bl33/boot-script.img": ("stage", "boot-script.img"),
    "bl33/loader.uboot.bin": ("loader", "loader.uboot.bin"),
    "bl33/constants.json": ("constants", "constants.json"),
    "tree/bootstrap/manifest.json": ("bootstrap_manifest", "bootstrap-manifest.json"),
}

# Only a package that seeds the Installer app has its pin.
_PACKAGE_OPTIONAL = {
    "tree/installer-bundle.json": ("installer_seed", "installer-bundle.json"),
}

def _from_package(package, group, basename, optional):
    found = [f for f in getattr(package[OutputGroupInfo], group).to_list() if f.basename == basename]
    if len(found) > 1 or not (found or optional):
        fail("{}'s {} output group holds {} files named {}".format(package.label, group, len(found), basename))
    return found

def _boot_assets_impl(ctx):
    kind = ctx.attr._kind[BuildSettingInfo].value
    stamp = ctx.actions.declare_file(ctx.label.name + "/boot.json")
    ctx.actions.write(stamp, json.encode({"build": IPSWS[kind]["build"], "format": FORMAT}) + "\n")
    members = {"boot.json": stamp}

    def add(member, f):
        if member in members:
            fail("two files for the member " + member)
        members[member] = f

    for table, optional in ((_PACKAGE, False), (_PACKAGE_OPTIONAL, True)):
        for member, (group, basename) in table.items():
            for f in _from_package(ctx.attr.package, group, basename, optional):
                add(member, f)
    for target, member in ctx.attr.files.items():
        files = target.files.to_list()
        if member.endswith("/"):
            for f in files:
                add(member + f.basename, f)
        elif len(files) > 1:
            fail("{} is {} files: give it a directory member (ending in /)".format(target.label, len(files)))
        elif files:
            add(member, files[0])
    return [
        DefaultInfo(files = depset(members.values())),
        PackageFilesInfo(dest_src_map = members, attributes = {"mode": "0644"}),
    ]

boot_assets = rule(
    implementation = _boot_assets_impl,
    doc = "The selected kind's boot assets by member name: //boot:package's release parts and the other files.",
    attrs = {
        "package": attr.label(mandatory = True, doc = "//boot:package, for the members its output groups hold"),
        "files": attr.label_keyed_string_dict(
            allow_files = True,
            doc = "file -> member; a member ending in / takes each of the target's files by its basename; a " +
                  "target with no files (an empty optional input) gives none",
        ),
        "_kind": attr.label(default = "//ipsw:kind"),
    },
)
