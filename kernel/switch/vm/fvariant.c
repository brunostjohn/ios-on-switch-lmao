// SPDX-License-Identifier: GPL-2.0-only
/*
 * The shared region side of <switch/fvariant.h>: which pinned cache a shared region maps, and the fill of a slid page.
 */

#include <switch/fvariant.h>

#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <libkern/OSAtomic.h>
#include <os/log.h>
#include <vm/vm_shared_region_xnu.h>

const struct switch_fvariant_cache *switch_fvariant_for_region(struct vm_shared_region *sr)
{
    uint8_t uuid[16];

    /*
     * Called while sr's mappings are slid, before vm_shared_region_map_file_final() copies the UUID into sr: the
     * header is already mapped in the mapping task, which is where final reads it too.
     */
    if (sr->sr_first_mapping == (mach_vm_offset_t)-1 ||
        copyin((user_addr_t)(sr->sr_base_address + sr->sr_first_mapping + offsetof(struct _dyld_cache_header, uuid)),
               uuid, sizeof(uuid)) != 0) {
        return NULL;
    }
    const struct switch_fvariant_cache *cache = switch_fvariant_lookup(uuid);
#if DEVELOPMENT || DEBUG
    static uint32_t logged;
    if (cache != NULL && OSCompareAndSwap(0, 1, &logged)) {
        os_log(OS_LOG_DEFAULT, "SwitchFvariant: cache %02x%02x%02x%02x: the pager fills %u function-variant slots",
               uuid[0], uuid[1], uuid[2], uuid[3], cache->count);
    }
#endif
    return cache;
}

void switch_fvariant_fill_page(const struct vm_shared_region_slide_info *si, uint8_t *page, uint64_t uaddr,
                               uint64_t size)
{
#if __has_feature(ptrauth_calls)
    /* The slots are authenticated pointers, which a kernel with pointer authentication would have to sign. */
    (void)si;
    (void)page;
    (void)uaddr;
    (void)size;
#else
    const uint32_t filled = switch_fvariant_fill(si->si_switch_fvariant, si->si_slide, page, uaddr, size);
#if DEVELOPMENT || DEBUG
    if (filled != 0) {
        os_log(OS_LOG_DEFAULT, "SwitchFvariant: filled %u function-variant slots in 0x%llx", filled, uaddr);
    }
#else
    (void)filled;
#endif
#endif
}
