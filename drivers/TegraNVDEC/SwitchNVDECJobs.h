// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct switch_dma_buffer;

bool switch_nvdec_jobs_start(const switch_dma_buffer &setup, uint32_t syncpt, uint32_t completed);
// New jobs wait until switch_nvdec_jobs_resume. drain: once the queued ones are done; else only if none is queued.
bool switch_nvdec_jobs_park(bool drain);
// Idle power gating (TegraNVDEC.cpp): a submission holds the powered engine (on again first if it was off), and the
// queue draining starts the idle timer.
int switch_nvdec_power_use();
void switch_nvdec_power_done();
void switch_nvdec_power_idle();
void switch_nvdec_jobs_resume(bool ready, uint32_t completed);
[[noreturn]] void switch_nvdec_job_fault(uint32_t fence, int error, uint64_t elapsedUs);
