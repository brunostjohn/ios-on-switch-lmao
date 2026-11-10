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

extern "C" int switch_vic_submit_after(const TegraVICJob *job, const TegraVICAcquire *acquire,
                                        void (*completion)(void *, int), void *context, uint32_t *fence);
extern "C" int switch_vic_wait(uint32_t fence);
int switch_vic_submit_batch(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount,
                            void (*completion)(void *, int), void *context, uint32_t *fence);
