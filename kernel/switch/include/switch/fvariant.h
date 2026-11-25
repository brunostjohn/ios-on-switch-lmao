// SPDX-License-Identifier: GPL-2.0-only
/*
 * dyld shared cache function-variant slots that the shared region pager fills (osfmk/vm/vm_shared_region_pager.c).
 *
 * iOS 27's cache keeps some function-variant pointers in __TPRO_CONST and ships them as 0: dyld selects and stores
 * each one in every process (Loader::adjustFunctionVariantsInDyldCache). Apple's kernel tells a TPRO-capable process
 * dyld_hw_tpro_pagers=1, and the store goes straight to the TPRO page. The A57 has no TPRO, so dyld's lazy cache
 * writer instead remaps every __DATA_CONST and __AUTH_CONST region of the cache VM_PROT_COPY to store those words,
 * which takes about 356 MB of read-only cache out of the shared nested pmap in each process. The pager delivers the
 * page with the value dyld selects on this system already stored, dyld's compare matches, and it writes nothing.
 *
 * A wrong value is harmless: dyld then stores its own, as it would without this.
 */
#ifndef SWITCH_FVARIANT_H
#define SWITCH_FVARIANT_H

#include <stddef.h>
#include <stdint.h>

#include "fvariant_24B119.h"
#include "fvariant_types.h"

static const struct switch_fvariant_cache *const switch_fvariant_caches[] = {
    &switch_fvariant_24B119,
};

static inline const struct switch_fvariant_cache *switch_fvariant_lookup(const uint8_t uuid[16])
{
    for (size_t i = 0; i < sizeof(switch_fvariant_caches) / sizeof(switch_fvariant_caches[0]); ++i) {
        if (__builtin_memcmp(switch_fvariant_caches[i]->uuid, uuid, 16) == 0) {
            return switch_fvariant_caches[i];
        }
    }
    return NULL;
}

/*
 * Stores the slots of `cache` that lie in the part of a page being paged in: `page` is its kernel address, `uaddr`
 * its address in the shared region, `slide` the region's slide. A slot is stored only while it holds the 0 the file
 * ships, and as a plain pointer: this kernel has no pointer authentication, and dyld's signing of it is the identity
 * on the A57 too. Returns how many it stored.
 */
static inline uint32_t switch_fvariant_fill(const struct switch_fvariant_cache *cache, uint32_t slide, uint8_t *page,
                                            uint64_t uaddr, uint64_t size)
{
    uint32_t filled = 0;
    if (cache == NULL || size < (uint64_t)sizeof(uint64_t)) {
        return 0;
    }
    for (uint32_t i = 0; i < cache->count; ++i) {
        const uint64_t at = cache->slots[i].slot + slide;
        if (at < uaddr || at - uaddr > size - (uint64_t)sizeof(uint64_t)) {
            continue;
        }
        uint64_t value = 0;
        __builtin_memcpy(&value, page + (at - uaddr), sizeof(value));
        if (value != 0) {
            continue;
        }
        value = cache->slots[i].target + slide;
        __builtin_memcpy(page + (at - uaddr), &value, sizeof(value));
        ++filled;
    }
    return filled;
}

#ifdef XNU_KERNEL_PRIVATE
struct vm_shared_region;
struct vm_shared_region_slide_info;

/* The pinned slots of the cache `sr` maps, read from its header in the mapping task; NULL for any other cache. */
const struct switch_fvariant_cache *switch_fvariant_for_region(struct vm_shared_region *sr);

/* Fills the slots of si's cache in [uaddr, uaddr + size), a just-slid part of a page at kernel address `page`. */
void switch_fvariant_fill_page(const struct vm_shared_region_slide_info *si, uint8_t *page, uint64_t uaddr,
                               uint64_t size);
#endif

#endif
