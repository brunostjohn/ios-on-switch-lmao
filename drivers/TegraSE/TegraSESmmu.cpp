// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraSESmmu.h.
#include <IOKit/IOLib.h>
#include "TegraSESmmu.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMURamView.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

namespace {
constexpr unsigned kEngines = 2, kOwned = 4;
constexpr uint32_t kAsid[kEngines] = {TEGRA_SMMU_ASID_SE1, TEGRA_SMMU_ASID_SE2};
// SE1's clients in the SE swgroup, sesrd and seswr (TRANSLATION_ENABLE_4 bits 0/1, T210 TRM 18.11.1.153). No document
// on hand names the clients of SE1's second swgroup or of SE2's (T210B01's): their bits are not checked.
constexpr uint32_t kClients[kEngines][2] = {{1u << 0 | 1u << 1, 0}, {0, 0}};
constexpr uint32_t kTranslationReg[kEngines][2] = {
    {TEGRA_SMMU_TRANSLATION_ENABLE_4, TEGRA_SMMU_TRANSLATION_ENABLE_4},
    {TEGRA_SMMU_TRANSLATION_ENABLE_4, TEGRA_SMMU_TRANSLATION_ENABLE_4}};
constexpr uint64_t k4GiB = 1ULL << 32;

struct Owned {
    uint64_t pa, bytes;
};

TegraSMMURamView gView[kEngines];
bool gAttached[kEngines];
Owned gOwned[kEngines][kOwned];

bool owned(unsigned engine, uint64_t pa)
{
    for (const Owned &o : gOwned[engine]) {
        if (pa - o.pa < o.bytes)
            return true;
    }
    return false;
}

uint32_t entries(const uint32_t *list)
{
    return list[0] + 1;
}
}  // namespace

tegra_se_smmu_attach_result tegra_se_smmu_attach(unsigned engine, const switch_dma_buffer *const *buffers,
                                                 unsigned count, char *line, size_t size)
{
    const uint32_t *swgroups = tegra_smmu_se_swgroups[engine];
    bool translated = false;
    for (unsigned i = 0; i < 2; ++i)
        translated |= (tegra_smmu_read(swgroups[i]) & (1u << 31)) != 0u;
    if (engine >= kEngines || count > kOwned || !gView[engine].init(kAsid[engine], kAsid[engine]) ||
        !gView[engine].reserveTables(k4GiB)) {
        snprintf(line, size, "no memory for its view of RAM");
        return translated ? TEGRA_SE_SMMU_TRANSLATED : TEGRA_SE_SMMU_UNTRANSLATED;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (!gView[engine].hold(buffers[i]->pa, buffers[i]->size)) {
            snprintf(line, size, "its buffer at %#llx is outside RAM below 4 GiB",
                     static_cast<unsigned long long>(buffers[i]->pa));
            return translated ? TEGRA_SE_SMMU_TRANSLATED : TEGRA_SE_SMMU_UNTRANSLATED;
        }
        gOwned[engine][i] = {buffers[i]->pa, buffers[i]->size};
    }
    gView[engine].commit();
    for (unsigned i = 0; i < 2; ++i) {
        if (gView[engine].attachNarrow(swgroups[i], kTranslationReg[engine][i], kClients[engine][i], line, size)) {
            if (i)
                tegra_smmu_detach(kAsid[engine], swgroups[0]);
            return translated ? TEGRA_SE_SMMU_TRANSLATED : TEGRA_SE_SMMU_UNTRANSLATED;
        }
    }
    gAttached[engine] = true;
    return TEGRA_SE_SMMU_ATTACHED;
}

bool tegra_se_smmu_hold(unsigned engine, const uint32_t *list)
{
    if (engine >= kEngines || !gAttached[engine])
        return true;
    const uint32_t n = entries(list);
    uint32_t held = 0;
    for (; held < n; ++held) {
        const uint64_t pa = list[1 + 2 * held], bytes = list[2 + 2 * held];
        if (!owned(engine, pa) && (pa + bytes > k4GiB || !gView[engine].hold(pa, bytes)))
            break;
    }
    if (held < n) {
        for (uint32_t i = 0; i < held; ++i) {
            if (!owned(engine, list[1 + 2 * i]))
                gView[engine].release(list[1 + 2 * i], list[2 + 2 * i]);
        }
    }
    gView[engine].commit();
    return held == n;
}

void tegra_se_smmu_release(unsigned engine, const uint32_t *list)
{
    if (engine >= kEngines || !gAttached[engine])
        return;
    const uint32_t n = entries(list);
    for (uint32_t i = 0; i < n; ++i) {
        if (!owned(engine, list[1 + 2 * i]))
            gView[engine].release(list[1 + 2 * i], list[2 + 2 * i]);
    }
    gView[engine].commit();
}
