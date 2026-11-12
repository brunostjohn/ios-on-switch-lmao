// SPDX-License-Identifier: MIT
// The raw GM20B path's direct compile of a render state (NativeMetalDirect.rs): what m2v's translator makes of the state for
// each of the pack generator's probe draws, with no pipeline state, command buffer or capture.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeMetalObjects.h"
#include "NativeMetalBlend.h"

#ifdef __cplusplus
extern "C" {
#endif

// One stage as the pipeline library m2v creates for it compiles it (nvk_switch_compile.h's stage info, with the state's layout)
typedef struct NmbDirectStage {
    const uint32_t *spirv;
    size_t spirvBytes;
    const char *entry;
    uint32_t stage;              // VkShaderStageFlagBits
    uint32_t flags;              // VkPipelineShaderStageCreateFlags
    uint32_t subgroupSize;       // 0: none required
    const void *specialization;  // VkSpecializationInfo, null without constants
    const void *semantic;
    const void *portableContext;
    bool (*portable)(const void *context, const uint32_t **words, size_t *bytes);
} NmbDirectStage;

typedef enum NmbDirectSource : uint32_t {
    NmbDirectBuffer,
    NmbDirectBytes,
    NmbDirectTexture,
    NmbDirectSampler,
    NmbDirectCombined,
    NmbDirectAttachment,         // a color attachment as an input attachment (the barrier form's fetch)
    NmbDirectStorageAttachment,  // a color attachment as a storage image (the interlocked storage form's)
    NmbDirectNeutral,
    NmbDirectColorTable,  // m2v's sRGB colour table (bind::color_write_table), the same bytes for every state
    NmbDirectOther,
    NmbDirectDepthStandIn,
    NmbDirectStencilStandIn,
    // a tessellation state's control stage: the encoder's factor buffer, the draw's patch and control-point index buffers
    NmbDirectTessFactors,
    NmbDirectPatchIndices,
    NmbDirectControlPointIndices
} NmbDirectSource;

#define NMB_DIRECT_IMMUTABLE UINT32_MAX
#define NMB_DIRECT_NEUTRAL   (UINT32_MAX - 1)

typedef struct NmbDirectWrite {
    uint32_t set, binding, element;
    int32_t type;  // VkDescriptorType
    NmbDirectSource source;
    uint32_t stage,
        slot;          // a buffer's or texture's Metal stage (1 vertex, 2 fragment) and index; an attachment's color
    uint32_t sampler;  // a sampler's slot, NMB_DIRECT_IMMUTABLE (the layout's constexpr sampler) or NMB_DIRECT_NEUTRAL
    uint64_t range;    // a buffer's bound range
    int32_t viewType;  // an image\'s VkImageViewType (a neutral\'s too), -1 for no image
} NmbDirectWrite;

// One probe draw on the variant its bind state settles on
typedef struct NmbDirectDraw {
    uint64_t set0;  // that variant's set 0 VkDescriptorSetLayout (0: none)
    const NmbDirectWrite *writes;
    uint32_t writeCount;
    uint32_t pushMask;  // the push-constant words [0, 128) the draw writes, and their values
    uint32_t push[32];
    uint32_t blendColorWord, halfBlendColorWord;  // push word + 1, zero when absent
    uint32_t depthTestWord, stencilTestWord;
    uint32_t pushHoles;   // push words the consumer fills with addresses
    uint32_t vertexSlot;  // the first fixed-function vertex buffer's Metal slot (UINT32_MAX: none) and stride
    uint32_t vertexStride;
    uint32_t vertexStrideDynamic;  // that buffer's stride is its bind's (MTLBufferLayoutStrideDynamic)
    // a pulled vertex buffer layout of a dynamic stride: its Metal buffer index and m2v's VertexBufferStride push word + 1
    uint32_t pulledStrides[4][2];
    // vertex amplification: the count the draw was planned at (the descriptor's maxVertexAmplificationCount, one draw per
    // amplification), the push words + 1 of m2v's AmplificationId and AmplificationCount (0: none)
    uint32_t amplificationMax, amplificationIdWord, amplificationCountWord;
    // and of its AmplificationLayerOffset and AmplificationViewportOffset (it is compiled for a layered pass whose
    // mappings offset the viewport), which each amplification writes
    uint32_t amplificationLayerWord, amplificationViewportWord;
    // push words + 1 of m2v's RenderTargetArrayLength (a vertex function that chooses its slice: compiled for a layered
    // pass) and ViewportCount (one that chooses its viewport), which each draw writes
    uint32_t arrayLengthWord, viewportCountWord;
    // the vertex attributes the translation reads in the shader, by Metal index: fixed-function input fetches the others,
    // numbered densely in Metal attribute order (m2v link/varyings)
    uint32_t pulledAttributes;
    const char *variant;
} NmbDirectDraw;

typedef struct NmbDirectStorage {
    uint32_t mask, writes, samplePlanes, samples;
    uint32_t formats[8];  // actual identity-mapped storage view, including sRGB's integer view
    uint32_t fragmentTests;
    uint32_t depthFormat, stencilFormat, testSamples, testPlanes;
    // the attachments whose fixed-function blend the shader computes, and the union of their blend models' assumptions
    // (m2v StorageAttachmentUse::blend_model)
    uint32_t blendMask, blendModel;
} NmbDirectStorage;

// A tessellation state's draws (m2v link/tessellation): each patch one control point of a PATCH_LIST of one. `parameters`:
// SET_TESSELLATION_PARAMETERS as NVK's NVK_MME_SET_TESS_PARAMS makes it of the control stage's modes; `controlWord`: the
// push word of the control stage's first field + 1 (control::PUSH_FIELDS: factor residual, instance stride, scale, patch
// index residual, patch index bound, control-point index residual); `controlPoints`: the function's [[patch(_, N)]] N (0:
// unstated); `indexBytes`: its control-point index size (0: none)
typedef struct NmbDirectTess {
    uint32_t parameters;
    uint32_t controlWord;
    uint32_t controlPoints;
    uint32_t indexBytes;
} NmbDirectTess;

typedef struct NmbDirectState {
    uint64_t device;           // VkDevice of the stage compile
    uint64_t layout;           // VkPipelineLayout of draw 0's variant
    uint32_t stageCount;       // 2, or 4 for a tessellation state
    NmbDirectStage stages[4];  // vertex, fragment, then a tessellation state's control and evaluation stages
    NmbDirectTess tess;
    bool sampleShading;
    bool storageForm;  // the fragment function reads color attachments through storage under the interlock
    NmbDirectStorage storage;
    uint32_t blended;  // the color attachments m2v hands a Vulkan blend, by index
    NmbDirectDraw draws
        [4];  // probe.mm's: 256, 16 and 4096 inline bytes per slot, and 256 bytes in buffers at each slot's own offset
} NmbDirectState;

// A fragment sampler slot the probe binds a cubic sampler state at (MTLSamplerMinMagFilterCubic, no mip filter): its address
// modes and border colour (MTLSamplerAddressMode, MTLSamplerBorderColor) and coordinates
typedef struct NmbDirectCubicSampler {
    uint32_t slot, sAddress, tAddress, rAddress, borderColor;
    bool normalized;
} NmbDirectCubicSampler;

// A fragment texture slot the probe binds a texture of another MTLPixelFormat at
typedef struct NmbDirectTextureFormat {
    uint32_t slot;
    uint64_t format;
} NmbDirectTextureFormat;

// What the probe binds beyond its tagged textures and samplers (the raw path's cubic variants: raw/runtime/raw_cubic.h)
typedef struct NmbDirectBinds {
    const NmbDirectCubicSampler *cubic;
    uint32_t cubicCount;
    const NmbDirectTextureFormat *textures;
    uint32_t textureCount;
    uint32_t fragmentTests;
    uint32_t bindStride;  // the attributeStride the probe binds every vertex buffer at (0: none)
} NmbDirectBinds;

// `descriptor`'s state (its stages' linked functions included) as the translator makes it for the probe's draws, handed to
// `each` (valid during the call). `barrier`: compiled as on a device without the pixel interlock; `textureFormat` and
// `textureSamples`: the MTLPixelFormat and sample count of the probe's textures (0: BGRA8Unorm, 1); `binds` (null: none):
// what the probe binds besides. False with nmb_last_error: refused, or `each`'s false
bool nmb_render_pipeline_direct(NmbDevice *device, const NmbRenderPipelineDescriptor *descriptor, bool barrier,
                                uint64_t textureFormat, uint32_t textureSamples, const NmbDirectBinds *binds,
                                bool (*each)(const NmbDirectState *state, void *ctx), void *ctx);

// A texture a kernel's dispatch binds at `slot`, as its MTLTextureDescriptor answers
typedef struct NmbDirectBoundTexture {
    uint32_t slot;
    uint64_t pixelFormat, textureType, usage, levels, arrayLength, samples;
} NmbDirectBoundTexture;

typedef struct NmbDirectBoundSampler {
    uint32_t slot;
    struct NmbSamplerDescriptor descriptor;
} NmbDirectBoundSampler;

// What a compute state's dispatch binds besides its buffers, which the export binds itself (the probe's: 4096 inline bytes at
// every slot, the same bytes in buffers elsewhere, and each buffer `1 + slot % 15` bytes past its start)
typedef struct NmbDirectDispatch {
    uint32_t threads[3];
    uint32_t memory[31];  // threadgroup memory lengths by index, 0: none
    const NmbDirectBoundTexture *textures;
    uint32_t textureCount;
    const NmbDirectBoundSampler *samplers;
    uint32_t samplerCount;
    uint32_t regionMode;
} NmbDirectDispatch;

typedef struct NmbDirectKernelWrite {
    NmbDirectWrite write;
    uint32_t viewFormat;  // a texture's view: its VkFormat, its VkComponentSwizzles with the texture's own composed in
    uint32_t components[4];  //   (identity where the function swizzles itself), 0: no view
    uint64_t sampler;  // a sampler made for the write (VkSampler on the kernel's device, live during the call), 0: none
} NmbDirectKernelWrite;

typedef struct NmbDirectKernel {
    uint64_t device;       // VkDevice of the stage compile
    uint64_t layout;       // VkPipelineLayout of the dispatch's variant
    uint64_t set0;         // its set 0 VkDescriptorSetLayout (0: none)
    NmbDirectStage stage;  // the dispatch's region: its specialization, stage flags and required subgroup size
    const NmbDirectKernelWrite *writes;
    uint32_t writeCount;
    // The push-constant words [0, 128) of the probe's three dispatches: inline bytes, buffers elsewhere, buffers off 16
    uint32_t pushMask, push[32], movedMask, moved[32], boundMask, bound[32];
    // The buffers-elsewhere dispatch ran on the same variant, specialization and set-0 plan; the bind-offset one too
    bool movedSame, boundSame;
    int32_t regionWord;  // the push word of m2v's DispatchRegion field, -1: none
    uint32_t regions;    // the plain dispatch's Vulkan dispatches
    uint32_t pushHoles;  // push words the consumer fills with addresses
    const char *variant;
    uint32_t bufferReads, bufferWrites, textureReads, textureWrites;
    // a stage-in kernel's: the push word of m2v's StageInRegion field (two uvec4: origin, size; -1: none), and each layout
    // of a dynamic stride's Metal buffer index and StageInStride push word + 1
    int32_t stageInRegionWord;
    uint32_t stageInStrides[4][2];
} NmbDirectKernel;

// `descriptor`'s compute state as the translator makes it for a dispatch of `dispatch`'s threadgroup shape and bindings, handed
// to `each` (valid during the call). False with nmb_last_error: refused, or `each`'s false
bool nmb_compute_pipeline_direct(NmbDevice *device, const struct NmbComputePipelineDescriptor *descriptor,
                                 const NmbDirectDispatch *dispatch,
                                 bool (*each)(const NmbDirectKernel *kernel, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
