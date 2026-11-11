// SPDX-License-Identifier: MIT
// DEVELOPMENT: the render server's blur-texture probe (Device::glassMem, raw_gpu.cpp). Each Core Animation command buffer
// names the textures its draws sampled that this process rendered or copied into: rebuilt (this command buffer wrote them)
// or reused (an earlier one did: its age in command buffers and the GPU-memory events since, Device::glassMemEvent), and
// what one row of each read at the command buffer's end, by the copy engine into a ring the CPU reads once the fence passed:
// black, non-finite, or for a reused one changed since the end of the command buffer that last wrote it. A texture header
// whose 32 bytes changed under the same index while its texture lived counts as moved. The ring is GPU-volatile control
// memory (an internal job's writes may sit in the L2 past its fence otherwise). The CPU fills a slot's rows with
// kGlassMemPoison before its submission, and the slot ends with a host semaphore release of its command buffer's cookie:
// a slot without the cookie (its segment never ran) or a row still poisoned (its copy never landed) is not judged.
// For a texture rebuilt in the command buffer, what its passes reached is kept (the first pass's load, the box its draws
// covered) and a second row is read outside that box: memory nothing of this command buffer wrote.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "raw_nonfinite.h"

namespace raw {

constexpr uint32_t kGlassMemTexels = 32, kGlassMemItems = 16, kGlassMemSlots = 8, kGlassMemRows = 2,
                   kGlassMemReaches = 32;
constexpr uint32_t kGlassMemRowBytes = kGlassMemTexels * 16, kGlassMemItemBytes = kGlassMemRows * kGlassMemRowBytes;
constexpr uint32_t kGlassMemCookieBytes = 16;
constexpr uint32_t kGlassMemSlotBytes = kGlassMemItems * kGlassMemItemBytes + kGlassMemCookieBytes;
constexpr uint64_t kGlassMemArmMs = 20000;
constexpr uint8_t kGlassMemPoison = 0xa5;
constexpr size_t kGlassMemLine = 900;  // os_log keeps about 1 KiB of a line
// Entries kept: a texture CA dropped without our seeing its destroy (a client's) ages out past this many
constexpr size_t kGlassMemTracked = 512;

enum : uint8_t {
    kGlassMemHeap = 1,       // an MTLHeap's (an arena: a purge takes its pages)
    kGlassMemPitch = 2,      // pitch-linear
    kGlassMemUnsampled = 4,  // no row taken (compressed, memoryless, multisampled, converted)
    kGlassMemAliased = 8,    // an MTLHeap texture made aliasable by the command buffer's end: its rows may be another's
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

// What a command buffer's passes into one image reached: the first pass's load ('c', 'l', 'd'; 0: none seen), the box
// their draws covered (the render area where their draws were not counted), the first pass's render area and the extent
// its clears reached from the origin (0 x 0: none)
struct GlassMemReach {
    char load = 0;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t areaW = 0, areaH = 0, clearW = 0, clearH = 0;
};

// One sampled texture's rows in a command buffer's record
struct GlassMemItem {
    uint64_t key = 0;    // its storage address (Texture::storageRange): a view's is its parent's
    uint64_t image = 0;  // the address of the image (level) sampled, which its passes name as their target
    uint32_t width = 0, height = 0, format = 0, levels = 1, level = 0, texels = 0, bpp = 0;
    uint8_t flags = 0;
    char state[5] = "-";                                 // the sampling state's family (the "T..." term of its name)
    bool rebuilt = false, moved = false, fresh = false;  // fresh: its first write ever is this command buffer's
    uint64_t age = 0, events = 0;                        // command buffers and memory events since its last write
    GlassMemReach reach;
    bool outside = false;  // a second row was read outside the reach
    uint32_t inX = 0, inY = 0, outX = 0, outY = 0;
};

// Where `it`'s rows are read: the first in the middle of what its passes reached (else of the image), the second outside
// that box, below or above it, else beside it; false when the box leaves no room for a row outside it
inline bool glassMemRowsAt(GlassMemItem &it)
{
    const GlassMemReach &r = it.reach;
    const bool known = r.load && r.x1 > r.x0 && r.y1 > r.y0;
    const uint32_t w = it.width, h = it.height, n = it.texels;
    const auto across = [&](uint32_t centre) {
        return std::min(centre > n / 2 ? centre - n / 2 : 0u, w - n);
    };
    it.inX = across(known ? (r.x0 + r.x1) / 2 : w / 2);
    it.inY = std::min(known ? (r.y0 + r.y1) / 2 : h / 2, h - 1);
    it.outside = known;
    if (known && r.y1 < h)
        it.outX = across(w / 2), it.outY = (r.y1 + h) / 2;
    else if (known && r.y0 > 0)
        it.outX = across(w / 2), it.outY = r.y0 / 2;
    else if (known && r.x1 + n <= w)
        it.outX = r.x1 + (w - r.x1 - n) / 2, it.outY = h / 2;
    else if (known && r.x0 >= n)
        it.outX = (r.x0 - n) / 2, it.outY = h / 2;
    else
        it.outside = false;
    return it.outside;
}

struct GlassMemCb {
    uint64_t serial = 0, mach = 0, events = 0;
    uint32_t seqno = 0, count = 0, dropped = 0, reaches = 0;
    GlassMemItem items[kGlassMemItems];
    std::pair<uint64_t, GlassMemReach> reach[kGlassMemReaches];
};

// The value a command buffer's slot's semaphore release writes: never 0, which a slot holds before its release lands
inline uint32_t glassMemCookie(uint64_t serial)
{
    return uint32_t(serial % 0xffffffffu) + 1;
}

inline bool glassMemGlassState(const char *state)
{
    return state[0] == 'T' && state[1] == 'g';
}

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
        if (!w.first)
            w.first = serial;
        w.serial = serial;
        w.events = events;
        trim();
    }

    // A pass of command buffer `serial` into the image at `image` ended: what it reached (GlassMemReach)
    void reached(uint64_t image, const GlassMemReach &pass, uint64_t serial)
    {
        const uint32_t x0 = pass.x0, y0 = pass.y0, x1 = pass.x1, y1 = pass.y1;
        std::lock_guard<std::mutex> l(lock_);
        open_.serial = serial;
        for (uint32_t i = 0; i < open_.reaches; i++)
            if (open_.reach[i].first == image) {
                GlassMemReach &r = open_.reach[i].second;
                r.x0 = std::min(r.x0, x0), r.y0 = std::min(r.y0, y0);
                r.x1 = std::max(r.x1, x1), r.y1 = std::max(r.y1, y1);
                return;
            }
        if (open_.reaches < kGlassMemReaches)
            open_.reach[open_.reaches++] = {image, pass};
    }

    // A draw of state `caName` sampled `item`'s texture in command buffer `serial`, header index `tic` holding bytes of
    // hash `header`: its row's index in this command buffer, or -1 (not one this process wrote, already taken, or no room)
    int sampled(const GlassMemItem &item, const char *caName, uint32_t tic, uint64_t header, uint64_t serial)
    {
        std::lock_guard<std::mutex> l(lock_);
        open_.serial = serial;
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
        char state[5];
        glassMemState(caName, state);
        uint32_t at = open_.count;
        if (at == kGlassMemItems) {
            // the glass draws sample last: one takes the place of the latest item no glass draw sampled
            open_.dropped++;
            if (!glassMemGlassState(state))
                return -1;
            while (at > 0 && glassMemGlassState(open_.items[at - 1].state))
                at--;
            if (!at)
                return -1;
            at--;
        } else {
            open_.count++;
        }
        GlassMemItem &it = open_.items[at];
        it = item;
        it.moved = moved;
        memcpy(it.state, state, sizeof state);
        return int(at);
    }

    // A command buffer is begun: what an abandoned one sampled is no record's
    void begin()
    {
        std::lock_guard<std::mutex> l(lock_);
        open_ = GlassMemCb();
    }

    // `key` died (a view's header `tic` alone, when `tic`)
    void forget(uint64_t key, uint32_t tic = 0)
    {
        std::lock_guard<std::mutex> l(lock_);
        if (tic) {
            headers_.erase(tic);
            return;
        }
        for (uint32_t i = 0; i < open_.count; i++)
            if (open_.items[i].key == key)
                open_.items[i].flags |= kGlassMemUnsampled;
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
            const auto found = writes_.find(it.key);
            const Write w = found == writes_.end() ? Write() : found->second;
            it.rebuilt = w.serial == serial;
            it.fresh = w.first == serial;
            it.age = serial - w.serial;
            it.events = events - w.events;
            for (uint32_t r = 0; it.rebuilt && r < cb.reaches; r++)
                if (cb.reach[r].first == it.image)
                    it.reach = cb.reach[r].second;
        }
        return cb;
    }

    // What `cb`'s rows read (`rows`: kGlassMemItemBytes an item, then the cookie): its lines (os_log's length each), none
    // when there is nothing to say and not `every`. Rows whose slot lacks the cookie are no read: '?', and count nothing.
    std::vector<std::string> finish(const GlassMemCb &cb, const uint8_t *rows, bool every)
    {
        std::lock_guard<std::mutex> l(lock_);
        uint32_t cookie;
        memcpy(&cookie, rows + uint64_t(kGlassMemItems) * kGlassMemItemBytes, sizeof cookie);
        const bool landed = cookie == glassMemCookie(cb.serial);
        if (landed)
            counts.cbs++;
        char head[160];
        int at = snprintf(head, sizeof head, "cb %llu mach %llu seq %u ev %llu n %u", (unsigned long long)cb.serial,
                          (unsigned long long)cb.mach, cb.seqno, (unsigned long long)cb.events, cb.count);
        if (cb.dropped)
            snprintf(head + at, sizeof head - size_t(at), " drop %u", cb.dropped);
        std::vector<std::string> lines{std::string(head) + " |"};
        bool notable = !landed;
        for (uint32_t i = 0; i < cb.count; i++) {
            const GlassMemItem &it = cb.items[i];
            const uint8_t *row = rows + uint64_t(i) * kGlassMemItemBytes;
            const std::string verdict = landed ? verdictOf(it, row, cb.serial, notable) : "?";
            char levels[12] = "", level[8] = "", events[16] = "", reach[96] = "";
            if (it.levels > 1)
                snprintf(levels, sizeof levels, "/L%u", it.levels);
            if (it.level)
                snprintf(level, sizeof level, ".%u", it.level);
            if (!it.rebuilt && it.events)
                snprintf(events, sizeof events, "/e%llu", (unsigned long long)it.events);
            if (it.reach.load)
                snprintf(reach, sizeof reach, "/%c%u,%u-%u,%u/a%ux%u/k%ux%u", it.reach.load, it.reach.x0, it.reach.y0,
                         it.reach.x1, it.reach.y1, it.reach.areaW, it.reach.areaH, it.reach.clearW, it.reach.clearH);
            char item[256];
            snprintf(item, sizeof item, " %04llx%s:%ux%u/f%u%s/%s%s/%s/%c%llu%s%s%s%s%s:%s",
                     (unsigned long long)(it.key >> 14 & 0xffff), level, it.width, it.height, it.format, levels,
                     it.flags & kGlassMemPitch ? "pitch" : "bl", it.flags & kGlassMemHeap ? "+heap" : "", it.state,
                     it.rebuilt ? 'R' : 'U', (unsigned long long)(it.rebuilt ? 0 : it.age), it.fresh ? "/n" : "", reach,
                     events, it.moved ? "/mv" : "", it.flags & kGlassMemAliased ? "/al" : "", verdict.c_str());
            if (lines.back().size() + strlen(item) > kGlassMemLine)
                lines.push_back(std::string(head) + " +|");
            lines.back() += item;
        }
        if (!notable && !every)
            lines.clear();
        return lines;
    }

    GlassMemCounts counts;

private:
    struct Write {
        uint64_t serial = 0, events = 0, first = 0;
    };

    struct Ref {
        uint64_t serial = 0, hash = 0;
        bool black = false;
    };

    struct Header {
        uint64_t key = 0, hash = 0;
    };

    bool poisoned(const GlassMemItem &it, const uint8_t *row) const
    {
        for (uint32_t b = 0; b < it.texels * it.bpp; b++)
            if (row[b] != kGlassMemPoison)
                return false;
        return true;
    }

    // The inside row's verdict, then the outside row's after a comma (no reference: it was never meant to be written)
    std::string verdictOf(const GlassMemItem &it, const uint8_t *row, uint64_t serial, bool &notable)
    {
        std::string v = insideVerdict(it, row, serial, notable);
        if (!it.outside || (it.flags & kGlassMemUnsampled))
            return v;
        const uint8_t *out = row + kGlassMemRowBytes;
        if (poisoned(it, out))
            return v + ",out:?";
        const GlassMemRow r = glassMemRow(out, it.texels, it.bpp, it.format);
        if (r.nonFinite) {
            counts.nonFinite++;
            notable = true;
        }
        char at[40];
        snprintf(at, sizeof at, ",out@%u,%u:%s", it.outX, it.outY, r.nonFinite ? "nf" : r.black ? "zero" : "ok");
        return v + at;
    }

    const char *insideVerdict(const GlassMemItem &it, const uint8_t *row, uint64_t serial, bool &notable)
    {
        (it.rebuilt ? counts.rebuilt : counts.reused)++;
        if (it.moved) {
            counts.moved++;
            notable = true;
        }
        if (it.flags & kGlassMemUnsampled)
            return "-";
        if (poisoned(it, row))
            return "?";
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
