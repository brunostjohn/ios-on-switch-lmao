// SPDX-License-Identifier: BSD-2-Clause-Patent
// The development probe's parts (UsbDmaProbe.h): reads of a buffer's first bytes through the cache and around it, and
// the table of mappings still held (UsbDmaProbeHeld.cpp).
#pragma once

#if DEVELOPMENT || DEBUG
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/cpu_number.h>
#include <stdint.h>
#include <string.h>
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "UsbDmaCache.h"

extern "C" {
// osfmk/arm/pmap/pmap.h: the per-CPU copy windows XNU's own cache maintenance and bcopy_phys use for pages outside the
// aperture. They write one VM page's PTEs with the given attributes and never touch the page's own
// (pmap_cache_attributes) or the aperture's mapping of it; only the PPL panics on a managed page, and this kernel has
// none.
unsigned int pmap_map_cpu_windows_copy(ppnum_t pn, vm_prot_t prot, unsigned int wimg_bits);
void pmap_unmap_cpu_windows_copy(unsigned int index);
vm_offset_t pmap_cpu_windows_copy_addr(int cpu_num, unsigned int index);
uint64_t kvtophys(vm_offset_t va);
void _disable_preemption(void);  // NOLINT(bugprone-reserved-identifier): XNU's name
void _enable_preemption(void);   // NOLINT(bugprone-reserved-identifier): XNU's name
}

namespace usb_dma_probe {
constexpr unsigned int kWimgDevice = 0x7;  // VM_WIMG_IO (osfmk/arm/memory_types.h, both XNUs): Device-nGnRE

inline uint64_t firstBytes(uint64_t length)
{
    return length < 8 ? length : 8;
}

// The aperture's alias stays cacheable whatever the page's own attributes (no __ARM_PTE_PHYSMAP__ without the PPL),
// so only a cacheable descriptor's bytes are read through it.
inline bool cacheable(IOMemoryDescriptor *memory)
{
    return usbDmaCacheable(memory->getFlags());
}

struct Hex {
    char text[17];

    explicit Hex(const uint8_t bytes[8]) : text()
    {
        static const char kDigits[] = "0123456789abcdef";
        for (size_t i = 0; i < 8; ++i) {
            text[2 * i] = kDigits[bytes[i] >> 4];
            text[(2 * i) + 1] = kDigits[bytes[i] & 0xfu];
        }
    }
};

// Where XNU's and our maintenance issue their DC instructions for pa (phystokv_range, as dcache_incoherent_io_*64
// and switch_dcache_phys_lines do), and what the MMU translates that address back to.
struct Aperture {
    uint64_t va = 0, back = 0;

    explicit Aperture(uint64_t pa)
    {
        if (pa == 0u || !switch_dma_pa_managed(pa))
            return;
        size_t chunk = 1;
        va = phystokv_range(pa, &chunk);
        back = kvtophys(static_cast<vm_offset_t>(va));
    }
};

inline void readCached(uint64_t pa, uint64_t length, uint8_t out[8])
{
    if (pa == 0u || !switch_dma_pa_managed(pa))
        return;
    size_t chunk = static_cast<size_t>(firstBytes(length));
    const uint64_t va = phystokv_range(pa, &chunk);
    memcpy(out, reinterpret_cast<const void *>(va), chunk);
}

// Byte loads: Device memory takes no unaligned access.
inline void readUncached(uint64_t pa, uint64_t length, uint8_t out[8])
{
    if (pa == 0u || !switch_dma_pa_managed(pa))
        return;
    const uint64_t inPage = PAGE_SIZE - (pa & PAGE_MASK);
    const uint64_t n = firstBytes(length) < inPage ? firstBytes(length) : inPage;
    _disable_preemption();
    const unsigned int index = pmap_map_cpu_windows_copy(static_cast<ppnum_t>(atop_64(pa)), VM_PROT_READ, kWimgDevice);
    const auto *alias =
        reinterpret_cast<const volatile uint8_t *>(pmap_cpu_windows_copy_addr(cpu_number(), index) | (pa & PAGE_MASK));
    for (uint64_t i = 0; i < n; ++i)
        out[i] = alias[i];
    pmap_unmap_cpu_windows_copy(index);
    _enable_preemption();
}

// UsbDmaProbeHeld.cpp. path: "own address", "window" or "bounced"; pa: the mapping's first byte (0 for a bounce).
void heldMap(IOMemoryDescriptor *memory, uint64_t address, uint64_t pa, const char *path, bool writable);
void heldUnmap(const IOMemoryDescriptor *memory, uint64_t address);
// One line per held mapping that is wrong to hold (cacheable, device-writable, at its own address or in the window).
void sampleHeld(const char *when);
}  // namespace usb_dma_probe
#endif
