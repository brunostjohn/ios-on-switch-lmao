// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#include <stddef.h>
#define SWITCH_MEM_COMMIT     0x1000ul
#define SWITCH_MEM_RESERVE    0x2000ul
#define SWITCH_MEM_RELEASE    0x8000ul
#define SWITCH_PAGE_READWRITE 0x04ul
__declspec(dllimport) void *__stdcall VirtualAlloc(void *addr, size_t size, unsigned long type, unsigned long protect);
__declspec(dllimport) int __stdcall VirtualFree(void *addr, size_t size, unsigned long free_type);
#else
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include <sys/mman.h>
#endif

#include "vmem.h"

void *switch_vmem_alloc(size_t size)
{
    if (!size)
        return NULL;
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, SWITCH_MEM_COMMIT | SWITCH_MEM_RESERVE, SWITCH_PAGE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

void switch_vmem_free(void *ptr, size_t size)
{
    if (!ptr || !size)
        return;
#if defined(_WIN32)
    (void)size;
    VirtualFree(ptr, 0, SWITCH_MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}
