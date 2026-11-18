// SPDX-License-Identifier: MIT
// DEVELOPMENT: Core Animation's catch-all ("generic") state by its label, and the process's draws of one. QuartzCore
// names the state's spec after "com.apple.coreanimation." (Pipeline::Spec::name): a pipeline segment, then after '_'
// the fragment segment, whose X flags hold g for the catch-all in both builds' name tables (pack/ca/qc-<build>.json).
// Only the pipeline segment's pixel format codes have capitals besides the field letters, so the fragment segment's
// one X starts its flags.
#pragma once
#include "stats.h"
#if SWITCH_METAL_DIAGNOSTICS
#include <atomic>
#include <cstdint>
#include <cstring>

namespace gm20b {

constexpr char kCaLabelPrefix[] = "com.apple.coreanimation.";

inline bool caCatchAll(const char *label)
{
    if (!label || strncmp(label, kCaLabelPrefix, sizeof kCaLabelPrefix - 1))
        return false;
    const char *fragment = strchr(label + sizeof kCaLabelPrefix - 1, '_');
    if (!fragment)
        return false;
    const char *end = strchr(++fragment, '_');
    const char *flags =
        static_cast<const char *>(memchr(fragment, 'X', end ? size_t(end - fragment) : strlen(fragment)));
    for (const char *f = flags ? flags + 1 : end; f && f != end && *f >= 'a' && *f <= 'z'; f++)
        if (*f == 'g')
            return true;
    return false;
}

// Recording threads add and the rates timer reads: a relaxed counter, the order of draws across threads unneeded
inline std::atomic<uint64_t> gCaCatchAllDraws{0};

}  // namespace gm20b
#endif
