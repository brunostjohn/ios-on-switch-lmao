// SPDX-License-Identifier: MIT
// See raw_gpu.h (createArena). An arena's GPU VA is a reservation and its host range is never given back while it lives: a
// purge frees the BO (the kernel releases the pages) and replaces the host range with untouched pages, so neither address
// moves and a restore is one import and one bind at the same VA, no texture header or CPU pointer rewritten.
#include "raw_gpu.h"
#include "TegraGM20BLayout.h"

#include <mach/mach.h>
#include <mach/memory_object_types.h>

namespace raw {
namespace {
constexpr uint64_t kArenaGranule = 0x10000;  // the GMMU big page every VA range starts and ends on (raw_kernel.cpp)
constexpr uint64_t kArenaPitchAlign = 256;   // a pitch render target's address and rows (linearTarget), a cbuf's base

// An arena's host pages at `at` (`fixed`: in place of the ones there). A shared heap's are write-combined, as the device's
// shared buffers are (Buffer::wc): a heap used whole (useHeap) leaves no resource of it for an encoder to clean, so what the
// CPU writes there must need no clean. A private heap's are cacheable: the CPU never writes them.
bool hostPages(uint64_t bytes, bool writeCombined, vm_address_t &at, bool fixed)
{
    const int place = fixed ? VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE : VM_FLAGS_ANYWHERE;
    if (!writeCombined)
        return vm_allocate(mach_task_self(), &at, bytes, place) == KERN_SUCCESS;
    memory_object_size_t size = bytes;
    mach_port_t entry = MACH_PORT_NULL;
    vm_prot_t permission = MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE;
    SET_MAP_MEM(MAP_MEM_WCOMB, permission);
    if (mach_make_memory_entry_64(mach_task_self(), &size, 0, permission, &entry, MACH_PORT_NULL) != KERN_SUCCESS)
        return false;
    const kern_return_t kr = vm_map(mach_task_self(), &at, vm_size_t(bytes), 0, place, entry, 0, FALSE,
                                    VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    mach_port_deallocate(mach_task_self(), entry);
    return kr == KERN_SUCCESS;
}
}  // namespace

bool Device::arenaFootprint(uint32_t width, uint32_t height, uint32_t format, bool blockLinear, uint64_t &bytes,
                            uint64_t &align)
{
    TextureAsk ask;
    ask.width = width;
    ask.height = height;
    ask.format = format;
    ask.tiled = blockLinear;
    return footprint(ask, bytes, align);
}

// The host range's pages wired and bound at the arena's VA, under its kind (lock_ held)
bool Device::bindArena(Arena &a)
{
    bool bound = k_.importPages(a.cpu, a.bytes, a.bo) && k_.bindImportAt(a.cpu, a.bo, a.va, a.kind) &&
                 (a.bufferVa == a.va || k_.bindAlias(a.bo, a.bufferVa, kPteKindPitch));
    for (const Arena::View &v : a.views)
        bound = bound && k_.bindAlias(a.bo, v.va, v.kind);
    if (!bound) {
        k_.free(a.bo);
        return false;
    }
    RAW_DIAGNOSTIC(stats.poolBytes += a.bytes);
    return true;
}

bool Device::createArena(uint64_t bytes, bool blockLinear, uint32_t &arena, bool explicitPlacement)
{
    const uint64_t logicalBytes = bytes;
    if (bytes > UINT64_MAX - (kArenaGranule - 1))
        return false;
    bytes = (bytes + kArenaGranule - 1) & ~(kArenaGranule - 1);
    vm_address_t cpu = 0;
    if (!bytes || !hostPages(bytes, !blockLinear, cpu, false))
        return false;
    Arena a;
    a.wc = !blockLinear;
    a.bytes = bytes;
    a.kind = blockLinear ? TGM_LAYOUT_KIND : kPteKindPitch;
    a.cpu = reinterpret_cast<uint8_t *>(cpu);
    a.place = HeapPlacement(logicalBytes, explicitPlacement);
    a.used = true;
    std::lock_guard<std::mutex> l(lock_);
    a.va = k_.reserveVa(bytes);
    // Kind 0xfe changes the sector interpretation inside a GOB. Buffers take a pitch view of the same BO.
    a.bufferVa = blockLinear ? k_.reserveVa(bytes) : a.va;
    if (!a.va || !a.bufferVa || !bindArena(a)) {
        if (a.va)
            k_.releaseVa(a.va, bytes);
        if (a.bufferVa && a.bufferVa != a.va)
            k_.releaseVa(a.bufferVa, bytes);
        vm_deallocate(mach_task_self(), cpu, bytes);
        return false;
    }
    uint32_t i = 0;
    while (i < arenas_.size() && arenas_[i].used)
        i++;
    if (i == arenas_.size())
        arenas_.emplace_back();
    arenas_[i] = std::move(a);
    arena = i;
    return true;
}

void Device::freeArena(Arena &a)
{
    if (a.purged) {
        purgedArenas_--;
    } else {
        k_.free(a.bo);
        RAW_DIAGNOSTIC(stats.poolBytes -= a.bytes);
    }
    k_.releaseVa(a.va, a.bytes);
    if (a.bufferVa != a.va)
        k_.releaseVa(a.bufferVa, a.bytes);
    for (const Arena::View &v : a.views)
        k_.releaseVa(v.va, a.bytes);
    vm_deallocate(mach_task_self(), vm_address_t(a.cpu), a.bytes);
    a = Arena();
}

void Device::destroyArena(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].used)
        return;
    Arena &a = arenas_[arena];
    a.dying = true;
    if (!a.leases)
        freeArena(a);
}

void Device::arenaFree(Span &s)
{
    Arena &a = arenas_[s.chunk];
    a.place.release(s.offset, s.bytes);
    s = {};
    if (a.dying && !a.leases)
        freeArena(a);
}

bool Device::aliasArena(const Span &s, Lease *&lease, Alias &a)
{
    const Arena &ar = arenas_[s.chunk];
    if (ar.purged)
        return false;
    a.cpu = ar.cpu;
    a.bytes = ar.bytes;
    a.handle = ar.bo.handle;
    uint64_t base = 0;
    if (!arenaKindAt(ar, s.va, base, a.kind))
        return false;
    a.va = base;
    a.lease = leaseSpan(lease, s, Memory::Arena);
    return true;
}

uint64_t Device::arenaView(Arena &a, uint32_t kind)
{
    if (kind == a.kind)
        return a.va;
    if (kind == kPteKindPitch && a.bufferVa != a.va)
        return a.bufferVa;
    for (const Arena::View &v : a.views)
        if (v.kind == kind)
            return v.va;
    if (a.purged)
        return 0;
    const uint64_t va = k_.reserveVa(a.bytes);
    if (!va)
        return 0;
    if (!k_.bindAlias(a.bo, va, kind)) {
        k_.releaseVa(va, a.bytes);
        return 0;
    }
    a.views.push_back({va, kind});
    return va;
}

bool Device::arenaKindAt(const Arena &a, uint64_t va, uint64_t &base, uint32_t &kind)
{
    const auto in = [&](uint64_t start, uint32_t k) {
        if (va < start || va - start >= a.bytes)
            return false;
        base = start;
        kind = k;
        return true;
    };
    if (in(a.va, a.kind) || in(a.bufferVa, kPteKindPitch))
        return true;
    for (const Arena::View &v : a.views)
        if (in(v.va, v.kind))
            return true;
    return false;
}

bool Device::arenaSpan(const ArenaAt &at, uint64_t bytes, uint64_t align, uint32_t kind, Texture &t)
{
    if (at.arena >= arenas_.size() || !arenas_[at.arena].used || arenas_[at.arena].dying)
        return false;
    Arena &a = arenas_[at.arena];
    uint64_t offset = at.offset.value_or(0);
    const uint64_t view = arenaView(a, kind);
    if (!view || !(at.offset ? a.place.placeAt(bytes, align, offset) : a.place.place(bytes, align, offset)))
        return false;
    t.span = {at.arena, offset, bytes, view + offset, a.cpu + offset};
    t.arena = true;
    t.wc = a.wc;
    return true;
}

bool Device::arenaTexture(uint32_t arena, uint32_t width, uint32_t height, uint32_t format, Texture &t,
                          std::optional<uint64_t> requestedOffset)
{
    bool blockLinear = false;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (arena >= arenas_.size() || !arenas_[arena].used)
            return false;
        blockLinear = arenas_[arena].kind != kPteKindPitch;
    }
    TextureAsk ask;
    ask.width = width;
    ask.height = height;
    ask.format = format;
    ask.tiled = blockLinear;
    return arenaTexture({arena, requestedOffset}, ask, t);
}

// An extended-range texture's stand-in follows it at the stand-in's alignment (footprint), where a placement heap's
// offset names the texture's
bool Device::arenaTexture(const ArenaAt &at, const TextureAsk &ask, Texture &t)
{
    const bool made = ask.kind == TextureAsk::Plain
                          ? createTexture(ask.width, ask.height, ask.format, t, false, ask.tiled, &at)
                          : createLaidOut(ask, t, &at);
    if (!made || !ask.standIn || !xrPipeline(xrForm(t.format)))
        return made;
    ArenaAt standIn = at;
    uint64_t bytes = 0, align = 0;
    if (!footprint(standInAsk(t), bytes, align)) {
        destroyTexture(t);
        return false;
    }
    if (at.offset)
        standIn.offset = (*at.offset + t.span.bytes + align - 1) / align * align;
    if (!createStandIn(t, &standIn)) {
        destroyTexture(t);
        return false;
    }
    return true;
}

bool Device::arenaBuffer(uint32_t arena, uint64_t length, Buffer &b, std::optional<uint64_t> requestedOffset)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].used || arenas_[arena].dying ||
        (arenas_[arena].kind != kPteKindPitch && arenas_[arena].bufferVa == arenas_[arena].va) ||
        length > UINT64_MAX - (kArenaPitchAlign - 1))
        return false;
    Arena &a = arenas_[arena];
    const uint64_t bytes = (std::max<uint64_t>(length, 16) + kArenaPitchAlign - 1) & ~(kArenaPitchAlign - 1);
    uint64_t offset = requestedOffset.value_or(0);
    if (!(requestedOffset ? a.place.placeAt(bytes, kArenaPitchAlign, offset)
                          : a.place.place(bytes, kArenaPitchAlign, offset)))
        return false;
    b = {};
    b.span = {arena, offset, bytes, a.bufferVa + offset, a.cpu + offset};
    b.arena = true;
    b.wc = a.wc;
    b.va = b.span.va;
    b.cpu = b.span.cpu;
    b.length = std::max<uint64_t>(length, 16);
    return true;
}

void Device::arenaRelease(Texture &t)
{
    std::lock_guard<std::mutex> l(lock_);
    if (t.arena)
        freeSpan(t.span, Memory::Arena, t.lease);
}

void Device::arenaRelease(Buffer &b)
{
    std::lock_guard<std::mutex> l(lock_);
    if (b.arena)
        freeSpan(b.span, Memory::Arena, b.lease);
}

uint64_t Device::arenaUsed(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    return arena < arenas_.size() ? arenas_[arena].place.used() : 0;
}

uint64_t Device::arenaLargestFree(uint32_t arena, uint64_t align)
{
    std::lock_guard<std::mutex> l(lock_);
    return arena < arenas_.size() ? arenas_[arena].place.largestFree(std::max<uint64_t>(align, 1)) : 0;
}

void Device::setArenaVolatile(uint32_t arena, bool isVolatile)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena < arenas_.size())
        arenas_[arena].isVolatile = isVolatile;
}

uint64_t Device::arenaBytes()
{
    std::lock_guard<std::mutex> l(lock_);
    uint64_t bytes = 0;
    for (const Arena &a : arenas_)
        bytes += a.used && !a.purged ? a.bytes : 0;
    return bytes;
}

bool Device::arenaPurged(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    return arena < arenas_.size() && arenas_[arena].purged;
}

void Device::purgeArena(uint32_t arena, const std::atomic<uint32_t> &committed)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size())
        return;
    Arena &a = arenas_[arena];
    if (!a.used || a.purged || a.leases || !done(submitted()))
        return;
    purgedArenas_++;
    if (committed.load()) {
        purgedArenas_--;
        return;
    }
    k_.free(a.bo);
    vm_address_t at = vm_address_t(a.cpu);
    hostPages(a.bytes, a.wc, at, true);
    RAW_DIAGNOSTIC(stats.poolBytes -= a.bytes);
    a.purged = true;
}

void Device::purgeArenas(const std::atomic<uint32_t> &committed)
{
    std::vector<uint32_t> volatileArenas;
    {
        std::lock_guard<std::mutex> l(lock_);
        for (uint32_t i = 0; i < arenas_.size(); i++)
            if (arenas_[i].used && arenas_[i].isVolatile && !arenas_[i].purged)
                volatileArenas.push_back(i);
    }
    for (const uint32_t i : volatileArenas)
        purgeArena(i, committed);
}

bool Device::restoreArena(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].purged)
        return true;
    Arena &a = arenas_[arena];
    if (!bindArena(a))
        return false;
    a.purged = false;
    purgedArenas_--;
    return true;
}

}  // namespace raw
