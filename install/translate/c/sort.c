// SPDX-License-Identifier: BSD-3-Clause
/*-
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Ronnie Kon at Mindcraft Inc., Kevin Lew and Elmer Yglesias.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * macOS libc's qsort, step for step: Apple Libc's stdlib/FreeBSD/qsort.c and heapsort.c. The dyld cache planner sorts
 * with comparators that tie, so its plan depends on where equal elements land, and CI's CLI gets macOS's order. Only
 * this copy reproduces it: FreeBSD's current qsort.c and a stable sort each give another plan. Apple's copy differs
 * from FreeBSD's in a recursion-depth limit that falls back to heapsort and an insertion-sort pass that gives up
 * after 1 + n/4 swaps.
 */
#include "sort.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int cmp_t(const void *, const void *);

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static char *med3(char *a, char *b, char *c, cmp_t *cmp)
{
    return cmp(a, b) < 0 ? (cmp(b, c) < 0 ? b : (cmp(a, c) < 0 ? c : a))
                         : (cmp(b, c) > 0 ? b : (cmp(a, c) < 0 ? a : c));
}

static int depth_of(size_t n)
{
    int fls = 0;
    for (uint64_t v = n; v; v >>= 1)
        fls++;
    return 2 * (fls - 1);
}

static void heapsort_bytes(void *vbase, size_t nmemb, size_t size, cmp_t *cmp)
{
    if (nmemb <= 1 || !size)
        return;
    char *k = malloc(size);
    if (!k)
        abort();
    char *base = (char *)vbase - size;
    size_t i, j, l;
    char *p, *t;
    for (l = nmemb / 2 + 1; --l;) {
        for (i = l; (j = i * 2) <= nmemb; i = j) {
            p = base + j * size;
            if (j < nmemb && cmp(p, p + size) < 0) {
                p += size;
                ++j;
            }
            t = base + i * size;
            if (cmp(p, t) <= 0)
                break;
            swap_bytes(t, p, size);
        }
    }
    while (nmemb > 1) {
        memcpy(k, base + nmemb * size, size);
        memcpy(base + nmemb * size, base + size, size);
        --nmemb;
        for (i = 1; (j = i * 2) <= nmemb; i = j) {
            p = base + j * size;
            if (j < nmemb && cmp(p, p + size) < 0) {
                p += size;
                ++j;
            }
            t = base + i * size;
            memcpy(t, p, size);
        }
        for (;;) {
            j = i;
            i = j / 2;
            p = base + j * size;
            t = base + i * size;
            if (j == 1 || cmp(k, t) < 0) {
                memcpy(p, k, size);
                break;
            }
            memcpy(p, t, size);
        }
    }
    free(k);
}

static bool insertion_sort(char *a, size_t n, size_t es, cmp_t *cmp, int swap_limit)
{
    int swaps = 0;
    for (char *pm = a + es; pm < a + n * es; pm += es) {
        for (char *pl = pm; pl > a && cmp(pl - es, pl) > 0; pl -= es) {
            swap_bytes(pl, pl - es, es);
            if (swap_limit && ++swaps > swap_limit)
                return false;
        }
    }
    return true;
}

static void sort(char *a, size_t n, size_t es, cmp_t *cmp, int depth_limit)
{
    char *pa, *pb, *pc, *pd, *pl, *pm, *pn;
    size_t d1, d2;
    int r, swapped;

loop:
    swapped = 0;
    if (depth_limit-- <= 0) {
        heapsort_bytes(a, n, es, cmp);
        return;
    }
    if (n <= 7) {
        insertion_sort(a, n, es, cmp, 0);
        return;
    }
    pl = a;
    pm = a + (n / 2) * es;
    pn = a + (n - 1) * es;
    if (n > 40) {
        size_t d = (n / 8) * es;
        pl = med3(pl, pl + d, pl + 2 * d, cmp);
        pm = med3(pm - d, pm, pm + d, cmp);
        pn = med3(pn - 2 * d, pn - d, pn, cmp);
    }
    pm = med3(pl, pm, pn, cmp);
    swap_bytes(a, pm, es);
    pa = pb = a + es;
    pc = pd = a + (n - 1) * es;
    for (;;) {
        while (pb <= pc && (r = cmp(pb, a)) <= 0) {
            if (r == 0) {
                swapped = 1;
                swap_bytes(pa, pb, es);
                pa += es;
            }
            pb += es;
        }
        while (pb <= pc && (r = cmp(pc, a)) >= 0) {
            if (r == 0) {
                swapped = 1;
                swap_bytes(pc, pd, es);
                pd -= es;
            }
            pc -= es;
        }
        if (pb > pc)
            break;
        swap_bytes(pb, pc, es);
        swapped = 1;
        pb += es;
        pc -= es;
    }
    pn = a + n * es;
    d1 = (size_t)(pa - a) < (size_t)(pb - pa) ? (size_t)(pa - a) : (size_t)(pb - pa);
    swap_bytes(a, pb - d1, d1);
    d1 = (size_t)(pd - pc) < (size_t)(pn - pd) - es ? (size_t)(pd - pc) : (size_t)(pn - pd) - es;
    swap_bytes(pb, pn - d1, d1);

    if (!swapped && insertion_sort(a, n, es, cmp, 1 + (int)(n / 4)))
        return;

    d1 = (size_t)(pb - pa);
    d2 = (size_t)(pd - pc);
    if (d1 <= d2) {
        if (d1 > es)
            sort(a, d1 / es, es, cmp, depth_limit);
        if (d2 > es) {
            a = pn - d2;
            n = d2 / es;
            goto loop;
        }
    } else {
        if (d2 > es)
            sort(pn - d2, d2 / es, es, cmp, depth_limit);
        if (d1 > es) {
            n = d1 / es;
            goto loop;
        }
    }
}

void switch_qsort(void *base, size_t n, size_t es, int (*cmp)(const void *, const void *))
{
    sort(base, n, es, cmp, depth_of(n));
}
