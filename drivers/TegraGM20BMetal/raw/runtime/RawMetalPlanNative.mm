// SPDX-License-Identifier: MIT
#import "RawMetalPlanAdapters.h"
#include "raw_compute.h"

namespace {

// The plan's bindings onto the encoders. The range's resources were named to the job before its encode
// (encodeRawAttempt) and every one a draw reads is named again where its binding table is built, so a binding names
// none; the command buffer holds every object, so none is retained here.
struct NativePlanSink {
    __unsafe_unretained RMCommandBuffer *cb;
    raw::CommandBuffer &commands;
    raw::RenderEncoder *render;
    raw::ComputeEncoder *compute;

    void buffer(uint32_t stage, uint32_t slot, const raw::BufferBinding &binding)
    {
        RAW_ENCODE_SPAN(Apply);
        RAW_ENCODE_COUNT(Applied);
        if (binding.source == raw::BufferBinding::Source::Offset) {
            if (stage == 3)
                compute->setBufferOffset(slot, binding.offset);
            else
                render->setBufferOffset(stage, slot, binding.offset);
            return;
        }
        if (binding.source == raw::BufferBinding::Source::Bytes) {
            RAW_ENCODE_SPAN(Uniforms);
            const raw::Buffer &chunk = cb->_bytes.chunks[binding.chunk];
            if (!chunk.wc)
                commands.clean(chunk.cpu + binding.offset, std::max<uint32_t>((binding.length + 255) & ~255u, 256));
            if (stage == 3)
                compute->setBytesAt(slot, chunk.va + binding.offset, binding.length, &chunk);
            else
                render->setBytesAt(stage, slot, chunk.va + binding.offset, binding.length,
                                   binding.shadow ? cb->_shadow.data() + binding.shadow - 1 : chunk.cpu + binding.offset,
                                   &chunk);
            return;
        }
        __unsafe_unretained RMBuffer *object = (__bridge RMBuffer *)binding.object;
        if (stage == 3)
            compute->setBuffer(slot, object ? &object->_raw : nullptr, binding.offset);
        else
            render->setBuffer(stage, slot, object ? &object->_raw : nullptr, binding.offset);
    }

    // A vertex texture (stage 1) binds nothing: no raw record reads one, raw_direct.cpp refuses a vertex-stage handle
    void texture(uint32_t stage, uint32_t slot, void *value)
    {
        RAW_ENCODE_SPAN(Apply);
        RAW_ENCODE_COUNT(Applied);
        __unsafe_unretained RMTexture *object = (__bridge RMTexture *)value;
        if (stage == 3)
            compute->setTexture(slot, object ? &object->_raw : nullptr);
        else if (stage == 2)
            render->setTexture(slot, object ? &object->_raw : nullptr);
    }

    void sampler(uint32_t stage, uint32_t slot, void *value)
    {
        RAW_ENCODE_SPAN(Apply);
        RAW_ENCODE_COUNT(Applied);
        __unsafe_unretained RMSampler *object = (__bridge RMSampler *)value;
        if (stage == 3)
            compute->setSampler(slot, object ? &object->_raw : nullptr);
        else if (stage == 2)
            render->setSampler(slot, object ? &object->_raw : nullptr);
    }

    void kernel(const raw::KernelState &, const raw::KernelState *)
    {}

    void raster(const raw::RasterState &next, const raw::RasterState *previous)
    {
        RAW_ENCODE_SPAN(Apply);
        RAW_ENCODE_COUNT(Applied);
        using R = raw::RasterState;
        using raw::rasterChanged;
        if (rasterChanged(next, previous, R::Pipeline, &R::pipeline) ||
            rasterChanged(next, previous, R::Pipeline, &R::nativePipeline) ||
            rasterChanged(next, previous, R::Pipeline, &R::nativeVariant)) {
            __unsafe_unretained RMPipeline *pipeline = (__bridge RMPipeline *)next.pipeline;
            auto *variant = static_cast<raw::Variant<raw::Pipeline> *>(next.nativeVariant);
            render->setPipeline(variant               ? variant->value()
                                : next.nativePipeline ? static_cast<raw::Pipeline *>(next.nativePipeline)
                                : pipeline            ? pipeline->_raw
                                                      : nullptr);
        }
        if (rasterChanged(next, previous, R::DepthStencil, &R::depthStencil)) {
            __unsafe_unretained RMDepthStencil *depth = (__bridge RMDepthStencil *)next.depthStencil;
            render->setDepthStencil(depth ? &depth->_raw : nullptr);
        }
        if (rasterChanged(next, previous, R::Scissor, &R::scissor))
            render->setScissor(next.scissor[0], next.scissor[1], next.scissor[2], next.scissor[3]);
        if (rasterChanged(next, previous, R::Viewport, &R::viewport))
            render->setViewport(raw::stateDouble(next.viewport[0]), raw::stateDouble(next.viewport[1]),
                                raw::stateDouble(next.viewport[2]), raw::stateDouble(next.viewport[3]),
                                raw::stateDouble(next.viewport[4]), raw::stateDouble(next.viewport[5]));
        if (rasterChanged(next, previous, R::Cull, &R::cull))
            render->setCullMode(uint8_t(next.cull));
        if (rasterChanged(next, previous, R::Winding, &R::winding))
            render->setWinding(uint8_t(next.winding));
        if (rasterChanged(next, previous, R::Fill, &R::fill))
            render->setFillMode(uint8_t(next.fill));
        if (rasterChanged(next, previous, R::Blend, &R::blend)) {
            const float values[4] = {raw::stateFloat(next.blend[0]), raw::stateFloat(next.blend[1]),
                                     raw::stateFloat(next.blend[2]), raw::stateFloat(next.blend[3])};
            render->setBlendColor(values);
        }
        if (rasterChanged(next, previous, R::Stencil, &R::stencil))
            render->setStencilReference(next.stencil[0], next.stencil[1]);
        if (rasterChanged(next, previous, R::DepthBias, &R::depthBias))
            render->setDepthBias(raw::stateFloat(next.depthBias[0]), raw::stateFloat(next.depthBias[1]),
                                 raw::stateFloat(next.depthBias[2]));
    }
};

}  // namespace

// (the command buffer is its caller's, so ARC need not retain it at each command)
__attribute__((objc_externally_retained)) void encodePlannedState(RMCommandBuffer *cb, size_t at,
                                                                  raw::CommandBuffer &commands,
                                                                  raw::RenderEncoder *render,
                                                                  raw::ComputeEncoder *compute,
                                                                  raw::StateCursor &cursor)
{
    const Op op = cb->_plan.operation(at).op;
    NativePlanSink sink{cb, commands, render, compute};
    if (isPlanDraw(op))
        raw::applyRenderState(cb->_plan, cb->_plan.draw(at).state, cursor, sink);
    else if (op == OpDispatch)
        raw::applyKernelState(cb->_plan, cb->_plan.dispatch(at).state, cursor, sink);
    else if (op == OpRender || op == OpCompute || op == OpEnd)
        cursor.reset();
}
