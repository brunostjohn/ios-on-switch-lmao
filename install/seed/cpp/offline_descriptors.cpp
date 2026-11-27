// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/seed/cpp/offline_metal.h"

#include "drivers/TegraGM20BMetal/render_key.h"

namespace switch_seed {
namespace {

constexpr uint64_t kPixelFormatRGBA16Float = 115;
constexpr uint64_t kPixelFormatTwoPlaneStandIn = 542;

bool blobBytes(NmbReflectionBlob *made, std::string &out, std::string &why)
{
    if (!made) {
        why = std::string("no reflection: ") + nmb_last_error();
        return false;
    }
    size_t length = 0;
    const uint8_t *bytes = nmb_reflection_blob_bytes(made, &length);
    out.assign(reinterpret_cast<const char *>(bytes), bytes ? length : 0);
    nmb_reflection_blob_release(made);
    return bytes != nullptr;
}

bool renderCallback(const void *descriptor, bool barrier, uint64_t textureFormat, uint32_t textureSamples,
                    const NmbDirectBinds *binds, uint32_t stageTextures, bool (*each)(const NmbDirectState *, void *),
                    void *ctx, const char **why)
{
    const auto &d = *static_cast<const RenderDescriptor *>(descriptor);
    NmbOfflineRenderPipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    if (!d.toNmb(p, linked)) {
        *why = "a function of another device";
        return false;
    }
    if (nmb_render_pipeline_offline_kind(&p, barrier, textureFormat, textureSamples, binds, stageTextures, each, ctx))
        return true;
    *why = nmb_last_error();
    return false;
}

bool computeCallback(const void *descriptor, const NmbDirectDispatch *dispatch,
                     bool (*each)(const NmbDirectKernel *, void *), void *ctx, const char **why)
{
    const auto &d = *static_cast<const ComputeDescriptor *>(descriptor);
    NmbOfflineComputePipelineDescriptor p = {};
    if (!d.toNmb(p)) {
        *why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    if (nmb_compute_pipeline_offline(&p, dispatch, each, ctx))
        return true;
    *why = nmb_last_error();
    return false;
}

}  // namespace

RenderDescriptor::RenderDescriptor()
{
    for (unsigned i = 0; i < 8; i++)
        color[i] = {0, 0xF, 0, 1, 0, 0, 1, 0, 0};
    for (unsigned i = 0; i < 31; i++)
        vertexLayouts[i] = {0, 1, 1};
}

gm20b::PipelineFields RenderDescriptor::fields() const
{
    gm20b::PipelineFields f;
    f.samples = samples;
    f.topology = topology;
    f.depthFormat = depthFormat;
    f.stencilFormat = stencilFormat;
    f.rasterization = rasterization ? 1 : 0;
    f.amplification = amplification;
    f.textureWriteRounding = textureWriteRounding;
    f.patchType = vertex ? vertex->patchType() : 0;
    f.alphaToCoverage = alphaToCoverage ? 1 : 0;
    f.tessellation = tessellation;
    for (unsigned i = 0; i < 8; i++)
        f.color[i] = color[i];
    if (hasVertexDescriptor) {
        for (unsigned i = 0; i < 31; i++) {
            const NmbVertexAttribute &a = vertexAttributes[i];
            if (!a.format)
                continue;
            const NmbVertexLayout &l = vertexLayouts[a.buffer < 31 ? a.buffer : 0];
            f.attributes[i] = {a.format, a.offset, a.buffer, l.stride, l.step_function, l.step_rate};
        }
    }
    return f;
}

std::vector<uint32_t> RenderDescriptor::activeVertexAttributes() const
{
    std::vector<uint32_t> out;
    if (!vertex || !vertex->handle())
        return out;
    const int64_t count = nmb_offline_function_vertex_attribute_count(vertex->handle());
    if (count <= 0)
        return out;
    for (uint32_t i = 0; i < uint32_t(count); i++) {
        char name[256];
        uint64_t info[5] = {};
        if (!nmb_offline_function_vertex_attribute(vertex->handle(), i, name, sizeof name, info))
            continue;
        const uint64_t index = info[0];
        const bool active = info[2] != 0;
        if (active && (!hasVertexDescriptor || (index < 31 && vertexAttributes[index].format != 0)))
            out.push_back(uint32_t(index));
    }
    return out;
}

bool RenderDescriptor::toNmb(NmbOfflineRenderPipelineDescriptor &p, std::vector<NmbOfflineFunction *> &linked) const
{
    if (!vertex || !vertex->handle() || (fragment && !fragment->handle()))
        return false;
    linked.clear();
    const auto pushAll = [&](const std::vector<std::shared_ptr<OfflineFunction>> &list) {
        for (const auto &fn : list) {
            if (!fn || !fn->handle())
                return false;
            linked.push_back(fn->handle());
        }
        return true;
    };
    const size_t vVisStart = linked.size();
    if (!pushAll(vertexLinked))
        return false;
    const size_t vVisCount = linked.size() - vVisStart;
    if (!pushAll(vertexPrivateLinked))
        return false;
    const size_t vHidCount = linked.size() - vVisStart - vVisCount;
    if (!pushAll(fragmentLinked))
        return false;
    const size_t fVisCount = linked.size() - vVisStart - vVisCount - vHidCount;
    if (!pushAll(fragmentPrivateLinked))
        return false;
    const size_t fHidCount = linked.size() - vVisStart - vVisCount - vHidCount - fVisCount;

    p.vertex_linked = linked.data();
    p.vertex_linked_count = vVisCount;
    p.vertex_private_linked = linked.data() + vVisCount;
    p.vertex_private_count = vHidCount;
    p.fragment_linked = linked.data() + vVisCount + vHidCount;
    p.fragment_linked_count = fVisCount;
    p.fragment_private_linked = linked.data() + vVisCount + vHidCount + fVisCount;
    p.fragment_private_count = fHidCount;
    p.vertex = vertex->handle();
    p.fragment = fragment ? fragment->handle() : nullptr;

    for (unsigned i = 0; i < 8; i++) {
        const gm20b::ColorFields &a = color[i];
        const uint64_t format = a.format == kPixelFormatTwoPlaneStandIn ? kPixelFormatRGBA16Float : a.format;
        p.colors[i] = {format,         a.writeMask,   a.blending,         a.sourceRGB,     a.destinationRGB,
                       a.operationRGB, a.sourceAlpha, a.destinationAlpha, a.operationAlpha};
    }
    p.depth_format = depthFormat;
    p.stencil_format = stencilFormat;
    p.samples = samples;
    p.topology = topology;
    p.rasterization = rasterization ? 1 : 0;
    if (tessellation.maxFactor != 16 || tessellation.partition || tessellation.factorFormat ||
        tessellation.factorStep || tessellation.winding || tessellation.controlPointIndex || tessellation.factorScale) {
        p.tessellation = 1;
        p.max_tessellation_factor = tessellation.maxFactor;
        p.partition_mode = tessellation.partition;
        p.factor_format = tessellation.factorFormat;
        p.factor_step_function = tessellation.factorStep;
        p.winding = tessellation.winding;
        p.control_point_index_type = tessellation.controlPointIndex;
        p.factor_scale_enabled = tessellation.factorScale;
    }
    p.max_amplification = amplification;
    p.texture_write_rounding = textureWriteRounding;
    p.alpha_to_coverage = alphaToCoverage ? 1 : 0;
    if (hasVertexDescriptor) {
        for (unsigned i = 0; i < 31; i++) {
            const NmbVertexAttribute &a = vertexAttributes[i];
            if (!a.format)
                continue;
            p.vertex_inputs = 1;
            p.attributes[i] = a;
            if (a.buffer < 31)
                p.layouts[a.buffer] = vertexLayouts[a.buffer];
        }
    }
    return true;
}

bool RenderDescriptor::renderCacheKey(const std::string &key, bool own, uint64_t stack, gm20b::CacheKey &out) const
{
    gm20b::AirHash vAir{}, fAir{};
    if (!vertex || !vertex->airHash(vAir) || (fragment && !fragment->airHash(fAir)))
        return false;
    std::vector<gm20b::LinkedStageAir> linked;
    const auto collect = [&](char stage, const std::vector<std::shared_ptr<OfflineFunction>> &list) {
        for (const auto &fn : list) {
            gm20b::LinkedStageAir entry{stage, {}};
            if (!fn || !fn->airHash(entry.air))
                return false;
            linked.push_back(entry);
        }
        return true;
    };
    if (!collect('v', vertexLinked) || !collect('v', vertexPrivateLinked) || !collect('f', fragmentLinked) ||
        !collect('f', fragmentPrivateLinked))
        return false;
    out = gm20b::renderCacheKey(stack, key, vAir, fAir, linked, own);
    return true;
}

ComputeDescriptor::ComputeDescriptor()
{
    for (unsigned i = 0; i < 31; i++)
        stageLayouts[i] = {0, 1, 1};
}

bool ComputeDescriptor::toNmb(NmbOfflineComputePipelineDescriptor &p) const
{
    if (!function || !function->handle())
        return false;
    if (hasStageInput) {
        for (unsigned i = 0; i < 31; i++) {
            const NmbVertexAttribute &a = stageAttributes[i];
            if (!a.format)
                continue;
            p.stage_inputs = 1;
            p.stage_attributes[i] = a;
            if (a.buffer < 31)
                p.stage_layouts[a.buffer] = stageLayouts[a.buffer];
        }
        p.stage_index_type = stageIndexType;
        p.stage_index_buffer = stageIndexBuffer;
    }
    p.function = function->handle();
    p.max_threads = maxThreads;
    p.multiple_of_execution_width = multipleOfWidth ? 1 : 0;
    p.indirect = indirect ? 1 : 0;
    p.max_call_stack_depth = maxCallStackDepth;
    p.adding_binary_functions = addingBinaryFunctions ? 1 : 0;
    for (unsigned i = 0; i < 31; i++)
        p.mutability[i] = mutability[i];
    return true;
}

gm20b::DirectTranslatorCore offlineTranslator()
{
    return {&renderCallback, &computeCallback};
}

bool kernelCompiles(const ComputeDescriptor &d, std::string &why)
{
    NmbOfflineComputePipelineDescriptor p = {};
    if (!d.toNmb(p)) {
        why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    if (nmb_offline_compute_pipeline_compiles(&p))
        return true;
    why = nmb_last_error();
    return false;
}

bool kernelFacts(const ComputeDescriptor &d, uint32_t facts[3], std::string &why)
{
    NmbOfflineComputePipelineDescriptor p = {};
    if (!d.toNmb(p)) {
        why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    if (nmb_offline_compute_pipeline_facts(&p, facts))
        return true;
    why = std::string("no compute prepare: ") + nmb_last_error();
    return false;
}

bool renderReflection(const RenderDescriptor &d, std::string &blob, std::string &why)
{
    NmbOfflineRenderPipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    if (!d.toNmb(p, linked)) {
        why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    return blobBytes(nmb_offline_render_pipeline_reflection(&p), blob, why);
}

bool computeReflection(const ComputeDescriptor &d, std::string &blob, std::string &why)
{
    NmbOfflineComputePipelineDescriptor p = {};
    if (!d.toNmb(p)) {
        why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    return blobBytes(nmb_offline_compute_pipeline_reflection(&p), blob, why);
}

}  // namespace switch_seed
