// SPDX-License-Identifier: MIT
// A compute encoder's execute of an indirect command buffer of dispatches (raw_icb_compute.h): the buffer's own words by
// reference where it inherits nothing, else words written now for the encoder's pipeline or buffers into the command buffer's
// upload memory; m2v's replay encodes the commands again into m2v's buffer, which executes them as Metal does; and a blit's
// reset or copy of the commands.
#import "RawMetalIndirectCompute.h"

namespace {

// The commands an execute reaches: all of them where the GPU reads the range
std::pair<uint32_t, uint32_t> reach(RMKernelExecute *x)
{
    return x->_range ? std::make_pair(0u, uint32_t(x->_buffer->_count))
                     : std::make_pair(x->_first, x->_first + x->_count);
}

uint32_t *slotOf(RMIndirectComputeBuffer *b, NSUInteger index)
{
    return reinterpret_cast<uint32_t *>(b->_raw.cpu) + index * raw::icb::kComputeSlotWords;
}

// Why the execute has no raw form (null: it has), each command's kernels made for the encoder's pipeline where it inherits it
const char *admit(RMKernelExecute *x)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::lock_guard<std::mutex> hold(b->_lock);
    if (b->_inheritPipeline)
        x->_kernels.resize(to - from);
    for (uint32_t i = from; i < to; i++) {
        const RMICBDispatch &c = b->_dispatches[i];
        if (!c.dispatched)
            continue;
        if (b->_inheritPipeline) {
            raw::ComputeDispatch shape;
            if (!(x->_kernels[i - from] = rmCommandKernels(x->_pipeline, c, shape)))
                return "an indirect dispatch command the raw path has no kernel for at the encoder's pipeline";
        } else if (!(b->_inheritBuffers ? bool(c.kernels) : c.raw)) {
            return "an indirect dispatch command the raw path has no kernel for";
        }
    }
    return nullptr;
}

std::vector<RMBuffer *> commandBuffers(RMKernelExecute *x)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::vector<RMBuffer *> out;
    std::lock_guard<std::mutex> hold(b->_lock);
    for (uint32_t i = from; i < to && !b->_inheritBuffers; i++)
        for (RMBuffer *buffer : b->_dispatches[i].buffers)
            if (buffer && std::find(out.begin(), out.end(), buffer) == out.end())
                out.push_back(buffer);
    return out;
}

// The commands' words for the state the buffer inherits, into the command buffer's upload memory: their slots, then each
// command's regions
bool inheritedSlots(raw::ComputeEncoder &e, RMKernelExecute *x, raw::Buffer &slots, const char **why)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    const uint32_t bytes = (to - from) * raw::icb::kComputeSlotWords * 4;
    slots = {};
    slots.cpu = e.upload(bytes, slots.va);
    slots.length = bytes;
    raw::ComputeEncoder::IcbBuffer inherited[31];
    std::array<uint32_t, 31> inheritedStrides{};
    if (b->_inheritBuffers)
        e.inheritBuffers(inherited, inheritedStrides);
    std::lock_guard<std::mutex> hold(b->_lock);
    for (uint32_t i = from; i < to; i++) {
        const RMICBDispatch &c = b->_dispatches[i];
        uint32_t *slot = reinterpret_cast<uint32_t *>(slots.cpu) + (i - from) * raw::icb::kComputeSlotWords;
        const std::shared_ptr<RMKernelSet> &kernels = b->_inheritPipeline ? x->_kernels[i - from] : c.kernels;
        RMComputeState *pipeline = b->_inheritPipeline ? x->_pipeline : c.pipeline;
        if (!c.dispatched || !kernels || !pipeline) {
            raw::icb::computeEmpty(slot);
            continue;
        }
        const raw::ComputeDispatch shape = raw::computeDispatch(c.grid, c.threads, c.perThread, pipeline->_threadLimit);
        raw::ComputeEncoder::IcbBuffer own[31];
        if (!b->_inheritBuffers)
            rmCommandBuffers(c, own);
        RMICBBlock block;
        block.bytes = rmCommandBytes(*kernels);
        block.cpu = e.upload(block.bytes, block.va);
        raw::icb::ComputeCommand words{};
        if (!rmCommandRegions(*kernels, shape, c, b->_inheritBuffers ? inherited : own,
                              b->_inheritBuffers ? inheritedStrides : c.strides, block, words, why))
            return false;
        words.barrier = c.barrier;
        raw::icb::dispatch(slot, i, words);
    }
    return true;
}

}  // namespace

@implementation RMComputeEncoder (Indirect)

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    [self executeKernels:buffer first:range.location count:range.length range:nil offset:0];
}

// The range the GPU reads at `offset` of `rangeBuffer` selects the commands: every command is referenced, and those outside
// it launch nothing (raw_mme.h kIcbGate)
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                 indirectBuffer:(id<MTLBuffer>)rangeBuffer
           indirectBufferOffset:(NSUInteger)offset
{
    RMBuffer *r = [(id)rangeBuffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)rangeBuffer : nil;
    if (!r || (offset & 3) || offset > r->_raw.length || r->_raw.length - offset < 8) {
        SWITCH_REFUSE(
            "raw", "icb.range",
            "executeCommandsInBuffer:indirectBuffer: on a compute encoder: a range buffer not this device's or "
            "outside it: none run");
        return;
    }
    RMIndirectCommandBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)buffer : nil;
    [self executeKernels:buffer first:0 count:b ? b->_count : 0 range:r offset:offset];
}

- (void)executeKernels:(id<MTLIndirectCommandBuffer>)buffer
                 first:(NSUInteger)first
                 count:(NSUInteger)count
                 range:(RMBuffer *)rangeBuffer
                offset:(NSUInteger)offset
{
    RMIndirectComputeBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectComputeBuffer class]] ? (RMIndirectComputeBuffer *)buffer : nil;
    if (!b || !count || first + count > b->_count) {
        SWITCH_REFUSE(
            "raw", "icb.range",
            "executeCommandsInBuffer: on a compute encoder: a buffer of dispatches not this device's or a range "
            "past it: none run");
        return;
    }
    RMKernelExecute *x = [RMKernelExecute new];
    x->_buffer = b;
    x->_range = rangeBuffer;
    x->_rangeOffset = offset;
    x->_first = uint32_t(first);
    x->_count = uint32_t(count);
    x->_pipeline = [self rmState];
    if (const char *why = admit(x)) {
        [_cb takeReason:WhyCompute why:@(why)];
        hit(_cb, PredComputeKernel, first);
    }
    std::vector<raw::ResourceUse> uses;
    [_cb retain:b];
    [_cb retain:x];
    if (rangeBuffer) {
        [_cb retain:rangeBuffer];
        uses.push_back({(__bridge void *)rangeBuffer, raw::ResourceUse::Read});
    }
    for (RMBuffer *used : commandBuffers(x)) {
        [_cb retain:used];
        uses.push_back({(__bridge void *)used, raw::ResourceUse::Read | raw::ResourceUse::Write});
    }
    Cmd m = cmd(OpExecuteKernels, 3);
    m.obj = (__bridge void *)x;
    [_cb record:m];
    if (!uses.empty())
        _cb->_plan.addUses(_cb->_plan.size() - 1, uses);
}

@end

bool encodeKernelCommands(raw::ComputeEncoder &e, raw::CommandBuffer &rc, RMKernelExecute *x, const char **why)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    for (RMBuffer *used : commandBuffers(x)) {
        useBuffer(&rc, used, true);
        if (used->_raw.hostVisible && !used->_raw.wc && used->_raw.cpu)
            rc.clean(used->_raw.cpu, used->_raw.length);
    }
    for (const raw::Buffer &chunk : b->_chunks)
        rc.use(chunk, false);
    raw::ComputeEncoder::IcbExecute run = {&b->_raw,
                                           x->_first,
                                           x->_first,
                                           x->_count,
                                           x->_range ? &x->_range->_raw : nullptr,
                                           x->_rangeOffset,
                                           b->_gpuWritten.load(std::memory_order_relaxed)};
    raw::Buffer inherited;
    if (b->_inheritPipeline || b->_inheritBuffers) {
        if (!inheritedSlots(e, x, inherited, why))
            return false;
        run.slots = &inherited;
        run.at = 0;
        run.written = false;
    } else if (x->_range) {
        run.at = 0;
        run.first = 0;
        run.count = uint32_t(b->_count);
    }
    if (x->_range)
        useBuffer(&rc, x->_range, false);
    return e.executeSlots(run, why);
}

void replayKernelCommands(id<MTLComputeCommandEncoder> encoder, RMKernelExecute *x, std::set<RMBuffer *> &buffers)
{
    RMIndirectComputeBuffer *b = x->_buffer;
    const auto [from, to] = reach(x);
    std::lock_guard<std::mutex> hold(b->_lock);
    if (!b->_m2v)
        b->_m2v = [gRM->fallback newIndirectCommandBufferWithDescriptor:b->_descriptor
                                                        maxCommandCount:b->_count
                                                                options:0];
    if (!b->_m2v) {
        SWITCH_REFUSE("raw", "icb.replay", "m2v made no indirect command buffer of dispatches: none run");
        return;
    }
    for (uint32_t i = from; i < to; i++) {
        const RMICBDispatch &c = b->_dispatches[i];
        id<MTLIndirectComputeCommand> t = [b->_m2v indirectComputeCommandAtIndex:i];
        [t reset];
        if (!b->_inheritPipeline && c.pipeline)
            [t setComputePipelineState:twinOf(c.pipeline)];
        for (uint32_t slot = 0; slot < 31 && !b->_inheritBuffers; slot++)
            if (RMBuffer *used = c.buffers[slot]) {
                buffers.insert(used);
                if (c.strides[slot] == raw::kNoBindStride)
                    [t setKernelBuffer:twinOf(used) offset:c.offsets[slot] atIndex:slot];
                else
                    [t setKernelBuffer:twinOf(used)
                                 offset:c.offsets[slot]
                        attributeStride:c.strides[slot]
                                atIndex:slot];
            }
        for (const auto &[index, bytes] : c.memory)
            [t setThreadgroupMemoryLength:bytes atIndex:index];
        if (c.stageIn)
            [t setStageInRegion:c.stageInRegion];
        if (c.barrier)
            [t setBarrier];
        if (!c.dispatched)
            continue;
        const MTLSize grid = MTLSizeMake(c.grid[0], c.grid[1], c.grid[2]),
                      threads = MTLSizeMake(c.threads[0], c.threads[1], c.threads[2]);
        if (c.perThread)
            [t concurrentDispatchThreads:grid threadsPerThreadgroup:threads];
        else
            [t concurrentDispatchThreadgroups:grid threadsPerThreadgroup:threads];
    }
    if (x->_range) {
        buffers.insert(x->_range);
        [encoder executeCommandsInBuffer:b->_m2v indirectBuffer:twinOf(x->_range) indirectBufferOffset:x->_rangeOffset];
    } else {
        [encoder executeCommandsInBuffer:b->_m2v withRange:NSMakeRange(x->_first, x->_count)];
    }
}

void encodeKernelCommandsBlit(raw::BlitEncoder *be, const Cmd &c)
{
    RMIndirectComputeBuffer *from = (__bridge RMIndirectComputeBuffer *)c.obj;
    RMIndirectComputeBuffer *to = (__bridge RMIndirectComputeBuffer *)c.obj2;
    constexpr uint64_t kSlotBytes = raw::icb::kComputeSlotWords * 4;
    const bool copy = c.op == OpCopyCommands;
    const NSUInteger first = NSUInteger(c.c), count = NSUInteger(c.x), into = copy ? c.a : first;
    std::vector<RMICBDispatch> commands(count);
    if (copy) {
        std::lock_guard<std::mutex> hold(from->_lock);
        std::copy_n(from->_dispatches.begin() + ptrdiff_t(first), count, commands.begin());
    }
    if (be) {
        be->access(copy ? (__bridge void *)from : nullptr, (__bridge void *)to);
        if (!copy)
            be->fillWords(to->_raw.va + first * kSlotBytes, count * kSlotBytes, raw::icb::kNop);
        else
            be->copyBytes(from->_raw.va + first * kSlotBytes, to->_raw.va + into * kSlotBytes, count * kSlotBytes);
        // a copied command launches as its new index (the gate compares it with the range)
        if (copy) {
            be->barrier();
            for (NSUInteger i = 0; i < count; i++)
                if (commands[i].raw)
                    be->fillWords(to->_raw.va + (into + i) * kSlotBytes + raw::icb::kComputeIndexWord * 4, 4,
                                  uint32_t(into + i));
        }
        to->_gpuWritten.store(true, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> hold(to->_lock);
    for (NSUInteger i = 0; i < count; i++) {
        to->_dispatches[into + i] = commands[i];
        // with no copy engine (the blit encoder replayed on m2v) the words are the CPU's to write, as Metal's would be the GPU's
        if (be)
            continue;
        if (commands[i].raw)
            raw::icb::dispatch(slotOf(to, into + i), uint32_t(into + i), commands[i].words);
        else
            raw::icb::computeEmpty(slotOf(to, into + i));
    }
}
