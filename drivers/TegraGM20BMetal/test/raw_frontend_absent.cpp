// SPDX-License-Identifier: MIT
// The frontend's and the bridge's entry points the raw facade links (NativeBridgeMetalFrontend.h, bridge/NativeMetalObjects.h)
// for a process whose m2v device is a stand-in (raw_facade_harness.h): each answers as the frontend does with no device of
// its own open, so nothing the facade asks of m2v is answered by anything but the stand-in it opened.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalBlend.h"
#include "bridge/NativeMetalObjects.h"

extern "C" {

id native_bridge_metal_open(const char *, const char *, uint64_t, void (*)(const char *, const char *))
{
    return nil;
}

bool native_bridge_metal_compile_open(const char *)
{
    return false;
}

id native_bridge_metal_compile_library(const char *, dispatch_data_t, NSError **)
{
    return nil;
}

id native_bridge_metal_buffer_over_foreign(const struct switch_foreign_memory *, void *, NSUInteger, NSUInteger)
{
    return nil;
}

bool native_bridge_metal_compute_direct(id, const struct NmbDirectDispatch *,
                                        bool (*)(const struct NmbDirectKernel *, void *), void *, const char **why)
{
    *why = "no device";
    return false;
}

id native_bridge_metal_compute_prepare(id, NSUInteger, id *, NSError **)
{
    return nil;
}

NSString *native_bridge_metal_function_library_name(id)
{
    return nil;
}

id native_bridge_metal_function_metallib(id, const void **, size_t *)
{
    return nil;
}

void native_bridge_metal_journal_dispatch(id, MTLSize, MTLSize, bool, const char *, const char *, const char *)
{}

void native_bridge_metal_journal_render(id, NSUInteger)
{}

bool native_bridge_metal_memory(uint64_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    return false;
}

void *native_bridge_metal_nvk_symbol(const char *)
{
    return nullptr;
}

bool native_bridge_metal_render_direct(id, bool, NSUInteger, NSUInteger, const struct NmbDirectBinds *,
                                       bool (*)(const struct NmbDirectState *, void *), void *, const char **why)
{
    *why = "no device";
    return false;
}

id native_bridge_metal_render_pipeline_lazy(id, id, NSUInteger)
{
    return nil;
}

int native_bridge_metal_render_pipeline_ready(id, void (^)(void))
{
    return -1;
}

int native_bridge_metal_render_pipeline_urge(id, void (^)(void), dispatch_queue_t)
{
    return -1;
}

void native_bridge_metal_set_surface_importer(switch_surface_importer)
{}

id native_bridge_metal_texture_in_place(id, id, NSUInteger, NSUInteger)
{
    return nil;
}

int native_bridge_metal_texture_memory(id, struct switch_nvk_image_memory *)
{
    return -3;
}

id native_bridge_metal_texture_tiled_in_place(id, id, NSUInteger)
{
    return nil;
}

const char *nmb_last_error(void)
{
    return "no device";
}

// the translator's blend colour words: no scenario here draws with a constant blend colour
void switch_nmb_blend_color_words(const float color[4], uint32_t words[8])
{
    (void)color;
    for (int i = 0; i < 8; i++)
        words[i] = 0;
}
}
