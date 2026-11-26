// SPDX-License-Identifier: BSD-2-Clause-Patent
// J3: the AppleJPEGDriver service. CMPhoto (ImageIO's JPEG decodes, UIKit's eager decompression, Photos and its
// thumbnails) and JPEGH1.videodecoder (VideoToolbox 'jpeg') open IOServiceMatching("AppleJPEGDriver") and talk to its
// AppleJPEGDriverUserClient; this puts NVJPG behind that pair. The contract (drivers/TegraNVJPG/CONTRACT.md) was read
// from 21H461: the callers in the dyld cache and Apple's com.apple.driver.AppleJPEGDriver 6.6.2 in the kernelcache.
//
// The classes are ours and carry Apple's names: com.apple.driver.AppleJPEGDriver is not in this kernel's prelinked set
// (nothing among its roots depends on it, and no prelinked kext's OSBundleLibraries names it), so the names are free.
// Matching (IOServiceMatching is by class) and the sandbox (iokit-open filters by user-client class) go by them, so
// every process Apple's own profiles let open the client gets it, and no other. The service's registry name is
// TegraNVJPG, not its class name: JPEGH1.videoencoder registers VideoToolbox's hardware 'jpeg' encoders only if
// IOServiceNameMatching("AppleJPEGDriver") finds a service, and NVJPG's encoder is not driven here.
//
//   sel 1  decode (0x58 bytes in and out), sel 5 decode with Apple's codec extensions (0x1000), sel 7 the 2024 decode
//          (0xda0; every decode on 24B119): the JPEG file in the
//          source IOSurface (looked up in the caller's task, like Apple's driver), the picture into the destination
//          IOSurface in its pixel format ('420f', 'yuvf', 'BGRA', 'RGBA', 32ARGB; '422f' and '444f' through VIC), at the
//          request's DCT scale and offset. The kernel parses the file (TegraNVJPGJpeg.h; Apple's hardware parses the
//          stream itself, so userland sends no tables) and plans the decode (TegraNVJPGRequest.h) in the calling
//          thread: a refusal is the call's own
//          result, kIOReturnUnsupported for what NVJPG does not do (TegraNVJPGTraits) and
//          kIOReturnBadArgument for a request that contradicts the file or the surfaces; CMPhoto then decodes that
//          image on the CPU, JPEGH1 that frame in software. Accepted requests are queued for the engine thread in
//          order. Without a callback (+0x30) the call sleeps until its decode is done and returns its status; with one
//          it returns once queued and the result comes back as an IOKit async completion on the port registered with
//          IOConnectSetNotificationPort: callback(refcon +0x38, status, context +0x40).
//   sel 0, 2  no-ops (Apple's). sel 3, 4, 6 encode: kIOReturnUnsupported (CMPhoto's unified
//          encoder falls back to its software encoder, __encodeBuffer 0x1aaeebef4). sel 8, 9 power on/off: Apple's
//          entitlement com.apple.applejpegdriver.poweron, else kIOReturnError, as Apple's; with it a no-op.
//
// Per decode, in the calling thread: the source wired (IOSurface::prepare), its headers read and parsed, its scan
// copied into a queue slot's buffer (the only CPU work proportional to the file: compressed bytes, a few per cent of
// the picture's). The engine thread maps a destination at its first decode only, cleans and invalidates it, runs the
// decode (host1x OP_DONE syncpoint, by interrupt) and cleans and invalidates it again for CPU readers.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <os/log.h>
#include <sys/proc.h>
#include <string.h>
#include "../TegraVIC/TegraVICSurface.h"
#include "TegraNVJPG.h"
#include "TegraNVJPGMapping.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"
#include "../../kits/SwitchMath/SwitchMath.h"
#include "../../kits/SwitchTime/SwitchTime.h"
#include "TegraNVJPGRepack.h"
#include "TegraNVJPGRequest.h"
#include "TegraNVJPGRoute.h"
#include "TegraNVJPGStats.h"
#include "../TegraVIC/SwitchVICDecodeOutput.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

// TegraGM20B: a surface the GPU left block-linear after its last import (TegraGM20BRecord.h) has its texels moved back
// to rows first, as that import's release did before the move was deferred: NVJPG reads and writes the pages as rows
extern "C" void tegra_gm20b_surface_engine_rows(OSObject *surface) __attribute__((weak));
extern "C" void *tegra_gm20b_surface_hold(uint64_t pa, int *tiled) __attribute__((weak));
extern "C" void tegra_gm20b_surface_unhold(void *hold) __attribute__((weak));
extern "C" int tegra_gm20b_surface_layout(uint64_t pa, struct tgm_layout *out) __attribute__((weak));
extern "C" int tegra_gm20b_surface_compressed(uint64_t pa) __attribute__((weak));

extern "C" {
vm_offset_t phystokv(uint64_t pa);
}

class AppleJPEGDriverUserClient;

namespace {
constexpr uint32_t kMaxConnections = 1000;    // Apple's limit (newUserClient: opens minus closes >= 1001 refused)
constexpr uint32_t kQueueMax = 16;            // decodes in flight: each holds a scan slot from its copy to its run
constexpr uint64_t kSinkBytes = 1ULL << 20;   // MCU padding a plan may write past a destination (15 rows of 64 KiB)
constexpr uint32_t kHeaderChunk = 128 << 10;  // headers read first (APP segments: EXIF, ICC, XMP)
constexpr uint32_t kHeaderMax = 4 << 20;      // ... and at most this far, if the first chunk ends before SOS
constexpr uint64_t kScanMax = 64ULL << 20;    // the longest scan a slot takes: its buffer stays wired for the boot

volatile uint32_t gConnections;
IOLock *gQueueLock;
SwitchNVJPGScan gScans[kQueueMax];
uint32_t gScansFree = (1u << kQueueMax) - 1;  // bit n: gScans[n]; gQueueLock

IOReturn ioreturn(int err)
{
    switch (err) {
    case 0:
        return kIOReturnSuccess;
    case -12:
        return kIOReturnNoMemory;
    case -22:
        return kIOReturnBadArgument;
    case -60:
        return kIOReturnTimeout;
    case -19:
        return kIOReturnNotReady;
    default:
        return kIOReturnIOError;
    }
}

#if TEGRA_NVJPG_STATS
// HW Test's image.jpeg_* route: a selector 7 call with kRouteMagic at +0xcac and no source surface answers the
// caller's TegraNVJPGRouteCounts, through the selector CMPhoto's own decodes use, which the app's sandbox admits where
// it may deny a registry or sysctl read.
constexpr uint32_t kRouteMagic = 0x5050;  // 'PP', as TegraGM20BMethodPaths
#if TEGRA_NVJPG_STATS
// A selector 7 decode with this at +0xcac, the tile mode at +0xcb0 and the block height code at +0xcb4: HW Test's
// image.nvjpg_tile_probe (TegraNVJPGRoute.h tegra_nvjpg_route_tile_probe)
constexpr uint32_t kTileProbeMagic = 0x5054;
#endif
static_assert(sizeof(TegraNVJPGRouteCounts::routes) / sizeof(uint64_t) == TEGRA_NVJPG_ROUTE_COUNT,
              "a route count for every route");
#endif

uint32_t gFirstLogged;  // bit n: a decode into TEGRA_NVJPG_OUT_n has been logged (engine thread)

// `why` is a literal: it is the refusal's key as well as its text. `format`: the TEGRA_NVJPG_OUT_* asked for, or
// TEGRA_NVJPG_OUT_COUNT.
void refused(const char *why, uint32_t format)
{
#if TEGRA_NVJPG_STATS
    tegra_nvjpg_count_refused(proc_selfpid(), format);
#else
    (void)format;
#endif
    switch_refusal("TegraNVJPG", why, "decode refused, the caller decodes on the CPU: %s", why);
}

struct Job {
    Job *next;
    AppleJPEGDriverUserClient *client;  // retained by async jobs until their completion is sent
    TegraVICSurface dst;
    switch_gpu_backing *backing;
    SwitchNVJPGScan *scan;  // the scan's copy, the job's alone until the engine has read it
    TegraNVJPGPlan plan;
    TegraNVJPGJob engine;
    uint64_t cpuUs;
    bool async;
    OSAsyncReference64 ref;
    uint64_t context;
    IOReturn status;
    uint32_t bytesUsed, cycles;
    uint64_t engineUs;
    int pid;
    bool done;
    // The engine thread's decode of it (engineThread). NVJPG's part runs from its start (OnEngine) until its signal; a
    // re-pack queued behind it (Repacking) until the re-pack's. `woken`, `engineEnded` and `repacked` are the signals',
    // set and read under gQueueLock; the rest is the engine thread's alone.
    enum class Phase : uint8_t { OnEngine, Repacking } phase;
    bool woken, engineEnded, repacked;
    void *hold;                         // the destination's layout, held until the decode has ended (routeJob)
    const SwitchNVJPGMapping *mapping;  // a direct decode's destination pages, as NVJPG writes them
    TegraNVJPGRepackRun repack;
    int engineResult;
    nvjpg_dec_status engineStatus;
    uint64_t engineBusy;
#if TEGRA_NVJPG_STATS
    bool tileProbe;  // HW Test's tile probe (kTileProbeMagic): tileMode and tileGob go to the setup unchanged
    uint32_t tileMode, tileGob;
    uint64_t cancelsBefore;  // tegra_nvjpg_cancellations() as the job began
#endif

    union {
        TegraNVJPGDecodeRequest base;
        TegraNVJPGDecodeRequestExt ext;
        TegraNVJPGDecodeRequest2024 v2024;
    } request;  // the caller's copy, in the job's one allocation
};

enum RequestKind {
    kSelector1,
    kSelector5,
    kSelector7
};

Job *gHead, *gTail;

SwitchNVJPGScan *takeScan(uint64_t bytes)
{
    IOLockLock(gQueueLock);
    while (!gScansFree)
        IOLockSleep(gQueueLock, &gScansFree, THREAD_UNINT);
    const uint32_t slot = switch_nvjpg_scan_pick(gScans, gScansFree, bytes);
    gScansFree &= ~(1u << slot);
    IOLockUnlock(gQueueLock);
    return &gScans[slot];
}

void giveScan(Job *job)
{
    if (!job->scan)
        return;
    IOLockLock(gQueueLock);
    gScansFree |= 1u << (job->scan - gScans);
    IOLockWakeup(gQueueLock, &gScansFree, false);
    IOLockUnlock(gQueueLock);
    job->scan = nullptr;
}

void dropJob(Job *job)
{
    giveScan(job);
    if (job->backing)
        switch_gpu_backing_release(job->backing);
    tegra_vic_surface_release(&job->dst);
    IODelete(job, Job, 1);
}

// The CPU's part of a grey picture into '420f': neutral chroma over the picture's rows, written through the physical
// aperture of the destination's wired pages, cleaned to DRAM for readers that do not snoop.
void fillChroma(const TegraNVJPGPlan &p, const uint64_t *addresses, uint32_t pages)
{
    const auto fill = [&] {
        for (uint64_t row = 0; row < p.fillRows; row++) {
            for (uint64_t at = p.fillStart + row * p.fillPitch, end = at + p.fillRowBytes; at < end;) {
                const uint32_t page = static_cast<uint32_t>(at >> SWITCH_HW_PAGE_SHIFT);
                const uint64_t stop = SWITCH_MIN(switch_align_down(at, SWITCH_HW_PAGE_SIZE) + SWITCH_HW_PAGE_SIZE, end);
                if (page >= pages || !switch_dma_pa_managed(addresses[page]))
                    return;  // the plan bounded it
                uint8_t *va = reinterpret_cast<uint8_t *>(phystokv(addresses[page])) + (at & SWITCH_HW_PAGE_MASK);
                memset(va, 0x80, stop - at);
                switch_dcache_clean_lines(va, stop - at);
                at = stop;
            }
        }
    };
    fill();
    switch_dcache_sync();
}

TegraNVJPGSurface surfaceDesc(const TegraVICSurfaceDesc &v)
{
    TegraNVJPGSurface d = {};
    d.fourcc = v.fourcc, d.width = v.width, d.height = v.height, d.planes = v.planes;
    for (uint32_t i = 0; i < 2; i++) {
        d.planeOffset[i] = v.planeOffset[i];
        d.planeBytesPerRow[i] = v.planeBytesPerRow[i];
        d.planeHeight[i] = v.planeHeight[i];
    }
    d.length = v.length;
    d.protection = v.protection;
    return d;
}

// The destination as GM20B laid it out (TegraGM20BTiled.cpp decides once per surface), held until the decode and any
// re-pack have ended so that no import moves its texels meanwhile. The caller has moved the surface's seed first: a GPU
// sampler whose first ask comes after this read then finds a written surface and keeps it pitch, instead of tiling in
// place the rows this decode is writing. NULL with the plan moved onto that layout (TegraNVJPGRoute.h), or why it
// cannot be written.
const char *routeJob(Job *job, const SwitchNVJPGBacking &target, void **hold, uint32_t *route)
{
    *hold = nullptr;
    TegraNVJPGDestinationLayout layout = {};
    if (tegra_gm20b_surface_hold && tegra_gm20b_surface_unhold && tegra_gm20b_surface_layout) {
        int tiled = 0;
        *hold = tegra_gm20b_surface_hold(target.pages[0], &tiled);
        layout.held = tiled != 0;
        layout.described = layout.held && tegra_gm20b_surface_layout(target.pages[0], &layout.layout) == 1;
        layout.compressed =
            layout.described && tegra_gm20b_surface_compressed && tegra_gm20b_surface_compressed(target.pages[0]);
    }
    const TegraNVJPGSurface d = surfaceDesc(job->dst.desc);
#if TEGRA_NVJPG_STATS
    const char *why = job->tileProbe
                          ? tegra_nvjpg_route_tile_probe(&job->request.base, job->dst.desc.length,
                                                         kTegraNVJPGOutputBytes, &layout, job->tileMode, job->tileGob,
                                                         &job->plan, &job->engine.setup, route)
                          : tegra_nvjpg_route(&job->request.base, &d, kTegraNVJPGOutputBytes,
                                              job->dst.memory != nullptr, &layout, &job->plan, &job->engine.setup,
                                              route);
    if (!why) {
        tegra_nvjpg_note_route(*route, layout.held, layout.layout.row_bytes, layout.layout.y_log2, target.count,
                               &job->engine.setup, job->plan.extent.width, job->plan.extent.height);
        tegra_nvjpg_count_route(job->pid, *route);
    }
#else
    const char *why = tegra_nvjpg_route(&job->request.base, &d, kTegraNVJPGOutputBytes, job->dst.memory != nullptr,
                                        &layout, &job->plan, &job->engine.setup, route);
#endif
    return why;
}

IOReturn ended(Job *job, int error, const nvjpg_dec_status &status, uint64_t busy)
{
#if TEGRA_NVJPG_STATS
    if (tegra_nvjpg_cancellations() != job->cancelsBefore)
        tegra_nvjpg_count_cancelled(job->pid);
#endif
    IOReturn ret = ioreturn(error);
    if (!ret && (status.error_status || !status.bytes_offset))
        ret = kIOReturnIOError;
    // AppleJPEGDriver ends its destination's write with IOSurface's device write end, as VIC does: the seed moves, so a
    // surface a decode wrote never reads as one nothing wrote (TegraGM20BRecord.h) and seed-keyed caches see the picture
    if (!ret)
        tegra_vic_surface_written(&job->dst);
    job->engineUs = busy;
    job->bytesUsed = status.bytes_offset;
    job->cycles = status.reserved0;  // the cycle count (DUMP_CYCLE_COUNT)
    if (ret == kIOReturnIOError && status.error_status) {
        IOLog("TegraNVJPG: decode error %#x after %u bytes (MCU %u,%u) of a %ux%u picture\n", status.error_status,
              status.bytes_offset, status.mcu_x, status.mcu_y, static_cast<unsigned>(job->engine.setup.frame_width),
              static_cast<unsigned>(job->engine.setup.frame_height));
    }
    return ret;
}

// NVJPG's end of the decode in flight, from its fence's work loop or the falcon line's: the engine thread finishes it.
void jobEngineEnded(void *context)
{
    auto *job = static_cast<Job *>(context);
    IOLockLock(gQueueLock);
    job->engineEnded = true;
    job->woken = true;
    IOLockWakeup(gQueueLock, &gHead, false);
    IOLockUnlock(gQueueLock);
}

// The end of the re-pack queued behind it, from VIC's or the copy engine's completion.
void jobRepackEnded(void *context)
{
    auto *job = static_cast<Job *>(context);
    IOLockLock(gQueueLock);
    job->repacked = true;
    job->woken = true;
    IOLockWakeup(gQueueLock, &gHead, false);
    IOLockUnlock(gQueueLock);
}

// Engine thread: whether the re-pack queued behind NVJPG has yet to signal its end.
bool repackPending(Job *job)
{
    if (!job->repack.queued)
        return false;
    IOLockLock(gQueueLock);
    const bool repacked = job->repacked;
    IOLockUnlock(gQueueLock);
    return !repacked;
}

// Engine thread, once nothing is pending: a re-packed decode's result.
IOReturn completeRepack(Job *job)
{
    const int error = tegra_nvjpg_repack_end(&job->repack, job->engineResult);
    const nvjpg_dec_status &status = job->engineStatus;
    if (!error && job->plan.fillChroma && !status.error_status && status.bytes_offset && job->backing) {
        const SwitchNVJPGBacking target = switch_nvjpg_backing(job->backing);
        fillChroma(job->plan, target.pages, target.count);
    }
    return ended(job, error, status, job->engineBusy);
}

// Engine thread: a VIC ('422f'/'444f') or Copy Engine (layout-staged) re-packed decode (TegraNVJPGRepack.h) started.
// True: it is in flight; false: it has ended, job->status its result.
bool beginRepack(Job *job)
{
    const TegraNVJPGSignal ended = {jobEngineEnded, job}, repacked = {jobRepackEnded, job};
    const char *why = nullptr;
    const int error = tegra_nvjpg_repack_start(&job->repack, &job->engine, &job->plan, &job->request.base, &job->dst,
                                               job->backing, job->scan->address, &ended, &repacked, &why);
    if (!error) {
        job->phase = Job::Phase::OnEngine;
        return true;
    }
    if (why) {
        refused(why, job->plan.target.format);
        job->status = kIOReturnUnsupported;
        return false;
    }
    job->engineResult = error;
    job->phase = Job::Phase::Repacking;
    if (repackPending(job))
        return true;
    job->status = completeRepack(job);
    return false;
}

// Engine thread: NVJPG's direct decode has ended and is finished.
IOReturn completeDirect(Job *job)
{
    const TegraNVJPGPlan &p = job->plan;
    // The CPU may have pulled destination lines in while NVJPG wrote: readers must see DRAM.
    switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, job->mapping->pages, job->mapping->count);
    switch_dcache_sync();
    const nvjpg_dec_status &status = job->engineStatus;
    if (!job->engineResult && p.fillChroma && !status.error_status && status.bytes_offset)
        fillChroma(p, job->mapping->pages, job->mapping->count);
    return ended(job, job->engineResult, status, job->engineBusy);
}

// Engine thread: NVJPG straight into the destination's pages, started. True: in flight; false: ended (job->status).
bool beginDirect(Job *job, const SwitchNVJPGBacking &target, uint32_t extent)
{
    const TegraNVJPGPlan &p = job->plan;
    const char *why = nullptr;
    job->mapping = switch_nvjpg_mapping(target, extent, p.writeEnd, &why, job->pid);
    if (!job->mapping) {
        refused(why, p.target.format);
        job->status = kIOReturnUnsupported;
        return false;
    }
    // NVJPG does not snoop the CPU's caches: no dirty line of the destination may be written back over the picture.
    switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, job->mapping->pages, job->mapping->count);
    switch_dcache_sync();
    job->engine.bitstream = job->scan->address;
    for (uint32_t i = 0; i < p.planes; i++)
        job->engine.plane[i] = job->mapping->address + p.planeStart[i];
    const TegraNVJPGSignal ended = {jobEngineEnded, job};
    tegra_nvjpg_lock();
    const int error = tegra_nvjpg_start(&job->engine, &ended);
    if (!error) {
        job->phase = Job::Phase::OnEngine;
        return true;
    }
    tegra_nvjpg_unlock();
    job->engineResult = error;
    job->status = completeDirect(job);
    return false;
}

// Engine thread: one decode started. VIC's re-pack writes planar YUV, which GM20B never lays out block-linear, through
// VIC's own view of the destination. True: in flight; false: ended, job->status its result.
bool beginJob(Job *job)
{
#if TEGRA_NVJPG_STATS
    job->cancelsBefore = tegra_nvjpg_cancellations();
#endif
    if (job->plan.repack == TEGRA_NVJPG_REPACK_VIC)
        return beginRepack(job);
    uint32_t extent = static_cast<uint32_t>(switch_div_round_up(job->dst.desc.length, SWITCH_HW_PAGE_SIZE));
    const SwitchNVJPGBacking target = switch_nvjpg_backing(job->backing);
    if (target.count < extent) {
        refused("destination longer than its backing", job->plan.target.format);
        job->status = kIOReturnUnsupported;
        return false;
    }
    tegra_vic_surface_written(&job->dst);
    uint32_t route = TEGRA_NVJPG_ROUTE_ROWS;
    if (const char *why = routeJob(job, target, &job->hold, &route)) {
        refused(why, job->plan.target.format);
        job->status = kIOReturnUnsupported;
        return false;
    }
    return job->plan.repack ? beginRepack(job) : beginDirect(job, target, extent);
}

// Engine thread, after a signal: the decode in flight carried on. True: it has ended, job->status its result.
bool advanceJob(Job *job)
{
    IOLockLock(gQueueLock);
    const bool engineEnded = job->engineEnded;
    IOLockUnlock(gQueueLock);
    if (job->phase == Job::Phase::OnEngine) {
        if (!engineEnded)
            return false;
        job->engineResult = tegra_nvjpg_finish(&job->engine, &job->engineStatus, &job->engineBusy);
        tegra_nvjpg_unlock();
        if (!job->plan.repack) {
            job->status = completeDirect(job);
            return true;
        }
        job->phase = Job::Phase::Repacking;
    }
    if (repackPending(job))
        return false;
    job->status = completeRepack(job);
    return true;
}

void finish(Job *job);

void giveHold(Job *job)
{
    if (job->hold) {
        tegra_gm20b_surface_unhold(job->hold);
        job->hold = nullptr;
    }
}

// Engine thread: the ended decode's layout hold given back, its scan slot freed, its counts kept, its client told.
void conclude(Job *job)
{
    giveHold(job);
    giveScan(job);
#if TEGRA_NVJPG_STATS
    tegra_nvjpg_count_job(job->pid, job->plan.target.format, job->status != 0, job->engineUs, job->cpuUs,
                          static_cast<uint64_t>(job->plan.extent.width) * job->plan.extent.height,
                          static_cast<uint64_t>(job->engine.setup.bitstream_size));
#endif
    const uint32_t format = 1u << job->plan.target.format;
    const bool first = (job->status == 0) && (gFirstLogged & format) == 0u;
    if (first)
        gFirstLogged |= format;
    if (first) {
        IOLog("TegraNVJPG: first hardware decode done: %ux%u to %ux%u (format %u, 1/%u) in %llu us engine (%u "
              "cycles), %llu us CPU\n",
              static_cast<unsigned>(job->engine.setup.frame_width),
              static_cast<unsigned>(job->engine.setup.frame_height), job->plan.extent.width, job->plan.extent.height,
              job->plan.target.format, 1u << job->plan.target.scale, job->engineUs, job->cycles, job->cpuUs);
    }
#if TEGRA_NVJPG_STATS
    tegra_nvjpg_stats_line();
#endif
    finish(job);
}

// One decode at a time, NVJPG's part and its re-pack ending by signal (jobEngineEnded, jobRepackEnded): between its
// start and its ends this thread sleeps on the queue's lock, not on the engine, and a signal only wakes it to carry the
// decode on (advanceJob). A queued decode starts once the one before it has ended.
void engineThread(void *, wait_result_t)
{
    Job *current = nullptr;
    for (;;) {
        Job *next = nullptr;
        IOLockLock(gQueueLock);
        while (current ? !current->woken : !gHead)
            IOLockSleep(gQueueLock, &gHead, THREAD_UNINT);
        if (current) {
            current->woken = false;
        } else {
            next = gHead;
            gHead = next->next;
            if (!gHead)
                gTail = nullptr;
        }
        IOLockUnlock(gQueueLock);
        if (next) {
            current = next;
            if (beginJob(current))
                continue;
        } else if (!advanceJob(current)) {
            continue;
        }
        conclude(current);
        current = nullptr;
    }
}

void queueJob(Job *job)
{
    IOLockLock(gQueueLock);
    job->next = nullptr;
    if (gTail) {
        gTail->next = job;
    } else {
        gHead = job;
    }
    gTail = job;
    IOLockWakeup(gQueueLock, &gHead, false);
    IOLockUnlock(gQueueLock);
}

// The source: wired for the copy, its headers parsed from a first chunk, the scan copied into a queue slot's buffer
// (EOI appended when the file has none). The scan runs from the SOS header to EOI or the data's end (the source's size
// may be its whole allocation: trailing bytes are cut at EOI).
const char *readSource(TegraVICSurface *src, uint64_t size, TegraNVJPGImage *img, Job *job, IOReturn *ret)
{
    *ret = kIOReturnBadArgument;
    if (size < 4 || size > src->desc.length)
        return "source size";
    const uint32_t pageCount = static_cast<uint32_t>(switch_div_round_up(size, SWITCH_HW_PAGE_SIZE));
    uint64_t *pages = IONewData(uint64_t, pageCount);
    if (!pages) {
        *ret = kIOReturnNoMemory;
        return "no memory";
    }
    uint32_t count = 0;
    *ret = tegra_vic_surface_prepare(src, size, pages, pageCount, &count);
    IODeleteData(pages, uint64_t, pageCount);
    if (*ret == kIOReturnNotAligned)
        *ret = kIOReturnUnsupported;  // never NotAligned: CMPhoto would copy and retry
    if (*ret)
        return "source not wired";
    const char *why = nullptr;
    uint32_t chunk = static_cast<uint32_t>(size < kHeaderChunk ? size : kHeaderChunk);
    uint8_t *head = nullptr;
    for (;;) {
        head = IONewData(uint8_t, chunk);
        if (!head || src->memory->readBytes(0, head, chunk) != chunk) {
            *ret = kIOReturnNoMemory;
            why = "source read";
            break;
        }
        why = tegra_nvjpg_parse_headers(head, chunk, img);
        if (!why || chunk == size || chunk >= kHeaderMax)
            break;
        IODeleteData(head, uint8_t, chunk);
        head = nullptr;
        // headers past the first chunk: read further
        chunk = static_cast<uint32_t>(size < kHeaderMax ? size : kHeaderMax);
    }
    if (head)
        IODeleteData(head, uint8_t, chunk);
    if (why) {
        if (*ret != kIOReturnNoMemory)
            *ret = kIOReturnUnsupported;
        tegra_vic_surface_complete(src);
        return why;
    }
    const uint64_t scan = size - img->scanOffset;
    if (scan + 2 > kScanMax) {
        tegra_vic_surface_complete(src);
        *ret = kIOReturnUnsupported;
        return "scan larger than 64 MiB";
    }
    job->scan = takeScan(scan + 2);
    const int fit = switch_nvjpg_scan_fit(job->scan, scan + 2);
    if (fit || src->memory->readBytes(img->scanOffset, job->scan->buffer.va, scan) != scan) {
        tegra_vic_surface_complete(src);
        *ret = fit == -28 ? kIOReturnUnsupported : kIOReturnNoMemory;
        return fit == -28 ? "input window full" : "scan copy";
    }
    tegra_vic_surface_complete(src);
    uint8_t *bytes = job->scan->buffer.va;
    TegraNVJPGImage scanned = *img;
    scanned.scanOffset = 0;
    why = tegra_nvjpg_scan_end(bytes, scan, &scanned);
    if (why) {
        *ret = kIOReturnUnsupported;
        return why;
    }
    uint32_t used = scanned.scanEnd;
    if (!scanned.eoi)
        bytes[used++] = 0xff, bytes[used++] = 0xd9;
    switch_dcache_clean(bytes, used);
    img->scanEnd = img->scanOffset + scanned.scanEnd;
    img->eoi = scanned.eoi;
    job->engine.setup.bitstream_size = static_cast<int>(used);
    *ret = kIOReturnSuccess;
    return nullptr;
}

TegraNVJPGAbility ability()
{
    const TegraNVJPGTraits *t = tegra_nvjpg_traits();
    uint32_t formats = t->formats;
    if (!switch_vic_decode_output_ready())
        formats &= ~(1u << TEGRA_NVJPG_OUT_NV16 | 1u << TEGRA_NVJPG_OUT_NV24);
    return TegraNVJPGAbility{formats, t->chroma, t->scales, t->clipped ? 1u : 0u, t->greyRgb ? 1u : 0u};
}
}  // namespace

class AppleJPEGDriverUserClient : public IOUserClient {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleJPEGDriverUserClient)

        IOLock *lock_ = nullptr;
    task_t task_ = nullptr;
    mach_port_t port_ = MACH_PORT_NULL;
    uint32_t pending_ = 0;
    bool counted_ = false, powerEntitled_ = false;

    IOReturn decode(const void *in, uint32_t inSize, void *out, uint32_t *outSize, RequestKind kind);

public:
    bool setUp(task_t task);
    void finished(Job *job);
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleJPEGDriverUserClient, IOUserClient)

    namespace
{
    void finish(Job * job)
    {
        job->client->finished(job);
    }
}  // namespace

bool AppleJPEGDriverUserClient::setUp(task_t task)
{
    task_ = task;
    counted_ = true;
    // Apple's client (init 0x52d1c) reads this entitlement for selectors 8 and 9 only.
    const OSObject *entitled = IOUserClient::copyClientEntitlement(task, "com.apple.applejpegdriver.poweron");
    powerEntitled_ = entitled == kOSBooleanTrue;
    OSSafeReleaseNULL(entitled);
    return (lock_ = IOLockAlloc()) != nullptr;
}

// Engine thread: a job is done. Async: the completion to the registered port; sync: wake the caller.
void AppleJPEGDriverUserClient::finished(Job *job)
{
    if (job->async) {
        io_user_reference_t argument = job->context;
        IOUserClient::sendAsyncResult64(job->ref, job->status, &argument, 1);
        dropJob(job);
        IOLockLock(lock_);
        pending_--;
        IOLockWakeup(lock_, &pending_, false);
        IOLockUnlock(lock_);
        release();
        return;
    }
    IOLockLock(lock_);
    job->done = true;
    IOLockWakeup(lock_, job, false);
    IOLockUnlock(lock_);
}

IOReturn AppleJPEGDriverUserClient::decode(const void *in, uint32_t inSize, void *out, uint32_t *outSize,
                                           RequestKind kind)
{
    const uint64_t began = switch_now_us();
    Job *job = IONewZero(Job, 1);
    if (!job)
        return kIOReturnNoMemory;
    memcpy(&job->request, in, inSize);
    const auto *req = &job->request;
#if TEGRA_NVJPG_STATS
    if (kind == kSelector7 && req->v2024.rsvdcac == kTileProbeMagic) {
        job->tileProbe = true;
        memcpy(&job->tileMode, req->v2024.rsvd, sizeof(job->tileMode));
        memcpy(&job->tileGob, req->v2024.rsvd + 4, sizeof(job->tileGob));
    }
#endif
    const TegraNVJPGDecodeRequest &r = req->base;
    IOReturn ret = kIOReturnSuccess;
    const char *why = nullptr;
    TegraVICSurface src = {};
    TegraNVJPGImage img = {};
    if ((kind == kSelector5 && req->ext.crop) || (kind == kSelector7 && req->v2024.crop)) {
        ret = kIOReturnUnsupported;
        why = "cropped decode (crop rectangle)";
    }
    if (!ret && kind == kSelector7 &&
        (req->v2024.header_bytes || req->v2024.strip_index || req->v2024.strip_entropy_bytes)) {
        ret = kIOReturnUnsupported;
        why = "strip decode (the service publishes one core)";
    }
    // Surfaces in the caller's task, as Apple's driver looks them up (not the task that opened the connection).
    if (!ret && (tegra_vic_surface_lookup(r.src_id, current_task(), &src) ||
                 tegra_vic_surface_lookup(r.dst_id, current_task(), &job->dst))) {
        ret = kIOReturnBadArgument;
        why = "surface ID not found for this task";
    }
    if (!ret && tegra_gm20b_surface_engine_rows) {
        tegra_gm20b_surface_engine_rows(src.surface);
        tegra_gm20b_surface_engine_rows(job->dst.surface);
    }
    if (!ret && src.desc.protection) {
        ret = kIOReturnNotPermitted;  // Apple's 0xe00002e2 for a protected source
        why = "protected source";
    }
    if (!ret) {
        uint64_t size = r.src_size;
        if (src.desc.length < size)
            size = src.desc.length;
        why = readSource(&src, size, &img, job, &ret);
    }
    tegra_vic_surface_release(&src);
    if (!ret && job->dst.desc.length + kSinkBytes > kTegraNVJPGOutputBytes) {
        ret = kIOReturnUnsupported;
        why = "destination larger than the 255 MiB output window";
    }
    if (!ret) {
        const TegraNVJPGSurface d = surfaceDesc(job->dst.desc);
        const TegraNVJPGAbility a = ability();
        uint32_t planKind = 0;
        why = tegra_nvjpg_plan(&r, &img, &d, &a, kSinkBytes, &job->plan, &planKind);
        if (why)
            ret = planKind == TEGRA_NVJPG_PLAN_UNSUPPORTED ? kIOReturnUnsupported : kIOReturnBadArgument;
    }
    if (!ret && job->plan.repack && job->plan.repackBytes > kTegraNVJPGOutputBytes) {
        ret = kIOReturnUnsupported;
        why = "re-packed planes larger than the 256 MiB output mapping";
    }
    if (!ret) {
        const int bytes = job->engine.setup.bitstream_size;
        tegra_nvjpg_setup_image(&img, static_cast<uint32_t>(bytes), &job->engine.setup);
        const char *outWhy = tegra_nvjpg_setup_output(&img, &job->plan.target, ability().clipped == 0,
                                                      &job->engine.setup, &job->plan.extent);
        if (outWhy) {
            ret = kIOReturnUnsupported;
            why = outWhy;
        }
        job->engine.planes = job->plan.planes;
        job->engine.pixels = static_cast<uint64_t>(img.mcusX) * img.mcuW * img.mcusY * img.mcuH;
        job->engine.selector = kind == kSelector1 ? 1 : kind == kSelector5 ? 5 : 7;
        job->engine.target = job->plan.target.format;
    }
    if (!ret && job->plan.repack != TEGRA_NVJPG_REPACK_VIC) {
        job->backing = switch_vic_surface_backing(&job->dst);
        if (!job->backing) {
            ret = kIOReturnNoMemory;
            why = "destination backing";
        }
    }
    if (ret) {
        if (why) {
            uint32_t planes = 0;
            refused(why, job->dst.surface ? tegra_nvjpg_format_of(job->dst.desc.fourcc, img.components == 1, &planes)
                                          : TEGRA_NVJPG_OUT_COUNT);
        }
#if TEGRA_NVJPG_STATS
        tegra_nvjpg_stats_line();
#endif
        dropJob(job);
        return ret;
    }
    job->cpuUs = switch_now_us() - began;
    job->pid = proc_selfpid();
    job->async = r.callback != 0;
    job->context = r.context;
    IOLockLock(lock_);
    const mach_port_t port = port_;
    pending_++;
    IOLockUnlock(lock_);
    if (job->async) {
        IOUserClient::setAsyncReference64(job->ref, port, r.callback, r.refcon, task_);
        retain();  // the job holds the connection until its completion is sent
    }
    job->client = this;
    const bool async = job->async;
    queueJob(job);
    if (async)
        return kIOReturnSuccess;  // the job is the engine thread's now
    IOLockLock(lock_);
    while (!job->done)
        IOLockSleep(lock_, job, THREAD_UNINT);
    pending_--;
    IOLockWakeup(lock_, &pending_, false);
    IOLockUnlock(lock_);
    ret = job->status;
    // The output: the request back (CMPhoto reads Ext out+0x14/+0x18), with Apple's result words (startDecoder
    // 0x46330: +0x4c 0 for a decode, +0x50/+0x54 codec registers userland does not read); for selector 5 the engine's
    // time and cycles (startDecoderExt: +0x70 microseconds, +0x74 cycles); for selector 7 the region decoded at
    // +0x4c/+0x50 and the time at +0xc94 (startDecoder2024).
    if (out && *outSize >= inSize) {
        memcpy(out, req, inSize);
        uint32_t *words = static_cast<uint32_t *>(out);
        words[0x4c / 4] = kind == kSelector7 ? r.width : 0;
        words[0x50 / 4] = kind == kSelector7 ? r.height : 0;
        words[0x54 / 4] = 0;
        if (kind == kSelector5) {
            words[0x70 / 4] = static_cast<uint32_t>(job->engineUs);
            words[0x74 / 4] = job->cycles;
        } else if (kind == kSelector7) {
            words[0xc94 / 4] = static_cast<uint32_t>(job->engineUs);
        }
        *outSize = inSize;
    }
    dropJob(job);
    return ret;
}

IOReturn AppleJPEGDriverUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                                   IOExternalMethodDispatch *dispatch, OSObject *target,
                                                   void *reference)
{
    (void)dispatch, (void)target, (void)reference;
    if (!a || a->structureInputDescriptor || a->structureOutputDescriptor || a->scalarInputCount ||
        a->scalarOutputCount)
        return kIOReturnBadArgument;
    // Apple's IOExternalMethodDispatch2022 table: exact struct sizes, in == out.
    static const uint32_t kSize[10] = {0, 0x58, 0, 0x58, 0x1000, 0x1000, 0xda0, 0xda0, 0, 0};
    if (selector >= 10)
        return kIOReturnBadArgument;
    if (a->structureInputSize != kSize[selector] ||
        (a->structureOutputSize != kSize[selector] && a->structureOutputSize != 0))
        return kIOReturnBadArgument;
    switch (selector) {
    case 0:
    case 2:
        return kIOReturnSuccess;
    case 1:
    case 5:
    case 7: {
#if TEGRA_NVJPG_STATS
        const auto *route = static_cast<const TegraNVJPGDecodeRequest2024 *>(a->structureInput);
        if (selector == 7 && route->rsvdcac == kRouteMagic && !route->base.src_id) {
            if (a->structureOutputSize != kSize[7])
                return kIOReturnBadArgument;
            auto *out = static_cast<uint64_t *>(a->structureOutput);
            bzero(out, kSize[7]);
            TegraNVJPGRouteCounts counts;
            tegra_nvjpg_counts_of(proc_selfpid(), &counts);
            static_assert(sizeof(counts) <= 0xda0, "route counts fit the reply");
            memcpy(out, &counts, sizeof(counts));
            return kIOReturnSuccess;
        }
#endif
        if (!tegra_nvjpg_ready())
            return kIOReturnNotReady;
        uint32_t outSize = a->structureOutputSize;
        const RequestKind kind = selector == 1 ? kSelector1 : selector == 5 ? kSelector5 : kSelector7;
        const IOReturn ret =
            decode(a->structureInput, a->structureInputSize, outSize ? a->structureOutput : nullptr, &outSize, kind);
        a->structureOutputSize = outSize;
        return ret;
    }
    case 8:
    case 9:
        return powerEntitled_ ? kIOReturnSuccess : kIOReturnError;
    default:
        return kIOReturnUnsupported;  // 3, 4, 6 encode
    }
}

// Apple's (0x531f4): the port is kept whatever the type; a second, different port is refused.
IOReturn AppleJPEGDriverUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
    (void)type, (void)refCon;
    IOLockLock(lock_);
    const bool busy = port_ != MACH_PORT_NULL && port_ != port;
    const bool kept = port_ == port;
    if (!busy)
        port_ = port;
    IOLockUnlock(lock_);
    // A refused port's right is the MIG server's to destroy: it does so when this returns an error.
    if (busy)
        return kIOReturnError;
    // Each registration brings its own right: a second one to the port already kept is released here
    if (kept)
        IOUserClient::releaseNotificationPort(port);
    return kIOReturnSuccess;
}

// Queued decodes hold the connection; closing waits for them (each is bounded by the engine), so no completion
// outlives the port it answers on. Sync callers close right after their call returns.
IOReturn AppleJPEGDriverUserClient::clientClose()
{
    IOLockLock(lock_);
    while (pending_)
        IOLockSleep(lock_, &pending_, THREAD_UNINT);
    IOLockUnlock(lock_);
    terminate();
    return kIOReturnSuccess;
}

void AppleJPEGDriverUserClient::free()
{
    if (port_ != MACH_PORT_NULL)
        IOUserClient::releaseNotificationPort(port_);
    port_ = MACH_PORT_NULL;
    if (lock_)
        IOLockFree(lock_);
    lock_ = nullptr;
    if (counted_)
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
    IOUserClient::free();
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class AppleJPEGDriver : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(AppleJPEGDriver)

    public : IOReturn newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
                                    IOUserClient **handler) APPLE_KEXT_OVERRIDE;

    IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE
    {
        (void)properties;
        return kIOReturnUnsupported;
    }
};

OSDefineMetaClassAndStructors(AppleJPEGDriver, IOService)

    // Apple's (0x43f08) ignores `type`, securityID and properties and checks no entitlement: any process the sandbox
    // lets open the client gets one, up to 1000 open at a time.
    IOReturn AppleJPEGDriver::newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
                                            IOUserClient **handler)
{
    if (!tegra_nvjpg_ready())
        return kIOReturnNotReady;
    if (__atomic_add_fetch(&gConnections, 1, __ATOMIC_SEQ_CST) > kMaxConnections) {
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
        return kIOReturnNoResources;
    }
    AppleJPEGDriverUserClient *client = OSTypeAlloc(AppleJPEGDriverUserClient);
    if (!client || !client->initWithTask(task, securityID, type, properties)) {
        if (client)
            client->release();
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
        return kIOReturnNoMemory;
    }
    if (!client->setUp(task) || !client->attach(this)) {  // from setUp on, free() gives the slot back
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

namespace {
void number(IOService *s, const char *key, uint32_t value)
{
    // 32-bit: JPEGH1 reads AppleJPEGNumCores with size 4
    s->setProperty(key, static_cast<unsigned long long>(value), 32);
}
}  // namespace

extern "C" int tegra_nvjpg_publish(void)
{
    const TegraNVJPGTraits *t = tegra_nvjpg_traits();
    gQueueLock = IOLockAlloc();
#if TEGRA_NVJPG_STATS
    const bool stats = tegra_nvjpg_stats_start();
#else
    const bool stats = true;
#endif
    if (!gQueueLock || !stats || !tegra_vic_surface_start()) {
        IOLog("TegraNVJPG: J3 FAIL no memory for the service, or no IOCoreSurfaceRoot\n");
        return -12;
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(engineThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("TegraNVJPG: J3 FAIL could not start the engine thread\n");
        return -12;
    }
    thread_deallocate(thread);
    IOService *resources = IOService::getResourceService();
    AppleJPEGDriver *service = OSTypeAlloc(AppleJPEGDriver);
    if (!resources || !service || !service->init()) {
        OSSafeReleaseNULL(service);
        IOLog("TegraNVJPG: J3 FAIL could not create the service\n");
        return -12;
    }
    // Apple's properties (start 0x433b8), as NVJPG has them: one core (CMPhoto's two-core strip decode needs two and
    // RST logging), DCT scaling as NVJPG decodes it, missing EOI (the copy appends one), the "interchange" formats (BGRA, RGBA
    // and 4:2:2/4:4:4 input; CMPhoto's automatic mode, which ImageIO uses, takes the hardware only with them), no
    // 12-bit or compressed formats, no MCU alignment requirement (read by the encoder only).
    const bool rgb =
        ((t->formats >> TEGRA_NVJPG_OUT_BGRA & 1) != 0u) && ((t->formats >> TEGRA_NVJPG_OUT_RGBA & 1) != 0u);
    number(service, "AppleJPEGNumCores", 1);
    number(service, "AppleJPEGRequiresMCUAlignment", 0);
    number(service, "AppleJPEGSupportsDCTScaling", t->scales == 0xf ? 1 : 0);
    number(service, "AppleJPEGSupportsRSTLogging", 0);
    number(service, "AppleJPEGSupportsMissingEOI", 1);
    number(service, "AppleJPEGSupportsAppleInterchangeFormats", rgb ? 1 : 0);
    number(service, "AppleJPEGSupports12BitsFormat", 0);
    number(service, "AppleJPEGSupportsCompressedInterchangeFormats", 0);
    number(service, "TegraNVJPGFormats", t->formats);
    number(service, "TegraNVJPGSampling", t->chroma);
    number(service, "TegraNVJPGScales", t->scales);
    service->setProperty("TegraNVJPGWritesClipped", t->clipped);
    service->setProperty("IOUserClientClass", "AppleJPEGDriverUserClient");
    service->setName("TegraNVJPG");  // not "AppleJPEGDriver": JPEGH1.videoencoder name-matches it (see the top)
    if (!service->attach(resources)) {
        service->release();
        IOLog("TegraNVJPG: J3 FAIL could not attach the service\n");
        return -12;
    }
    if (!service->start(resources)) {
        service->detach(resources);
        service->release();
        IOLog("TegraNVJPG: J3 FAIL could not start the service\n");
        return -12;
    }
    service->registerService();
    IOLog("TegraNVJPG: J3 ok AppleJPEGDriver published (user client AppleJPEGDriverUserClient; formats %#x, sampling "
          "%#x, scales %#x, interchange formats %u, writes %s)\n",
          t->formats, t->chroma, t->scales, rgb ? 1u : 0u, t->clipped ? "clipped" : "whole MCUs");
    service->release();  // the attachment keeps it
    return 0;
}
