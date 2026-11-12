// SPDX-License-Identifier: MIT
// The system shader compiler's compile context (NativeMetalCompile.rs): the direct compile (NativeMetalDirect.h) and the
// libraries and functions it compiles, with no runtime device.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeMetalDirect.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmbCompileContext NmbCompileContext;

// NVK (`loader`, as nmb_device_open takes it) and m2v's compile devices, as a runtime device of this process would hold them,
// over a Vulkan device of no queues. Null with nmb_last_error
NmbCompileContext *nmb_compile_context_open(const char *loader);

// A library as nmb_library_new makes one, of no device: its functions (nmb_function_new*) answer as a device's do and
// compile only directly, on a context
NmbLibrary *nmb_compile_library_new(const void *bytes, size_t length, bool air);

// nmb_render_pipeline_direct and nmb_compute_pipeline_direct on the context
bool nmb_compile_render_direct(const NmbCompileContext *context, const NmbRenderPipelineDescriptor *descriptor,
                               bool barrier, uint64_t textureFormat, uint32_t textureSamples,
                               const NmbDirectBinds *binds, bool (*each)(const NmbDirectState *state, void *ctx),
                               void *ctx);
bool nmb_compile_compute_direct(const NmbCompileContext *context, const struct NmbComputePipelineDescriptor *descriptor,
                                const NmbDirectDispatch *dispatch,
                                bool (*each)(const NmbDirectKernel *kernel, void *ctx), void *ctx);

// Whether the context's enable set and compile devices are `device`'s (host tests)
bool nmb_compile_context_matches(const NmbCompileContext *context, const NmbDevice *device);

#ifdef __cplusplus
}
#endif
