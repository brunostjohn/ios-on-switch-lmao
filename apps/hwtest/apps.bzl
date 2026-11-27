"""The hardware test apps' rules.

    hwtest_app(name, minimum_os_version, ...)   one iOS version's app: ad hoc signed, iPad, the shaders as its library
    ios_arm64e(name, app)                        an ios_application's .ipa built for arm64e alone, whatever
                                                 --ios_multi_cpus says (.bazelrc: arm64)
    reproducible_apps(name, apps, twin)          the apps' .ipa files, and twin's built again in a configuration of
                                                 its own (reproducible_test)
    metal_airs(name, apps)                       the .air files the apps' shaders compile to, which their
                                                 default.metallib is linked from (reproducible_test)

rules_apple takes an app's architectures from --ios_multi_cpus only, so a per-app arm64e build needs ios_arm64e's
outgoing transition.
"""

load("@rules_apple//apple:ios.bzl", "ios_application")

def hwtest_app(name, minimum_os_version, deps, bundle_id, bundle_name, entitlements, infoplist, **kwargs):
    """One iOS version's hardware test app.

    Ad hoc signed like the overlay's other code (--@rules_apple//apple/build_settings:signing_certificate_name=- in
    .bazelrc), so its CDHashes go in the boot package's trust cache. rules_apple wants a profile for any device build:
    adhoc.mobileprovision is a plain-XML stand-in. It lands in the bundle as embedded.mobileprovision, which nothing
    reads for a trust-cached platform app.

    Args:
        name: the target.
        minimum_os_version: the iOS version the app is built for.
        deps: its swift_library, with the @main.
        bundle_id: the bundle identifier.
        bundle_name: the bundle's and the executable's name.
        entitlements: the entitlements plist.
        infoplist: the Info.plist.
        **kwargs: passed to ios_application (tags, visibility).
    """
    ios_application(
        name = name,
        bundle_id = bundle_id,
        bundle_name = bundle_name,
        entitlements = entitlements,
        entitlements_validation = "skip",
        families = ["ipad"],
        infoplists = [infoplist],
        minimum_os_version = minimum_os_version,
        provisioning_profile = "//apps:adhoc.mobileprovision",
        resources = native.glob(["shaders/*.metal"]),
        deps = deps,
        **kwargs
    )

def _arm64e_transition_impl(_settings, _attr):
    return {"//command_line_option:ios_multi_cpus": ["arm64e"]}

_arm64e_transition = transition(
    implementation = _arm64e_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:ios_multi_cpus"],
)

def _ios_arm64e_impl(ctx):
    return [DefaultInfo(files = ctx.attr.app[0][DefaultInfo].files)]

ios_arm64e = rule(
    implementation = _ios_arm64e_impl,
    attrs = {
        "app": attr.label(
            cfg = _arm64e_transition,
            mandatory = True,
            doc = "the ios_application to build for arm64e",
        ),
    },
    doc = "An ios_application's .ipa, built for arm64e alone.",
)

def _twin_transition_impl(_settings, _attr):
    return {"//apps/hwtest:twin": True}

_twin_transition = transition(
    implementation = _twin_transition_impl,
    inputs = [],
    outputs = ["//apps/hwtest:twin"],
)

def _ipa(app):
    ipa = [f for f in app[DefaultInfo].files.to_list() if f.extension == "ipa"]
    if len(ipa) != 1:
        fail("{} builds no single .ipa".format(app.label))
    return ipa[0]

def _reproducible_apps_impl(ctx):
    # Links of their own names: an app's .ipa and its twin's share one path, which a runfiles tree holds once.
    twin = ctx.attr.twin[0]
    links = []
    for app, suffix in [(app, ".ipa") for app in ctx.attr.apps] + [(twin, ".twin.ipa")]:
        link = ctx.actions.declare_file("{}/{}{}".format(ctx.label.name, app.label.name, suffix))
        ctx.actions.symlink(output = link, target_file = _ipa(app))
        links.append(link)
    return [DefaultInfo(files = depset(links))]

reproducible_apps = rule(
    implementation = _reproducible_apps_impl,
    attrs = {
        "apps": attr.label_list(mandatory = True, doc = "the apps whose .ipa the test reads"),
        "twin": attr.label(
            cfg = _twin_transition,
            mandatory = True,
            doc = "an app to build a second time, under //apps/hwtest:twin",
        ),
    },
    doc = "<name>/<app>.ipa for each app, and <name>/<twin>.twin.ipa: the twin's app with every action run again.",
)

_MetalAirInfo = provider(doc = "An app's .air files.", fields = ["airs"])

def _metal_air_aspect_impl(target, _ctx):
    airs = [air for action in target.actions if action.mnemonic == "MetalCompile" for air in action.outputs.to_list()]
    return [_MetalAirInfo(airs = depset(airs))]

_metal_air_aspect = aspect(implementation = _metal_air_aspect_impl)

def _metal_airs_impl(ctx):
    return [DefaultInfo(files = depset(transitive = [app[_MetalAirInfo].airs for app in ctx.attr.apps]))]

metal_airs = rule(
    implementation = _metal_airs_impl,
    attrs = {
        "apps": attr.label_list(mandatory = True, aspects = [_metal_air_aspect], doc = "the ios_application targets"),
    },
    doc = "The .air files rules_apple's MetalCompile actions write for the apps' shaders.",
)

_METAL_COMPILE_SHELL = """\
set -euo pipefail
DEVELOPER_DIR="$(/usr/bin/sed -n 's/^DEVELOPER_DIR=//p' "$1")"
export DEVELOPER_DIR
src="$2"
shift 2
{ printf '#line 1 "%s"\\n' "$src"; cat "$src"; } | /usr/bin/xcrun "$@" -x metal -
"""

_METALLIB_LINK_SHELL = """\
set -euo pipefail
DEVELOPER_DIR="$(/usr/bin/sed -n 's/^DEVELOPER_DIR=//p' "$1")"
export DEVELOPER_DIR
shift
exec /usr/bin/xcrun "$@"
"""

def _hwtest_metallib_impl(ctx):
    target = "air64-apple-ios" + ctx.attr.minimum_os_version
    env = {"LC_ALL": "C", "PATH": "/usr/bin:/bin", "TZ": "UTC"}
    xcode_inputs = [ctx.file._xcode_env, ctx.file._xcode_identity]
    airs = []
    for src in sorted(ctx.files.srcs, key = lambda f: f.path):
        stem = src.basename.removesuffix(".metal")
        air = ctx.actions.declare_file("{}_airs/{}.air".format(ctx.label.name, stem))
        args = ctx.actions.args()
        args.add(ctx.file._xcode_env)
        args.add(src)
        args.add_all(["--sdk", "iphoneos", "metal", "-c", "-target", target, "-ffast-math", "-o", air])
        args.add_all(["-Xclang", "-main-file-name", "-Xclang", src])
        args.add("-iquote", src.dirname)
        ctx.actions.run_shell(
            command = _METAL_COMPILE_SHELL,
            arguments = [args],
            inputs = [src] + xcode_inputs,
            outputs = [air],
            env = env,
            mnemonic = "HwtestMetalCompile",
            progress_message = "Compiling {} ({})".format(src.basename, target),
            execution_requirements = {"requires-darwin": ""},
        )
        airs.append(air)

    out = ctx.actions.declare_file("{}/default.metallib".format(ctx.label.name))
    link_args = ctx.actions.args()
    link_args.add(ctx.file._xcode_env)
    link_args.add_all(["--sdk", "iphoneos", "metallib", "-o", out])
    link_args.add_all(airs)
    ctx.actions.run_shell(
        command = _METALLIB_LINK_SHELL,
        arguments = [link_args],
        inputs = airs + xcode_inputs,
        outputs = [out],
        env = env,
        mnemonic = "HwtestMetallibLink",
        progress_message = "Linking {} ({})".format(out.basename, target),
        execution_requirements = {"requires-darwin": ""},
    )
    return [DefaultInfo(files = depset([out]))]

hwtest_metallib = rule(
    implementation = _hwtest_metallib_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".metal"], mandatory = True),
        "minimum_os_version": attr.string(mandatory = True),
        "_xcode_env": attr.label(default = "@xcode//:xcode.env", allow_single_file = True),
        "_xcode_identity": attr.label(default = "@xcode//:identity", allow_single_file = True),
    },
    doc = "Compiles the HW Test app's Metal shaders into default.metallib without IPSW inputs.",
)
