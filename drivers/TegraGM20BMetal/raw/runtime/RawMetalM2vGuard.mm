// SPDX-License-Identifier: MIT
// What m2v has no form of: its frontend's encoders implement no drawPatches:, setTessellationFactorBuffer:,
// setVertexAmplificationCount:viewMappings: or attribute stride forms (NativeBridgeMetalFrontend.mm answers each zero, so
// m2v draws without them), a state the raw path made of tessellation, amplification, a dynamic attribute stride or a
// stage-in kernel has no twin that draws it as Metal does, and the replay of indirect draw commands reads no range the GPU
// wrote (replayCommands). A range of a command buffer the raw path refused that holds any of these never replays there:
// the command buffer fails as Metal reports an error, naming the refusal.
#import "RawMetalIndirectCompute.h"

namespace {

const char *drawMissing(RMCommandBuffer *cb, size_t at, Op op)
{
    if (op == OpDrawPatches || op == OpDrawPatchesIndirect)
        return "a patch draw";
    const auto &plan = cb->_plan;
    if (op == OpExecuteCommands && ([(__bridge RMIndirectCommandBuffer *)plan.draw(at).arguments kinds] & RMICBPatches))
        return "indirect commands of patches";
    if (op == OpExecuteCommands && plan.draw(at).indices)
        return "an execute of indirect commands by a range the GPU reads";
    const raw::RenderStateRef &state = plan.renderStates[plan.draw(at).state];
    const raw::RasterState &raster = plan.raster[state.raster];
    if (raster.amplification[0] > 1)
        return "a vertex amplification count above 1";
    id pipeline = (__bridge id)raster.pipeline;
    if ([pipeline isKindOfClass:[RMPipeline class]] && ((RMPipeline *)pipeline)->_m2vMissing)
        return ((RMPipeline *)pipeline)->_m2vMissing;
    const auto &vertexBuffers = plan.buffers[0][state.buffers[0]];
    for (uint32_t slot = 0; slot < 31; slot++)
        if (vertexBuffers[slot].stride != raw::kStaticStride)
            return "a vertex buffer bound with an attribute stride";
    return nullptr;
}

const char *kernelMissing(id state)
{
    return [state isKindOfClass:[RMComputeState class]] ? ((RMComputeState *)state)->_m2vMissing : nullptr;
}

// The compute encoder's state `index` (RMCommandPlan::kernelStates) at a dispatch or an execute of dispatch commands
const char *kernelStateMissing(RMCommandBuffer *cb, uint32_t index)
{
    const auto &plan = cb->_plan;
    const raw::KernelStateRef &state = plan.kernelStates[index];
    if (const char *missing = kernelMissing((__bridge id)plan.kernel[state.state].pipeline))
        return missing;
    const auto &buffers = plan.buffers[2][state.buffers];
    for (uint32_t slot = 0; slot < 31; slot++)
        if (buffers[slot].stride != raw::kStaticStride)
            return "a kernel buffer bound with an attribute stride";
    return nullptr;
}

const char *executeMissing(RMCommandBuffer *cb, size_t at)
{
    const RMPlanControl &control = cb->_plan.control(at);
    if (control.compute != raw::kNoSnapshot)
        if (const char *missing = kernelStateMissing(cb, control.compute))
            return missing;
    RMKernelExecute *x = (__bridge RMKernelExecute *)cb->_plan.legacy(at).obj;
    if (const char *missing = kernelMissing(x->_pipeline))
        return missing;
    RMIndirectComputeBuffer *b = x->_buffer;
    std::lock_guard<std::mutex> hold(b->_lock);
    for (const RMICBDispatch &c : b->_dispatches) {
        if (c.stageIn)
            return "an indirect dispatch command of a stage-in region";
        if (const char *missing = kernelMissing(c.pipeline))
            return missing;
        for (const uint32_t stride : c.strides)
            if (stride != raw::kNoBindStride)
                return "an indirect dispatch command's buffer bound with an attribute stride";
    }
    return nullptr;
}

// What of [from, to) m2v has no form of (null: nothing)
const char *rangeMissing(RMCommandBuffer *cb, size_t from, size_t to)
{
    for (size_t at = from; at < to; at++) {
        const Op op = cb->_plan.operation(at).op;
        const char *missing = nullptr;
        if (isPlanDraw(op))
            missing = drawMissing(cb, at, op);
        else if (op == OpDispatch)
            missing = kernelStateMissing(cb, cb->_plan.dispatch(at).state);
        else if (op == OpExecuteKernels)
            missing = executeMissing(cb, at);
        if (missing)
            return missing;
    }
    return nullptr;
}

}  // namespace

const char *rmM2vMissingState(MTLRenderPipelineDescriptor *d)
{
    if (d.vertexFunction.patchType != MTLPatchTypeNone)
        return "a tessellation state";
    if (d.maxVertexAmplificationCount > 1)
        return "a state of vertex amplification";
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (NSUInteger i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format != MTLVertexFormatInvalid && a.bufferIndex < 31 &&
                v.layouts[a.bufferIndex].stride == MTLBufferLayoutStrideDynamic)
                return "a state of a dynamic attribute stride";
        }
    return nullptr;
}

const char *rmM2vMissingKernel(MTLComputePipelineDescriptor *d)
{
    MTLStageInputOutputDescriptor *in = d.stageInputDescriptor;
    for (NSUInteger i = 0; in && i < 31; i++)
        if (in.attributes[i].format != MTLAttributeFormatInvalid)
            return "a stage-in kernel";
    return nullptr;
}

bool rmRefuseM2vRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    const char *missing = rangeMissing(cb, from, to);
    if (!missing)
        return false;
    NSString *refusal = cb->_why ?: @"?";
    SWITCH_REFUSE_DETAIL("raw", "replay.m2v_missing", uintptr_t(missing),
                         "a command buffer range the raw path refused (%s) holds %s, which m2v has no form of: the "
                         "command buffer fails",
                         refusal.UTF8String, missing);
    failCommandBuffer(cb, [NSString stringWithFormat:@"The raw path refused %@, and the work it left to m2v holds %s, "
                                                     @"which m2v does not draw",
                                                     refusal, missing]);
    return true;
}
