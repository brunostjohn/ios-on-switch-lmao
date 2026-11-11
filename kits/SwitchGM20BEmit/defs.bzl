"""Mesa's generated push helpers for GM20B's classes, and what a kernel driver adds to use the kit.

Each class's nv_push_<cl>.h is Mesa's class_parser.py output (mesa/<cl>/, unchanged, for Mesa's own sources built on
the host) and that output made kernel-safe (gen/, mesa/kernel_safe_push_header.py) for everything else."""

load("//kits/SwitchGM20B:defs.bzl", "SWITCH_GM20B_INCLUDES")

_MESA_HEADERS = "//third_party/mesa/src/nouveau/headers"

# The kit's sources, for user space (:emit) and the kernel's compile check alike.
EMIT_SOURCES = [
    "SwitchGM20BEmit3D.c",
    "SwitchGM20BEmitBarrier.c",
    "mesa/nouveau_copy_rect.c",
]

# GM20B's engine classes, and Fermi's 3D and 2D classes Mesa's MME simulator names.
PUSH_CLASSES = ["clb197", "clb1c0", "clb0b5", "cl902d", "cla140", "clb06f", "cl906f", "cl9097"]

# The kernel toolchain passes no cc_library include paths: a kernel driver names them.
SWITCH_GM20B_EMIT_INCLUDES = SWITCH_GM20B_INCLUDES + [
    "-I",
    "kits/SwitchGM20BEmit",
    "-I",
    "kits/SwitchGM20BEmit/mesa",
    "-I",
    "$(GENDIR)/kits/SwitchGM20BEmit/gen",
]

def push_headers(name, classes = PUSH_CLASSES):
    """`name`: the kernel-safe headers (gen/nv_push_<cl>.h); `name`_mesa: Mesa's own (mesa/<cl>/nv_push_<cl>.h)."""
    safe, pristine = [], []
    for cl in classes:
        header = "{}:nvidia/classes/{}.h".format(_MESA_HEADERS, cl)
        mesa_out = "mesa/{}/nv_push_{}.h".format(cl, cl)
        native.genrule(
            name = "nv_push_{}_mesa".format(cl),
            srcs = [header],
            outs = [mesa_out],
            cmd = "$(execpath {}:class_parser) --in-h $(execpath {}) --out-h $@".format(_MESA_HEADERS, header),
            tools = ["{}:class_parser".format(_MESA_HEADERS)],
        )
        safe_out = "gen/nv_push_{}.h".format(cl)
        native.genrule(
            name = "nv_push_{}".format(cl),
            srcs = [mesa_out],
            outs = [safe_out],
            cmd = "$(execpath :kernel_safe_push_header) $(execpath {}) $@ {}.h".format(mesa_out, cl),
            tools = [":kernel_safe_push_header"],
        )
        safe.append(safe_out)
        pristine.append(mesa_out)
    native.filegroup(name = name, srcs = safe)
    native.filegroup(name = name + "_mesa", srcs = pristine)
