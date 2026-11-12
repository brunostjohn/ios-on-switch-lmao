// SPDX-License-Identifier: MIT
// A metallib function's post-tessellation patch, from its function list entry's TESS tag: one byte, the patch type in its
// low two bits (MTLPatchType's numbering) and the control point count above them, 63 for a patch that names none (Xcode's
// metal and metallib: [[patch(triangle, 3)]] 0x0d, [[patch(quad, 16)]] 0x42, [[patch(quad)]] 0xfe; an M4 Max answers
// patchType 1, 2, 2 and patchControlPointCount 3, 16, -1 for them). A function with no TESS tag is no patch function.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include "MetallibSlice.h"

namespace raw {

struct FunctionTess {
    uint8_t patchType = 0;
    int32_t controlPoints = -1;
};

inline FunctionTess decodeTess(uint8_t tag)
{
    const uint8_t count = tag >> 2;
    return {uint8_t(tag & 3), count == 63 ? -1 : int32_t(count)};
}

namespace tess_detail {

template <class T> bool readAt(const uint8_t *file, size_t size, size_t at, T &v)
{
    if (at > size || sizeof v > size - at)
        return false;
    memcpy(&v, file + at, sizeof v);
    return true;
}

}  // namespace tess_detail

// `name`'s patch in a metallib (its AIR slice, as raw_metallib.h reads the function list). False: no function of that name, or
// not a metallib whose function list this reads.
inline bool metallibFunctionTess(const uint8_t *whole, size_t wholeBytes, std::string_view name, FunctionTess &out)
{
    using tess_detail::readAt;
    constexpr size_t kHeaderBytes = 0x58, kFunctionListAt = 24;
    size_t sliceAt = 0, size = 0;
    if (!switch_metal::airSlice(whole, wholeBytes, sliceAt, size))
        return false;
    const uint8_t *file = whole + sliceAt;
    uint64_t listAt = 0, listBytes = 0;
    uint32_t count = 0;
    if (size < kHeaderBytes || memcmp(file, "MTLB", 4) || !readAt(file, size, kFunctionListAt, listAt) ||
        !readAt(file, size, kFunctionListAt + 8, listBytes) || !readAt(file, size, size_t(listAt), count) ||
        listAt > size || listBytes > size - listAt - 4)
        return false;
    const size_t end = size_t(listAt + 4 + listBytes);
    size_t at = size_t(listAt + 4);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bytes;
        if (!readAt(file, end, at, bytes) || bytes < 8 || bytes > end - at)
            return false;
        const size_t entryEnd = at + bytes;
        std::string_view entryName;
        FunctionTess tess;
        for (size_t t = at + 4; t + 4 <= entryEnd && memcmp(file + t, "ENDT", 4);) {
            uint16_t length;
            if (!readAt(file, entryEnd, t + 4, length) || length > entryEnd - t - 6)
                return false;
            const char *data = reinterpret_cast<const char *>(file + t + 6);
            if (!memcmp(file + t, "NAME", 4))
                entryName = std::string_view(data, strnlen(data, length));
            else if (!memcmp(file + t, "TESS", 4) && length == 1)
                tess = decodeTess(uint8_t(data[0]));
            t += 6 + length;
        }
        if (entryName == name) {
            out = tess;
            return true;
        }
        at = entryEnd;
    }
    return false;
}

}  // namespace raw
