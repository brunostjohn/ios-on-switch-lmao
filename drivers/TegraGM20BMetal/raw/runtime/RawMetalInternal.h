// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetal.h"
#import <IOSurface/IOSurfaceRef.h>
#import <TargetConditionals.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <os/log.h>
#include <pthread.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "GpuMemoryRelease.h"
#include "LimitsTable.h"
#include "raw_invocation.h"
#include "SwitchFrameTimeline.h"
#include "../../../../kits/SwitchRefusal/SwitchRefusalSite.h"
#include "RawAlias.h"
#include "RawComplete.h"
#include "raw_follow.h"
#include "raw_gpu.h"
#include "TegraGM20BLayout.h"
#include "../../../../kits/SwitchRefusal/SwitchRefusal.h"
#include "RawMetalObjects.h"
#include "raw_retirement.h"

// Buffer-backed texture views share the caller's cached CPU pointer with their parent buffer.
static inline RMBuffer *cpuBufferOfResource(id object)
{
    if ([object isKindOfClass:[RMBuffer class]])
        return (RMBuffer *)object;
    if (![object isKindOfClass:[RMTexture class]])
        return nil;
    RMTexture *texture = (RMTexture *)object;
    while (texture->_kind == RMTextureView && texture->_parent)
        texture = texture->_parent;
    return texture->_buffer;
}

// The m2v frontend's in-place textures over a buffer, of rows or the device's optimal layout (NativeBridgeMetalFrontend.mm):
// weak, so the host benchmark links without them.
extern "C" id native_bridge_metal_texture_in_place(id buffer, id descriptor, NSUInteger offset, NSUInteger bytesPerRow)
    __attribute__((weak_import));
extern "C" id native_bridge_metal_texture_tiled_in_place(id buffer, id descriptor, NSUInteger offset)
    __attribute__((weak_import));
extern "C" id native_bridge_metal_render_pipeline_lazy(id device, id descriptor, NSUInteger options)
    __attribute__((weak_import));
extern "C" void native_bridge_metal_journal_render(id descriptor, NSUInteger options) __attribute__((weak_import));
extern "C" int native_bridge_metal_render_pipeline_ready(id pipeline, void (^observer)(void))
    __attribute__((weak_import));
extern "C" int native_bridge_metal_render_pipeline_urge(id pipeline, void (^observer)(void), dispatch_queue_t queue)
    __attribute__((weak_import));
// Where NVK keeps a texture's texels (a stored depth/stencil texture the raw path renders and copies in place)
struct switch_nvk_image_memory;
extern "C" int native_bridge_metal_texture_memory(id texture, struct switch_nvk_image_memory *out)
    __attribute__((weak_import));
// Why the bridge's last call on this thread failed (bridge/NativeMetalObjects.h)
extern "C" const char *nmb_last_error(void) __attribute__((weak_import));

// A wait the plugin bounds (r9-hang: r7n's and r7o's freezes were a userland wait with no end in backboardd's render thread):
// past kWaitLimit, one line naming the wait and the object, then abort(), for a crash report with the stack instead of a silent
// freeze, the userland twin of the kernel's missing-interrupt panic. Nothing waits that long when it works: within 10 s the kernel
// kills the channel a GPU wait is stuck on (or panics).
static constexpr auto kWaitLimit = std::chrono::seconds(20);
[[noreturn]] void waitTooLong(const char *what, const void *object);

os_log_t rawLog();
void once(NSString *text);
double hostSeconds();
uint64_t nowUs();

struct Stats {
#if SWITCH_RAW_DIAGNOSTICS
    std::atomic<uint64_t> raw{0}, m2v{0}, deferred{0}, aborted{0}, encodeUs{0}, draws{0}, passes{0}, copies{0},
        reasons[WhyCount]{}, switches{0}, cpuOrdered{0}, acquires{0}, eventsReleased{0}, heldReturns{0};
    // shared event signals published as their raw work completes (publishRawEventSignals), those the CPU set after the head
    // waited for its own work, and native event waits that went on a submitted command buffer's promise
    std::atomic<uint64_t> signalsPublished{0}, signalsAfterWork{0}, waitsOnPromise{0};
    // diagnostics (r10-rawsweep): the raw encode thrown away by aborted command buffers, the m2v replay's time on the committing
    // thread (replay + commit), and the bytes replay cleans
    std::atomic<uint64_t> abortUs{0}, replayUs{0}, replayClean{0};
    // command buffers submitted in segments (planSegments), and their segments on m2v; of those with both drivers' encoders, the
    // ones split around their shared events, and the ones kept whole for an event wait or signal between encoders
    std::atomic<uint64_t> splits{0}, splitM2v{0}, eventSplits{0}, eventWhole{0};
    // command buffers completed (finish): with the commits, what the stall line (rawStall) watches
    std::atomic<uint64_t> finished{0};
    // waitUntilScheduled / waitUntilCompleted calls that blocked, and the time they blocked: CPU waits on the caller's thread
    // (Core Animation's render thread flushes with waitUntilScheduled every frame)
    std::atomic<uint64_t> scheduledWaits{0}, scheduledWaitUs{0}, completedWaits{0}, completedWaitUs{0};
#if SWITCH_FRAME_TIMELINE
    // the m2v replay's parts (noteReplay)
    std::atomic<uint64_t> replaySlotUs{0}, replayTwinUs{0}, replayCommitUs{0}, replayMaxUs{0};
    // the draws it replayed, and the twins it made: textures, buffers, anything else
    std::atomic<uint64_t> replayDraws{0}, twinTextures{0}, twinBuffers{0}, twinOthers{0}, twinTextureUs{0};
    // command buffers completed before their submitting thread scheduled them (finish() ran their scheduled handlers)
    std::atomic<uint64_t> scheduledByFinish{0};
    std::atomic<uint64_t> twinForwards{
        0};  // selectors m2v's object answers that a facade object has no method for (answerMissing)
    std::atomic<uint64_t> zeroAnswers{0};  // selectors answered zero, as m2v's objects answer them (answerZero)
#endif
#endif
};

// A wait that blocked, counted with its time into two Stats counters when it ends (debug builds; nothing in release).
class BlockedWait {
public:
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    BlockedWait(std::atomic<uint64_t> &waits, std::atomic<uint64_t> &waitedUs)
        : waits_(&waits), waitedUs_(&waitedUs), began_(nowUs())
    {}

    ~BlockedWait()
    {
        waits_->fetch_add(1, std::memory_order_relaxed);
        waitedUs_->fetch_add(nowUs() - began_, std::memory_order_relaxed);
    }

    BlockedWait(const BlockedWait &) = delete;
    BlockedWait &operator=(const BlockedWait &) = delete;
    BlockedWait(BlockedWait &&) = delete;
    BlockedWait &operator=(BlockedWait &&) = delete;

private:
    std::atomic<uint64_t> *waits_;
    std::atomic<uint64_t> *waitedUs_;
    uint64_t began_;
#else
    BlockedWait(std::atomic<uint64_t> &, std::atomic<uint64_t> &)
    {}
#endif
};

// The process's one raw device (the render server's): its state. The device object Core Animation holds is an instance of a
// subclass of the m2v device's own class (raw_metal_device_open), so it is an _MTLDevice that conforms to Metal's protocols as
// the m2v device does and answers every selector the facade does not override with the m2v device's own method; it has no
// instance variables of its own (the base class's ABI is fixed), so its state lives here.
struct RMNativeEventSignal {
    void *event;
    raw::EventTimeline::Promise promise;
};

struct RMRawRetirement {
    RMCommandBuffer *buffer;
    uint32_t seqno;
    std::shared_ptr<const raw::RetirementWrites> writes;
    std::vector<RMNativeEventSignal> signals;
    std::vector<raw::EventTimeline::Promise> waits;
    uint64_t generation = 0;
    raw::CpuCompletion cpu;
};

struct RawState {
    raw::Device *raw = nullptr;
    std::shared_ptr<raw::SharedEvents> sharedEvents;
    id<MTLDevice> fallback;  // the m2v device
    id device;               // the raw device object
    std::mutex schedule;
    std::mutex eventValues;
    raw::EventTimeline events;
    raw::SubmitQueue<void *, RMCommandBuffer *> waiting;
    std::condition_variable waitingChanged;
    std::mutex m2vLock;
    std::condition_variable m2vChanged;
    uint32_t m2vPending = 0;      // m2v command buffers committed and not completed
    uint32_t m2vUnscheduled = 0;  // and not scheduled yet: their kernel submission (NVK's Exec) is not made
    // The two drivers' order (submit lock): on the GPU when the kernel has Order (TegraGM20BInfoOrder), else on the CPU (the
    // m2v work before a raw command buffer completes first, and the raw work before an m2v one).
    bool gpuOrder = false;
    uint64_t orderRefusedAt = 0;  // raw::Device::Stats::reopens when the kernel last refused an Order (raw_order.h)
    bool rawSinceM2v =
        false;  // raw work submitted since the last m2v commit: the next m2v one waits for it (BeforeTask)
    bool m2vSinceRaw =
        false;           // m2v work submitted since the last raw submission: the next raw one waits for it (AfterTask)
#if SWITCH_RAW_DIAGNOSTICS
    int lastDriver = 0;
#endif
    MTLSharedEventListener *listener;  // the CPU-ordered remainder's event waits
    std::mutex doneLock;
    std::condition_variable doneChanged;
    std::deque<RMRawRetirement> inflight;  // immutable range fences; later submissions never change an earlier wait
    dispatch_queue_t handlers;             // where scheduled handlers run (in order, never inside commit)
    std::atomic<uint32_t> committed{0};    // command buffers committed and not finished (gRawRelease's drain)
    Stats stats;
    std::unique_ptr<raw::Completion> completion;  // the states the pack lacks (raw_metal_complete_states)
};

extern RawState *gRM;

// RawMetalPredicates.mm
const char *metalTypes(SEL selector);
// m2v's object of `twinClass` has a method for `selector`: else it answers zero (the frontend's forwardZero), and so does the
// facade's object, with no twin made and nothing sent to m2v (answerZero)
bool twinAnswers(Class twinClass, SEL selector);
void answerZero(id object, NSInvocation *invocation);
// A selector m2v's object answers that raw has no method for: answered zero, a fault in debug builds and the rates line's tw=../f
void answerMissing(id object, NSInvocation *invocation);
bool rawShape(MTLTextureDescriptor *d);
bool chainShape(MTLTextureDescriptor *d);
bool lineShape(MTLTextureDescriptor *d);
bool volumeShape(MTLTextureDescriptor *d);
bool volumeRendered(MTLTextureDescriptor *d);
uint32_t arrayType(MTLTextureDescriptor *d);
bool zetaDescriptor(MTLTextureDescriptor *d);
bool multisampleShape(MTLTextureDescriptor *d);
uint32_t rawShapeTerm(MTLTextureDescriptor *d);
bool identitySwizzle(MTLTextureSwizzleChannels s);
// `t` reading its channels through `s` (a descriptor's swizzle, or a swizzled view's): its raw header, its descriptor
id<MTLTexture> swizzled(id<MTLTexture> t, MTLTextureSwizzleChannels s);
// A texture the raw path does not keep: its predicate, and the predicate's first line with the texture's details
void refuseTexture(RMTexture *t, RMPred p, uint64_t a, uint64_t b, NSString *kind, NSString *extra);
void keepExtendedRange(MTLTextureDescriptor *d, raw::Texture &t, bool overBuffer);

// Where plain Metal reads or writes each buffer row of a copy between a buffer and a texture: rows `bytesPerRow` apart from
// `offset`. A layout off Metal's rules (an offset or bytesPerRow off the texel, or rows that overlap) is placed as Apple's
// GPUs place it (m2v blit_plan.rs placed_rows, measured): each row rounded down to the texel's component (at most 4 bytes),
// except that a copy into a texture of more than one row whose bytesPerRow rounded down to the texel is a multiple of 16,
// from an offset that is, takes Metal's aligned path: its rows at that rounded pitch from offset 0. False: a layout
// Metal's behaviour is not known for, refused as m2v refuses it (rows that overlap out of a texture; on the aligned path,
// a pitch below a row or a nonzero offset).
struct BufferRows {
    uint64_t first, pitch;  // row y at first + y * pitch where `uniform`
    bool uniform;
    uint32_t word;
};

// RawMetalBlit.mm
bool bufferRows(uint64_t offset, uint64_t bytesPerRow, uint32_t rows, uint32_t rowBytes, uint32_t bpp, bool intoTexture,
                BufferRows &out);
uint64_t bufferRow(const BufferRows &r, uint64_t offset, uint64_t bytesPerRow, uint32_t y);

// RawMetalLibrary.mm
RMLibrary *wrapLibrary(id device, id<MTLLibrary> twin, NSString *path);
std::string libraryName(RMFunction *f);

// RawMetalStates.mm: a descriptor whose functions are this device's, over their m2v twins
MTLRenderPipelineDescriptor *twinDescriptor(MTLRenderPipelineDescriptor *d);

// RawMetalCubic.mm: the cubic binds (raw_cubic.h) of a draw of `pl` with these fragment samplers and textures bound (empty: no
// cubic sampler where it samples), and a packed state's variants, the pack's records of them (under its pack key's hash)
// placed at its creation
raw::CubicBinds cubicBinds(const raw::Pipeline &pl, RMSampler *const samplers[16], RMTexture *const textures[16]);
void packedVariants(RMPipeline *p, uint64_t keyHash);

// RawMetalHeap.mm: the purged heaps a committed command buffer's resources lie in, given their pages back before it is encoded
// (it counts as committed already, so no purge runs meanwhile)
void restoreArenas(RMCommandBuffer *cb);
// A heap's pages back if it was purged (an m2v twin aliases them); true without a heap or an arena
bool heapResident(RMHeap *h);

// RawMetalTexture.mm
void declareGpuWrites(RMTexture *t);
#if SWITCH_FRAME_TIMELINE
void traceSurfaceImage(RMTexture *t, const char *what, uint32_t seed);
#endif

// RawMetalEncode.mm: the imports a texture's or buffer's memory lies in, named to the submission
void useTexture(raw::CommandBuffer *rc, RMTexture *t, bool write);
void useBuffer(raw::CommandBuffer *rc, RMBuffer *b, bool write);

// RawMetalEncodeBlit.mm: a blit encoder's command of the tape onto the raw path's copy engine; a texture it writes leaves
// `fresh` and `standInFresh` (its pool image and its stand-in are refreshed again before a sample)
void encodeBlit(RMCommandBuffer *cb, const Cmd &c, raw::CommandBuffer *rc, raw::BlitEncoder *be,
                std::set<RMTexture *> &fresh, std::set<RMTexture *> &standInFresh);
// Levels first + 1 .. last of a chain by a pass of the pack's mip state a level (raw_mip.h); false where the pack has none or a
// pass is refused (*why)
bool mipPasses(raw::CommandBuffer *rc, RMTexture *t, uint32_t first, uint32_t last, const char **why);
// A chain's generateMipmapsForTexture: each step on the 2D engine or by mipPasses (raw/fx/fx_mip_route.h); `be` is the blit
// encoder encoding goes on with; `probe2d` the command's probe format (fx_mip_probe.h), 0 for none
bool encodeMipmaps(raw::CommandBuffer *rc, RMTexture *t, uint32_t probe2d, raw::BlitEncoder *&be, const char **why);

// RawMetalTextureCpu.mm: `bytes` from `from` cleaned to memory (or cleaned and invalidated), then a DSB
void cacheLines(const uint8_t *from, uint64_t bytes, bool invalidate);
bool transferTexture(RMTexture *texture, MTLRegion region, NSUInteger level, NSUInteger slice,
                     void *bytes, NSUInteger row, NSUInteger image, bool write);
void cpuTiled(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write);
const tgm_layout *textureCpuImportLayout(RMTexture *texture);
void cpuLevel(RMTexture *t, MTLRegion r, NSUInteger level, uint8_t *bytes, NSUInteger row, bool write);
void cpuVolume(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, NSUInteger image, bool write);
void cpuLayer(RMTexture *t, MTLRegion r, NSUInteger slice, uint8_t *bytes, NSUInteger row, bool write);

// RawMetalDevice.mm
const char *ycbcrPlan(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::YcbcrPlan &out);

// RawMetalStates.mm: the state of `d` as the pack and the archives it lists answer, nil where it is Metal's binary archive
// miss (`*error`), never compiled; and whether the archives miss the state of `d` (its functions this device's)
RMPipeline *rmPipelineFor(RMDevice *device, MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error);
bool rmRenderArchiveMissed(MTLRenderPipelineDescriptor *d, MTLPipelineOption o, NSError **error);

// RawMetalYcbcrTargets.mm: two-plane YCbCr code textures rendered into (raw_ycbcr_store.h). A target's stand-in and chroma
// textures (Texture::planarStandIn, chromaCodes, chromaHalved), the reason a pass into one goes to m2v (null: none), its chroma
// blocks from the codes its store wrote, and a descriptor's first such attachment's format (0: none)
void makeYcbcrTarget(raw::Texture &t, uint32_t pixelFormat);
const char *ycbcrTargetPassRefusal(const raw::Texture &t, MTLRenderPassColorAttachmentDescriptor *a);
void storeYcbcrChroma(raw::CommandBuffer *rc, const raw::Texture &t);
uint32_t ycbcrTargetFormat(MTLRenderPipelineDescriptor *d);

@interface RMDevice (YcbcrTargets)
- (id<MTLRenderPipelineState>)ycbcrTargetState:(MTLRenderPipelineDescriptor *)d
                                       options:(MTLPipelineOption)o
                                    reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                         error:(NSError **)error;
@end

// RawMetalReplay.mm
id<MTLCommandBuffer> replay(RMCommandBuffer *cb, size_t from, size_t to, bool part);

// RawMetalEncode.mm
bool encodeRaw(RMCommandBuffer *cb, size_t from, size_t to, const char **why, size_t *refusedEncoder);

// RawMetalEvents.mm
extern std::mutex gHoldersLock;
extern std::set<RMCommandBuffer *> gHolders;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// A held command buffer's shared event wait as it was armed, for the still-waiting line (rawStallTick); gHoldersLock
struct RMEventWaitNote {
    void *event;
    uint64_t value;
    uint64_t sinceUs, saidUs;
};
extern std::map<RMCommandBuffer *, RMEventWaitNote> gEventWaits;
#endif
bool waitsForEvent(RMCommandBuffer *cb);
void armEventWaits(RMCommandBuffer *cb);
raw::EventReadiness eventReadiness(RMCommandBuffer *cb, raw::EventDeadline::Time &nextWake);
// A value a submitted command buffer's signal reaches once its work completes (raw_event_timeline.h)
bool promisedEvent(void *event, uint64_t value);
bool expiredEventWait(RMCommandBuffer *cb, size_t index);
std::vector<RMNativeEventSignal> publishRawEventSignals(RMCommandBuffer *cb);
void retireEventSignals(const std::vector<RMNativeEventSignal> &signals, bool success);
raw::EventDependencyState validateNativeEventWaits(RMCommandBuffer *cb, uint64_t generation);
bool waitsOnRetiredProducer(RMCommandBuffer *cb);
void releaseEventWaits(RMCommandBuffer *cb);
std::shared_ptr<raw::SharedEventLease> sharedEventLease(id event);
int sharedEventReadiness(RMCommandBuffer *cb, id event, uint64_t value, bool native);
bool sharedWaitsReady(RMCommandBuffer *cb, bool native);
void prepareSharedEvents(RMCommandBuffer *cb, raw::CommandBuffer *commands);
void publishSharedEvents(RMCommandBuffer *cb);
void enqueueBuffer(RMCommandBuffer *cb);
enum RMReadyDependency : uint32_t { RMRawProgress = 1, RMCompatibilityProgress = 2 };
void wakeSubmit(void *queue, uint32_t dependencies = 0);
bool m2vClear();
void planSegments(RMCommandBuffer *cb);
bool submitRange(RMCommandBuffer *cb);

// RawMetalPrivateComp.mm (raw_private_comp.h): submissions and the moves of compressed private textures take the submit
// lock; a move inside a raw encode is refused
std::recursive_mutex &submitLock();
extern thread_local bool tRawEncoding;
uint32_t privateCompressedKind(MTLTextureDescriptor *d);
bool rmTexturePlain(RMTexture *t);
void rmRangePlain(RMCommandBuffer *cb, size_t from, size_t to);
void unhold(RMCommandBuffer *cb);

// RawMetalFinish.mm
void forEachWritten(RMCommandBuffer *cb, size_t from, size_t to, void (*written)(void *));
extern GpuMemoryRelease &gRawRelease;
void scheduled(RMCommandBuffer *cb);
dispatch_queue_t followingRootQueue();
dispatch_queue_t followingSerialQueue(const char *label);
void finish(RMCommandBuffer *cb, NSError *error);
// The command buffer's error, unless it has one: under its lock, which waitUntilScheduled reads it under
void failCommandBuffer(RMCommandBuffer *cb, NSString *reason);
void segmentCompleted(RMCommandBuffer *cb, NSError *error, const raw::RetirementWrites &writes, uint32_t dependencies,
                      const std::vector<RMNativeEventSignal> &signals = {},
                      const std::vector<raw::EventTimeline::Promise> &waits = {},
                      const raw::CpuCompletion *cpu = nullptr);
void retireRawRange(RMCommandBuffer *cb, size_t from, size_t to);
std::shared_ptr<const raw::RetirementWrites> captureWrittenRange(RMCommandBuffer *cb, size_t from, size_t to);
void markWrittenRange(RMCommandBuffer *cb, size_t from, size_t to);
void completionLoop();

// RawMetalSurfaceWrites.mm
void surfaceConverted(IOSurfaceRef s, bool made);  // a planar or YCbCr texture over `s` made, or gone
uint64_t surfaceWrites(IOSurfaceRef s);
void noteSurfaceWrite(IOSurfaceRef s);
void noteSurfaceWrites(RMCommandBuffer *cb, size_t from, size_t to);  // after [from, to) of `cb` was submitted

// RawMetalDeviceAnswers.mm: the per-kind capability answers (raw_device_answers.h) as methods of the device class `cls`, where
// it has none of its own; and an argument query's answer
void installDeviceAnswers(Class cls);
uint64_t deviceAnswer(SEL selector, uint64_t argument);

// RawMetalQueuePriority.mm
void queuePriorityAdd(RMQueue *queue);
void queuePriorityRemove(RMQueue *queue);

// RawMetalSubmit.mm
void orderRefused(const char *what, int kr);
void commitBuffer(RMCommandBuffer *cb);
void submitLoop();
bool admitComputeDispatch(RMCommandBuffer *cb, size_t at, const raw::ComputeKernelSet<raw::ComputeState> *kernels, const char **why);
int computeReadiness(RMCommandBuffer *cb, const RMSegment &range);

// RawMetalRates.mm
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void rawStallTick();
void watchFetchBarrierWay(raw::Device *dev);
void watchKeylineProbe();
// RawMetalGlassProbe.mm
void watchGlassProbe(raw::Device *dev);
void glassProbeCommitted(RMCommandBuffer *cb);
bool rmWholePrograms();
bool rmZeroNewBuffers();
#endif
