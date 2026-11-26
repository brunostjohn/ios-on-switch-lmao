// SPDX-License-Identifier: GPL-2.0-or-later
#include "kext.h"

#include <stdlib.h>
#include <string.h>

struct code_ctx {
    const struct switch_a64x_kext *kext;
    const uint8_t *island;
    uint64_t lo, hi, words; /* the kext's whole pages, and their word count (one island bit each) */
};

static bool in_sections(const struct switch_a64x_kext *kext, uint64_t va)
{
    for (size_t i = 0; i < kext->nsections; i++) {
        if (va >= kext->sections[i][0] && va < kext->sections[i][0] + kext->sections[i][1]) {
            return true;
        }
    }
    return false;
}

static bool is_code(void *opaque, uint64_t va)
{
    const struct code_ctx *c = opaque;
    if (c->island && va >= c->lo && (va - c->lo) / 4 < c->words) {
        uint64_t w = (va - c->lo) / 4;
        if (c->island[w / 8] & (1u << (w & 7))) {
            return false;
        }
    }
    return in_sections(c->kext, va);
}

/* The 4 KB page at va, zero past the end of the text. */
static size_t page_in(const struct switch_a64x_kexts *kexts, uint64_t va, uint32_t in[1024])
{
    memset(in, 0, 4096);
    if (!(va >= kexts->va && va - kexts->va < kexts->size)) {
        return 0;
    }
    size_t at = va - kexts->va, take = at + 4096 <= kexts->size ? 4096 : kexts->size - at;
    memcpy(in, kexts->original + at, take);
    return take;
}

/* Data islands, as the kernel finds them for an image without a plan, seeded with the function starts. */
static uint64_t find_islands(const struct switch_a64x_kexts *kexts, const struct switch_a64x_kext *kext,
                             const struct a64x_code_map *cm, const struct code_ctx *cc, uint8_t *fsb, uint8_t *isl)
{
    for (size_t k = 0; k < kext->nstarts; k++) {
        if (kext->starts[k] >= cc->lo && kext->starts[k] < cc->hi) {
            uint64_t w = (kext->starts[k] - cc->lo) / 4;
            fsb[w / 8] |= (uint8_t)(1u << (w & 7));
        }
    }
    struct a64x_islands is = {isl, fsb, 0, 0, false, false, 0, 0, 0, 0};
    for (uint64_t va = cc->lo; va < cc->hi; va += 4096) {
        uint32_t in[1024];
        page_in(kexts, va, in);
        a64x_islands_page(&is, in, 1024, (va - cc->lo) / 4, va, cm);
    }
    a64x_islands_end(&is, cc->words);
    return is.words;
}

static uint32_t translate_pages(struct switch_a64x_kexts *kexts, const struct switch_a64x_kext *kext,
                                const struct a64x_code_map *cm, const struct code_ctx *cc, const struct a64x_walk *walk,
                                struct switch_a64x_kext_result *result)
{
    uint32_t cursor = kexts->stub_words;
    for (uint64_t va = cc->lo; va < cc->hi; va += 4096) {
        uint32_t in[1024], out[1024];
        size_t take = page_in(kexts, va, in);
        if (!take) {
            continue;
        }
        struct a64x_stub_region sr = {kexts->stubs_va, kexts->stubs, kexts->stub_cap, 0, walk};
        cursor = a64x_translate_page(in, out, 1024, va, cm, &sr, cursor, &result->stats);
        for (uint32_t w = 0; w < take / 4; w++) {
            /* only this kext's instruction sections (its data islands come back unchanged) */
            if (!in_sections(kext, va + 4 * w)) {
                continue;
            }
            result->words++;
            if (in[w] != out[w]) {
                result->changed++;
                memcpy(kexts->text + (va - kexts->va) + 4 * (size_t)w, &out[w], 4);
            }
        }
    }
    return cursor;
}

/* Data islands inside the kext's instruction sections: the prelinker must not patch them either. */
static void report_islands(const struct code_ctx *cc, const uint8_t *isl,
                           void (*island)(void *ctx, uint64_t va, uint64_t words), void *ctx)
{
    for (uint64_t w = 0; w < cc->words; w++) {
        if (!(isl[w / 8] & (1u << (w & 7)))) {
            continue;
        }
        uint64_t e = w;
        while (e + 1 < cc->words && (isl[(e + 1) / 8] & (1u << ((e + 1) & 7)))) {
            e++;
        }
        if (island) {
            island(ctx, cc->lo + 4 * w, e - w + 1);
        }
        w = e;
    }
}

size_t switch_a64x_kext_result_size(void)
{
    return sizeof(struct switch_a64x_kext_result);
}

int switch_a64x_kext_translate(struct switch_a64x_kexts *kexts, const struct switch_a64x_kext *kext,
                               void (*island)(void *ctx, uint64_t va, uint64_t words), void *ctx,
                               struct switch_a64x_kext_result *result)
{
    uint64_t lo = UINT64_MAX, hi = 0;
    for (size_t i = 0; i < kext->nsections; i++) {
        if (kext->sections[i][0] < lo) {
            lo = kext->sections[i][0];
        }
        if (kext->sections[i][0] + kext->sections[i][1] > hi) {
            hi = kext->sections[i][0] + kext->sections[i][1];
        }
    }
    lo &= ~4095ull;
    hi = (hi + 4095) & ~4095ull;
    struct code_ctx cc = {kext, NULL, lo, hi, (hi - lo) / 4};
    struct a64x_code_map cm = {&cc, is_code, A64X_MAP_KERNEL, NULL};
    void *walk_buf = malloc(A64X_WALK_BYTES(1024));
    uint8_t *fsb = calloc(cc.words / 8 + 1, 1);
    uint8_t *isl = calloc(cc.words / 8 + 1, 1);
    if (walk_buf && fsb && isl) {
        struct a64x_walk walk = a64x_walk_in(walk_buf, 1024);
        memset(result, 0, sizeof(*result));
        result->island_words = find_islands(kexts, kext, &cm, &cc, fsb, isl);
        cc.island = isl;
        result->stub_first = kexts->stub_words;
        result->stub_end = kexts->stub_words = translate_pages(kexts, kext, &cm, &cc, &walk, result);
        report_islands(&cc, isl, island, ctx);
    }
    int status = walk_buf && fsb && isl ? 0 : -1;
    free(walk_buf);
    free(fsb);
    free(isl);
    return status;
}
