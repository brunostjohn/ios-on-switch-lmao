// SPDX-License-Identifier: MIT
// A metallib's function list. Header: the list's offset at 24, the bitcode section's offset and size at 72 and 80. The list is a
// u32 count, then per entry a u32 size (counting itself) and tags (a fourcc, a u16 length and the bytes) up to ENDT.
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace switch_metal {

constexpr size_t kMetallibHeaderBytes = 88;

// TYPE of an extern function, a Core Image kernel library's (metal -fcikernel: Core Image's ci_stdlib and ci_filters, CoreUI's
// default.metallib). Metal lists these in externFunctionNames and every other entry in functionNames, whatever the header's file
// type, and refuses a library that has both ("Library with both qualified functions and extern functions is not supported").
constexpr uint8_t kMetallibExternType = 5;

struct MetallibEntry {
    std::string name;
    std::optional<uint8_t> type;
    uint64_t bitcodeOffset = 0;  // OFFT's third u64, into the bitcode section
    uint64_t bitcodeBytes = 0;   // MDSZ
    bool placed = false;         // both OFFT and MDSZ are there
};

// Each entry, in order, until `visit` answers false. False when the metallib is malformed or there are no bytes.
template <class Visit> bool forEachMetallibEntry(const uint8_t *file, size_t length, Visit &&visit)
{
    if (!file || length < kMetallibHeaderBytes || memcmp(file, "MTLB", 4))
        return false;
    uint64_t listAt;
    memcpy(&listAt, file + 24, 8);
    if (listAt > length - 4)
        return false;
    uint32_t count;
    memcpy(&count, file + listAt, 4);
    size_t at = listAt + 4;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size;
        if (at > length - 4)
            return false;
        memcpy(&size, file + at, 4);
        if (size < 8 || size > length - at)
            return false;
        const size_t end = at + size;
        MetallibEntry entry;
        bool sized = false, offset = false;
        for (size_t t = at + 4; t + 4 <= end && memcmp(file + t, "ENDT", 4);) {
            if (t + 6 > end)
                return false;
            uint16_t n;
            memcpy(&n, file + t + 4, 2);
            const uint8_t *v = file + t + 6;
            if (n > end - (t + 6))
                return false;
            if (!memcmp(file + t, "NAME", 4))
                entry.name.assign(reinterpret_cast<const char *>(v), strnlen(reinterpret_cast<const char *>(v), n));
            else if (!memcmp(file + t, "TYPE", 4) && n >= 1)
                entry.type = v[0];
            else if (!memcmp(file + t, "MDSZ", 4) && n >= 8) {
                memcpy(&entry.bitcodeBytes, v, 8);
                sized = true;
            } else if (!memcmp(file + t, "OFFT", 4) && n >= 24) {
                memcpy(&entry.bitcodeOffset, v + 16, 8);
                offset = true;
            }
            t += 6 + n;
        }
        entry.placed = sized && offset;
        if (!visit(entry))
            return true;
        at = end;
    }
    return true;
}

// The extern entries' names, in file order (Metal answers them in an order of its own, neither this nor sorted); none for a
// malformed metallib.
inline std::vector<std::string> metallibExternNames(const uint8_t *file, size_t length)
{
    std::vector<std::string> names;
    const bool whole = forEachMetallibEntry(file, length, [&](const MetallibEntry &entry) {
        if (entry.type == kMetallibExternType)
            names.push_back(entry.name);
        return true;
    });
    if (!whole)
        names.clear();
    return names;
}

}  // namespace switch_metal
