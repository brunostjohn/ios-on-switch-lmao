// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AppleJPEGDriver service's decodes (TegraNVJPGJobs.cpp): each accepted request's job, queued in order for the
// engine thread, which runs one at a time; the user client (TegraNVJPGUserClient.cpp) makes them and is told each end.
#pragma once
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include "../TegraVIC/TegraVICSurface.h"
#include "TegraNVJPG.h"
#include "TegraNVJPGMapping.h"
#include "TegraNVJPGRepack.h"
#include "TegraNVJPGRequest.h"

class AppleJPEGDriverUserClient;

namespace tegra_nvjpg {
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
    enum class Phase : uint8_t {
        OnEngine,
        Repacking
    } phase;
    bool woken, engineEnded, repacked;
    void *hold;                         // the destination's layout, held until the decode has ended (routeJob)
    const SwitchNVJPGMapping *mapping;  // a direct decode's destination pages, as NVJPG writes them
    TegraNVJPGRepackRun repack;
    int engineResult;
    nvjpg_dec_status engineStatus;
    uint64_t engineBusy;
#if TEGRA_NVJPG_PROBES
    bool tileProbe;   // HW Test's tile probe (kTileProbeMagic): tileMode and tileGob go to the setup unchanged
    bool guardProbe;  // HW Test's copy-guard probe (kGuardProbeMagic): the re-pack's guard closed
    uint32_t tileMode, tileGob;
    uint64_t cancelsBefore;  // tegra_nvjpg_cancellations() as the job began
#endif

    union {
        TegraNVJPGDecodeRequest base;
        TegraNVJPGDecodeRequestExt ext;
        TegraNVJPGDecodeRequest2024 v2024;
    } request;  // the caller's copy, in the job's one allocation
};

extern IOLock *gQueueLock;

// `why` is a literal: it is the refusal's key as well as its text. `format`: the TEGRA_NVJPG_OUT_* asked for, or
// TEGRA_NVJPG_OUT_COUNT.
void refused(const char *why, uint32_t format);
TegraNVJPGSurface surfaceDesc(const TegraVICSurfaceDesc &v);
// A free scan slot for a scan of `bytes`, waited for when every slot holds a queued decode
SwitchNVJPGScan *takeScan(uint64_t bytes);
void dropJob(Job *job);
void queueJob(Job *job);
void engineThread(void *, wait_result_t);
// The ended job to its client (TegraNVJPGUserClient.cpp): an async one's completion, a sync caller woken
void finish(Job *job);
}  // namespace tegra_nvjpg
