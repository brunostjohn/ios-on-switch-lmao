// SPDX-License-Identifier: MIT
// Where a library of the facade came from, as a compile request names it (raw_compile_request.h): a system volume's by its
// path, any other by its bytes, hashed once at the first request that needs it (the bytes are the m2v library's own).
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include "raw_compile_request.h"

namespace raw {

struct LibrarySource {
    explicit LibrarySource(std::string from) : path(std::move(from)), system(systemLibraryPath(path))
    {}

    const std::string path;  // "": made from data, or from source
    const bool system;

    const LibraryIdentity &identity(const void *bytes, size_t size)
    {
        std::call_once(once_, [&] {
            identity_.system = system;
            identity_.digest = system ? sha256(path.data(), path.size()) : sha256(bytes, size);
        });
        return identity_;
    }

private:
    std::once_flag once_;
    LibraryIdentity identity_;
};

}  // namespace raw
