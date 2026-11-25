// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thread call functions by the CPU they used (callouts.c, debug kernels). The scheduling report names a thread call
 * worker ("thread call kernel-high #3") but not the functions it ran; thread_call_invoke adds each function's on-core
 * time and calls to its CPU's table, and the report adds the tables up. A table has one writer, its CPU with preemption
 * disabled, and is read by the report; a function a full table has no row for goes to its `other` row. Rows are never
 * freed: a kernel has a bounded set of thread call functions. Plain C, so kernel/tests runs it on the host; the
 * including file declares snprintf. Times are mach ticks.
 */
#ifndef SWITCH_SCHED_CALLOUTS_H
#define SWITCH_SCHED_CALLOUTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_CALLOUT_ROWS 64u /* a power of two */
#define SWITCH_CALLOUT_SUM  256u
#define SWITCH_CALLOUT_TOP  4u

struct switch_callout_row {
    uint64_t func; /* 0: empty */
    uint64_t cpu, calls;
};

struct switch_callout_table {
    struct switch_callout_row rows[SWITCH_CALLOUT_ROWS];
    struct switch_callout_row other;
};

static inline void switch_callout_bump(uint64_t *field, uint64_t n)
{
    __atomic_store_n(field, __atomic_load_n(field, __ATOMIC_RELAXED) + n, __ATOMIC_RELAXED);
}

static inline void switch_callout_add(struct switch_callout_table *t, uint64_t func, uint64_t cpu)
{
    uint32_t i = (uint32_t)((func * 0x9e3779b97f4a7c15ull) >> 58) & (SWITCH_CALLOUT_ROWS - 1);
    struct switch_callout_row *row = &t->other;

    for (uint32_t probe = 0; probe < SWITCH_CALLOUT_ROWS; probe++, i = (i + 1) & (SWITCH_CALLOUT_ROWS - 1)) {
        const uint64_t at = __atomic_load_n(&t->rows[i].func, __ATOMIC_RELAXED);

        if (at == func) {
            row = &t->rows[i];
            break;
        }
        if (at == 0) {
            row = &t->rows[i];
            __atomic_store_n(&row->func, func, __ATOMIC_RELEASE);
            break;
        }
    }
    switch_callout_bump(&row->cpu, cpu);
    switch_callout_bump(&row->calls, 1);
}

/* The report's running sums by function, across every CPU's table: totals since boot. */
struct switch_callout_sum {
    uint32_t count;
    struct switch_callout_row rows[SWITCH_CALLOUT_SUM];
    struct switch_callout_row other;
};

static inline struct switch_callout_row *switch_callout_sum_row(struct switch_callout_sum *s, uint64_t func)
{
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->rows[i].func == func) {
            return &s->rows[i];
        }
    }
    if (s->count == SWITCH_CALLOUT_SUM) {
        return &s->other;
    }
    s->rows[s->count].func = func;
    s->rows[s->count].cpu = s->rows[s->count].calls = 0;
    return &s->rows[s->count++];
}

static inline void switch_callout_sum_table(struct switch_callout_sum *s, const struct switch_callout_table *t)
{
    for (uint32_t i = 0; i < SWITCH_CALLOUT_ROWS; i++) {
        const uint64_t func = __atomic_load_n(&t->rows[i].func, __ATOMIC_ACQUIRE);

        if (func != 0) {
            struct switch_callout_row *row = switch_callout_sum_row(s, func);

            row->cpu += __atomic_load_n(&t->rows[i].cpu, __ATOMIC_RELAXED);
            row->calls += __atomic_load_n(&t->rows[i].calls, __ATOMIC_RELAXED);
        }
    }
    s->other.cpu += __atomic_load_n(&t->other.cpu, __ATOMIC_RELAXED);
    s->other.calls += __atomic_load_n(&t->other.calls, __ATOMIC_RELAXED);
}

static inline size_t switch_callout_put(size_t size, size_t at, int n)
{
    if (n < 0 || at >= size) {
        return at;
    }
    return at + (size_t)n < size ? at + (size_t)n : size - 1;
}

static inline struct switch_callout_row switch_callout_delta(const struct switch_callout_row *now,
                                                             const struct switch_callout_sum *seen)
{
    struct switch_callout_row d = *now;

    for (uint32_t j = 0; j < seen->count; j++) {
        if (seen->rows[j].func == d.func) {
            d.cpu -= seen->rows[j].cpu < d.cpu ? seen->rows[j].cpu : d.cpu;
            d.calls -= seen->rows[j].calls < d.calls ? seen->rows[j].calls : d.calls;
            break;
        }
    }
    return d;
}

/* `d` into `top` (most CPU first); what that pushes out goes to `rest`. */
static inline void switch_callout_rank(struct switch_callout_row *top, struct switch_callout_row d,
                                       struct switch_callout_row *rest)
{
    for (uint32_t k = 0; k < SWITCH_CALLOUT_TOP; k++) {
        if (d.cpu > top[k].cpu) {
            const struct switch_callout_row lower = top[k];

            top[k] = d;
            d = lower;
        }
    }
    rest->cpu += d.cpu;
    rest->calls += d.calls;
}

/*
 * "0xfffffff0081a2c40 736 ms (5400), ...; others 3 ms (12)": the window's SWITCH_CALLOUT_TOP functions by CPU, from
 * the sums now and at the last report (`seen`, which becomes `now`). `unslide` turns a function into the address the
 * kernel's symbols give.
 */
static inline size_t switch_callout_format(char *out, size_t size, const struct switch_callout_sum *now,
                                           struct switch_callout_sum *seen, uint64_t (*unslide)(uint64_t),
                                           uint32_t numer, uint32_t denom)
{
    struct switch_callout_row top[SWITCH_CALLOUT_TOP];
    struct switch_callout_row rest = {0, 0, 0};
    size_t at = 0;

    __builtin_memset(top, 0, sizeof(top));

    for (uint32_t i = 0; i < now->count; i++) {
        switch_callout_rank(top, switch_callout_delta(&now->rows[i], seen), &rest);
    }
    rest.cpu += now->other.cpu - (seen->other.cpu < now->other.cpu ? seen->other.cpu : now->other.cpu);
    rest.calls += now->other.calls - (seen->other.calls < now->other.calls ? seen->other.calls : now->other.calls);
    *seen = *now;
    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    for (uint32_t k = 0; k < SWITCH_CALLOUT_TOP && top[k].cpu != 0; k++) {
        at = switch_callout_put(size, at,
                                snprintf(out + at, size - at, "%s0x%llx %llu ms (%llu)", k ? ", " : "",
                                         (unsigned long long)unslide(top[k].func),
                                         (unsigned long long)(top[k].cpu * numer / denom / 1000000u),
                                         (unsigned long long)top[k].calls));
    }
    return switch_callout_put(size, at,
                              snprintf(out + at, size - at, "%sothers %llu ms (%llu)", at ? "; " : "",
                                       (unsigned long long)(rest.cpu * numer / denom / 1000000u),
                                       (unsigned long long)rest.calls));
}

#endif /* SWITCH_SCHED_CALLOUTS_H */
