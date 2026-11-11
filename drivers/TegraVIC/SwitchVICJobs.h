// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraVIC.h"
#include "SwitchVICQueue.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

constexpr uint32_t kSwitchVICConfigStride = 4096;
constexpr uint32_t kSwitchVICConfigBytes = SwitchVICQueue::kSlots * kSwitchVICConfigStride;

bool switch_vic_jobs_start(const TegraVICBuffer &configs, uint32_t syncpt, uint32_t completed);
void switch_vic_jobs_suspend();
void switch_vic_jobs_resume(bool ready, uint32_t completed);

extern "C" int switch_vic_wait(uint32_t fence);
int switch_vic_submit_batch(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount, void (*completion)(void *, int),
                            void *context, uint32_t *fence);

// Supplied ownership leases remain held through commit and final completion of the conversion.
int switch_vic_submit_owned(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount, switch_gpu_cpu_lease *const *leases,
                            uint32_t leaseCount, void (*completion)(void *, int), void *context, uint32_t *fence);

struct SwitchVICEvents;

struct SwitchVICOwnership {
    switch_gpu_backing_use *uses;
    uint32_t useCount;
    const switch_gpu_backing_guard *guards;
    uint32_t guardCount;
    switch_gpu_cpu_lease *const *leases;
    uint32_t leaseCount;
};

int switch_vic_submit_checked(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                              const SwitchVICOwnership &ownership, void (*completion)(void *, int), void *context,
                              uint32_t *fence, SwitchVICEvents *events = nullptr);
// Debug kernels: which check of switch_vic_submit_checked refused the last job it refused.
const char *switch_vic_last_refusal();
