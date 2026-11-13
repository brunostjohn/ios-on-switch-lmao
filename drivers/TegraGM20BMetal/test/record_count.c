// SPDX-License-Identifier: MIT
// What one thread does while it records (raw_record_cost_test): its Objective-C messages, locks and allocations, counted
// by interposers in this library, which the test links so dyld applies them at launch. Only the thread that called
// record_count_start counts, so a completion or a queue thread of the facade never adds to it.
#include <malloc/malloc.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                 \
    __attribute__((used)) static struct {                                       \
        const void *replacement;                                                \
        const void *replacee;                                                   \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee};

struct record_count {
    uint64_t sends, locks, mallocs, frees;
};

// the counting thread's TPIDRRO_EL0 with its low bits clear (0: none), which the message trampoline compares
uintptr_t record_count_thread;
uint64_t record_count_sends;
static uint64_t locks, mallocs, frees;

static uintptr_t threadWord(void)
{
    uintptr_t word;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(word));
    return word & ~(uintptr_t)7;
}

static bool counting(void)
{
    return record_count_thread && threadWord() == record_count_thread;
}

__asm__(".globl _record_count_msgSend\n"
        ".p2align 2\n"
        "_record_count_msgSend:\n"
        "    adrp x16, _record_count_thread@PAGE\n"
        "    ldr x9, [x16, _record_count_thread@PAGEOFF]\n"
        "    cbz x9, 1f\n"
        "    mrs x10, TPIDRRO_EL0\n"
        "    and x10, x10, #~7\n"
        "    cmp x9, x10\n"
        "    b.ne 1f\n"
        "    adrp x16, _record_count_sends@PAGE\n"
        "    add x16, x16, _record_count_sends@PAGEOFF\n"
        "    ldr x9, [x16]\n"
        "    add x9, x9, #1\n"
        "    str x9, [x16]\n"
        "1:\n"
        "    b _objc_msgSend\n");

extern void objc_msgSend(void);
extern void record_count_msgSend(void);
DYLD_INTERPOSE(record_count_msgSend, objc_msgSend)

static int countedMutexLock(pthread_mutex_t *m)
{
    locks += counting();
    return pthread_mutex_lock(m);
}
DYLD_INTERPOSE(countedMutexLock, pthread_mutex_lock)

static int countedMutexTryLock(pthread_mutex_t *m)
{
    locks += counting();
    return pthread_mutex_trylock(m);
}
DYLD_INTERPOSE(countedMutexTryLock, pthread_mutex_trylock)

static void countedUnfairLock(os_unfair_lock_t l)
{
    locks += counting();
    os_unfair_lock_lock(l);
}
DYLD_INTERPOSE(countedUnfairLock, os_unfair_lock_lock)

static bool countedUnfairTryLock(os_unfair_lock_t l)
{
    locks += counting();
    return os_unfair_lock_trylock(l);
}
DYLD_INTERPOSE(countedUnfairTryLock, os_unfair_lock_trylock)

typedef void(malloc_logger_t)(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result,
                              uint32_t num_hot_frames_to_skip);
extern malloc_logger_t *malloc_logger;

static void countAllocations(uint32_t type, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uint32_t skip)
{
    (void)a, (void)b, (void)c, (void)d, (void)skip;
    enum {
        kAllocate = 2,
        kDeallocate = 4
    };
    if (!counting())
        return;
    mallocs += (type & kAllocate) != 0;
    frees += (type & kDeallocate) != 0;
}

void record_count_start(void)
{
    record_count_sends = locks = mallocs = frees = 0;
    malloc_logger = countAllocations;
    atomic_thread_fence(memory_order_seq_cst);
    record_count_thread = threadWord();
}

void record_count_stop(struct record_count *out)
{
    record_count_thread = 0;
    atomic_thread_fence(memory_order_seq_cst);
    malloc_logger = 0;
    out->sends = record_count_sends;
    out->locks = locks;
    out->mallocs = mallocs;
    out->frees = frees;
}
