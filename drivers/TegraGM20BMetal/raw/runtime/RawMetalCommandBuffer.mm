// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_execution_plan.h"
#include "raw_compute.h"
#include "raw_scheduled_wait.h"

// ---- the device's submission machinery ----

// A finished command buffer's recording storage, kept for the next one (its capacity a frame's): Core Animation's steady frames
// allocate none of it
struct RMRecording {
    RMCommandPlan plan;
    std::vector<RMPass> passes;
    RMBytes bytes;
    std::vector<uint64_t> extra;
    std::vector<id> owners;
    std::unordered_set<void *> ownerKeys;
};

static std::mutex gRecordingsLock;
static std::vector<RMRecording> gRecordings;

static void takeRecording(RMCommandBuffer *b)
{
    {
        std::lock_guard<std::mutex> l(gRecordingsLock);
        if (!gRecordings.empty()) {
            RMRecording &r = gRecordings.back();
            std::swap(b->_plan, r.plan);
            b->_passes.swap(r.passes);
            std::swap(b->_bytes, r.bytes);
            b->_extra.swap(r.extra);
            b->_owners.swap(r.owners);
            b->_ownerKeys.swap(r.ownerKeys);
            gRecordings.pop_back();
            return;
        }
    }
    b->_owners.reserve(256);
}

static void dropBytes(RMBytes &s)
{
    for (raw::Buffer &k : s.chunks)
        if (k.va)
            gRM->raw->destroyBuffer(k);
        else
            free(k.cpu);
    s = {};
}

static void giveRecording(RMCommandBuffer *b)
{
    constexpr size_t kKept = 4,
                     kKeptBytes = 1 << 20;  // (a command buffer of a few megabytes of setBytes: its own, freed)
    uint64_t bytes = 0;
    for (const raw::Buffer &k : b->_bytes.chunks)
        bytes += k.va ? k.length : kKeptBytes;  // (heap chunks are not kept)
    if (bytes + b->_plan.storageBytes() + b->_extra.capacity() * 8 > kKeptBytes) {
        dropBytes(b->_bytes);
        return;
    }
    RMRecording r;
    std::swap(r.plan, b->_plan);
    r.passes.swap(b->_passes);
    std::swap(r.bytes, b->_bytes);
    r.extra.swap(b->_extra);
    r.owners.swap(b->_owners);
    r.ownerKeys.swap(b->_ownerKeys);
    r.plan.clear();
    r.passes.clear();
    r.bytes.at = 0;
    r.bytes.used = 0;
    r.extra.clear();
    r.owners.clear();
    r.ownerKeys.clear();
    {
        std::lock_guard<std::mutex> l(gRecordingsLock);
        if (gRecordings.size() < kKept) {
            gRecordings.push_back(std::move(r));
            return;
        }
    }
    dropBytes(r.bytes);
}

@implementation RMCommandBuffer

- (void)dealloc
{
    giveRecording(self);
}

- (void)retain:(id)object
{
    if (!object || !_ownerKeys.insert((__bridge void *)object).second)
        return;
    _owners.push_back(object);
    RMResourceFacts facts;
    facts.cpuBuffer = (__bridge void *)cpuBufferOfResource(object);
    if ([object isKindOfClass:[RMBuffer class]])
        facts.kind = RMResourceKind::Buffer;
    else if ([object isKindOfClass:[RMTexture class]]) {
        facts.kind = RMResourceKind::Texture;
        RMTexture *texture = (RMTexture *)object;
        facts.roles = uint8_t((texture->_raw.converted() ? RMResourceFacts::Converted : 0) |
                              (texture->_raw.planarStandIn ? RMResourceFacts::PlanarTarget : 0));
        __unsafe_unretained RMTexture *root = texture;
        while (root->_kind == RMTextureView && root->_parent)
            root = root->_parent;
        facts.surfaceWrites = root->_surfaceWrites;
    } else if ([object isKindOfClass:[RMSampler class]])
        facts.kind = RMResourceKind::Sampler;
    _plan.registerResource((__bridge void *)object, facts);
}

- (void)record:(const Cmd &)cmd
{
    if (_plan.record(cmd, _extra)) {
        std::vector<raw::ResourceUse> uses;
        if (isPlanDraw(cmd.op)) {
            const uint32_t index = _plan.draw(_plan.size() - 1).state;
            const auto &state = _plan.renderStates[index];
            if (!state.usesFinal) {
                const auto &raster = _plan.raster[state.raster];
                RMPipeline *pipeline = (__bridge RMPipeline *)raster.pipeline;
                const raw::Pipeline *native = static_cast<const raw::Pipeline *>(raster.nativePipeline);
                if (!native && [(id)pipeline isKindOfClass:[RMPipeline class]])
                    native = pipeline->_raw;
                if (native && native->rec) {
                    _plan.resolveRenderUses(index, native, [&] {
                        std::vector<raw::ResourceUse> bound;
                        const auto &record = *native->rec;
                        for (uint32_t i = 0; i < record.entries; ++i) {
                            const auto &entry = record.entry[i];
                            if (entry.kind == raw::EntryUbo && entry.stage >= 1 && entry.stage <= 2 && entry.slot < 31) {
                                const auto &binding =
                                    _plan.buffers[entry.stage - 1][state.buffers[entry.stage - 1]][entry.slot];
                                if (binding.source == raw::BufferBinding::Source::Buffer)
                                    bound.push_back({binding.object,
                                                    uint8_t(raw::ResourceUse::Read |
                                                            (raw::bufferStorage(entry) ? raw::ResourceUse::Write : 0))});
                            } else if ((entry.kind == raw::EntryTexture || entry.kind == raw::EntryTexture3d) &&
                                       entry.slot < 31) {
                                bound.push_back({_plan.textures[1][state.textures[1]].objects[entry.slot],
                                                raw::ResourceUse::Read | raw::ResourceUse::Sample});
                            }
                            if ((entry.kind == raw::EntryTexture || entry.kind == raw::EntryTexture3d ||
                                 entry.kind == raw::EntrySamplerOnly) &&
                                entry.samplerKind == raw::SamplerSlot && entry.sampler < 16)
                                bound.push_back(
                                    {_plan.samplers[1][state.samplers[1]].objects[entry.sampler], raw::ResourceUse::Read});
                        }
                        if (record.vertexSlot < 31 && record.vertexStride) {
                            const auto &binding = _plan.buffers[0][state.buffers[0]][record.vertexSlot];
                            if (binding.source == raw::BufferBinding::Source::Buffer)
                                bound.push_back({binding.object, raw::ResourceUse::Read});
                        }
                        return bound;
                    });
                }
            }
        } else if (cmd.op == OpDispatch && cmd.obj2) {
            const char *why = nullptr;
            if (!admitComputeDispatch(self, _plan.size() - 1,
                                       static_cast<const raw::ComputeKernelSet<raw::ComputeState> *>(cmd.obj2), &why))
                [self takeReason:WhyCompute why:[NSString stringWithUTF8String:why]];
        } else if (cmd.op == OpRender) {
            const RMPass &pass = _passes[cmd.x];
            for (const RMPass::Color &a : pass.color)
                if (a.texture)
                    uses.push_back({(__bridge void *)a.texture,
                                    uint8_t(raw::ResourceUse::Write |
                                            (a.load == MTLLoadActionLoad ? raw::ResourceUse::Read : 0))});
            if (pass.depth)
                uses.push_back({(__bridge void *)pass.depth,
                                uint8_t(raw::ResourceUse::Write |
                                        (pass.depthLoad == MTLLoadActionLoad || pass.stencilLoad == MTLLoadActionLoad
                                             ? raw::ResourceUse::Read
                                             : 0))});
            for (const RMPass::Color &a : pass.color)
                if (a.resolve)
                    uses.push_back({(__bridge void *)a.resolve, raw::ResourceUse::Write});
        } else if (cmd.op == OpInvocation) {
            raw::forEachObject((__bridge NSInvocation *)cmd.obj, [&](id object) {
                uses.push_back({(__bridge void *)object, raw::ResourceUse::Read | raw::ResourceUse::Write});
                [self retain:object];
            });
        }
        if (!uses.empty())
            _plan.addUses(_plan.size() - 1, uses);
        if (cmd.op == OpEnd && _encOpen)
            _plan.prepareRange(_encAt, _plan.size());
    }
    const size_t consumed = cmd.op == OpViewport || cmd.op == OpDispatch       ? 6
                            : cmd.op == OpBlendColor                           ? 4
                            : cmd.op == OpDepthBias || cmd.op == OpDrawIndexed ? 3
                                                                               : 0;
    if (consumed && cmd.x + consumed == _extra.size())
        _extra.resize(cmd.x);
}

// The encoder recorded last: whether it goes to m2v, into its first command
- (void)closeEncoder
{
    if (_encOpen && _encAt < _plan.size())
        _plan.setRefused(_encAt, _encRefused);
    _encOpen = false;
}

- (void)takeReason:(RMReason)reason why:(NSString *)why
{
    if (_active)
        _encRefused = true;  // this encoder goes to m2v
    else
        _whole = true;
    if (_reason != WhyRaw)
        return;
    _reason = reason;
    _why = why;
}

// A refusal of the command buffer's own (an event m2v must see, a call on the command buffer): the whole of it goes to m2v
- (void)takeWholeReason:(RMReason)reason why:(NSString *)why
{
    _whole = true;
    [self takeReason:reason why:why];
}

// Answered here, not recorded as invocations (each one sent the whole command buffer to m2v)
- (NSMutableDictionary *)userDictionary
{
    std::lock_guard<std::mutex> l(_lock);
    if (!_userDictionary)
        _userDictionary = [NSMutableDictionary dictionary];
    return _userDictionary;
}

- (uint64_t)globalTraceObjectID
{
    std::lock_guard<std::mutex> l(_lock);
    if (!_traceId)
        _traceId = nextTraceId();
    return _traceId;
}

- (void)useResidencySet:(id)set
{
    _residency.add(&set, 1);
}

// (QuartzCore 27, per command buffer: the kernel charges GPU time to the task that submits it, this one)
- (void)setResponsibleTaskIDs:(const uint32_t *)taskIDs count:(uint32_t)count
{
    (void)taskIDs;
    (void)count;
}

- (void)useResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    _residency.add(sets, count);
}

- (id)rmTwin
{
    return nil;
}

- (MTLCommandBufferStatus)status
{
    std::lock_guard<std::mutex> l(_lock);
    return _status;
}

- (NSError *)error
{
    return _error;
}

- (id<MTLCommandQueue>)commandQueue
{
    return (id<MTLCommandQueue>)_queue;
}

- (BOOL)retainedReferences
{
    return YES;
}

- (MTLCommandBufferErrorOption)errorOptions
{
    return MTLCommandBufferErrorOptionNone;
}

- (id<MTLLogContainer>)logs
{
    return nil;
}

- (CFTimeInterval)GPUStartTime
{
    return _gpuStart;
}

- (CFTimeInterval)GPUEndTime
{
    return _gpuEnd;
}

- (CFTimeInterval)kernelStartTime
{
    return _kernelStart;
}

- (CFTimeInterval)kernelEndTime
{
    return _kernelEnd;
}

// SPI Core Animation calls on every command buffer: content protection does not exist on this device (no protected surfaces,
// no secure display), so the options are kept for the getter and change nothing
- (uint64_t)protectionOptions
{
    return _protection;
}

- (void)setProtectionOptions:(uint64_t)options
{
    _protection = options;
}

- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained *)groups count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;  // no cache hints on this GPU
}

- (void)encodeCacheHintFinalize:(uint64_t)tag
                 resourceGroups:(const id __unsafe_unretained *)groups
                          count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}

- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}

- (void)popDebugGroup
{}

- (void)addScheduledHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    {
        std::lock_guard<std::mutex> l(_lock);
        // once scheduled() took the handlers, one added is run behind them (was: until they ran, it joined a list no one read
        // again, and a presentDrawable added then never presented)
        if (!_scheduledSent) {
            if (!_scheduledHandlers)
                _scheduledHandlers = [NSMutableArray array];
            [_scheduledHandlers addObject:[block copy]];
        } else {
            _handlersQueued = true;
            dispatch_async(_queue->_handlers, ^{
                block((id<MTLCommandBuffer>)self);
            });
        }
    }
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    bool now;
    {
        std::lock_guard<std::mutex> l(_lock);
        now = _completedSent;  // as addScheduledHandler: once finish() took the handlers
        if (!now) {
            if (!_completedHandlers)
                _completedHandlers = [NSMutableArray array];
            [_completedHandlers addObject:[block copy]];
        }
    }
    if (now)
        block((id<MTLCommandBuffer>)self);
}

- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable present];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)t
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable presentAtTime:t];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)d
{
    [self addScheduledHandler:^(id<MTLCommandBuffer>) {
        [drawable presentAfterMinimumDuration:d];
    }];
}

- (void)enqueue
{
    enqueueBuffer(self);
}

- (void)commit
{
    [self enqueue];
    {
        std::lock_guard<std::mutex> l(_lock);
        if (_committed)
            return;
        _committed = true;
        _status = MTLCommandBufferStatusCommitted;
        _kernelStart = hostSeconds();
    }
#if SWITCH_FRAME_TIMELINE
    static thread_local switch_frame::Since since;
    const raw::Device::Stats &k = gRM->raw->stats;
    switch_frame::committed(_frame, since,
                            {gSurfaceTextures.load(std::memory_order_relaxed),
                             k.importsMade.load(std::memory_order_relaxed),
                             k.importHits.load(std::memory_order_relaxed), gTwinsMade.load(std::memory_order_relaxed)},
                            __builtin_frame_address(0));
#endif
    if (_active)
        [(RMEncoder *)_active endEncoding];
    [(RMDevice *)_device rmCommit:self];
#if SWITCH_FRAME_TIMELINE
    _frame.mark(switch_frame::Back);
#endif
}

- (BOOL)commitAndWaitUntilSubmitted
{
    [self commit];
    [self waitUntilScheduled];
    return YES;
}

- (void)waitUntilScheduled
{
    std::unique_lock<std::mutex> l(_lock);
    auto returns = [&] {
        return raw::scheduledWaitReturns(_scheduledDone, _held, _segError != nil);
    };
    if (_committed && !returns()) {
#if SWITCH_RAW_DIAGNOSTICS
        const BlockedWait blocked(gRM->stats.scheduledWaits, gRM->stats.scheduledWaitUs);
#endif
        _changed.wait(l, returns);
    }
#if SWITCH_RAW_DIAGNOSTICS
    if (_held && !_scheduledDone && !_heldReturnUs) {
        _heldReturnUs = nowUs();
        gRM->stats.heldReturns++;
    }
#endif
}

- (void)waitUntilCompleted
{
    std::unique_lock<std::mutex> l(_lock);
    if (!_committed || _completedDone)
        return;
#if SWITCH_RAW_DIAGNOSTICS
    const BlockedWait blocked(gRM->stats.completedWaits, gRM->stats.completedWaitUs);
#endif
    _changed.wait(l, [&] {
        return _completedDone;
    });
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
- (NSNumber *)switchRawWholePrograms
{
    std::lock_guard<std::mutex> lock(_lock);
    if (_committed)
        return @NO;
    _forceWholePrograms = true;
    return @YES;
}

// hw_test_app's gpu.path.state_segments: this command buffer binds every state as an inline copy (raw_gpu.h inlineStates)
- (NSNumber *)switchRawInlineStates
{
    std::lock_guard<std::mutex> lock(_lock);
    if (_committed)
        return @NO;
    _inlineStates = true;
    return @YES;
}

// hw_test_app's clock of GPUStartTime/GPUEndTime, final once the command buffer completed: "ns" (its submission's GPU
// stamps) or "host" (the host's window from submission to completion, which stands in where no stamp was read)
- (NSString *)switchGpuClock
{
    std::lock_guard<std::mutex> l(_lock);
    if (!_completedSent)
        return @"pending";
    return _gpuStamped ? @"ns" : @"host";
}

// hw_test_app's attribution, final once the command buffer completed (its completed handlers may ask): "raw",
// "m2v:<reason>:<first predicate>" or "split:<raw segments>+<m2v segments>:<reason>:<first predicate>"
- (NSString *)switchRawPath
{
    std::lock_guard<std::mutex> l(_lock);
    if (!_completedSent)
        return @"pending";
    raw::ExecutionAttribution route;
    for (const RMSegment &range : _segs)
        route.add(range.raw, range.event);
    if (route.mixed())
        return [NSString stringWithFormat:@"split:%zu+%zu:%s:%s", route.native, route.compatibility,
                                          kReasonNames[_reason], kPredNames[_pred0]];
    if (!route.onlyCompatibility() && !_m2v)
        return @"raw";
    return [NSString stringWithFormat:@"m2v:%s:%s", kReasonNames[_reason], kPredNames[_pred0]];
}
#endif

// A selector a command buffer does not answer here: recorded for m2v (the command buffer goes there), answered zero; when m2v's
// command buffer has no method for it either, only answered zero.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector])
        return s;
    Class c = NSClassFromString(@"NBCommandBuffer");
    if (NSMethodSignature *s = c ? [c instanceMethodSignatureForSelector:selector] : nil)
        return s;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return nil;
}

- (BOOL)respondsToSelector:(SEL)selector
{
    if ([super respondsToSelector:selector])
        return YES;
    Class c = NSClassFromString(@"NBCommandBuffer");
    return c && [c instancesRespondToSelector:selector];
}

- (void)forwardInvocation:(NSInvocation *)inv
{
    if (!twinAnswers(NSClassFromString(@"NBCommandBuffer"), inv.selector)) {
        answerZero(self, inv);
        return;
    }
    [inv retainArguments];
    if (!raw::keepPointedArguments(inv))
        SWITCH_REFUSE_DETAIL("raw", "call.pointer", uintptr_t(inv.selector),
                             "-[MTLCommandBuffer %s] recorded with a pointer argument of no known extent",
                             sel_getName(inv.selector));
    [self retain:inv];
    Cmd c = cmd(OpInvocation);
    c.obj = (__bridge void *)inv;
    c.stage = 0xff;  // the command buffer's own, not an encoder's
    [self record:c];
    [self takeWholeReason:WhyCall why:@"a command buffer call recorded as an invocation"];
    hit(self, PredCallCBInvocation, uintptr_t(inv.selector));
    SWITCH_REFUSE_DETAIL("raw", "call.cbinvocation", uintptr_t(inv.selector), "-[MTLCommandBuffer %s] answered zero",
                         sel_getName(inv.selector));
    const NSUInteger n = inv.methodSignature.methodReturnLength;
    if (n) {
        void *zero = calloc(1, n);
        [inv setReturnValue:zero];
        free(zero);
    }
}

@end

@implementation RMQueue

+ (Class)twinClass
{
    return NSClassFromString(@"NBQueue");
}

- (instancetype)init
{
    if ((self = [super init])) {
        queuePriorityAdd(self);
        _handlers = followingSerialQueue("com.brunostjohn.switch.raw.command.handlers");
        _urges = followingSerialQueue("com.brunostjohn.switch.raw.command.urge");
    }
    return self;
}

- (void)dealloc
{
    queuePriorityRemove(self);
}

- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    return twinPublish(self, _twin, [gRM->fallback newCommandQueue]);
}

- (id<MTLCommandBuffer>)commandBuffer
{
    SWITCH_API(ApiObject);
    RMCommandBuffer *b = [RMCommandBuffer new];
    b->_device = _device;
    b->_queue = self;
    b->_status = MTLCommandBufferStatusNotEnqueued;
    takeRecording(b);
#if SWITCH_FRAME_TIMELINE
    switch_frame::made(b->_frame);
#endif
    return (id<MTLCommandBuffer>)b;
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return [self commandBuffer];  // retained anyway: lifetimes hang on it (raw_gpu.h)
}

- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d
{
    (void)d;
    return [self commandBuffer];
}

- (void)insertDebugCaptureBoundary
{}

- (void)addResidencySet:(id)set
{
    _residency.add(&set, 1);
}

- (void)addResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    _residency.add(sets, count);
}

- (void)removeResidencySet:(id)set
{
    _residency.remove(&set, 1);
}

- (void)removeResidencySets:(const __unsafe_unretained id *)sets count:(NSUInteger)count
{
    _residency.remove(sets, count);
}

- (void)setCompletionQueue:(dispatch_queue_t)queue
{
    _completionQueue = queue;
}

- (dispatch_queue_t)completionQueue
{
    return _completionQueue;
}

@end
