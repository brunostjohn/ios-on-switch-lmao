// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SWITCH_TRANSLATE_VMEM_H
#define SWITCH_TRANSLATE_VMEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direct OS anonymous zero-filled pages (`mmap`/`VirtualAlloc`), returned to the OS immediately on free. */
void *switch_vmem_alloc(size_t size);
void switch_vmem_free(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

#endif
