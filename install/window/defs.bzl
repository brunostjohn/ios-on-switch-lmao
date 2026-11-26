"""switch_window's tests' inputs: //boot:package's trust, window and bl33 steps for the selected IPSW kind, and a test
built at -c opt that reads them from the selected configuration (so the package is not built twice)."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//ipsw:defs.bzl", "PretranslatedPinsInfo")
load("//ipsw:ipsw.bzl", "ios_version", "kernel_virt_base", "os_volume_uuid")

def _named(files, basename):
    found = [f for f in files if f.basename == basename]
    if len(found) > 1:
        fail("more than one " + basename)
    return found

def _window_case_impl(ctx):
    kind = ctx.attr._kind[BuildSettingInfo].value
    pretranslated = ctx.attr.pretranslated[PretranslatedPinsInfo]
    groups = {
        "kernel_window": ctx.files.kernel_window,
        "stage": ctx.files.stage,
        "overlay": ctx.files.overlay,
        "launchd_catalog": ctx.files.launchd_catalog,
    }
    named = {
        "window": ("kernel_window", "kernel-window.bin"),
        "constants": ("kernel_window", "constants.json"),
        "plan": ("kernel_window", "plan.json"),
        "boot_args": ("stage", "boot-args.txt"),
        "u_boot": ("stage", "u-boot.bin"),
        "script": ("stage", "boot-script.img"),
        "overlay_trust": ("overlay", "overlay.trustcache"),
    }
    case = {
        "kind": kind,
        "virt_base": kernel_virt_base(kind),
        "root_volume_uuid": os_volume_uuid(kind),
        "boot_manifest_hash": ios_version(kind) >= 27,
    }
    inputs = []
    for key, (group, basename) in named.items():
        found = _named(groups[group], basename)
        if not found:
            fail("//boot:package's {} output group has no {}".format(group, basename))
        case[key] = found[0].short_path
        inputs += found
    single = {
        "kernel": ctx.file.kernel,
        "adt": ctx.file.adt,
        "trust": ctx.file.trust,
        "bl33": ctx.file.bl33,
        "loader": ctx.file.loader,
        "static_trust": ctx.file.static_trust,
        "overlay_trust_base": ctx.file.overlay_trust_base,
        "installd_cdhash": ctx.file.installd_cdhash,
        "data_seed": ctx.file.data_seed,
        "exporter": ctx.file.exporter,
        "dtb": ctx.file.dtb,
    }
    for key, f in single.items():
        case[key] = f.short_path
        inputs.append(f)

    # boot/defs.bzl BootTrust's modules after the static one, the overlay's without installd.
    modules = ([ctx.file.system_trust, ctx.file.app_trust, ctx.file.overlay_trust_base] +
               _named(ctx.files.launchd_catalog, "launchd.trustcache") + ctx.files.gpu_trustcache +
               pretranslated.trust)
    case["modules"] = [f.short_path for f in modules]
    inputs += modules
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(out, json.encode_indent(case, indent = "  ") + "\n")
    return [DefaultInfo(files = depset([out]), runfiles = ctx.runfiles(files = [out] + inputs))]

window_case = rule(
    implementation = _window_case_impl,
    doc = "The selected IPSW kind's window inputs and what //boot:package made of them, as JSON of runfiles paths.",
    attrs = {
        "kernel": attr.label(mandatory = True, allow_single_file = True, doc = "//kernel:prelinked"),
        "adt": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:package's adt output group"),
        "trust": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:package's trust output group"),
        "bl33": attr.label(mandatory = True, allow_single_file = True, doc = "//boot:package's bl33 output group"),
        "kernel_window": attr.label(mandatory = True, allow_files = True),
        "stage": attr.label(mandatory = True, allow_files = True, doc = "boot-args, u-boot.bin, boot-script.img"),
        "overlay": attr.label(mandatory = True, allow_files = True),
        "overlay_trust_base": attr.label(mandatory = True, allow_single_file = True),
        "launchd_catalog": attr.label(mandatory = True, allow_files = True),
        "loader": attr.label(mandatory = True, allow_single_file = True),
        "static_trust": attr.label(mandatory = True, allow_single_file = True),
        "system_trust": attr.label(mandatory = True, allow_single_file = True),
        "app_trust": attr.label(mandatory = True, allow_single_file = True),
        "gpu_trustcache": attr.label(mandatory = True, allow_files = True),
        "pretranslated": attr.label(mandatory = True, providers = [PretranslatedPinsInfo]),
        "installd_cdhash": attr.label(mandatory = True, allow_single_file = True),
        "data_seed": attr.label(mandatory = True, allow_single_file = True),
        "exporter": attr.label(mandatory = True, allow_single_file = True),
        "dtb": attr.label(mandatory = True, allow_single_file = True),
        "_kind": attr.label(default = "//ipsw:kind"),
    },
)

def _opt_transition_impl(_settings, _attr):
    return {"//command_line_option:compilation_mode": "opt"}

_opt_transition = transition(
    implementation = _opt_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:compilation_mode"],
)

def _opt_test_impl(ctx):
    test = ctx.attr.test[0][DefaultInfo]
    out = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(output = out, target_file = test.files_to_run.executable, is_executable = True)
    env = {k: ctx.expand_location(v, ctx.attr.data) for k, v in ctx.attr.env.items()}
    runfiles = ctx.runfiles(files = ctx.files.data).merge(test.default_runfiles)
    for target in ctx.attr.data:
        runfiles = runfiles.merge(target[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = out, runfiles = runfiles), RunEnvironmentInfo(environment = env)]

opt_test = rule(
    implementation = _opt_test_impl,
    doc = "`test` compiled at -c opt, run on `data` from the configuration the build selects.",
    attrs = {
        "test": attr.label(mandatory = True, executable = True, cfg = _opt_transition),
        "data": attr.label_list(allow_files = True),
        "env": attr.string_dict(),
    },
    test = True,
)

def _reads_installd(action):
    installd = [f for f in action.inputs.to_list() if f.owner.package == "drivers/SwitchInstallPolicy" and f.basename == "installd"]
    return len(installd) > 0

_OVERLAY_MODULES = ("OverlayTrust", "OverlayTrustBase")

def _overlay_modules_impl(ctx):
    env = analysistest.begin(ctx)
    actions = {a.mnemonic: a for a in analysistest.target_actions(env) if a.mnemonic in _OVERLAY_MODULES}
    asserts.equals(env, ["OverlayTrust", "OverlayTrustBase"], sorted(actions.keys()))
    asserts.true(env, _reads_installd(actions["OverlayTrust"]), "the package's overlay module hashes installd")
    asserts.false(env, _reads_installd(actions["OverlayTrustBase"]), "the release's overlay module reads installd")
    return analysistest.end(env)

overlay_modules_test = analysistest.make(
    _overlay_modules_impl,
    doc = "//boot:package's overlay module for the release (OverlayTrustBase) never reads installd, made from Apple's; " +
          "the package's own (OverlayTrust) does.",
)
