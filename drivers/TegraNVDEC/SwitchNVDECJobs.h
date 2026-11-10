// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct switch_dma_buffer;

bool switch_nvdec_jobs_start(const switch_dma_buffer &setup, uint32_t syncpt, uint32_t completed);
void switch_nvdec_jobs_suspend();
void switch_nvdec_jobs_resume(bool ready, uint32_t completed);
[[noreturn]] void switch_nvdec_job_fault(uint32_t fence, int error, uint64_t elapsedUs);
