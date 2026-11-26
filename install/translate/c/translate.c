// SPDX-License-Identifier: GPL-3.0-or-later
/* The loops of tools/pretranslate.c and tools/dsc.c's dsc-apply, over the library they call, without the files. */
#include "translate.h"

#include <stdio.h>
#include <string.h>

#include "../../../translate/arm64e/macho/dsc.h"
#include "../../../translate/arm64e/macho/pretranslate.h"

_Static_assert(sizeof(struct switch_cdhash_pair) == sizeof(struct a64x_cdhash_pair), "a pair is two cdhashes");
_Static_assert(SWITCH_TRANSLATE_CDHASH == A64X_CDHASH_SIZE, "cdhash size");

static enum switch_slice_status status_of(enum a64x_pt_status s)
{
    switch (s) {
    case A64X_PT_TRANSLATED:
        return SWITCH_SLICE_TRANSLATED;
    case A64X_PT_FALLBACK:
        return SWITCH_SLICE_FALLBACK;
    default:
        return SWITCH_SLICE_UNCHANGED;
    }
}

int switch_translate_macho(const uint8_t *in, size_t size, uint8_t *out, const struct switch_translate_edit *edits,
                           size_t nedits, struct switch_translate_slice *slices, char *err)
{
    struct a64x_pt_edit pt[SWITCH_TRANSLATE_EDITS];
    struct a64x_slice found[SWITCH_TRANSLATE_SLICES];
    err[0] = 0;
    if (nedits > SWITCH_TRANSLATE_EDITS) {
        snprintf(err, SWITCH_TRANSLATE_ERR_LEN, "%zu edits, at most %d", nedits, SWITCH_TRANSLATE_EDITS);
        return -1;
    }
    for (size_t i = 0; i < nedits; i++)
        pt[i] = (struct a64x_pt_edit){edits[i].fileoff, edits[i].stock,   edits[i].stock_len,
                                      edits[i].at,      edits[i].patched, edits[i].patched_len};
    if (size)
        memcpy(out, in, size);
    int n = size ? a64x_macho_slices(in, size, found, SWITCH_TRANSLATE_SLICES) : -1;
    if (nedits && n != 1) {
        snprintf(err, SWITCH_TRANSLATE_ERR_LEN, "edits require a single-slice Mach-O (got %d)", n);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        char why[SWITCH_TRANSLATE_ERR_LEN - 32];
        struct a64x_macho m;
        struct a64x_pt_result r;
        struct a64x_pt_check v;
        if (a64x_macho_parse(in + found[i].offset, found[i].size, &m, why, sizeof(why)) ||
            a64x_pretranslate_edits(&m, out + found[i].offset, pt, nedits, &r, why, sizeof(why))) {
            snprintf(err, SWITCH_TRANSLATE_ERR_LEN, "slice %d: %s", i, why);
            return -1;
        }
        if (r.status == A64X_PT_TRANSLATED &&
            a64x_pt_verify_edits(&m, out + found[i].offset, pt, nedits, &v, why, sizeof(why)) < 0) {
            snprintf(err, SWITCH_TRANSLATE_ERR_LEN, "slice %d: verification failed: %s", i, why);
            return -1;
        }
        slices[i].status = status_of(r.status);
        memcpy(slices[i].cdhash_in, r.cdhash_in, A64X_CDHASH_SIZE);
        memcpy(slices[i].cdhash_out, r.cdhash_out, A64X_CDHASH_SIZE);
    }
    return n < 0 ? 0 : n;
}

int switch_translate_dsc(const uint8_t *plan, size_t plan_size, const uint8_t *in, size_t size,
                         const struct switch_cdhash_pair *pairs, size_t npairs, uint8_t *out,
                         uint8_t cdhash[SWITCH_TRANSLATE_CDHASH], char *err)
{
    struct a64x_plan p;
    struct a64x_dsc_file f;
    struct a64x_dsc_stats st;
    char why[SWITCH_TRANSLATE_ERR_LEN - 32];
    err[0] = 0;
    if (a64x_plan_open(plan, plan_size, &p, why, sizeof(why))) {
        snprintf(err, SWITCH_TRANSLATE_ERR_LEN, "plan: %s", why);
        return -1;
    }
    if (a64x_dsc_parse(in, size, &f, why, sizeof(why)) ||
        a64x_dsc_apply(&p, &f, (const struct a64x_cdhash_pair *)(const void *)pairs, npairs, out, cdhash, &st, why,
                       sizeof(why))) {
        snprintf(err, SWITCH_TRANSLATE_ERR_LEN, "%s", why);
        return -1;
    }
    return 0;
}
