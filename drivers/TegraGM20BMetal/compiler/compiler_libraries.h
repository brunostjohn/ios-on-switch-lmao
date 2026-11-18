// SPDX-License-Identifier: MIT
// The libraries every request kind of the engine's (compiler_engine.h) is made from, made by the translator alone
// (TegraGM20BMetalOffline.h). A system library is read once per process: its file is mapped and read in place, its
// functions' HASH tags taken as the sealed volume states them, so a request reads its function list and the bitcode
// it compiles, and nothing of it is copied or hashed (an MPS library is 84 MB). A library of a client's data is kept
// while compiles come, so a burst of one library's states reads it once, and let go once none ran for a while: its
// bytes are a heap copy of the request's, which a compiler at rest has no use for.
#pragma once
#import <Metal/Metal.h>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "../compile_request.h"

namespace switch_compiler {

class LibraryCache : public std::enable_shared_from_this<LibraryCache> {
public:
    // `root`: where the system volumes are (SystemLibraries)
    explicit LibraryCache(std::string root) : root_(std::move(root))
    {}

    // A compile begins and ends: the libraries are let go `kKeptFor` after the last ends, unless another begins
    void began();
    void ended();
    // The library `l` names, nil with `why`
    id<MTLLibrary> library(const gm20b::RequestLibrary &l, std::string &why);

private:
    static constexpr int64_t kKeptForNs = 5 * int64_t(NSEC_PER_SEC);
    static constexpr size_t kDataLibraries = 16;

    const std::string root_;
    std::mutex lock_;
    uint32_t running_ = 0;
    uint64_t generation_ = 0;  // compiles begun, which a pending release checks
    std::map<std::string, id<MTLLibrary>> system_;
    std::map<gm20b::Digest, id<MTLLibrary>> data_;
    std::deque<gm20b::Digest> dataOrder_;

    void release(uint64_t generation);
};

}  // namespace switch_compiler
