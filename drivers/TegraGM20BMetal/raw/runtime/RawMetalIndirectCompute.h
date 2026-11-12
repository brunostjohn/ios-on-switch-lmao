// SPDX-License-Identifier: MIT
// Indirect command buffers of dispatches (RawMetalIndirectCompute.mm, RawMetalIndirectExecute.mm, raw_icb_compute.h)
#pragma once
#import "RawMetalComputeState.h"
#import "RawMetalIndirect.h"
#include <memory>
#include "raw_compute.h"
#include "raw_icb_compute.h"

using RMKernelSet = raw::ComputeKernelSet<raw::ComputeState>;

// GPU memory a command's regions lie in. A blit's copy of a command shares its source's until either is encoded again.
struct RMICBBlock {
    uint8_t *cpu = nullptr;
    uint64_t va = 0;
    uint32_t bytes = 0;
};

// A dispatch command as the CPU encoded it
struct RMICBDispatch {
    RMComputeState *pipeline = nil;
    RMBuffer *buffers[31] = {};
    uint64_t offsets[31] = {};
    std::array<uint32_t, 31> strides = raw::StageIn::unbound();  // setKernelBuffer:offset:attributeStride:atIndex:'s
    std::vector<std::pair<uint32_t, uint32_t>> memory;  // threadgroup memory lengths by index, as the encoder keys them
    std::array<uint64_t, 3> grid{};
    std::array<uint32_t, 3> threads{};
    bool dispatched = false, perThread = false, barrier = false, stageIn = false;
    MTLRegion stageInRegion{};
    std::shared_ptr<RMKernelSet> kernels;  // its own pipeline's at its shape
    std::shared_ptr<RMICBBlock> block;
    raw::icb::ComputeCommand words{};  // what its slot launches
    bool raw = false;                  // its slot launches it (a buffer that inherits nothing)
};

@interface RMIndirectComputeBuffer : RMIndirectCommandBuffer {
@public
    MTLIndirectCommandBufferDescriptor *_descriptor;
    bool _inheritPipeline, _inheritBuffers;
    std::vector<RMICBDispatch> _dispatches;
    std::vector<raw::Buffer> _chunks;  // the blocks' memory, freed with the buffer
    uint64_t _chunkUsed;
    id<MTLIndirectCommandBuffer> _m2v;  // m2v's buffer, into which its replay encodes the commands again
}
@end

@interface RMIndirectComputeCommand : RMObject {
@public
    RMIndirectComputeBuffer *_buffer;
    NSUInteger _index;
}
@end

// A compute encoder's execute of a buffer of dispatches (OpExecuteKernels' object, retained by its command buffer)
@interface RMKernelExecute : NSObject {
@public
    RMIndirectComputeBuffer *_buffer;
    RMBuffer *_range;  // the execution range the GPU reads at _rangeOffset; nil: [_first, _first + _count)
    uint64_t _rangeOffset;
    RMComputeState *_pipeline;  // the encoder's, for a buffer that inherits it
    uint32_t _first, _count;
    // a buffer that inherits the pipeline: each command's kernels at the encoder's (a slot of none launches nothing)
    std::vector<std::shared_ptr<RMKernelSet>> _kernels;
}
@end

// RawMetalIndirectCompute.mm
id rmNewComputeCommandBuffer(id device, MTLIndirectCommandBufferDescriptor *d, NSUInteger count);
// The kernels of `pipeline` for command `c`'s dispatch (null: no raw form), made on the calling thread where no other
// dispatch of the shape made them, and `shape` its regions
std::shared_ptr<RMKernelSet> rmCommandKernels(RMComputeState *pipeline, const RMICBDispatch &c,
                                              raw::ComputeDispatch &shape);
// Command `c`'s regions written into `block` (computeRegionBytes each) with `buffers` bound at `strides`, and the words that
// launch them
bool rmCommandRegions(const RMKernelSet &kernels, const raw::ComputeDispatch &shape, const RMICBDispatch &c,
                      const raw::ComputeEncoder::IcbBuffer (&buffers)[31], const std::array<uint32_t, 31> &strides,
                      const RMICBBlock &block, raw::icb::ComputeCommand &words, const char **why);
uint32_t rmCommandBytes(const RMKernelSet &kernels);
void rmCommandBuffers(const RMICBDispatch &c, raw::ComputeEncoder::IcbBuffer (&out)[31]);

// RawMetalCompute.mm: the pipeline the encoder has set
@interface RMComputeEncoder (State)
- (RMComputeState *)rmState;
@end

// RawMetalIndirectExecute.mm: the raw encode of OpExecuteKernels, m2v's replay of it (the commands encoded again into m2v's
// buffer, which executes them), and a blit's reset or copy of a buffer of dispatches (be null: m2v's replay)
bool encodeKernelCommands(raw::ComputeEncoder &e, raw::CommandBuffer &rc, RMKernelExecute *x, const char **why);
void replayKernelCommands(id<MTLComputeCommandEncoder> encoder, RMKernelExecute *x, std::set<RMBuffer *> &buffers);
void encodeKernelCommandsBlit(raw::BlitEncoder *be, const Cmd &c);
