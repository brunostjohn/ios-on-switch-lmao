// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SWITCH_TRANSLATE_SORT_H
#define SWITCH_TRANSLATE_SORT_H

#include <stddef.h>

void switch_qsort(void *base, size_t n, size_t es, int (*cmp)(const void *, const void *));

#endif
