// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Security Engines' DMA through the SMMU (TegraSESmmu.cpp). Each engine's two swgroups (TegraSMMU.h
// tegra_smmu_se_swgroups) are attached narrow (the engines take 32-bit addresses) to a view of RAM at its own addresses
// that holds the engine's list page and bounce buffers for good. An engine that moves data through its memory clients
// (MCCIF) has each operation's list entries held from before the start until it ended, so a caller's pages are reachable
// only while an operation of theirs runs. Thread context throughout.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct switch_dma_buffer;

enum tegra_se_smmu_attach_result {
    TEGRA_SE_SMMU_ATTACHED,
    TEGRA_SE_SMMU_UNTRANSLATED,  // not attached, and nothing translates the engine's clients: it may run as before
    TEGRA_SE_SMMU_TRANSLATED,    // not attached, and another owner translates them: the engine stays off
};

// Before the engine's first DMA, with its own buffers (all below 4 GiB). `line` says why when not attached.
enum tegra_se_smmu_attach_result tegra_se_smmu_attach(unsigned engine, const struct switch_dma_buffer *const *buffers,
                                                      unsigned count, char *line, size_t size);
// The entries of the list at `list` (list[0] entries less one, then address and size pairs), held and published.
// True when the engine is not attached. False (nothing held): an entry outside RAM below 4 GiB.
bool tegra_se_smmu_hold(unsigned engine, const uint32_t *list);
// The same list's holds end, once the engine can no longer reach them.
void tegra_se_smmu_release(unsigned engine, const uint32_t *list);
