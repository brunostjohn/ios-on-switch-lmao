// SPDX-License-Identifier: MIT
// See raw_gpu.h (createArena). An arena's GPU VA is a reservation and its host range is never given back while it lives: a
// purge frees the BO (the kernel releases the pages) and replaces the host range with untouched pages, so neither address
// moves and a restore is one import and one bind at the same VA, no texture header or CPU pointer rewritten. A trim
// (raw_arena_trim.h) gives back only the ranges no resource is placed in, the same way, and binds the rest in parts.
#include "raw_gpu.h"
#include "TegraGM20BLayout.h"
#include "raw_arena_purge.h"

#include <mach/mach.h>

namespace raw {
namespace {
constexpr uint64_t kArenaGranule = 0x10000;  // the GMMU big page every VA range starts and ends on (raw_kernel.cpp)
constexpr uint64_t kArenaPitchAlign = 256;   // a pitch render target's address and rows (linearTarget), a cbuf's base
}  // namespace

bool Device::arenaFootprint(uint32_t width, uint32_t height, uint32_t format, bool blockLinear, uint64_t &bytes,
                            uint64_t &align)
{
    Texture t;
    if (!shape(width, height, format, t) || t.blockW > 1 || t.blockH > 1)
        return false;
    if (blockLinear) {
        tgm_layout bl;
        if (!t.renderable || !tgm_layout_tiled(width, height, t.bpp, &bl))
            return false;
        bytes = (bl.bytes + kBlAlign - 1) & ~(kBlAlign - 1);
        align = std::max<uint64_t>(bl.align, kBlAlign);
        return true;
    }
    const uint64_t pitch = (uint64_t(width) * t.bpp + 255) & ~uint64_t(255);
    bytes = pitch * height;
    align = kArenaPitchAlign;
    return true;
}

// The host range's pages wired and bound at the arena's VA, under its kind (lock_ held)
bool Device::bindArena(Arena &a)
{
    if (!k_.importPages(a.cpu, a.bytes, a.bo) ||
        !k_.bindImportAt(a.cpu, a.bo, a.va, a.kind) ||
        (a.bufferVa != a.va && !k_.bindAlias(a.bo, a.bufferVa, kPteKindPitch))) {
        k_.free(a.bo);
        return false;
    }
    Bo alias = a.bo;
    alias.va = alias.vaBase = a.bufferVa;
    set_.add(k_, {{&a.bo, TegraGM20BSurfaceRead}, {&alias, TegraGM20BSurfaceRead}});
    RAW_DIAGNOSTIC(stats.poolBytes += a.bytes);
    return true;
}

bool Device::bindArenaPart(Arena &a, uint64_t offset, uint64_t bytes)
{
    Bo part;
    uint8_t *cpu = a.cpu + offset;
    if (!k_.importPages(cpu, bytes, part) || !k_.bindImportAt(cpu, part, a.va + offset, a.kind) ||
        (a.bufferVa != a.va && !k_.bindAlias(part, a.bufferVa + offset, kPteKindPitch))) {
        k_.free(part);
        return false;
    }
    Bo alias = part;
    alias.va = alias.vaBase = a.bufferVa + offset;
    set_.add(k_, {{&part, TegraGM20BSurfaceRead}, {&alias, TegraGM20BSurfaceRead}});
    a.parts.emplace_back(offset, part);
    RAW_DIAGNOSTIC(stats.poolBytes += bytes);
    return true;
}

void Device::unbindArena(Arena &a)
{
    if (a.parts.empty()) {
        set_.free(k_, a.bo);
        RAW_DIAGNOSTIC(stats.poolBytes -= a.bytes);
        return;
    }
    for (auto &[offset, part] : a.parts) {
        RAW_DIAGNOSTIC(stats.poolBytes -= part.bytes);
        set_.free(k_, part);
    }
    a.parts.clear();
    a.holes.clear();
}

bool Device::createArena(uint64_t bytes, bool blockLinear, uint32_t &arena, bool explicitPlacement)
{
    const uint64_t logicalBytes = bytes;
    if (bytes > UINT64_MAX - (kArenaGranule - 1))
        return false;
    bytes = (bytes + kArenaGranule - 1) & ~(kArenaGranule - 1);
    vm_address_t cpu = 0;
    if (!bytes || vm_allocate(mach_task_self(), &cpu, bytes, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return false;
    Arena a;
    a.bytes = bytes;
    a.kind = blockLinear ? TGM_LAYOUT_KIND : kPteKindPitch;
    a.cpu = reinterpret_cast<uint8_t *>(cpu);
    a.place = HeapPlacement(logicalBytes, explicitPlacement);
    a.used = true;
    std::lock_guard<std::mutex> l(lock_);
    a.va = k_.reserveVa(bytes);
    // Kind 0xfe changes the sector interpretation inside a GOB. Buffers retain a pitch view of the same BO.
    a.bufferVa = explicitPlacement && blockLinear ? k_.reserveVa(bytes) : a.va;
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
    if (a.purged)
        purgedArenas_--;
    else
        unbindArena(a);
    k_.releaseVa(a.va, a.bytes);
    if (a.bufferVa != a.va)
        k_.releaseVa(a.bufferVa, a.bytes);
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
    uint64_t from = 0, bytes = ar.bytes, handle = ar.bo.handle;
    for (const auto &[offset, part] : ar.parts)
        if (s.offset >= offset && s.offset + s.bytes <= offset + part.bytes) {
            from = offset;
            bytes = part.bytes;
            handle = part.handle;
        }
    if (!handle)
        return false;  // a trimmed arena's span across two parts: no one BO holds it
    a.cpu = ar.cpu + from;
    a.bytes = bytes;
    a.handle = handle;
    a.va = s.va - s.offset + from;
    a.kind = s.va - s.offset == ar.va ? ar.kind : kPteKindPitch;
    a.lease = leaseSpan(lease, s, Memory::Arena);
    return true;
}

bool Device::arenaTexture(uint32_t arena, uint32_t width, uint32_t height, uint32_t format, Texture &t,
                          std::optional<uint64_t> requestedOffset)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].used || arenas_[arena].dying)
        return false;
    Arena &a = arenas_[arena];
    const bool blockLinear = a.kind == TGM_LAYOUT_KIND;
    uint64_t bytes, align, offset = requestedOffset.value_or(0);
    if (!arenaFootprint(width, height, format, blockLinear, bytes, align) || !shape(width, height, format, t) ||
        !(requestedOffset ? a.place.placeAt(bytes, align, offset) : a.place.place(bytes, align, offset)))
        return false;
    if (!backPlacement(a, offset, bytes)) {
        a.place.release(offset, bytes);
        return false;
    }
    t.span = {arena, offset, bytes, a.va + offset, a.cpu + offset};
    t.arena = true;
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    tgm_layout bl;
    if (blockLinear && tgm_layout_tiled(width, height, t.bpp, &bl)) {
        t.pitch = bl.row_bytes;
        t.blockLinear = true;
        t.blockHeight = bl.y_log2;
        t.layerBytes = bl.bytes;
    } else {
        t.pitch = (width * t.bpp + 255) & ~255u;
    }
    if (!(t.tic = allocTic())) {
        arenaFree(t.span);
        t = {};
        return false;
    }
    writeTic(t);
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
    if (!backPlacement(a, offset, bytes)) {
        a.place.release(offset, bytes);
        return false;
    }
    b = {};
    b.span = {arena, offset, bytes, a.bufferVa + offset, a.cpu + offset};
    b.arena = true;
    b.va = b.span.va;
    b.cpu = b.span.cpu;
    b.length = std::max<uint64_t>(length, 16);
    return true;
}

void Device::arenaRelease(Texture &t)
{
#if SWITCH_RAW_DIAGNOSTICS
    if (t.arena && glassMemOn.load(std::memory_order_relaxed))
        glassMem->forget(t.storageRange().va);
#endif
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

bool Device::setArenaVolatile(uint32_t arena, bool isVolatile)
{
    std::lock_guard<std::mutex> l(lock_);
    return arena < arenas_.size() && arenaSetVolatile(arenas_[arena], !isVolatile);
}

uint64_t Device::arenaBytes()
{
    std::lock_guard<std::mutex> l(lock_);
    uint64_t bytes = 0;
    for (const Arena &a : arenas_)
        bytes += a.used && !a.purged ? a.bytes - rangeBytes(a.holes) : 0;
    return bytes;
}

void Device::arenaUse(uint64_t &placed, uint64_t &volatileBytes, uint64_t *givenBack)
{
    std::lock_guard<std::mutex> l(lock_);
    placed = volatileBytes = 0;
    uint64_t given = 0;
    for (const Arena &a : arenas_)
        if (a.used && !a.purged) {
            placed += std::min(a.place.used(), a.bytes);
            volatileBytes += a.isVolatile ? a.bytes : 0;
            given += rangeBytes(a.holes);
        }
    if (givenBack)
        *givenBack = given;
}

bool Device::arenaPurged(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    return arena < arenas_.size() && arenas_[arena].purged;
}

// lock_ held. The commit path counts itself in `committed` before it counts purged arenas, so one of the two sees the
// other: a command buffer committed meanwhile keeps the pages.
void Device::purgeLocked(Arena &a, const std::atomic<uint32_t> &committed)
{
    purgedArenas_++;
    if (committed.load()) {
        purgedArenas_--;
        return;
    }
    unbindArena(a);
    vm_address_t at = vm_address_t(a.cpu);
    vm_allocate(mach_task_self(), &at, a.bytes, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
    RAW_DIAGNOSTIC(glassMemEvent("heap purged", a.bytes));
    a.purged = true;
}

void Device::purgeArena(uint32_t arena, const std::atomic<uint32_t> &committed)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena < arenas_.size() && arenaPurgeable(arenas_[arena], done(submitted()), committed.load() != 0, true))
        purgeLocked(arenas_[arena], committed);
}

void Device::purgeArenas(const std::atomic<uint32_t> &committed)
{
    std::lock_guard<std::mutex> l(lock_);
    purgeIdleArenas(arenas_, done(submitted()), committed.load() != 0, [&](Arena &a) {
        purgeLocked(a, committed);
    });
}

// lock_ held, the device idle: the import goes (no GPU work can read it), each hole's host range is replaced by untouched
// pages, and the placed parts are imported again at their VAs. It counts as a purge while it runs (purgeLocked's order
// with the commit path), so a command buffer committed meanwhile waits in restoreArena for the parts to be bound again,
// and one committed before it keeps the arena as it is. A part the kernel refuses binds the whole arena again.
void Device::trimLocked(Arena &a, const std::atomic<uint32_t> &committed)
{
    std::vector<ArenaRange> holes = arenaHoles(a.place, a.bytes, kArenaGranule);
    if (holes.empty() || holes == a.holes)
        return;
    purgedArenas_++;
    if (committed.load()) {
        purgedArenas_--;
        return;
    }
    unbindArena(a);
    for (const ArenaRange &h : holes) {
        vm_address_t at = vm_address_t(a.cpu + h.offset);
        vm_allocate(mach_task_self(), &at, h.bytes, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
    }
    bool bound = true;
    for (const ArenaRange &b : arenaBacked(holes, a.bytes))
        bound = bound && bindArenaPart(a, b.offset, b.bytes);
    if (bound) {
        a.holes = std::move(holes);
        RAW_DIAGNOSTIC(glassMemEvent("heap trimmed", rangeBytes(a.holes)));
    } else {
        unbindArena(a);
        if (!bindArena(a)) {
            a.purged = true;
            purgedArenas_++;
        }
    }
    purgedArenas_--;
}

void Device::trimArenas(const std::atomic<uint32_t> &committed)
{
    std::lock_guard<std::mutex> l(lock_);
    if (!done(submitted()))
        return;
    for (Arena &a : arenas_)
        if (a.used && !a.purged && !a.dying && !a.leases && !a.isVolatile)
            trimLocked(a, committed);
}

bool Device::backPlacement(Arena &a, uint64_t offset, uint64_t bytes)
{
    if (a.holes.empty())
        return true;
    for (const ArenaRange &r : takeHoles(a.holes, offset, bytes, kArenaGranule))
        if (!bindArenaPart(a, r.offset, r.bytes)) {
            a.holes.push_back(r);
            std::sort(a.holes.begin(), a.holes.end(), [](const ArenaRange &x, const ArenaRange &y) {
                return x.offset < y.offset;
            });
            return false;
        }
    return true;
}

bool Device::restoreArena(uint32_t arena)
{
    std::lock_guard<std::mutex> l(lock_);
    if (arena >= arenas_.size() || !arenas_[arena].purged)
        return true;
    Arena &a = arenas_[arena];
    if (!bindArena(a))
        return false;
    RAW_DIAGNOSTIC(glassMemEvent("heap restored", a.bytes));
    a.purged = false;
    purgedArenas_--;
    return true;
}

}  // namespace raw
