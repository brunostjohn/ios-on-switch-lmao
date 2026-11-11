// SPDX-License-Identifier: MIT
// DEVELOPMENT: the render server's blur-texture probe (Device::glassMem, raw_gpu.cpp). Each Core Animation command buffer
// names the textures its draws sampled that this process rendered or copied into: rebuilt (this command buffer wrote them)
// or reused (an earlier one did: its age in command buffers and the GPU-memory events since, Device::glassMemEvent), and
// what one row of each read at the command buffer's end, by the copy engine into a ring the CPU reads once the fence passed:
// black, non-finite, or for a reused one changed since the end of the command buffer that last wrote it. A texture header
// whose 32 bytes changed under the same index while its texture lived counts as moved.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include "raw_nonfinite.h"

namespace raw {

constexpr uint32_t kGlassMemTexels = 32, kGlassMemItems = 16, kGlassMemSlots = 8;
constexpr uint32_t kGlassMemItemBytes = kGlassMemTexels * 16, kGlassMemSlotBytes = kGlassMemItems * kGlassMemItemBytes;
constexpr uint64_t kGlassMemArmMs = 20000;
// Entries kept: a texture CA dropped without our seeing its destroy (a client's) ages out past this many
constexpr size_t kGlassMemTracked = 512;

enum : uint8_t {
    kGlassMemHeap = 1,       // an MTLHeap's (an arena: a purge takes its pages)
    kGlassMemPitch = 2,      // pitch-linear
    kGlassMemUnsampled = 4,  // no row taken (compressed, memoryless, multisampled, converted)
};

// One row's read: every texel's colour (alpha left out) zero, any float component NaN or infinite, and its bytes' hash
struct GlassMemRow {
    bool black = false, nonFinite = false;
    uint64_t hash = 0;
};

// The byte range of a texel's alpha in Metal format `format` ([begin, end): none for a format without one)
inline void glassMemAlpha(uint32_t format, uint32_t &begin, uint32_t &end)
{
    switch (format) {
    case 70:
    case 71:
    case 80:
    case 81:
        begin = 3, end = 4;
        return;
    case 115:
        begin = 6, end = 8;
        return;
    case 125:
        begin = 12, end = 16;
        return;
    default:
        begin = end = 0;
    }
}

inline GlassMemRow glassMemRow(const uint8_t *row, uint32_t texels, uint32_t bpp, uint32_t format)
{
    GlassMemRow r;
    r.black = texels > 0;
    r.hash = 0xcbf29ce484222325ull;
    uint32_t alphaBegin, alphaEnd;
    glassMemAlpha(format, alphaBegin, alphaEnd);
    const uint32_t component = floatComponentBytes(format);
    for (uint32_t t = 0; t < texels; t++) {
        const uint8_t *texel = row + uint64_t(t) * bpp;
        for (uint32_t b = 0; b < bpp; b++) {
            r.hash = (r.hash ^ texel[b]) * 0x100000001b3ull;
            const bool alpha = b >= alphaBegin && b < alphaEnd;
            // a float zero's sign bit: -0 is black too
            const bool sign = component && (b % component) == component - 1;
            if (!alpha && (texel[b] & (sign ? 0x7f : 0xff)))
                r.black = false;
        }
        for (uint32_t c = 0; component && c + component <= bpp; c += component)
            r.nonFinite |= nonFiniteComponent(texel + c, component);
    }
    return r;
}

// One sampled texture's row in a command buffer's record
struct GlassMemItem {
    uint64_t key = 0;  // its storage address (Texture::storageRange): a view's is its parent's
    uint32_t width = 0, height = 0, format = 0, levels = 1, level = 0, texels = 0, bpp = 0;
    uint8_t flags = 0;
    char state[5] = "-";  // the sampling state's family (the "T..." term of its name)
    bool rebuilt = false, moved = false;
    uint64_t age = 0, events = 0;  // command buffers and memory events since its last write
};

struct GlassMemCb {
    uint64_t serial = 0, mach = 0, events = 0;
    uint32_t seqno = 0, count = 0;
    GlassMemItem items[kGlassMemItems];
};

struct GlassMemCounts {
    std::atomic<uint64_t> cbs{0}, rebuilt{0}, reused{0}, changed{0}, nonFinite{0}, moved{0}, black{0};
};

// The "T" term of a Core Animation state's name (PBGRAXm_TvrdA2Xhf_Isrc: Tvrd), four letters at most
inline void glassMemState(const char *caName, char out[5])
{
    out[0] = '-', out[1] = 0;
    const char *t = caName ? strstr(caName, "_T") : nullptr;
    if (!t)
        return;
    size_t n = 0;
    for (const char *c = t + 1; *c && n < 4 && (n == 0 || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')); c++)
        out[n++] = *c;
    out[n] = 0;
}

class GlassMem {
public:
    // A write into the memory at `key` by command buffer `serial`, with `events` memory events so far
    void wrote(uint64_t key, uint64_t serial, uint64_t events)
    {
        std::lock_guard<std::mutex> l(lock_);
        Write &w = writes_[key];
        w.serial = serial;
        w.events = events;
        trim();
    }

    // A draw of state `caName` sampled `item`'s texture in command buffer `serial`, header index `tic` holding bytes of
    // hash `header`: its row's index in this command buffer, or -1 (not one this process wrote, already taken, or no room)
    int sampled(const GlassMemItem &item, const char *caName, uint32_t tic, uint64_t header, uint64_t serial)
    {
        std::lock_guard<std::mutex> l(lock_);
        if (open_.serial != serial) {
            open_ = GlassMemCb();
            open_.serial = serial;
        }
        auto w = writes_.find(item.key);
        if (w == writes_.end() || !w->second.serial)
            return -1;
        bool moved = false;
        if (tic) {
            Header &h = headers_[tic];
            moved = h.key == item.key && h.hash != header;
            h = {item.key, header};
        }
        for (uint32_t i = 0; i < open_.count; i++)
            if (open_.items[i].key == item.key && open_.items[i].level == item.level) {
                open_.items[i].moved |= moved;
                return -1;
            }
        if (open_.count == kGlassMemItems)
            return -1;
        GlassMemItem &it = open_.items[open_.count];
        it = item;
        it.moved = moved;
        glassMemState(caName, it.state);
        return int(open_.count++);
    }

    // `key` died (a view's header `tic` alone, when `tic`)
    void forget(uint64_t key, uint32_t tic = 0)
    {
        std::lock_guard<std::mutex> l(lock_);
        if (tic) {
            headers_.erase(tic);
            return;
        }
        writes_.erase(key);
        for (auto it = refs_.begin(); it != refs_.end();)
            it = it->first - key < 16 ? refs_.erase(it) : std::next(it);
        for (auto it = headers_.begin(); it != headers_.end();)
            it = it->second.key == key ? headers_.erase(it) : std::next(it);
    }

    // Command buffer `serial`'s record, closed at its commit: rebuilt or reused is known now that every write was seen
    GlassMemCb close(uint64_t serial, uint64_t mach, uint64_t events)
    {
        std::lock_guard<std::mutex> l(lock_);
        GlassMemCb cb = open_.serial == serial ? open_ : GlassMemCb();
        open_ = GlassMemCb();
        cb.serial = serial;
        cb.mach = mach;
        cb.events = events;
        for (uint32_t i = 0; i < cb.count; i++) {
            GlassMemItem &it = cb.items[i];
            const Write &w = writes_[it.key];
            it.rebuilt = w.serial == serial;
            it.age = serial - w.serial;
            it.events = events - w.events;
        }
        return cb;
    }

    // What `cb`'s rows read (`rows`: kGlassMemItemBytes an item): its line, empty when there is nothing to say and not `every`
    std::string finish(const GlassMemCb &cb, const uint8_t *rows, bool every)
    {
        std::lock_guard<std::mutex> l(lock_);
        counts.cbs++;
        char line[1000];
        int at = snprintf(line, sizeof line, "cb %llu mach %llu seq %u ev %llu n %u |", (unsigned long long)cb.serial,
                          (unsigned long long)cb.mach, cb.seqno, (unsigned long long)cb.events, cb.count);
        bool notable = false;
        for (uint32_t i = 0; i < cb.count && at > 0 && size_t(at) < sizeof line; i++) {
            const GlassMemItem &it = cb.items[i];
            const char *verdict = verdictOf(it, rows + uint64_t(i) * kGlassMemItemBytes, cb.serial, notable);
            char levels[12] = "", level[8] = "", events[16] = "";
            if (it.levels > 1)
                snprintf(levels, sizeof levels, "/L%u", it.levels);
            if (it.level)
                snprintf(level, sizeof level, ".%u", it.level);
            if (!it.rebuilt && it.events)
                snprintf(events, sizeof events, "/e%llu", (unsigned long long)it.events);
            at += snprintf(line + at, sizeof line - size_t(at), " %04llx%s:%ux%u/f%u%s/%s%s/%s/%c%llu%s%s:%s",
                           (unsigned long long)(it.key >> 14 & 0xffff), level, it.width, it.height, it.format, levels,
                           it.flags & kGlassMemPitch ? "pitch" : "bl", it.flags & kGlassMemHeap ? "+heap" : "",
                           it.state, it.rebuilt ? 'R' : 'U', (unsigned long long)(it.rebuilt ? 0 : it.age), events,
                           it.moved ? "/mv" : "", verdict);
        }
        return notable || every ? std::string(line) : std::string();
    }

    GlassMemCounts counts;

private:
    struct Write {
        uint64_t serial = 0, events = 0;
    };

    struct Ref {
        uint64_t serial = 0, hash = 0;
        bool black = false;
    };

    struct Header {
        uint64_t key = 0, hash = 0;
    };

    const char *verdictOf(const GlassMemItem &it, const uint8_t *row, uint64_t serial, bool &notable)
    {
        (it.rebuilt ? counts.rebuilt : counts.reused)++;
        if (it.moved) {
            counts.moved++;
            notable = true;
        }
        if (it.flags & kGlassMemUnsampled)
            return "-";
        const GlassMemRow r = glassMemRow(row, it.texels, it.bpp, it.format);
        Ref &ref = refs_[it.key + it.level];
        if (r.nonFinite) {
            counts.nonFinite++;
            notable = true;
        }
        if (it.rebuilt) {
            ref = {serial, r.hash, r.black};
            return r.nonFinite ? "nf" : r.black ? "zero" : "ok";
        }
        if (r.black)
            counts.black++;
        const bool referenced = ref.serial && ref.serial == serial - it.age;
        if (referenced && ref.hash != r.hash) {
            counts.changed++;
            notable = true;
            return r.nonFinite ? "nfchg" : r.black ? "zchg" : "chg";
        }
        if (r.nonFinite)
            return "nf";
        if (r.black)
            return referenced ? "zero" : "zero?";
        return referenced ? "ok" : "ok?";
    }

    void trim()
    {
        if (writes_.size() <= kGlassMemTracked)
            return;
        auto oldest = writes_.begin();
        for (auto it = writes_.begin(); it != writes_.end(); ++it)
            if (it->second.serial < oldest->second.serial)
                oldest = it;
        const uint64_t key = oldest->first;
        writes_.erase(oldest);
        for (auto it = refs_.begin(); it != refs_.end();)
            it = it->first - key < 16 ? refs_.erase(it) : std::next(it);
    }

    std::mutex lock_;
    std::unordered_map<uint64_t, Write> writes_;
    std::unordered_map<uint64_t, Ref> refs_;        // by key plus level: the row read at its last write's end
    std::unordered_map<uint32_t, Header> headers_;  // by header index: whose it is and its bytes' hash
    GlassMemCb open_;
};

// The notify words HW Test reads (hw_test_app app27 ui.glass_sidebar): the probe's counts, and the pool's shape
inline uint64_t glassMemWord(const GlassMemCounts &c)
{
    const auto sat = [](uint64_t v, unsigned bits) {
        return std::min<uint64_t>(v, (1ull << bits) - 1);
    };
    return (c.cbs.load() & 0xfffff) | sat(c.changed.load(), 12) << 20 | sat(c.nonFinite.load(), 12) << 32 |
           sat(c.moved.load(), 12) << 44 | sat(c.black.load(), 8) << 56;
}

inline uint64_t glassMemPoolWord(uint64_t poolMB, uint64_t chunks, uint64_t backings, uint64_t backingMB,
                                 uint64_t compHeld, uint64_t compMade)
{
    const auto sat = [](uint64_t v, unsigned bits) {
        return std::min<uint64_t>(v, (1ull << bits) - 1);
    };
    return sat(poolMB, 12) | sat(chunks, 8) << 12 | sat(backings, 8) << 20 | sat(backingMB, 12) << 28 |
           sat(compHeld, 8) << 40 | (compMade & 0xffff) << 48;
}

}  // namespace raw
