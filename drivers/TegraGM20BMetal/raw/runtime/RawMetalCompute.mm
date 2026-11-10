// SPDX-License-Identifier: MIT
// The facade's compute encoders (raw_compute.h): a dispatch the pack holds the kernel of (raw_compute_key.h) runs on the raw
// channel; any other one is the command buffer's reason for m2v, which replays the encoder's commands. A compute state is
// m2v's with the key's parts its function gives: its AIR, its constants and the descriptor's limits.
#import "RawMetalInternal.h"
#include "NativeBridgeMetalFrontend.h"
#include "RawComplete.h"
#include "RawDirect.h"
#include "raw_compute.h"
#include "raw_compute_key.h"
#include "raw_compute_lookup.h"
#include "raw_metallib.h"
#include "raw_view.h"
#include "raw_surface_view.h"

using RMKernelResult = raw::ComputeKernelResult<raw::ComputeState>;

@interface RMComputeState : RMTwinObject {
@public
    bool _keyed;  // its function's AIR is known: the pack may hold its kernels
    raw::AirHash _air;
    std::string _constants;
    uint32_t _maxThreads;
    bool _multipleOfWidth;
    raw::ComputeKernelCache<raw::ComputeState> _kernels;
    MTLComputePipelineDescriptor *_direct;  // over m2v's functions: what a kernel the pack lacks is made from
}
@end

@implementation RMComputeState

// What NAK made of the kernel, as the m2v state it carries compiled it
- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return [(id<MTLComputePipelineState>)_twin maxTotalThreadsPerThreadgroup];
}

- (NSUInteger)threadExecutionWidth
{
    return [(id<MTLComputePipelineState>)_twin threadExecutionWidth];
}

- (NSUInteger)staticThreadgroupMemoryLength
{
    return [(id<MTLComputePipelineState>)_twin staticThreadgroupMemoryLength];
}

- (RMKernelResult)kernel:(const raw::ComputeSpecialization &)specialization
                    make:(RMKernelResult (^)(MTLComputePipelineDescriptor *, const std::string &,
                                             const raw::ComputeBindingText &))make
{
    if (!_keyed)
        return {};
    return _kernels.get(specialization, [&] {
        const raw::ComputeBindingText text = specialization.text();
        const std::string key =
            raw::computeKey(_air, _constants, _maxThreads, _multipleOfWidth, specialization.threads.data(), text.memory,
                            text.textures, text.samplers);
        if (const raw::ComputeState *state = raw::computeState(*gRM->raw, raw::computeKeyHash(key)))
            return RMKernelResult{state, false};
        return make(_direct, key, text);
    });
}

@end

static bool airOf(id<MTLFunction> function, raw::AirHash &out)
{
    const void *bytes = nullptr;
    size_t length = 0;
    std::map<std::string, raw::AirHash> hashes;
    if (!native_bridge_metal_function_metallib(twinOf(function), &bytes, &length) ||
        !raw::metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes))
        return false;
    const auto f = hashes.find(native_bridge_metal_function_library_name(twinOf(function)).UTF8String ?: "");
    if (f == hashes.end())
        return false;
    out = f->second;
    return true;
}

id rmComputeState(id device, id twin, id<MTLFunction> function, MTLComputePipelineDescriptor *descriptor)
{
    if (!twin)
        return nil;
    RMComputeState *s = [RMComputeState new];
    s->_device = device;
    s->_twin = twin;
    RMFunction *f = [(id)function isKindOfClass:[RMFunction class]] ? (RMFunction *)function : nil;
    // linked functions are no part of the key: such a state's dispatches stay m2v's
    s->_keyed = f && !descriptor.linkedFunctions.functions.count &&
                !descriptor.linkedFunctions.privateFunctions.count && airOf(function, s->_air);
    s->_constants = f ? f->_constants : "";
    s->_maxThreads = uint32_t(descriptor.maxTotalThreadsPerThreadgroup);
    s->_multipleOfWidth = descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    s->_direct = [descriptor copy];
    s->_direct.computeFunction = twinOf(function);
    s.label = descriptor.label;
    return s;
}

@interface RMComputeEncoder () {
    RMComputeState *_state;
    raw::ComputeSpecialization _specialization;
    RMKernelResult _kernel;
    bool _kernelCurrent;
    // what the dispatches' kernels may read (unretained: the command buffer retains them): the textures, and the buffers'
    // addresses (0: setBytes:, which the raw path uploads aligned)
    RMTexture *_textures[31];
    RMSampler *_samplers[16];
    RMBuffer *_buffers[31];
    uint64_t _offsets[31];
    std::vector<void *> _explicitResources;
}

@end

@implementation RMComputeEncoder

+ (Class)twinClass
{
    return NSClassFromString(@"NBComputeEncoder");
}

- (MTLDispatchType)dispatchType
{
    return _type;
}

- (void)refuse:(RMPred)pred value:(uint64_t)a why:(NSString *)why
{
    [_cb takeReason:WhyCompute why:why];
    hit(_cb, pred, a);
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
    RMComputeState *next = [(id)state isKindOfClass:[RMComputeState class]] ? (RMComputeState *)state : nil;
    if (_state != next) {
        _state = next;
        _kernelCurrent = false;
    }
    setBinding(_cb, OpKernelState, 3, 0, state, 0);
}

- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    if (index < 31)
        _buffers[index] = nil;
    recordBytes(_cb, cmd(OpKernelBytes, 3, uint16_t(index)), bytes, length);
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (buffer && ![(id)buffer isKindOfClass:[RMBuffer class]])
        [self refuse:PredCompute value:index why:@"a kernel's buffer not made by this device"];
    if (index < 31) {
        _buffers[index] = [(id)buffer isKindOfClass:[RMBuffer class]] ? (RMBuffer *)buffer : nil;
        _offsets[index] = offset;
    }
    setBinding(_cb, OpKernelBuffer, 3, index, buffer, offset);
}

- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (index < 31)
        _offsets[index] = offset;
    Cmd m = cmd(OpKernelBufferOffset, 3, uint16_t(index));
    m.c = offset;
    [_cb record:m];
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])buffers
           offsets:(const NSUInteger[])offsets
         withRange:(NSRange)range
{
    for (NSUInteger k = 0; k < range.length; k++)
        [self setBuffer:buffers[k] offset:offsets[k] atIndex:range.location + k];
}

- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    RMTexture *t = [(id)texture isKindOfClass:[RMTexture class]] ? (RMTexture *)texture : nil;
    if (index < 31 && _textures[index] != t) {
        _textures[index] = t;
        raw::BoundTexture bound = {};
        if (t) {
            MTLTextureDescriptor *d = t->_descriptor;
            const MTLTextureSwizzleChannels w = d.swizzle;
            bound = {uint32_t(index),    d.pixelFormat, d.textureType,
                     d.mipmapLevelCount, d.arrayLength, {w.red, w.green, w.blue, w.alpha}};
        }
        if (_specialization.bindTexture(uint32_t(index), t ? &bound : nullptr))
            _kernelCurrent = false;
    }
    if (texture && !t)
        [self refuse:PredCompute value:index why:@"a kernel's texture not made by this device"];
    setBinding(_cb, OpKernelTexture, 3, index, texture, 0);
}

- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])textures withRange:(NSRange)range
{
    for (NSUInteger k = 0; k < range.length; k++)
        [self setTexture:textures[k] atIndex:range.location + k];
}

- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    if (sampler && ![(id)sampler isKindOfClass:[RMSampler class]])
        [self refuse:PredCompute value:index why:@"a kernel's sampler not made by this device"];
    RMSampler *s = [(id)sampler isKindOfClass:[RMSampler class]] ? (RMSampler *)sampler : nil;
    if (index < 16 && _samplers[index] != s) {
        _samplers[index] = s;
        raw::BoundSampler bound = {};
        if (s) {
            MTLSamplerDescriptor *d = s->_descriptor;
            bound = {uint32_t(index),
                     d.minFilter,
                     d.magFilter,
                     d.mipFilter,
                     d.sAddressMode,
                     d.tAddressMode,
                     d.rAddressMode,
                     d.maxAnisotropy,
                     d.compareFunction,
                     d.borderColor,
                     d.lodMinClamp,
                     d.lodMaxClamp,
                     bool(d.normalizedCoordinates),
                     bool(d.lodAverage)};
        }
        if (_specialization.bindSampler(uint32_t(index), s ? &bound : nullptr))
            _kernelCurrent = false;
    }
    setBinding(_cb, OpKernelSampler, 3, index, sampler, 0);
}

- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])samplers withRange:(NSRange)range
{
    for (NSUInteger k = 0; k < range.length; k++)
        [self setSamplerState:samplers[k] atIndex:range.location + k];
}

- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
    if (_specialization.bindMemory(uint32_t(index), uint32_t((length + 15) & ~NSUInteger(15))))
        _kernelCurrent = false;
    Cmd m = cmd(OpKernelMemory, 3, uint16_t(index));
    m.a = uint32_t(length);
    [_cb record:m];
}

- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    setBinding(_cb, OpKernelUse, 3, 0, resource, usage);
    void *object = (__bridge void *)resource;
    if (object && std::find(_explicitResources.begin(), _explicitResources.end(), object) == _explicitResources.end())
        _explicitResources.push_back(object);
}

- (void)useResources:(const id<MTLResource> __unsafe_unretained[])resources
               count:(NSUInteger)count
               usage:(MTLResourceUsage)usage
{
    for (NSUInteger k = 0; k < count; k++)
        [self useResource:resources[k] usage:usage];
}

- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
{
    (void)scope;
    [_cb record:cmd(OpKernelBarrier, 3)];
}

- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained[])resources count:(NSUInteger)count
{
    (void)resources;
    (void)count;
    [_cb record:cmd(OpKernelBarrier, 3)];
}

// A texture a raw kernel samples as it is: no image of its own the raw path would refresh first (a chain's levels, planes
// converted, an extended-range stand-in, a shadow of rows the texture header cannot take)
static bool plainTexture(RMTexture *t)
{
    const raw::Texture &x = t->_raw;
    return t->_rawOk && x.tic && !x.chain && !x.converted() && !x.standIn && (!x.sampled || x.surfaceView) &&
           !x.memoryless;
}

// The kernel the pack lacks, of `descriptor` at `key`, made for this dispatch (RawComplete.h), and journaled as m2v journals a
// dispatch it runs, so the next pack holds it
- (RMKernelResult)make:(MTLComputePipelineDescriptor *)descriptor
                   key:(const std::string &)key
                  text:(const raw::ComputeBindingText &)text
                  grid:(MTLSize)grid
               threads:(MTLSize)threads
             perThread:(bool)perThread
{
    if (!gRM->completion)
        return {nullptr, true};
    const uint32_t t[3] = {uint32_t(threads.width), uint32_t(threads.height), uint32_t(threads.depth)};
    const std::map<uint32_t, uint32_t> memory(_specialization.memory.begin(), _specialization.memory.end());
    raw::KernelDispatch ask(t, memory);
    for (uint32_t slot = 0; slot < 31; slot++)
        if (_textures[slot])
            ask.texture(slot, _textures[slot]->_descriptor);
    for (uint32_t slot = 0; slot < 16; slot++)
        if (_samplers[slot])
            ask.sampler(slot, _samplers[slot]->_descriptor);
    bool retry = false;
    const raw::ComputeState *k = gRM->completion->kernel(descriptor, ask.dispatch(), key, &retry);
    if (k)
        native_bridge_metal_journal_dispatch(_state->_twin, grid, threads, perThread, text.memory.c_str(),
                                             text.textures.c_str(), text.samplers.c_str());
    return {k, retry};
}

- (void)dispatch:(MTLSize)grid threads:(MTLSize)threads perThread:(bool)perThread
{
    const std::array<uint32_t, 3> t = {uint32_t(threads.width), uint32_t(threads.height), uint32_t(threads.depth)};
    const raw::ComputeDispatch dispatch = raw::computeDispatch({grid.width, grid.height, grid.depth}, t, perThread);
    const auto &groups = dispatch.groups;
    bool refused = dispatch.reason != raw::ComputeDispatch::Ready;
    if (dispatch.reason == raw::ComputeDispatch::Edge)
        [self refuse:PredComputeEdge
               value:groups[dispatch.axis]
                 why:@"dispatchThreads: of a grid with an edge threadgroup"];
    else if (dispatch.reason == raw::ComputeDispatch::Range)
        [self refuse:PredComputeKernel value:groups[0] why:@"a dispatch whose threadgroups exceed the raw grid limits"];
    const raw::ComputeState *k = nullptr;
    uint32_t surfaceSlots = 0;
    bool lookedUp = false;
    if (!refused && _state) {
        if (_specialization.threads != t) {
            _specialization.threads = t;
            _kernelCurrent = false;
        }
        if (!_kernelCurrent) {
            lookedUp = true;
            _kernel = [_state kernel:_specialization
                                make:^RMKernelResult(MTLComputePipelineDescriptor *descriptor, const std::string &key,
                                                     const raw::ComputeBindingText &text) {
                                    return [self make:descriptor
                                                  key:key
                                                 text:text
                                                 grid:grid
                                              threads:threads
                                            perThread:perThread];
                                }];
            _kernelCurrent = !_kernel.retry;
        }
        k = _kernel.state;
    }
    if (lookedUp && !k) {
        const raw::ComputeBindingText text = _specialization.text();
        SWITCH_REFUSE_DETAIL("raw", "compute.kernel", uint64_t(t[0]) << 32 | t[1] << 16 | t[2],
                             "a kernel the pack lacks (%s, threads %u,%u,%u, threadgroup memory %s, textures %s)",
                             _state.label.UTF8String ?: "-", t[0], t[1], t[2], text.memory.c_str(),
                             text.textures.c_str());
    }
    for (uint32_t e = 0; k && e < k->rec->entries; e++) {
        const raw::PackKernelEntry &x = k->rec->entry[e];
        RMTexture *shadow = x.kind == raw::KernelTexture && x.slot < 31 ? _textures[x.slot] : nil;
        if (shadow && shadow->_raw.surfaceView) {
            surfaceSlots |= 1u << x.slot;
            bool aliases = false;
            const auto *backing = shadow->_raw.import;
            const auto shares = [&](const raw::Import *other) {
                return other && raw::surfaceBackingAliases(backing->id, backing->base, backing->bytes, other->id,
                                                           other->base, other->bytes);
            };
            for (uint32_t other = 0; other < k->rec->entries; ++other) {
                const auto &entry = k->rec->entry[other];
                if (entry.slot >= 31)
                    continue;
                RMTexture *texture = entry.kind == raw::KernelTexture ? _textures[entry.slot] : nil;
                RMBuffer *buffer = entry.kind == raw::KernelBuffer ? _buffers[entry.slot] : nil;
                aliases |= (texture && texture != shadow && shares(texture->_raw.import)) ||
                           (buffer && shares(buffer->_raw.import));
            }
            for (void *value : _explicitResources) {
                id resource = (__bridge id)value;
                if ([resource isKindOfClass:[RMTexture class]])
                    aliases |= shares(((RMTexture *)resource)->_raw.import);
                else if (RMBuffer *buffer = cpuBufferOfResource(resource))
                    aliases |= shares(buffer->_raw.import);
            }
            if (aliases) {
                [self refuse:PredComputeImage
                       value:x.slot
                         why:@"a cropped surface view aliases another dispatch resource"];
                k = nullptr;
                refused = true;
                break;
            }
        }
        if (x.kind == raw::KernelTexture && x.slot < 31 && _textures[x.slot] && !plainTexture(_textures[x.slot])) {
            [self refuse:PredComputeImage
                   value:x.slot
                     why:@"a kernel's texture the raw path samples through an image of its own"];
            k = nullptr;
            refused = true;
        }
        RMTexture *viewed = x.kind == raw::KernelTexture && x.slot < 31 ? _textures[x.slot] : nil;
        const raw::FormatInfo *f = viewed ? raw::formatInfo(viewed->_raw.format) : nullptr;
        if (f && raw::slotView(x.window, f->tic0, raw::swizzledWord0(f->tic0, viewed->_raw.swizzle), f->bpp,
                               raw::blockCompressed(f)) == raw::SlotView::None) {
            [self refuse:PredComputeView
                   value:x.slot
                     why:@"a kernel m2v binds a view of another texel size at a texture slot"];
            k = nullptr;
            refused = true;
        }
        // m2v's descriptor starts at the bind offset rounded down to 16, the rest in a push word the record names
        if (x.kind == raw::KernelBuffer && raw::kernelResidualWord(x) < 0 && x.slot < 31 && _buffers[x.slot] &&
            ((_buffers[x.slot]->_raw.va + _offsets[x.slot]) & 15)) {
            [self refuse:PredComputeOff16
                   value:x.slot
                     why:@"a kernel's buffer bound at an offset that is not a multiple of 16"];
            k = nullptr;
            refused = true;
        }
    }
    if (!refused && !k) {
        [self refuse:PredComputeKernel value:groups[0] why:@"a dispatch the raw path has no kernel for"];
        k = nullptr;
    }
    // extras: Metal's grid (threadgroups, or threads for dispatchThreads:), then the threadgroups the raw path dispatches
    Cmd m = cmd(OpDispatch, 3, perThread);
    m.a = surfaceSlots;
    m.c = uint64_t(t[0]) | uint64_t(t[1]) << 16 | uint64_t(t[2]) << 32;
    m.x = _cb->_extra.size();
    for (uint64_t v :
         {uint64_t(grid.width), uint64_t(grid.height), uint64_t(grid.depth), groups[0], groups[1], groups[2]})
        _cb->_extra.push_back(v);
    m.obj2 = const_cast<raw::ComputeState *>(k);
    [_cb record:m];
}

- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads
{
    [self dispatch:groups threads:threads perThread:false];
}

- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)threads
{
    [self dispatch:grid threads:threads perThread:true];
}

@end

bool isKernelCommand(Op op)
{
    return op >= OpKernelState && op <= OpDispatch;
}

void replayKernel(id encoder, RMCommandBuffer *cb, const Cmd &c, std::set<RMBuffer *> &buffers)
{
    id<MTLComputeCommandEncoder> e = encoder;
    id object = (__bridge id)c.obj;
    switch (c.op) {
    case OpKernelState:
        [e setComputePipelineState:twinOf(object)];
        break;
    case OpKernelBytes:
        [e setBytes:cb->_bytes.chunks[c.c].cpu + c.a length:c.b atIndex:c.index];
        break;
    case OpKernelBuffer:
        if ([object isKindOfClass:[RMBuffer class]])
            buffers.insert((RMBuffer *)object);
        [e setBuffer:twinOf(object) offset:c.c atIndex:c.index];
        break;
    case OpKernelBufferOffset:
        [e setBufferOffset:c.c atIndex:c.index];
        break;
    case OpKernelTexture:
        [e setTexture:twinOf(object) atIndex:c.index];
        break;
    case OpKernelSampler:
        [e setSamplerState:twinOf(object) atIndex:c.index];
        break;
    case OpKernelMemory:
        [e setThreadgroupMemoryLength:c.a atIndex:c.index];
        break;
    case OpKernelUse:
        if ([object isKindOfClass:[RMBuffer class]])
            buffers.insert((RMBuffer *)object);
        [e useResource:twinOf(object) usage:MTLResourceUsage(c.c)];
        break;
    case OpKernelBarrier:
        [e memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
        break;
    case OpDispatch: {
        const MTLSize grid = MTLSizeMake(extra(cb, c.x), extra(cb, c.x + 1), extra(cb, c.x + 2)),
                      threads = MTLSizeMake(c.c & 0xffff, (c.c >> 16) & 0xffff, c.c >> 32);
        if (c.index)
            [e dispatchThreads:grid threadsPerThreadgroup:threads];
        else
            [e dispatchThreadgroups:grid threadsPerThreadgroup:threads];
        break;
    }
    default:
        break;
    }
}

bool encodeKernel(raw::ComputeEncoder &e, raw::CommandBuffer &rc, RMCommandBuffer *cb, const Cmd &c, const char **why)
{
    id object = (__bridge id)c.obj;
    switch (c.op) {
    case OpKernelBytes: {
        const raw::Buffer &k = cb->_bytes.chunks[c.c];
        if (!k.wc)
            rc.clean(k.cpu + c.a, std::max<uint32_t>((c.b + 255) & ~255u, 256));
        e.setBytesAt(c.index, k.va + c.a, c.b, &k);
        break;
    }
    case OpKernelBuffer: {
        RMBuffer *b = (RMBuffer *)object;
        if (b)
            rc.use(b->_raw, true);
        e.setBuffer(c.index, b ? &b->_raw : nullptr, c.c);
        break;
    }
    case OpKernelBufferOffset:
        e.setBufferOffset(c.index, c.c);
        break;
    case OpKernelTexture: {
        RMTexture *t = (RMTexture *)object;
        if (t)
            rc.use(t->_raw, true);
        e.setTexture(c.index, t ? &t->_raw : nullptr);
        break;
    }
    case OpKernelSampler:
        e.setSampler(c.index, object ? &((RMSampler *)object)->_raw : nullptr);
        break;
    case OpKernelBarrier:
        e.barrier();
        break;
    case OpDispatch: {
        const uint32_t groups[3] = {uint32_t(extra(cb, c.x + 3)), uint32_t(extra(cb, c.x + 4)),
                                    uint32_t(extra(cb, c.x + 5))},
                       threads[3] = {uint32_t(c.c & 0xffff), uint32_t(c.c >> 16) & 0xffff, uint32_t(c.c >> 32)};
        e.setState(static_cast<const raw::ComputeState *>(c.obj2));
        return e.dispatch(groups, threads, why);
    }
    default:  // the state and threadgroup memory are the dispatch's kernel's; residency is every buffer's already
        break;
    }
    return true;
}
