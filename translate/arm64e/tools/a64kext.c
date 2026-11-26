/*
 * a64kext: translate the kexts of a prelinked kernel (kernel mode, PAC as the identity, LSE/FP stubs, corecrypto's
 * MSR DIT/SSBS/PAN), each kext's stubs in its own block of one stub region after the kexts' text.
 *
 *   a64kext BLOB BLOB_VA JOBS STUBS_VA OUT_BLOB OUT_STUBS
 *
 * BLOB: the kexts' coalesced __TEXT_EXEC bytes as laid out at BLOB_VA (other bytes are not touched).
 * JOBS: per kext "K <id>", then "R <va> <size>" per instruction section, "F <va>" per function start, "E".
 * Prints one line per kext: "S <id> <words> <changed> <stub_first_word> <stub_end_word> <class>=<rw>/<stub>/<left> ..."
 */
#include "kext.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct job {
    char id[160];
    uint64_t r[64][2];
    int nr;
    uint64_t *f;
    size_t nf, capf;
};

static uint8_t *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) {
        perror(p);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 8);
    if (fread(b, 1, *n, f) != *n) {
        perror(p);
        exit(1);
    }
    fclose(f);
    return b;
}

static void print_island(void *ctx, uint64_t va, uint64_t words)
{
    (void)ctx;
    printf("I %" PRIx64 " %" PRIu64 "\n", va, words);
}

int a64x_cmd_kext(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: a64kext BLOB BLOB_VA JOBS STUBS_VA OUT_BLOB OUT_STUBS\n");
        return 2;
    }
    size_t n;
    uint8_t *blob = slurp(argv[1], &n);
    uint64_t base = strtoull(argv[2], NULL, 0), stubs_va = strtoull(argv[4], NULL, 0);
    uint8_t *orig = malloc(n);
    memcpy(orig, blob, n);
    FILE *jf = fopen(argv[3], "r");
    if (!jf) {
        perror(argv[3]);
        return 1;
    }
    struct switch_a64x_kexts kexts = {
        orig, blob, n, base, stubs_va, calloc(SWITCH_A64X_KEXT_STUB_CAP, 4), SWITCH_A64X_KEXT_STUB_CAP, 0};
    char line[512];
    struct job j;
    memset(&j, 0, sizeof(j));
    while (fgets(line, sizeof(line), jf)) {
        if (line[0] == 'K') {
            memset(&j, 0, sizeof(j));
            sscanf(line + 2, "%159s", j.id);
            continue;
        }
        if (line[0] == 'R') {
            sscanf(line + 2, "%" SCNx64 " %" SCNx64, &j.r[j.nr][0], &j.r[j.nr][1]);
            j.nr++;
            continue;
        }
        if (line[0] == 'F') {
            if (j.nf == j.capf) {
                j.capf = j.capf ? 2 * j.capf : 1024;
                j.f = realloc(j.f, j.capf * 8);
            }
            sscanf(line + 2, "%" SCNx64, &j.f[j.nf++]);
            continue;
        }
        if (line[0] != 'E')
            continue;
        const struct switch_a64x_kext kext = {(const uint64_t (*)[2])j.r, (size_t)j.nr, j.f, j.nf};
        struct switch_a64x_kext_result r;
        if (switch_a64x_kext_translate(&kexts, &kext, print_island, NULL, &r)) {
            fprintf(stderr, "a64kext: out of memory\n");
            return 1;
        }
        printf("S %s %" PRIu64 " %" PRIu64 " %u %u islands=%" PRIu64, j.id, r.words, r.changed, r.stub_first,
               r.stub_end, r.island_words);
        for (int c = 1; c < A64X_NCLASSES; c++)
            if (r.stats.rewrite1[c] || r.stats.stubs[c] || r.stats.left[c])
                printf(" %s=%" PRIu64 "/%" PRIu64 "/%" PRIu64, a64x_class_name(c), r.stats.rewrite1[c],
                       r.stats.stubs[c], r.stats.left[c]);
        printf("\n");
        free(j.f);
        j.f = NULL;
    }
    FILE *o = fopen(argv[5], "wb");
    fwrite(blob, 1, n, o);
    fclose(o);
    o = fopen(argv[6], "wb");
    fwrite(kexts.stubs, 4, kexts.stub_words, o);
    fclose(o);
    return 0;
}
