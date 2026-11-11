"""What a kernel driver adds to use kits/SwitchGM20B: the kernel toolchain passes no cc_library include paths, and
NVIDIA's class headers include "nvtypes.h" from Mesa's headers directory, not their own."""

SWITCH_GM20B_INCLUDES = ["-I", "third_party/mesa/src/nouveau/headers"]

# The NVIDIA headers the kit's umbrella headers take, from Mesa's vendored copy.
SWITCH_GM20B_MESA_HEADERS = [
    "//third_party/mesa/src/nouveau/headers:drf.h",
    "//third_party/mesa/src/nouveau/headers:nvtypes.h",
] + ["//third_party/mesa/src/nouveau/headers:nvidia/classes/{}.h".format(c) for c in [
    "cl902d",
    "cl906f",
    "cla140",
    "clb06f",
    "clb0b5",
    "clb197",
    "clb197tex",
    "clb1c0",
    "clb1c0qmd",
]]
