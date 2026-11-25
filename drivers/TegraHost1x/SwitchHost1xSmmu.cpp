// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's command DMA through the SMMU (swgroup HC, one ASID field; SwitchHost1xChannels.h
// switch_host1x_channels_confine). host1x fetches only the push buffers and the arenas their GATHERs point into, all
// the driver's own and below 4 GiB, so they are held for good and every other address faults at the MC.
#include <IOKit/IOLib.h>
#include "SwitchHost1xChannels.h"
#include "TegraHost1x.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMURamView.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

namespace {
TegraSMMURamView gView;
// host1xdmar and host1xr (TRANSLATION_ENABLE_0 bits 22/23), host1xw (TRANSLATION_ENABLE_1 bit 22): T210 TRM
// 18.11.1.66-67.
constexpr uint32_t kHostReads = 1u << 22 | 1u << 23, kHostWrites = 1u << 22;
}  // namespace

bool switch_host1x_channels_confine(char *line, size_t size)
{
    const switch_dma_buffer *buffers[2 * TEGRA_HOST1X_CHANNELS] = {};
    const uint32_t count = switch_host1x_channels_buffers(buffers, 2 * TEGRA_HOST1X_CHANNELS);
    if (!gView.init(TEGRA_SMMU_ASID_HOST1X, TEGRA_SMMU_ASID_HOST1X)) {
        snprintf(line, size, "no memory for its directories");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (buffers[i]->pa + buffers[i]->size > (1ULL << 32) || !gView.hold(buffers[i]->pa, buffers[i]->size)) {
            snprintf(line, size, "no page table for buffer %#llx", static_cast<unsigned long long>(buffers[i]->pa));
            return false;
        }
    }
    gView.commit();
    return !gView.attachNarrow(TEGRA_SMMU_HC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0, kHostReads, line, size) &&
           !gView.attachNarrow(TEGRA_SMMU_HC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_1, kHostWrites, line, size);
}
