// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The kexts of a prelinked kernel, translated one kext at a time into one shared stub region (kernel mode: PAC as the
 * identity, LSE/FP stubs, corecrypto's MSR DIT/SSBS/PAN). `arm64e kext` (a64kext.c) and the installer call this.
 */
#ifndef SWITCH_A64X_KEXT_H
#define SWITCH_A64X_KEXT_H

#include "../a64x/a64x.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The stub words `arm64e kext` gives the region. A stub that does not fit leaves its site untranslated, so every
 * caller that must match the CLI's bytes passes this cap. */
#define SWITCH_A64X_KEXT_STUB_CAP (1u << 22)

struct switch_a64x_kext {
    const uint64_t (*sections)[2]; /* {va, size} of each instruction section */
    size_t nsections;
    const uint64_t *starts; /* function starts (LC_FUNCTION_STARTS), as addresses */
    size_t nstarts;
};

/*
 * original and text: the kexts' coalesced text, size bytes at va. A kext is translated in whole pages read from
 * original, and a page can hold a neighbour's words, so original must stay untranslated: text is a separate copy.
 */
struct switch_a64x_kexts {
    const uint8_t *original;
    uint8_t *text;
    size_t size;
    uint64_t va;
    uint64_t stubs_va;
    uint32_t *stubs;
    uint32_t stub_cap;
    uint32_t stub_words; /* the region's cursor: the next kext's stubs start here */
};

struct switch_a64x_kext_result {
    uint64_t words, changed, island_words;
    uint32_t stub_first, stub_end;
    struct a64x_stats stats;
};

/*
 * Writes the kext's translated instruction words into text (its data islands stay as they are) and its stubs at the
 * region's cursor. island() gets each run of data words inside its instruction sections, in address order: the
 * prelinker must not patch those either. 0, or -1 when out of memory.
 */
int switch_a64x_kext_translate(struct switch_a64x_kexts *kexts, const struct switch_a64x_kext *kext,
                               void (*island)(void *ctx, uint64_t va, uint64_t words), void *ctx,
                               struct switch_a64x_kext_result *result);

/* sizeof(struct switch_a64x_kext_result), for bindings that mirror it */
size_t switch_a64x_kext_result_size(void);

#ifdef __cplusplus
}
#endif

#endif /* SWITCH_A64X_KEXT_H */
