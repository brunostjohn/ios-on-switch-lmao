// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>
#include "SwitchFrameTimeline.h"
#include "raw_gpu.h"
#include "RawMetalPredicates.h"
#include "RawResidency.h"
#include "raw_handler_order.h"
#include "raw_submit_queue.h"
#include "raw_event_timeline.h"
#include "raw_variant.h"

namespace raw {
template <typename T> class ComputeKernelJob;
template <typename T> class ComputeKernelSet;
struct ComputeState;
}

@class RMDevice, RMCommandBuffer;
struct SurfaceWriteCount;

// An m2v twin made outside its object's lock, kept if no other thread kept one first (theirs is returned, this one dropped). The
// object's lock is never held across a call into m2v: a thread inside m2v holding one of its locks (its handlers run Core
// Animation's, which may ask for a twin) and a thread here holding the object's lock asking m2v for one of its own would wait for
// each other for good (r7p8's freeze: a wait with no bound).
static inline id twinPublish(id object, __strong id &slot, id made)
{
    @synchronized(object) {
        if (!slot)
            slot = made;
        return slot;
    }
}

// ---- the common base: an m2v twin and the selectors this facade does not answer itself ----
@interface RMObject : NSObject {
@public
    __weak id _device;
    id _twin;  // the m2v object over the same memory (made on first need)
}
@property (nonatomic, copy) NSString *label;
- (id)rmTwin;  // made now if needed; nil if it cannot be
- (Class)rmTwinClass;
@end

@interface RMDevice : NSObject  // the methods the device class gets (raw_metal_device_open); no state of its own
- (void)rmCommit:(RMCommandBuffer *)cb;
- (id<MTLTexture>)rmNewTexture:(MTLTextureDescriptor *)d tiled:(bool)tiled;
@end

// ---- functions and libraries: the m2v library and functions, with the name and constants text the pack keys by ----
// The libraries the pack's states come from are frameworks' default.metallib (QuartzCore's, RenderBox's): a state of their
// functions is looked up by its key, which names the framework but QuartzCore's (raw_metal_pipeline_key)
@interface RMLibrary : RMObject {
@public
    std::string _framework;  // "QuartzCore", "RenderBox"...; empty: not a framework's default library (never looked up)
}
@end
@interface RMFunction : RMObject {
@public
    std::string _constants;
    std::string _framework;
    uint8_t _argumentBuffers;  // whether it takes an argument buffer at a buffer index: 0 not asked yet, 1 no, 2 yes
}
@property (nonatomic, copy) NSString *name;
@end

// ---- resources ----
// A heap of the device (RawMetalHeap.mm): an arena its resources are placed in, or with none (a size or storage mode no arena
// takes) every resource made as the device makes it, counted against its size
@interface RMHeap : RMObject {
@public
    MTLHeapDescriptor *_descriptor;
    std::atomic<uint64_t> _used;  // what its resources made outside its arena hold
    uint32_t _arena;              // its arena + 1; 0: none
    std::atomic<int> _purgeable;  // the MTLPurgeableState asked last (0: never asked)
    // what a command buffer that uses it names (raw_heap_members.h): its arena, and the resources it made outside it
    std::shared_ptr<raw::HeapMembers> _members;
}
@end

@interface RMBuffer : RMObject {
@public
    raw::Buffer _raw;
    MTLResourceOptions _options;
    std::atomic<bool> _gpuDirty;  // the GPU wrote it since the CPU last read it
    void (^_deallocator)(void *, NSUInteger);
    RMHeap *_heap;
    uint64_t _heapBytes;  // what it counts against its heap outside the arena
    bool _aliasable;
    MTLPurgeableState _purgeable;
    // an argument encoder wrote into it: the raw device's resource IDs and addresses, which no m2v twin reads
    bool _rawArguments;
}
@end
@interface RMBuffer (Textures)
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row;
@end

typedef NS_ENUM(uint8_t, RMTextureKind) {
    RMTexturePool,
    RMTextureSurface,
    RMTextureBuffer,
    RMTextureView,
    RMTextureMemoryless,
    RMTextureTwinOnly
};

@interface RMTexture : RMObject {
@public
    raw::Texture _raw;
    bool _rawOk;  // the raw path can draw with it (else every command buffer using it goes to m2v)
    RMTextureKind _kind;
    std::atomic<bool> _gpuWrites;  // its surface was declared GPU-written (surfaceGpuWrites)
    MTLTextureDescriptor *_descriptor;
    IOSurfaceRef _surface;
    SurfaceWriteCount *_surfaceWrites;  // its surface's writes (RawMetalSurfaceWrites.mm), held while it lives
    NSUInteger _plane;
    // a planar texture's image (4:2:0 converted, 4:4:4 codes) holds its surface at this seed (_imageCurrent): converted
    // again only once the surface's seed moves (r8a: a YouTube page's thumbnails converted again at every command buffer)
    uint32_t _imageSeed;
    uint64_t _imageNativeGeneration;
    uint64_t _imageWrites;  // and its surface's writes the seed does not show (surfaceWrites)
    bool _imageCurrent;
    RMTexture *_parent;  // a view's
    // a view of part of its parent (newTextureViewWithPixelFormat:textureType:levels:slices:): those of the parent, which its
    // twin views too; the view's type is its descriptor's
    NSRange _viewLevels, _viewSlices;
    bool _viewPart;
    RMBuffer *_buffer;  // a texture over a buffer's
    NSUInteger _offset, _rowBytes;
    std::atomic<bool> _gpuDirty;
    MTLPurgeableState _purgeable;
    uint8_t _pred;  // why the raw path does not keep it (RMPred; 0: it does)
    RMHeap *_heap;
    uint64_t _heapBytes;  // as RMBuffer's
    bool _aliasable;
    std::atomic<bool> _idNamed;  // its gpuResourceID was asked: the device's handle tables name it (RawMetalResourceIds.mm)
}
@end

@interface RMSampler : RMObject {
@public
    raw::Sampler _raw;
    MTLSamplerDescriptor *_descriptor;
    std::atomic<bool> _idNamed;
    raw::Sampler _normalized;  // a pixel-coordinate sampler's twin of normalized coordinates, what a heap holds of it
    NSMutableDictionary<NSValue *, id<MTLSamplerState>> *_clamped;  // its descriptor's with a bind's LOD clamps
}
@end

@interface RMPipeline : RMObject {
@public
    // null: not in the pack (every command buffer using it goes to m2v), or not yet made by the runtime (completeLater: set once,
    // released, from its queue)
    raw::Pipeline *_raw;
    bool _packable;     // its functions are one framework's default library's (looked up in the pack)
    bool _packed;       // its record is the pack's, and so are its variants' (RawMetalCubic.mm packedVariants)
    bool _completable;  // both its functions are this device's (the runtime makes its record if the pack has none)
    bool _archived;     // an archive its creation listed declares it: its twin is made as a packed one's
    bool _caTransform;
    bool _asked;                             // its record asked of the runtime (completeLater)
    std::string _key;                        // its pack key (completable states')
    MTLRenderPipelineDescriptor *_madeFrom;  // completable states': what its cubic variants are made of
    std::mutex _variantLock;
    std::map<std::string, std::shared_ptr<raw::Variant<raw::Pipeline>>> _variants;
}
@end

@interface RMTwinObject : RMObject  // compute states, and any m2v object the facade only carries
@end

@interface RMDepthStencil : RMObject {
@public
    raw::DepthStencilState _raw;
    MTLDepthStencilDescriptor *_descriptor;
}
@end

#include "RawMetalPlan.h"

@interface RMQueue : RMObject {
@public
    dispatch_queue_t _completionQueue;
    dispatch_queue_t _handlers;  // its command buffers' scheduled handlers, in its order
    // the compiles its head waits for, made one at a time at the committers' priority (raw_follow.h): one urged compile per
    // waiting Metal queue at most
    dispatch_queue_t _urges;
    std::atomic<void *> _last;  // the command buffer enqueued last, retained until its completed handlers ran
    uint32_t _gpuPriority, _backgroundGpuPriority;  // what it asked for (RawMetalQueuePriority.mm)
    raw::ResidencySets _residency;
}
@end

// What the raw encode reads of a render pass, taken from the descriptor's copy as its encoder is made (the copy itself is what
// m2v's replay encodes): no Metal getter, no attachment object Metal makes on a first read, per encode. Objects unretained: the
// command buffer's owners hold them.
struct RMPass {
    struct Color {
        __unsafe_unretained id texture;
        NSUInteger level;
        MTLLoadAction load;
        MTLStoreAction store;
        MTLClearColor clear;
        __unsafe_unretained id resolve;
    } color[8];

    __unsafe_unretained id depth;
    MTLLoadAction depthLoad, stencilLoad;
    // the descriptor's depth (0) and stencil (1) attachments as the encode reads them, the store as the encoder set it
    struct Zeta {
        __unsafe_unretained id texture;
        NSUInteger level;
        MTLLoadAction load;
        MTLStoreAction store;
        bool resolve;
    } zeta[2];
    double clearDepth;
    uint32_t clearStencil;
    NSUInteger width, height;
    raw::SamplePositions positions;
    bool testsHiddenZeta = false;  // a depth/stencil state set in it tests depth or stencil (raw::testsHiddenZeta)
    bool continues = false;  // a parallel encoder's sub-encoder after its first: the pass before it goes on (raw_parallel.h)
};

// A command buffer's setBytes data, written once where the GPU reads it: write-combined pool chunks the raw encode binds in place
// (m2v's replay reads them back), each copy 256-byte aligned and zero to the next 256 bytes as an upload chunk is. The chunks go
// with the recording (giveRecording), so they outlive every submission that reads them.
struct RMBytes {
    std::vector<raw::Buffer> chunks;  // va 0: heap memory (the GPU pool had none; the command buffer is m2v's)
    size_t at = 0;                    // the chunk being filled
    uint32_t used = 0;                // its bytes taken
};

// A part of a command buffer submitted on its own (RMCommandBuffer::_segs): tape [from, to), on the raw path or m2v
struct RMSegment {
    uint32_t from, to;
    bool raw;
    bool event = false;
    bool gpuSignal = false;
    bool gpuWait = false;
};

struct RMSharedEventWait {
    void *event;
    std::shared_ptr<raw::SharedEventLease> lease;
    uint64_t value;
    std::shared_ptr<raw::SharedEventCompletion> completion = {};
};

@interface RMCommandBuffer : RMObject {
@public
    uint64_t _protection;  // setProtectionOptions: (kept, no effect)
    NSMutableDictionary *_userDictionary;
    uint64_t _traceId;
    raw::ResidencySets _residency;
    // Its encoders: each one's first command (OpRender, OpBlit, OpCompute) carries index 1 where it goes to m2v (a refusal taken
    // while it was the active encoder: _encRefused, written into it as the next encoder begins or at commit, _encAt the
    // command's place). A refusal outside any encoder keeps the command buffer whole (_whole). One whose encoders are some raw's
    // and some m2v's is submitted in segments (_segs, planSegments): each run of encoders of one driver on its own, in order on
    // the GPU as two command buffers of the drivers are (the kernel's Order), so a refused pass costs m2v that pass, not the
    // frame. The command buffer completes with its last segment.
    size_t _encAt;
    bool _encOpen, _encRefused, _whole;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    bool _forceWholePrograms, _inlineStates;
    // hw_test_app's probes (RawMetalProbe.mm): its barriers' forms, where they took effect, and its 2D-engine work, each
    // at the blit encoder whose first command is at `at`
    raw::ProbeForms _probeForms;
    raw::ProbeTaken _probeTaken;
    struct ProbeOp {
        size_t at;
        raw::ProbeOp op;
        RMTexture *read, *written;
    };
    std::vector<ProbeOp> _probeOps;
#endif
    std::vector<RMSegment> _segs;
    size_t _segNext;
    NSError *_segError;  // an m2v segment's error, reported at completion
    uint32_t _segmentPending;
    uint64_t _channelGeneration, _rawPrefixGeneration;
    bool _gpuPrefixEligible;
    std::vector<raw::EventTimeline::Promise> _gpuEventWaits;
    std::vector<RMSharedEventWait> _sharedEventWaits;
    std::vector<size_t> _sharedSignalRanges;
    std::shared_ptr<raw::SharedEventWatch> _sharedEventWatch;
    std::shared_ptr<raw::ResourceReadyCompletion> _resourceReady;
    void *_nativeWaitEvent;
    bool _finishReady, _finishing, _submittedM2v;
    bool
        _sampledShadows;  // it binds a texture sampled through a pool image (raw::Texture::sampled): encodeRaw refreshes them
    bool _standIns;  // it binds an extended-range texture (raw::Texture::standIn): encodeRaw draws its stand-in
    RMCommandPlan _plan;
    std::vector<RMPass> _passes;  // OpRender's (Cmd::x)
    RMBytes _bytes;               // OpBytes': chunk Cmd::c, offset Cmd::a, length Cmd::b
    // a pass's vertex setBytes before its first draw, again in cached memory (Cmd::x: offset + 1): the draw's geometry
    // the encode reads (the first draw's cover test) is not read from the write-combined chunk
    std::vector<uint8_t> _shadow;
    std::vector<uint64_t> _extra;
    std::vector<id> _owners;
    std::unordered_set<void *> _ownerKeys;  // everything the tape names, retained until completion
    RMReason _reason;
    NSString *_why;
    NSMutableArray *_scheduledHandlers, *_completedHandlers;
    bool _handlersQueued;      // scheduled handlers went to its queue's _handlers: its completed ones go behind them
    RMCommandBuffer *_before;  // enqueued before it on its queue, until its own completed handlers were handed on
    raw::HandlerTurn _turn;
    MTLCommandBufferStatus _status;
    NSError *_error;
    RMQueue *_queue;
    bool _enqueued;
    raw::SubmitQueue<void *, RMCommandBuffer *>::Reservation _reservation;
    std::vector<size_t> _eventWaits;
    raw::EventDeadline::Time _eventStart;
    std::unordered_set<void *> _pipelineObserved;  // one readiness subscription per compatibility state
    std::unordered_set<void *> _variantObserved;
    std::map<void *, std::shared_ptr<raw::ComputeKernelSet<raw::ComputeState>>> _computeJobs;
    size_t _eventArmed;  // operation position + 1; only the current wait installs a listener
    bool _committed, _scheduledDone, _completedDone, _m2v;
    // its queue's head waits on a shared event Metal's GPU would wait for: waitUntilScheduled returns, its scheduled
    // handlers still wait for its submission (raw_scheduled_wait.h)
    bool _held;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    uint64_t _heldReturnUs;  // when waitUntilScheduled returned for it held, until it was submitted
#endif
    // its event signals are in m2v work (replayed whole, or in its last segment): finish() makes none
    bool _signalsOnM2v;
    bool
        _partial;  // the kernel refused its raw submission after part of it ran (raw::CommandBuffer::partial): never replayed
    // scheduled() and finish() ran (their handlers taken): a handler added after goes at once, and scheduled() runs once (a
    // command buffer held for a shared event is scheduled at its commit, eventHeld)
    bool _scheduledSent, _completedSent;
    std::mutex _lock;
    std::condition_variable _changed;
    uint32_t _seqno;
    uint32_t _stamp;  // its raw submission's GPU time stamps (raw::CommandBuffer::stamp), 0 for any other
    double _kernelStart, _kernelEnd, _gpuStart, _gpuEnd;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    bool _gpuStamped;  // GPUStartTime/GPUEndTime are its submission's GPU stamps, not the host's window
#endif
    __weak id _active;        // the encoder being recorded (weak: it holds the command buffer, its caller holds it)
    uint64_t _preds;          // every refusal predicate it hit (1 << RMPred)
    uint64_t _predA, _predB;  // the first one's values
    uint8_t _pred0;           // the first one
#if SWITCH_FRAME_TIMELINE
    switch_frame::Stamps _frame;
#endif
}
- (void)takeReason:(RMReason)reason why:(NSString *)why;
- (void)takeWholeReason:(RMReason)reason why:(NSString *)why;
- (void)closeEncoder;
- (void)record:(const Cmd &)cmd;
- (void)retain:(id)object;
@end

// A refusal predicate hit while recording (or encoding) `cb`: into its mask; the first one's values kept; the process's first hit
// of `p` logged with them
static inline void hit(RMCommandBuffer *cb, RMPred p, uint64_t a = 0, uint64_t b = 0)
{
    cb->_preds |= 1ull << p;
    if (!cb->_pred0) {
        cb->_pred0 = p;
        cb->_predA = a;
        cb->_predB = b;
    }
    if (!predLogged(p))
        logPred(p, a, b, nil);
}

// ---- encoders: they record ----
@interface RMEncoder : RMObject {
@public
    RMCommandBuffer *_cb;
    bool _ended;
    uint64_t _traceId;
}
@end

// The next of the device's trace object IDs, which its command buffers and encoders take at their first globalTraceObjectID
// (RawMetalEncoder.mm)
uint64_t nextTraceId();

// A render encoder keeps what its draws need to decide, as they are recorded, whether the raw path draws them exactly as m2v
// would: the state, each bound buffer's address, the textures, the pass's formats. A draw it would not is the command buffer's
// reason then, so encodeRaw never meets it (a fallback costs no encoding).
@interface RMRenderEncoder : RMEncoder {
@public
    RMPipeline *_pipe;
    uint64_t _bufVa[3]
                   [31];  // stage 1 vertex, 2 fragment: the bound buffer's address plus offset (setBytes: 0, aligned)
    RMBuffer *_buf[3][31];
    RMTexture *_tex[2][raw::kStageTextures];  // vertex, fragment textures (unretained: the command buffer retains them)
    RMSampler *_samp[16];  // fragment samplers, alike; `_cubicMask`: the slots of those that are cubic (raw_cubic.h)
    uint16_t _cubicMask;
    uint64_t _checkedGeneration;
    const raw::Pipeline *_checkedPipeline;
    raw::Pipeline *_drawn;
    raw::Variant<raw::Pipeline> *_drawnVariant;
    uint32_t _formats[8];   // the pass's color attachments' formats as the raw path holds them (0: none)
    uint32_t _count;        // its attachment count (the last attachment's index + 1)
    uint32_t _zeta;         // its depth/stencil attachment's format as the raw path holds it (0: none)
    uint32_t _samples;      // its color attachments' samples a pixel
    uint8_t _turn;          // its turned attachments' rotation (raw_rotation.h; 0: none)
    uint32_t _passAt;       // its pass's RMPass (the command buffer's _passes)
    raw::DepthStencilDesc _ds;
    MTLRenderPassDescriptor *_pass;  // the command buffer's copy, which the raw encode and m2v's replay read
    bool _drew;                      // a draw recorded
}
@end
@interface RMBlitEncoder : RMEncoder
@end
@interface RMParallelRenderEncoder : RMEncoder  // (RawMetalParallel.mm)
@end
@interface RMComputeEncoder : RMEncoder {
@public
    MTLDispatchType _type;
}
@end

// RawMetalCompute.mm: a compute state over m2v's (`twin`), with what the raw pack keys its kernels by
id rmComputeState(id device, id<MTLDevice> fallback, MTLComputePipelineDescriptor *descriptor, MTLPipelineOption options,
                  MTLAutoreleasedComputePipelineReflection *reflection, NSError **error);
// m2v's replay and the raw encode of a compute encoder's commands (the Op*Kernel* ones and OpDispatch): false from the raw
// encode where it refuses one (*why)
bool isKernelCommand(Op op);
// OpBytes and OpKernelBytes (`m`): the bytes into the command buffer's RMBytes, m.a/b/c set (RawMetalBindings.mm)
void recordBytes(RMCommandBuffer *cb, Cmd m, const void *bytes, NSUInteger length, bool shadow = false);
void replayKernel(id encoder, RMCommandBuffer *cb, const Cmd &c, std::set<RMBuffer *> &buffers);
bool encodeKernel(raw::ComputeEncoder &e, raw::CommandBuffer &rc, RMCommandBuffer *cb, const Cmd &c, const char **why);

static inline void setBinding(RMCommandBuffer *cb, Op op, int stage, NSUInteger index, id object, uint64_t c)
{
    if (object)
        [cb retain:object];
    Cmd m = cmd(op, uint8_t(stage), uint16_t(index));
    m.obj = (__bridge void *)object;
    m.c = c;
    [cb record:m];
}

// The encoder `e` records into this command buffer from now on, the one before it ended (RawMetalEncoder.mm)
@interface RMCommandBuffer (Encoders)
- (void)begin:(RMEncoder *)e;
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d;
@end

@interface RMEncoder ()
- (void)endEncoding;
- (void)updateFence:(id<MTLFence>)fence;
- (void)waitForFence:(id<MTLFence>)fence;
@end

// `s` with a bind's LOD clamps (RawMetalBindings.mm)
id<MTLSamplerState> clampedSampler(id<MTLSamplerState> s, float lodMin, float lodMax);

@interface RMRenderEncoder ()
- (void)checkDraw;  // (RawMetalRenderEncoder.mm)
@end

@interface RMRenderEncoder (Bindings)
- (void)texture:(int)stage object:(id<MTLTexture>)t index:(NSUInteger)index;
- (void)sampler:(int)stage object:(id<MTLSamplerState>)s index:(NSUInteger)index;
@end

@interface RMRenderEncoder (Cubic)
- (const raw::Pipeline *)drawnRecord;  // (RawMetalCubic.mm)
@end

#if SWITCH_FRAME_TIMELINE
extern std::atomic<uint64_t> gSurfaceTextures, gTwinsMade;
extern thread_local uint64_t tTwinTicks;
void countTwin(id object, uint64_t ticks);  // the rates line's tw= (RawMetalReplay.mm)
#endif
static inline id twinOf(id object)
{
    if (!object)
        return nil;
    if (![object isKindOfClass:[RMObject class]])
        return object;  // an m2v object already (events, fences, anything the device forwarded)
#if SWITCH_FRAME_TIMELINE
    if (!((RMObject *)object)->_twin) {
        const uint64_t began = mach_absolute_time();
        id twin = [(RMObject *)object rmTwin];
        const uint64_t took = mach_absolute_time() - began;
        tTwinTicks += took;
        gTwinsMade.fetch_add(1, std::memory_order_relaxed);
        countTwin(object, took);
        return twin;
    }
#endif
    return [(RMObject *)object rmTwin];
}

static inline uint64_t extra(RMCommandBuffer *cb, uint64_t i)
{
    return cb->_extra[i];
}

static inline double extraDouble(RMCommandBuffer *cb, uint64_t i)
{
    double d;
    memcpy(&d, &cb->_extra[i], 8);
    return d;
}

static inline float extraFloat(RMCommandBuffer *cb, uint64_t i)
{
    const uint32_t u = uint32_t(cb->_extra[i]);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
