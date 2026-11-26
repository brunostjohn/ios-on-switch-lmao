// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * `arm64e pretranslate` and `arm64e dsc-apply` (translate/arm64e/tools) on buffers. Both are re-entrant: the
 * translator keeps no state between calls.
 */
#ifndef SWITCH_TRANSLATE_H
#define SWITCH_TRANSLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_TRANSLATE_CDHASH  20
#define SWITCH_TRANSLATE_SLICES  16
#define SWITCH_TRANSLATE_EDITS   64
#define SWITCH_TRANSLATE_ERR_LEN 256

enum switch_slice_status {
    SWITCH_SLICE_UNCHANGED = 0,
    SWITCH_SLICE_TRANSLATED = 1,
    SWITCH_SLICE_FALLBACK = 2,
};

/* A checked byte edit (translate_tree's -e FILEOFF:STOCK:AT:PATCHED), offsets within the slice. */
struct switch_translate_edit {
    uint64_t fileoff;
    const uint8_t *stock;
    size_t stock_len;
    size_t at;
    const uint8_t *patched;
    size_t patched_len;
};

struct switch_translate_slice {
    uint32_t status;
    uint8_t cdhash_in[SWITCH_TRANSLATE_CDHASH];
    uint8_t cdhash_out[SWITCH_TRANSLATE_CDHASH];
};

/*
 * out (size bytes) becomes the file `in` with every arm64 slice translated in place, checked against its original and
 * re-signed. Edits need a file of one slice. Returns how many slices (each in slices[], at most
 * SWITCH_TRANSLATE_SLICES), 0 for a file that isn't a Mach-O, or -1 with a message in err. Buffers 16-byte aligned.
 */
int switch_translate_macho(const uint8_t *in, size_t size, uint8_t *out, const struct switch_translate_edit *edits,
                           size_t nedits, struct switch_translate_slice *slices, char *err);

/* An old and a new cdhash, 40 bytes. */
struct switch_cdhash_pair {
    uint8_t old[SWITCH_TRANSLATE_CDHASH], now[SWITCH_TRANSLATE_CDHASH];
};

/*
 * out (size bytes) becomes the dyld cache file `in` translated by plan, with its prebuilt loaders' cdhashes replaced
 * by pairs (sorted by old) and re-signed; cdhash gets its new cdhash. 0, or -1 with a message in err.
 */
int switch_translate_dsc(const uint8_t *plan, size_t plan_size, const uint8_t *in, size_t size,
                         const struct switch_cdhash_pair *pairs, size_t npairs, uint8_t *out,
                         uint8_t cdhash[SWITCH_TRANSLATE_CDHASH], char *err);

#ifdef __cplusplus
}
#endif

#endif
