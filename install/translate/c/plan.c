// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The planner, a64plan2.c, compiled unchanged into this file. What it asks of the system comes from here instead:
 * open/fstat/mmap of its cache files from caller buffers (plan_shim/), fopen/fwrite of its plan into a buffer, its
 * printing into a log, getenv as unset, exit as a return, and qsort as macOS's (sort.c), since its comparators tie.
 * It never frees and keeps everything in file-scope statics, so every block it allocates is tracked and freed after
 * the run, and reset() puts each static back to its initial value.
 */
#include "plan.h"
#include "sort.h"
#include "vmem.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plan_shim/fcntl.h"
#include "plan_shim/sys/mman.h"
#include "plan_shim/sys/stat.h"
#include "plan_shim/unistd.h"

#define CACHE_PATH "cache"

struct bytes {
    char *data;
    size_t len, cap;
};

static struct bytes plan_out, plan_log;
static const struct switch_plan_file *plan_files;
static size_t plan_nfiles;
static jmp_buf plan_bail;

static void bytes_reserve(struct bytes *b, size_t more)
{
    if (b->len + more + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + more + 1)
        cap *= 2;
    char *data = realloc(b->data, cap);
    if (!data)
        abort();
    b->data = data;
    b->cap = cap;
}

static void bytes_put(struct bytes *b, const void *p, size_t n)
{
    bytes_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void bytes_vprintf(struct bytes *b, const char *fmt, va_list ap)
{
    va_list again;
    va_copy(again, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    if (n > 0) {
        bytes_reserve(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, again);
        b->len += (size_t)n;
    }
    va_end(again);
}

/* Allocations at or above 1 MiB bypass libc malloc so realloc growth and plan_free_all return pages immediately. */
#define PLAN_VMEM_THRESHOLD (1u << 20)

/* The planner's heap: a circular list of every live block, headed by 32 bytes so blocks keep malloc's alignment. */
struct block {
    struct block *prev, *next;
    size_t vmem_size;
    size_t user_size;
};

static struct block plan_blocks = {&plan_blocks, &plan_blocks, 0, 0};

static void *link_block(struct block *b)
{
    if (!b)
        return NULL;
    b->prev = &plan_blocks;
    b->next = plan_blocks.next;
    plan_blocks.next->prev = b;
    plan_blocks.next = b;
    return b + 1;
}

static void unlink_block(struct block *b)
{
    b->prev->next = b->next;
    b->next->prev = b->prev;
}

static void free_block(struct block *b)
{
    if (b->vmem_size)
        switch_vmem_free(b, b->vmem_size);
    else
        free(b);
}

static struct block *alloc_block(size_t n, int zero)
{
    if (n > SIZE_MAX - sizeof(struct block))
        return NULL;
    size_t total = sizeof(struct block) + n;
    struct block *b;
    if (total >= PLAN_VMEM_THRESHOLD) {
        b = switch_vmem_alloc(total);
        if (!b)
            return NULL;
        b->vmem_size = total;
    } else {
        b = zero ? calloc(1, total) : malloc(total);
        if (!b)
            return NULL;
        b->vmem_size = 0;
    }
    b->user_size = n;
    return b;
}

static void *plan_malloc(size_t n)
{
    return link_block(alloc_block(n, 0));
}

static void *plan_calloc(size_t n, size_t size)
{
    if (size && n > (SIZE_MAX - sizeof(struct block)) / size)
        return NULL;
    return link_block(alloc_block(n * size, 1));
}

static void plan_free(void *p)
{
    if (!p)
        return;
    struct block *b = (struct block *)p - 1;
    unlink_block(b);
    free_block(b);
}

static void *plan_realloc(void *p, size_t n)
{
    if (!p)
        return plan_malloc(n);
    if (n > SIZE_MAX - sizeof(struct block))
        return NULL;
    struct block *b = (struct block *)p - 1;
    size_t total = sizeof(struct block) + n;
    if (b->vmem_size || total >= PLAN_VMEM_THRESHOLD) {
        struct block *grown = alloc_block(n, 0);
        if (!grown)
            return NULL;
        size_t copy = b->user_size < n ? b->user_size : n;
        if (copy)
            memcpy(grown + 1, p, copy);
        unlink_block(b);
        free_block(b);
        return link_block(grown);
    }
    unlink_block(b);
    struct block *grown = realloc(b, total);
    if (!grown) {
        link_block(b);
        return NULL;
    }
    grown->vmem_size = 0;
    grown->user_size = n;
    return link_block(grown);
}

static void plan_free_all(void)
{
    while (plan_blocks.next != &plan_blocks) {
        struct block *b = plan_blocks.next;
        unlink_block(b);
        free_block(b);
    }
}

int switch_plan_optind = 1;
char *switch_plan_optarg;

int switch_plan_getopt(int argc, char *const argv[], const char *options)
{
    if (switch_plan_optind >= argc || argv[switch_plan_optind][0] != '-' || !argv[switch_plan_optind][1])
        return -1;
    const char *arg = argv[switch_plan_optind++];
    const char *spec = strchr(options, arg[1]);
    if (!spec || arg[1] == ':')
        return '?';
    if (spec[1] != ':')
        return arg[2] ? '?' : arg[1];
    if (arg[2]) {
        switch_plan_optarg = (char *)arg + 2;
    } else if (switch_plan_optind < argc) {
        switch_plan_optarg = argv[switch_plan_optind++];
    } else {
        return '?';
    }
    return arg[1];
}

int switch_plan_open(const char *path, int flags, ...)
{
    (void)flags;
    size_t base = strlen(CACHE_PATH);
    if (strncmp(path, CACHE_PATH, base))
        return -1;
    for (size_t i = 0; i < plan_nfiles; i++) {
        if (!strcmp(path + base, plan_files[i].suffix))
            return (int)i;
    }
    return -1;
}

int switch_plan_close(int fd)
{
    (void)fd;
    return 0;
}

int switch_plan_fstat(int fd, struct switch_plan_stat *st)
{
    st->st_size = plan_files[fd].size;
    return 0;
}

void *switch_plan_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t offset)
{
    (void)addr;
    (void)prot;
    (void)flags;
    if (offset || len > plan_files[fd].size)
        return (void *)-1;
    return (void *)(uintptr_t)plan_files[fd].data;
}

int switch_plan_munmap(void *addr, size_t len)
{
    (void)addr;
    (void)len;
    return 0;
}

static FILE *plan_fopen(const char *path, const char *mode)
{
    (void)path;
    if (strcmp(mode, "wb"))
        return NULL;
    plan_out.len = 0;
    return (FILE *)(void *)&plan_out;
}

static size_t plan_fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    if (f != (FILE *)(void *)&plan_out)
        return 0;
    bytes_put(&plan_out, p, size * n);
    return n;
}

static int plan_fclose(FILE *f)
{
    (void)f;
    return 0;
}

static int plan_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bytes_vprintf(&plan_log, fmt, ap);
    va_end(ap);
    return 0;
}

static int plan_fprintf(FILE *f, const char *fmt, ...)
{
    (void)f;
    va_list ap;
    va_start(ap, fmt);
    bytes_vprintf(&plan_log, fmt, ap);
    va_end(ap);
    return 0;
}

static void plan_perror(const char *what)
{
    plan_printf("%s: not among the cache's files\n", what);
}

static char *plan_getenv(const char *name)
{
    (void)name;
    return NULL;
}

_Noreturn static void plan_exit(int code)
{
    longjmp(plan_bail, code ? code : 1);
}

#define malloc  plan_malloc
#define calloc  plan_calloc
#define realloc plan_realloc
#define free    plan_free
#define qsort   switch_qsort
#define fopen   plan_fopen
#define fwrite  plan_fwrite
#define fclose  plan_fclose
#define printf  plan_printf
#define fprintf plan_fprintf
#define perror  plan_perror
#define getenv  plan_getenv
#define exit    plan_exit
#define g_dens  switch_plan_densities
#define cmpd    switch_plan_by_density

#include "../../../translate/arm64e/tools/a64plan2.c"

#undef malloc
#undef calloc
#undef realloc
#undef free
#undef fopen
#undef fwrite
#undef fclose
#undef printf
#undef fprintf
#undef perror
#undef getenv
#undef exit

/* Every file-scope static of a64plan2.c, as it starts. Its function-local statics only count warnings it prints. */
static void reset(void)
{
    memset(fm, 0, sizeof(fm));
    nfm = 0;
    memset(fb, 0, sizeof(fb));
    memset(fsz, 0, sizeof(fsz));
    memset(codebits, 0, sizeof(codebits));
    memset(hotbits, 0, sizeof(hotbits));
    secs = NULL;
    nsecs = capsecs = 0;
    vr = NULL;
    nvr = capvr = 0;
    fstarts = NULL;
    nfstarts = capfs = 0;
    memset(refbits, 0, sizeof(refbits));
    memset(reachbits, 0, sizeof(reachbits));
    dic_lo = dic_hi = NULL;
    ndic = capdic = 0;
    lpads = NULL;
    nlpads = caplpads = 0;
    n_lsda = n_lsda_bad = 0;
    n_indirect_br = n_jt = n_jt_targets = n_jt_unresolved = 0;
    memset(fsbits, 0, sizeof(fsbits));
    n_roots_rejected = 0;
    isl_runs = isl_words = isl_mod = isl_sites = code_runs = code_runs_mod = code_mod_words = code_sites = 0;
    reached_impossible = tail_runs = tail_words = 0;
    isl_out = NULL;
    memset(ri_hist, 0, sizeof(ri_hist));
    memset(cls_unreached_code, 0, sizeof(cls_unreached_code));
    memset(xmaps, 0, sizeof(xmaps));
    nxmaps = 0;
    n_ref_code = n_ref_data = 0;
    holes = NULL;
    nholes = capholes = 0;
    hole_page_va = n3 = n5 = 0;
    memset(&d3, 0, sizeof(d3));
    memset(&d5, 0, sizeof(d5));
    patches = NULL;
    npatch = cappatch = 0;
    ctx_tag = "?";
    ctx_site = 0;
    bin_base = 0;
    nbins = 0;
    bin_room = bin_first = bin_end = NULL;
    stat_single = stat_pieces_sites = stat_pieces_total = 0;
    memset(sh_tab, 0, sizeof(sh_tab));
    sh_copies = sh_words = 0;
    chain_sites = chain_hops = 0;
    g_dens = NULL;
    fp_sites = fp_vspill_before = fp_vspill_after = fp_words_before = fp_words_after = 0;
    wide_budget = 2000;
    cmp_out = NULL;
    memset(size_hist, 0, sizeof(size_hist));
}

int switch_plan_make(const struct switch_plan_file *files, size_t nfiles, uint8_t **plan, size_t *plan_size, char **log)
{
    char *argv[] = {"plan", "-o", "plan", CACHE_PATH, NULL};
    plan_files = files;
    plan_nfiles = nfiles;
    plan_out = (struct bytes){0};
    plan_log = (struct bytes){0};
    bytes_reserve(&plan_log, 0);
    plan_log.data[0] = 0;
    reset();
    switch_plan_optind = 1;
    switch_plan_optarg = NULL;
    int rc = setjmp(plan_bail);
    if (!rc)
        rc = a64x_cmd_plan2(4, argv);
    plan_free_all();
    reset();
    if (!rc && !plan_out.len) {
        plan_printf("no plan written\n");
        rc = 1;
    }
    *plan = (uint8_t *)plan_out.data;
    *plan_size = plan_out.len;
    *log = plan_log.data;
    plan_out = (struct bytes){0};
    plan_log = (struct bytes){0};
    return rc ? 1 : 0;
}

void switch_plan_release(void *buffer)
{
    free(buffer);
}
