"""C and C++17 toolchains for the Linux (musl) and Windows (MSVC) desktop targets on @llvm's clang: what our cc_library
targets and crates' build scripts (cc-rs, through rules_rust) compile with. Rust links these targets itself with
rust-lld (bazel/desktop/BUILD.bazel), so the link actions here only carry what rules_rust reads from them: the Windows
libraries' directories, as LIB, and on Linux the static C++ runtime (libc++.a, libc++abi.a) via static_runtime_lib.

  linux    clang --target=<arch>-unknown-linux-musl --sysroot=@desktop_musl//<arch>, -fPIC (Rust links static-pie);
           C++ adds -std=c++17 -nostdinc++ -isystem <sysroot>/usr/include/c++/v1
  windows  clang-cl --target=<arch>-pc-windows-msvc /MT /Brepro, the CRT's and SDK's headers by /imsvc;
           C++ adds /std:c++17 /EHsc

No dependency file (no_dotd_file): Bazel checks a .d file's headers against the targets' declared ones, and the
sysroots are neither those nor absolute paths it could exempt.
"""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load(
    "@rules_cc//cc:cc_toolchain_config_lib.bzl",
    "action_config",
    "artifact_name_pattern",
    "env_entry",
    "env_set",
    "feature",
    "flag_group",
    "flag_set",
    "tool",
    "variable_with_value",
)
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")
load("//bazel/llvm:llvm.bzl", "RESOURCE_HEADERS")

_C = [ACTION_NAMES.c_compile]
_CXX = [ACTION_NAMES.cpp_compile]
_ASM = [ACTION_NAMES.assemble, ACTION_NAMES.preprocess_assemble]
_COMPILE = _C + _CXX + _ASM
_LINK = [ACTION_NAMES.cpp_link_executable, ACTION_NAMES.cpp_link_dynamic_library, ACTION_NAMES.cpp_link_nodeps_dynamic_library]
_ARCHIVE = [ACTION_NAMES.cpp_link_static_library]

def _flags(actions, flags):
    return flag_set(actions = actions, flag_groups = [flag_group(flags = flags)])

def _variable_flags(actions, flags, variable):
    return flag_set(actions = actions, flag_groups = [flag_group(flags = flags, expand_if_available = variable)])

def _object_list(flag_format):
    return flag_group(
        iterate_over = "libraries_to_link",
        expand_if_available = "libraries_to_link",
        flag_groups = [
            flag_group(
                flags = [flag_format.format("%{libraries_to_link.name}")],
                expand_if_equal = variable_with_value(name = "libraries_to_link.type", value = "object_file"),
            ),
            flag_group(
                flags = [flag_format.format("%{libraries_to_link.object_files}")],
                iterate_over = "libraries_to_link.object_files",
                expand_if_equal = variable_with_value(name = "libraries_to_link.type", value = "object_file_group"),
            ),
        ],
    )

def _linux(ctx, triple, resource_dir):
    compile_flags = [
        "--target=" + triple,
        "--sysroot=" + ctx.file.sysroot.dirname,
        "-resource-dir=" + resource_dir,
        "-no-canonical-prefixes",
        "-fPIC",
    ]

    # -isystem precedes --sysroot's usr/include so libc++ wrappers (#include_next) resolve into musl.
    cxx_flags = [
        "-std=c++17",
        "-nostdinc++",
        "-isystem",
        ctx.file.sysroot.dirname + "/usr/include/c++/v1",
    ]
    features = [
        feature(name = "linux_compile", enabled = True, flag_sets = [_flags(_COMPILE, compile_flags)]),
        feature(name = "linux_cxx_compile", enabled = True, flag_sets = [_flags(_CXX, cxx_flags)]),
        feature(name = "static_link_cpp_runtimes", enabled = True),
        feature(name = "preprocessor_defines", enabled = True, flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
            flags = ["-D%{preprocessor_defines}"],
            iterate_over = "preprocessor_defines",
            expand_if_available = "preprocessor_defines",
        )])]),
        feature(name = "include_paths", enabled = True, flag_sets = [flag_set(actions = _COMPILE, flag_groups = [
            flag_group(flags = ["-iquote", "%{quote_include_paths}"], iterate_over = "quote_include_paths", expand_if_available = "quote_include_paths"),
            flag_group(flags = ["-I%{include_paths}"], iterate_over = "include_paths", expand_if_available = "include_paths"),
            flag_group(flags = ["-isystem", "%{system_include_paths}"], iterate_over = "system_include_paths", expand_if_available = "system_include_paths"),
        ])]),
        feature(name = "user_compile_flags", enabled = True, flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
            flags = ["%{user_compile_flags}"],
            iterate_over = "user_compile_flags",
            expand_if_available = "user_compile_flags",
        )])]),
        feature(name = "compiler_input_flags", enabled = True, flag_sets = [_variable_flags(_COMPILE, ["-c", "%{source_file}"], "source_file")]),
        feature(name = "compiler_output_flags", enabled = True, flag_sets = [_variable_flags(_COMPILE, ["-o", "%{output_file}"], "output_file")]),
        feature(name = "archiver_flags", enabled = True, flag_sets = [flag_set(actions = _ARCHIVE, flag_groups = [
            flag_group(flags = ["rcsD", "%{output_execpath}"], expand_if_available = "output_execpath"),
            _object_list("{}"),
        ])]),
    ]
    tools = struct(compile = ctx.executable.clang, link = ctx.executable.clang, archive = ctx.executable.ar)
    return features, tools, []

def _windows(ctx, triple, resource_dir):
    root = ctx.file.sysroot.dirname
    arch = triple.split("-")[0]
    includes = ["crt/include", "sdk/include/ucrt", "sdk/include/um", "sdk/include/shared"]
    compile_flags = ["--target=" + triple, "-resource-dir=" + resource_dir, "/MT", "/Brepro"]
    for include in includes:
        compile_flags += ["/imsvc", root + "/" + include]
    cxx_flags = ["/std:c++17", "/EHsc"]
    libs = ";".join([root + "/" + lib.format(arch) for lib in ("crt/lib/{}", "sdk/lib/um/{}", "sdk/lib/ucrt/{}")])
    features = [
        feature(name = "windows_compile", enabled = True, flag_sets = [_flags(_COMPILE, compile_flags)]),
        feature(name = "windows_cxx_compile", enabled = True, flag_sets = [_flags(_CXX, cxx_flags)]),
        feature(name = "windows_libraries", enabled = True, env_sets = [env_set(actions = _LINK, env_entries = [env_entry("LIB", libs)])]),
        feature(name = "preprocessor_defines", enabled = True, flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
            flags = ["/D%{preprocessor_defines}"],
            iterate_over = "preprocessor_defines",
            expand_if_available = "preprocessor_defines",
        )])]),
        feature(name = "include_paths", enabled = True, flag_sets = [flag_set(actions = _COMPILE, flag_groups = [
            flag_group(flags = ["/I%{quote_include_paths}"], iterate_over = "quote_include_paths", expand_if_available = "quote_include_paths"),
            flag_group(flags = ["/I%{include_paths}"], iterate_over = "include_paths", expand_if_available = "include_paths"),
            flag_group(flags = ["/imsvc", "%{system_include_paths}"], iterate_over = "system_include_paths", expand_if_available = "system_include_paths"),
        ])]),
        feature(name = "user_compile_flags", enabled = True, flag_sets = [flag_set(actions = _COMPILE, flag_groups = [flag_group(
            flags = ["%{user_compile_flags}"],
            iterate_over = "user_compile_flags",
            expand_if_available = "user_compile_flags",
        )])]),
        feature(name = "compiler_input_flags", enabled = True, flag_sets = [_variable_flags(_COMPILE, ["/c", "%{source_file}"], "source_file")]),
        feature(name = "compiler_output_flags", enabled = True, flag_sets = [_variable_flags(_COMPILE, ["/Fo%{output_file}"], "output_file")]),
        feature(name = "archiver_flags", enabled = True, flag_sets = [flag_set(actions = _ARCHIVE, flag_groups = [
            flag_group(flags = ["/OUT:%{output_execpath}"], expand_if_available = "output_execpath"),
            _object_list("{}"),
        ])]),
    ]
    tools = struct(compile = ctx.executable.clang_cl, link = ctx.executable.lld_link, archive = ctx.executable.lib)
    patterns = [
        artifact_name_pattern(category_name = "object_file", prefix = "", extension = ".obj"),
        artifact_name_pattern(category_name = "static_library", prefix = "", extension = ".lib"),
        artifact_name_pattern(category_name = "alwayslink_static_library", prefix = "", extension = ".lo.lib"),
        artifact_name_pattern(category_name = "executable", prefix = "", extension = ".exe"),
        artifact_name_pattern(category_name = "dynamic_library", prefix = "", extension = ".dll"),
        artifact_name_pattern(category_name = "interface_library", prefix = "", extension = ".if.lib"),
    ]
    return features, tools, patterns

def _impl(ctx):
    triple = ctx.attr.triple
    resource_dir = ctx.executable.clang.dirname.rpartition("/")[0] + "/" + RESOURCE_HEADERS.rpartition("/")[0]
    features, tools, patterns = (_windows if "windows" in triple else _linux)(ctx, triple, resource_dir)
    env = env_set(actions = _COMPILE + _LINK + _ARCHIVE, env_entries = [
        env_entry("PATH", "/usr/bin:/bin"),
        env_entry("LC_ALL", "C"),
        env_entry("TZ", "UTC"),
    ])
    features = [
        feature(name = "no_legacy_features", enabled = True),
        feature(name = "no_dotd_file", enabled = True),
        feature(name = "desktop_env", enabled = True, env_sets = [env]),
    ] + features
    action_configs = [
        action_config(action_name = name, enabled = True, tools = [tool(tool = tools.compile)])
        for name in _COMPILE
    ] + [
        action_config(action_name = name, enabled = True, tools = [tool(tool = tools.link)])
        for name in _LINK
    ] + [
        action_config(action_name = name, enabled = True, tools = [tool(tool = tools.archive)])
        for name in _ARCHIVE
    ]
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier = "switch-desktop-" + triple,
        host_system_name = "aarch64-apple-darwin",
        target_system_name = triple,
        target_cpu = triple.split("-")[0],
        target_libc = triple.split("-")[-1],
        compiler = "clang",
        abi_version = "none",
        abi_libc_version = "none",
        features = features,
        action_configs = action_configs,
        artifact_name_patterns = patterns,
    )

desktop_cc_toolchain_config = rule(
    implementation = _impl,
    doc = "A CcToolchainConfigInfo for one Linux musl or Windows MSVC triple on @llvm's clang.",
    attrs = {
        "triple": attr.string(mandatory = True),
        "sysroot": attr.label(mandatory = True, allow_single_file = True, doc = "the SYSROOT marker at the root of the target's headers"),
        "clang": attr.label(default = "@llvm//:bin/clang", executable = True, cfg = "exec", allow_single_file = True),
        "clang_cl": attr.label(default = "@llvm//:bin/clang-cl", executable = True, cfg = "exec", allow_single_file = True),
        "lld_link": attr.label(default = "@llvm//:bin/lld-link", executable = True, cfg = "exec", allow_single_file = True),
        "ar": attr.label(default = "@llvm//:bin/llvm-ar", executable = True, cfg = "exec", allow_single_file = True),
        "lib": attr.label(default = "@llvm//:bin/llvm-lib", executable = True, cfg = "exec", allow_single_file = True),
    },
    provides = [CcToolchainConfigInfo],
)
