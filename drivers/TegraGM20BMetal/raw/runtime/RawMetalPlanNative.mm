// SPDX-License-Identifier: MIT
#import "RawMetalPlanAdapters.h"
#include "raw_compute.h"

namespace {

struct NativePlanSink {
    RMCommandBuffer *cb;
    raw::CommandBuffer &commands;
    raw::RenderEncoder *render;
    raw::ComputeEncoder *compute;

    void buffer(uint32_t stage, uint32_t slot, const raw::BufferBinding &binding)
    {
        if (binding.source == raw::BufferBinding::Source::Offset) {
            if (stage == 3)
                compute->setBufferOffset(slot, binding.offset);
            else
                render->setBufferOffset(stage, slot, binding.offset);
            return;
        }
        if (binding.source == raw::BufferBinding::Source::Bytes) {
            const raw::Buffer &chunk = cb->_bytes.chunks[binding.chunk];
            if (!chunk.wc)
                commands.clean(chunk.cpu + binding.offset, std::max<uint32_t>((binding.length + 255) & ~255u, 256));
            if (stage == 3)
                compute->setBytesAt(slot, chunk.va + binding.offset, binding.length, &chunk);
            else
                render->setBytesAt(stage, slot, chunk.va + binding.offset, binding.length, chunk.cpu + binding.offset,
                                   &chunk);
            return;
        }
        RMBuffer *object = (__bridge RMBuffer *)binding.object;
        useBuffer(&commands, object, stage == 3);
        if (stage == 3)
            compute->setBuffer(slot, object ? &object->_raw : nullptr, binding.offset);
        else
            render->setBuffer(stage, slot, object ? &object->_raw : nullptr, binding.offset);
    }

    void texture(uint32_t stage, uint32_t slot, void *value)
    {
        RMTexture *object = (__bridge RMTexture *)value;
        if (stage == 3) {
            useTexture(&commands, object, true);
            compute->setTexture(slot, object ? &object->_raw : nullptr);
        } else if (stage == 2) {
            useTexture(&commands, object, false);
            render->setTexture(slot, object ? &object->_raw : nullptr);
        }
    }

    void sampler(uint32_t stage, uint32_t slot, void *value)
    {
        RMSampler *object = (__bridge RMSampler *)value;
        if (stage == 3)
            compute->setSampler(slot, object ? &object->_raw : nullptr);
        else if (stage == 2)
            render->setSampler(slot, object ? &object->_raw : nullptr);
    }

    void kernel(const raw::KernelState &, const raw::KernelState *)
    {}

    void raster(const raw::RasterState &next, const raw::RasterState *previous)
    {
        using R = raw::RasterState;
        using raw::rasterChanged;
        if (rasterChanged(next, previous, R::Pipeline, &R::pipeline) ||
            rasterChanged(next, previous, R::Pipeline, &R::nativePipeline)) {
            RMPipeline *pipeline = (__bridge RMPipeline *)next.pipeline;
            render->setPipeline(next.nativePipeline ? static_cast<raw::Pipeline *>(next.nativePipeline)
                                : pipeline          ? pipeline->_raw
                                                    : nullptr);
        }
        if (rasterChanged(next, previous, R::DepthStencil, &R::depthStencil)) {
            RMDepthStencil *depth = (__bridge RMDepthStencil *)next.depthStencil;
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

void encodePlannedState(RMCommandBuffer *cb, size_t at, raw::CommandBuffer &commands, raw::RenderEncoder *render,
                        raw::ComputeEncoder *compute, raw::StateCursor &cursor)
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
