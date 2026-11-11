// SPDX-License-Identifier: MIT
// MTLIndirectCommandBuffer of draws that inherit their pipeline and buffers (raw_icb.h): the commands are push words in
// write-combined GPU memory, written as the CPU encodes each, and a render encoder's executeCommandsInBuffer:withRange:
// runs the range by reference after one prepare of the state they inherit. Commands that set their own pipeline, buffers or
// raster state, compute dispatches and patches have no form here: such a descriptor's creation is nil.
#import "RawMetalIndirect.h"
#include "raw_icb.h"

namespace {

// A buffer's commands of triangles, and of lines or points: the two take different raster state (raw::icb::triangles)
enum PrimitiveKinds : uint8_t {
    KindTriangles = 1,
    KindOthers = 2
};

const char *unsupportedDescriptor(MTLIndirectCommandBufferDescriptor *d)
{
    if (d.commandTypes & ~NSUInteger(MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed))
        return "patches, dispatches or mesh draws";
    if (!d.inheritPipelineState || !d.inheritBuffers)
        return "commands that set their own pipeline or buffers";
    for (NSString *inherits : @[
             @"inheritDepthStencilState", @"inheritDepthBias", @"inheritCullMode", @"inheritDepthClipMode",
             @"inheritFrontFacingWinding", @"inheritTriangleFillMode"
         ])
        if ([d respondsToSelector:NSSelectorFromString(inherits)] && ![[d valueForKey:inherits] boolValue])
            return "commands that set their own raster or depth/stencil state";
    return nullptr;
}

uint32_t *slot(RMIndirectCommandBuffer *b, NSUInteger index)
{
    return reinterpret_cast<uint32_t *>(b->_raw.cpu) + index * raw::icb::kSlotWords;
}

}  // namespace

@implementation RMIndirectCommandBuffer

- (void)dealloc
{
    if (_raw.va)
        gRM->raw->destroyBuffer(_raw);
}

// Command `index` is now `command` (its words already written): the index buffers and primitive kinds of the whole buffer
- (void)take:(RMICBCommand)command at:(NSUInteger)index
{
    std::lock_guard<std::mutex> hold(_lock);
    RMICBCommand &was = _commands[index];
    if (was.indices && --_indexBuffers[(__bridge void *)was.indices] == 0)
        _indexBuffers.erase((__bridge void *)was.indices);
    if (was.kind != RMICBCommand::None)
        _kinds[raw::icb::triangles(was.primitive) ? 0 : 1]--;
    if (command.indices)
        _indexBuffers[(__bridge void *)command.indices]++;
    if (command.kind != RMICBCommand::None)
        _kinds[raw::icb::triangles(command.primitive) ? 0 : 1]++;
    was = command;
}

- (uint8_t)kinds
{
    std::lock_guard<std::mutex> hold(_lock);
    return uint8_t((_kinds[0] ? KindTriangles : 0) | (_kinds[1] ? KindOthers : 0));
}

- (std::vector<RMBuffer *>)indexBuffers
{
    std::lock_guard<std::mutex> hold(_lock);
    std::vector<RMBuffer *> out;
    for (const auto &[buffer, uses] : _indexBuffers)
        out.push_back((__bridge RMBuffer *)buffer);
    return out;
}

- (NSUInteger)size
{
    return _count;
}

- (id<MTLIndirectRenderCommand>)indirectRenderCommandAtIndex:(NSUInteger)index
{
    if (index >= _count)
        return nil;
    RMIndirectRenderCommand *c = [RMIndirectRenderCommand new];
    c->_buffer = self;
    c->_index = index;
    return (id<MTLIndirectRenderCommand>)c;
}

- (id<MTLIndirectComputeCommand>)indirectComputeCommandAtIndex:(NSUInteger)index
{
    (void)index;
    return nil;
}

- (void)resetWithRange:(NSRange)range
{
    for (NSUInteger i = range.location; i < std::min<NSUInteger>(NSMaxRange(range), _count); i++) {
        raw::icb::empty(slot(self, i));
        [self take:RMICBCommand {} at:i];
    }
}

- (MTLResourceID)gpuResourceID
{
    MTLResourceID id;
    static_assert(sizeof id == sizeof _raw.va);
    memcpy(&id, &_raw.va, sizeof id);
    return id;
}

- (NSUInteger)allocatedSize
{
    return NSUInteger(_raw.length);
}

- (MTLStorageMode)storageMode
{
    return MTLStorageModeShared;
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return MTLCPUCacheModeWriteCombined;
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return MTLHazardTrackingModeTracked;
}

- (MTLResourceOptions)resourceOptions
{
    return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    (void)state;
    return MTLPurgeableStateNonVolatile;
}

@end

@implementation RMIndirectRenderCommand

- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)baseInstance
{
    raw::icb::draw(slot(_buffer, _index), uint32_t(_index),
                   {uint32_t(type), uint32_t(count), uint32_t(instances), uint32_t(start), uint32_t(baseInstance)});
    RMICBCommand c;
    c.kind = RMICBCommand::Draw;
    c.primitive = uint8_t(type);
    c.first = uint32_t(start);
    c.count = uint32_t(count);
    c.instances = uint32_t(instances);
    c.baseInstance = uint32_t(baseInstance);
    [_buffer take:c at:_index];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
                   baseVertex:(NSInteger)baseVertex
                 baseInstance:(NSUInteger)baseInstance
{
    RMBuffer *b = [(id)buffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)buffer : nil;
    const uint32_t size = indexType == MTLIndexTypeUInt16 ? 2 : 4;
    if (!b || !b->_raw.va || offset % size || offset >= b->_raw.length) {
        SWITCH_REFUSE("raw", "icb.indices",
                      "indirect command %lu: an index buffer not this device's or off its type (offset %lu)",
                      (unsigned long)_index, (unsigned long)offset);
        [self reset];
        return;
    }
    raw::icb::indexed(slot(_buffer, _index), uint32_t(_index),
                      {uint32_t(type), size, b->_raw.va + offset, b->_raw.va + b->_raw.length - 1, uint32_t(count),
                       uint32_t(instances), 0, int32_t(baseVertex), uint32_t(baseInstance)});
    RMICBCommand c;
    c.kind = RMICBCommand::Indexed;
    c.primitive = uint8_t(type);
    c.count = uint32_t(count);
    c.instances = uint32_t(instances);
    c.baseVertex = int32_t(baseVertex);
    c.baseInstance = uint32_t(baseInstance);
    c.indexType = uint8_t(indexType);
    c.indexOffset = offset;
    c.indices = b;
    [_buffer take:c at:_index];
}

- (void)reset
{
    raw::icb::empty(slot(_buffer, _index));
    [_buffer take:RMICBCommand {} at:_index];
}

// The buffer inherits these from the encoder (its descriptor's creation said so): Metal's validation refuses them
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
    (void)state;
    SWITCH_REFUSE("raw", "icb.inherited", "an indirect command set a pipeline its buffer inherits");
}

- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    (void)buffer, (void)offset, (void)index;
    SWITCH_REFUSE("raw", "icb.inherited", "an indirect command set a buffer its buffer inherits");
}

- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    (void)buffer, (void)offset, (void)index;
    SWITCH_REFUSE("raw", "icb.inherited", "an indirect command set a buffer its buffer inherits");
}

@end

@implementation RMBlitEncoder (Indirect)

- (void)resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    RMIndirectCommandBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)buffer : nil;
    if (!b || NSMaxRange(range) > b->_count) {
        SWITCH_REFUSE("raw", "icb.range", "resetCommandsInBuffer: a buffer not this device's or a range past it");
        return;
    }
    [_cb retain:b];
    Cmd m = cmd(OpResetCommands);
    m.obj = m.obj2 = (__bridge void *)b;
    m.c = range.location;
    m.x = range.length;
    [_cb record:m];
}

- (void)copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)source
                      sourceRange:(NSRange)range
                      destination:(id<MTLIndirectCommandBuffer>)destination
                 destinationIndex:(NSUInteger)index
{
    RMIndirectCommandBuffer *from =
        [(id)source isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)source : nil;
    RMIndirectCommandBuffer *to =
        [(id)destination isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)destination : nil;
    if (!from || !to || NSMaxRange(range) > from->_count || index + range.length > to->_count) {
        SWITCH_REFUSE("raw", "icb.range", "copyIndirectCommandBuffer: a buffer not this device's or a range past one");
        return;
    }
    [_cb retain:from];
    [_cb retain:to];
    Cmd m = cmd(OpCopyCommands);
    m.obj = (__bridge void *)from;
    m.obj2 = (__bridge void *)to;
    m.c = range.location;
    m.x = range.length;
    m.a = uint32_t(index);
    [_cb record:m];
}

// Metal's optimization keeps what the commands draw: these are already the channel's words
- (void)optimizeIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    (void)buffer, (void)range;
}

@end

@interface RMDevice (Indirect)
@end
@implementation RMDevice (Indirect)

- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)d
                                                       maxCommandCount:(NSUInteger)count
                                                               options:(MTLResourceOptions)options
{
    (void)options;
    if (const char *why = unsupportedDescriptor(d)) {
        SWITCH_REFUSE_DETAIL("raw", "icb.descriptor", uintptr_t(why), "indirect command buffer of %s: nil", why);
        return nil;
    }
    RMIndirectCommandBuffer *b = [RMIndirectCommandBuffer new];
    b->_device = self;
    b->_count = count;
    b->_commands.resize(count);
    if (!count || !gRM->raw->createBuffer(uint64_t(count) * raw::icb::kSlotWords * 4, b->_raw, true)) {
        SWITCH_REFUSE("raw", "icb.memory", "no memory for an indirect command buffer of %lu commands",
                      (unsigned long)count);
        return nil;
    }
    for (NSUInteger i = 0; i < count; i++)
        raw::icb::empty(slot(b, i));
    return (id<MTLIndirectCommandBuffer>)b;
}

@end

@implementation RMRenderEncoder (Indirect)

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    [self execute:buffer first:range.location count:range.length range:nil offset:0];
}

// The range the GPU reads at `offset` of `rangeBuffer` (an MTLIndirectCommandBufferExecutionRange) selects the commands:
// every command is referenced, and those outside it draw nothing (raw_mme.h kIcbDraw)
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                 indirectBuffer:(id<MTLBuffer>)rangeBuffer
           indirectBufferOffset:(NSUInteger)offset
{
    RMBuffer *r = [(id)rangeBuffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)rangeBuffer : nil;
    if (!r || (offset & 3) || offset > r->_raw.length || r->_raw.length - offset < 8) {
        SWITCH_REFUSE("raw", "icb.range",
                      "executeCommandsInBuffer:indirectBuffer: a range buffer not this device's or "
                      "outside it: none run");
        return;
    }
    RMIndirectCommandBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)buffer : nil;
    [self execute:buffer first:0 count:b ? b->_count : 0 range:r offset:offset];
}

- (void)execute:(id<MTLIndirectCommandBuffer>)buffer
          first:(NSUInteger)first
          count:(NSUInteger)count
          range:(RMBuffer *)rangeBuffer
         offset:(NSUInteger)offset
{
    SWITCH_API(ApiDraw);
    RMIndirectCommandBuffer *b =
        [(id)buffer isKindOfClass:[RMIndirectCommandBuffer class]] ? (RMIndirectCommandBuffer *)buffer : nil;
    if (!b || !count || first + count > b->_count) {
        SWITCH_REFUSE("raw", "icb.range",
                      "executeCommandsInBuffer: a buffer not this device's or a range past it: none run");
        return;
    }
    const uint8_t kinds = [b kinds];
    if (kinds == (KindTriangles | KindOthers)) {
        [_cb takeReason:WhyEncode why:@"an indirect command buffer of triangles and lines or points"];
        hit(_cb, PredEncGeometry, 0, kinds);
    }
    const std::vector<RMBuffer *> indices = [b indexBuffers];
    [self checkDraw];
    [_cb retain:b];
    [_cb retain:rangeBuffer];
    for (RMBuffer *i : indices)
        [_cb retain:i];
    Cmd m = cmd(OpExecuteCommands, kinds & KindTriangles ? 3 : 0);
    m.obj = (__bridge void *)rangeBuffer;
    m.obj2 = (__bridge void *)b;
    m.a = !indices.empty();
    m.b = uint32_t(offset);
    m.x = first;
    m.c = count;
    [_cb record:m];
}

@end

bool encodeCommands(raw::RenderEncoder *re, raw::CommandBuffer *rc, const RMPlanDraw &draw, const char **why)
{
    RMIndirectCommandBuffer *b = (__bridge RMIndirectCommandBuffer *)draw.arguments;
    std::vector<const raw::Buffer *> indices;
    for (RMBuffer *buffer : [b indexBuffers]) {
        useBuffer(rc, buffer, false);
        indices.push_back(&buffer->_raw);
    }
    RMBuffer *range = (__bridge RMBuffer *)draw.indices;
    if (range)
        useBuffer(rc, range, false);
    return re->executeSlots({&b->_raw, uint32_t(draw.argumentOffset), uint32_t(draw.indexOffset),
                             range ? &range->_raw : nullptr, draw.first, draw.primitive, draw.indexType != 0,
                             b->_gpuWritten.load(std::memory_order_relaxed), indices.data(), indices.size()},
                            why);
}

void encodeCommandsBlit(raw::BlitEncoder *be, const Cmd &c)
{
    RMIndirectCommandBuffer *from = (__bridge RMIndirectCommandBuffer *)c.obj;
    RMIndirectCommandBuffer *to = (__bridge RMIndirectCommandBuffer *)c.obj2;
    constexpr uint64_t kSlotBytes = raw::icb::kSlotWords * 4;
    const NSUInteger first = NSUInteger(c.c), count = NSUInteger(c.x), into = c.op == OpCopyCommands ? c.a : first;
    if (be) {
        be->access(c.op == OpCopyCommands ? (__bridge void *)from : nullptr, (__bridge void *)to);
    }
    std::vector<RMICBCommand> commands(count);
    if (c.op == OpCopyCommands) {
        std::lock_guard<std::mutex> hold(from->_lock);
        std::copy_n(from->_commands.begin() + ptrdiff_t(first), count, commands.begin());
    }
    if (be) {
        if (c.op == OpResetCommands)
            be->fillWords(to->_raw.va + first * kSlotBytes, count * kSlotBytes, raw::icb::kNop);
        else
            be->copyBytes(from->_raw.va + first * kSlotBytes, to->_raw.va + into * kSlotBytes, count * kSlotBytes);
        // a copied command draws as its new index (the gated draw compares it with the range)
        if (c.op == OpCopyCommands) {
            be->barrier();
            for (NSUInteger i = 0; i < count; i++)
                if (commands[i].kind != RMICBCommand::None)
                    be->fillWords(to->_raw.va + (into + i) * kSlotBytes + raw::icb::kIndexWord * 4, 4,
                                  uint32_t(into + i));
        }
        to->_gpuWritten.store(true, std::memory_order_relaxed);
    }
    for (NSUInteger i = 0; i < count; i++)
        [to take:commands[i] at:into + i];
}

void replayCommands(id<MTLRenderCommandEncoder> encoder, const RMPlanDraw &draw, const std::function<id(void *)> &use)
{
    RMIndirectCommandBuffer *b = (__bridge RMIndirectCommandBuffer *)draw.arguments;
    if (draw.indices) {
        SWITCH_REFUSE("raw", "icb.replay",
                      "an execute of a range the GPU reads, replayed on m2v: the CPU reads no GPU range, none drawn");
        return;
    }
    std::vector<RMICBCommand> commands;
    {
        std::lock_guard<std::mutex> hold(b->_lock);
        commands.assign(b->_commands.begin() + ptrdiff_t(draw.argumentOffset),
                        b->_commands.begin() + ptrdiff_t(draw.argumentOffset + draw.indexOffset));
    }
    for (const RMICBCommand &c : commands)
        if (c.kind == RMICBCommand::Draw)
            [encoder drawPrimitives:MTLPrimitiveType(c.primitive)
                        vertexStart:c.first
                        vertexCount:c.count
                      instanceCount:c.instances
                       baseInstance:c.baseInstance];
        else if (c.kind == RMICBCommand::Indexed)
            [encoder drawIndexedPrimitives:MTLPrimitiveType(c.primitive)
                                indexCount:c.count
                                 indexType:MTLIndexType(c.indexType)
                               indexBuffer:use((__bridge void *)c.indices)
                         indexBufferOffset:c.indexOffset
                             instanceCount:c.instances
                                baseVertex:c.baseVertex
                              baseInstance:c.baseInstance];
}

// Every draw a command of a buffer makes takes the encoder's pipeline (the buffer inherits it), so any render state draws
// one; no compute buffer is made (its descriptor's creation is nil), so no compute state runs from one
@implementation RMPipeline (Indirect)

- (BOOL)supportIndirectCommandBuffers
{
    return YES;
}

@end

@implementation RMComputeEncoder (Indirect)

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range
{
    (void)buffer, (void)range;
    SWITCH_REFUSE("raw", "icb.compute",
                  "executeCommandsInBuffer: on a compute encoder: no buffer of dispatches is made");
}

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                 indirectBuffer:(id<MTLBuffer>)range
           indirectBufferOffset:(NSUInteger)offset
{
    (void)buffer, (void)range, (void)offset;
    SWITCH_REFUSE("raw", "icb.compute",
                  "executeCommandsInBuffer: on a compute encoder: no buffer of dispatches is made");
}

@end
