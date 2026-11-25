// SPDX-License-Identifier: MIT
// A request's key in the system shader compiler's store (compiler/compiler_store.h), shared by every process: what its
// record is made from and nothing that says where the request came from. Each stage's function is its AIR (the HASH tag
// Apple's compiler wrote into the metallib's function list: the SHA-256 of the function's bitcode, which the translator
// checks against the bitcode before it compiles anything of a library it did not read off a sealed volume) with the
// constants it was made with; the main stages' names and every library's path or bytes are left out, so one shader
// in two apps' libraries, or in a framework and a copy of it, is one entry. A linked function keeps the name it was made
// under, which its caller links by. A render state adds the descriptor's fields as its key names them and its variant; a
// compute request its CacheKey's text (its AIR, constants, stage input, linked functions, limits and the client's
// binding text, which its record is made by) and a kernel its dispatch.
//
// The entry's head holds `content` and every reader compares it whole (gm20b_cache.cpp readEntryHead), so an entry is
// taken only by a request of the same content byte for byte. `name` only routes a lookup: a fixed-size combine of the
// content's words, one multiply for each 8 bytes and no digest. Two contents that meet in a name cost a compile, never a
// wrong record.
#pragma once
#include <vector>
#include "compile_request.h"
#include "metallib.h"

namespace gm20b {

struct StoreKey {
    CacheKey content;
    Digest name{};
};

// `k`: the request's CacheKey as the compiler makes it (compiler/compiler_request.h prepare); `air[i]`: r.stages[i]'s AIR
StoreKey storeKeyOf(const CacheKey &k, const CompileRequest &r, const std::vector<AirHash> &air);

// The store's name of a program slot's bytes (record.h programSlot), apart from every record's name: its size and a fixed
// number of its words, its first 256 bytes (its header and first instructions) and words spread over the rest. Like a
// record's name it only routes: whoever runs a slot found under it compares the bytes whole first (remote.cpp findSlot),
// and the compiler leaves out a slot whose name another holds (compiler_store.cpp putProgramSlots).
Digest programSlotName(const void *slot, size_t size);

// A render record a reader took from the store, given the key hashes of its own key (record.h packKeyHash of `key`, the
// reader's CacheKey text, and its stride twin's). An entry another process made carries that process's: alike-named
// states of two processes would otherwise share a hash in a reader that has both (Device::addRecord files by it).
inline void rekeyRecord(RuntimeRecord &record, const std::string &key)
{
    record.pipeline.keyHash = packKeyHash(key);
    if (!record.strideTwin)
        return;
    auto twin = std::make_shared<RuntimeRecord>(*record.strideTwin);
    twin->pipeline.keyHash = packKeyHash(strideTwinKey(key));
    record.pipeline.strideTwin = twin->pipeline.keyHash;
    record.strideTwin = std::move(twin);
}

}  // namespace gm20b
