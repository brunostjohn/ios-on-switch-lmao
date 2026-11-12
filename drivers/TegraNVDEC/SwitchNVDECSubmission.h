// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "TegraNVDEC.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

struct SwitchNVDECSubmission {
    TegraNVDECBlock blocks[TEGRA_NVDEC_MAX_ENTRIES] = {};
    switch_gpu_backing_use uses[TEGRA_NVDEC_MAX_ENTRIES] = {};
    uint32_t *words = nullptr;
    uint32_t wordCapacity = 0;
    uint32_t useCount = 0;
};

using SwitchNVDECCheck = const char *(*)(const TegraNVDECSubmit *, const uint64_t *);

// The submit's buffers resolved and checked, its accesses classified and its CPU-written ranges published.
int switch_nvdec_prepare(const TegraNVDECSubmit &submit, TegraNVDECResolve resolve, void *context,
                         SwitchNVDECCheck check, SwitchNVDECSubmission &work, const char **why);
// Room for the job's stream with `dependencies` syncpoint waits in front of it.
int switch_nvdec_grow_words(SwitchNVDECSubmission &work, uint32_t dependencies);
