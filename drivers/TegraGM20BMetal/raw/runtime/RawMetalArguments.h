// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

enum Op : uint8_t {
    OpRender,
    OpBlit,
    OpCompute,
    OpEnd,
    OpPipeline,
    OpBytes,
    OpBuffer,
    OpBufferOffset,
    OpTexture,
    OpSampler,
    OpScissor,
    OpViewport,
    OpDraw,
    OpCull,
    OpWinding,
    OpDepthStencil,
    OpFillMode,
    OpBlendColor,
    OpStencilRef,
    OpDepthBias,
    OpBarrier,
    OpUse,
    OpUpdateFence,
    OpWaitFence,
    OpCopyTexture,
    OpCopyBuffer,
    OpCopyBufferToTexture,
    OpCopyTextureToBuffer,
    OpMipmaps,
    OpInvocation,
    OpSignalEvent,
    OpWaitEvent,
    OpDrawIndexed,
    OpVisibility,
    OpFillBuffer,
    // a compute encoder's (RawMetalCompute.mm)
    OpKernelState,
    OpKernelBytes,
    OpKernelBuffer,
    OpKernelBufferOffset,
    OpKernelTexture,
    OpKernelSampler,
    OpKernelMemory,
    OpKernelUse,
    OpKernelBarrier,
    OpDispatch,
    OpDrawIndirect,
    OpDrawIndexedIndirect,
};

// Transient adapter arguments. Native command plans do not store setter records.
struct Cmd {
    Op op;
    uint8_t stage;
    uint16_t index;
    uint32_t a, b;
    uint64_t c, x;
    void *obj, *obj2;
};

inline Cmd cmd(Op op, uint8_t stage = 0, uint16_t index = 0)
{
    Cmd c = {};
    c.op = op;
    c.stage = stage;
    c.index = index;
    return c;
}

inline bool isPlanDraw(Op op)
{
    return op == OpDraw || op == OpDrawIndexed || op == OpDrawIndirect || op == OpDrawIndexedIndirect;
}
