// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchNVDECSubmission.h"
#include "SwitchNVDECAccess.h"
#include "../TegraHost1x/TegraHost1x.h"

int switch_nvdec_prepare(const TegraNVDECSubmit &submit, TegraNVDECResolve resolve, void *context,
                         SwitchNVDECCheck check, SwitchNVDECSubmission &work, const char **why)
{
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {};
    for (uint32_t i = 0; i < submit.entry_count; ++i) {
        const auto &entry = submit.entries[i];
        if (entry.kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        auto &block = work.blocks[i];
        if (!resolve(context, entry.value, &block) || !block.backing || entry.offset > block.size) {
            *why = "relocation outside a buffer";
            return -22;
        }
        avail[i] = block.size - entry.offset;
    }
    *why = check(&submit, avail);
    if (*why)
        return -22;
    for (uint32_t i = 0; i < submit.entry_count; ++i) {
        if (submit.entries[i].kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        const uint32_t access = switch_nvdec_entry_access(submit, i);
        if (!access) {
            *why = "unclassified buffer access";
            return -22;
        }
        work.uses[work.useCount++] = {work.blocks[i].backing, access, 0};
    }
    for (uint32_t i = 0; i < submit.clean_count; ++i) {
        const auto &range = submit.clean[i];
        TegraNVDECBlock block = {};
        if (!resolve(context, range.handle, &block) || !block.backing ||
            static_cast<uint64_t>(range.offset) + range.length > block.size) {
            *why = "clean range";
            return -22;
        }
        if (!range.length)
            continue;
        // The caller already wrote this range. Acquiring it must preserve those dirty CPU lines.
        constexpr uint32_t access = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE;
        int error = switch_gpu_backing_cpu_acquire(block.backing, range.offset, range.length, access);
        if (error)
            return error;
        error = switch_gpu_backing_cpu_publish(block.backing, range.offset, range.length);
        const int released = switch_gpu_backing_cpu_release(block.backing, range.offset, range.length, access);
        if (error || released)
            return error ? error : released;
    }
    return 0;
}

int switch_nvdec_grow_words(SwitchNVDECSubmission &work, uint32_t dependencies)
{
    const uint32_t count = 2 + 3 * (TEGRA_NVDEC_MAX_ENTRIES + 1) + SWITCH_HOST1X_WAIT_WORDS * (dependencies + 1);
    if (work.wordCapacity >= count)
        return 0;
    auto *words = IONew(uint32_t, count);
    if (!words)
        return -12;
    if (work.words)
        IODelete(work.words, uint32_t, work.wordCapacity);
    work.words = words;
    work.wordCapacity = count;
    return 0;
}
