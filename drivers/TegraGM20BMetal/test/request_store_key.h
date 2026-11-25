// SPDX-License-Identifier: MIT
// The store key a test's stand-in compiler files request `r` of CacheKey `k` under (store_key.h), each stage's AIR read
// off its library as the system compiler reads it: a data library's bytes, a system library at its path
#pragma once
#include <map>
#include "../metallib.h"
#include "../store_key.h"

namespace harness {

inline gm20b::StoreKey requestStoreKey(const gm20b::CacheKey &k, const gm20b::CompileRequest &r)
{
    std::vector<gm20b::AirHash> air;
    for (const gm20b::RequestStage &s : r.stages) {
        std::map<std::string, gm20b::AirHash> hashes;
        const gm20b::RequestLibrary &l = r.libraries[s.library];
        if (l.system)
            gm20b::metallibAirHashes(l.path, hashes);
        else
            gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(l.bytes.data()), l.bytes.size(), hashes);
        const auto found = hashes.find(s.function);
        air.push_back(found == hashes.end() ? gm20b::AirHash{} : found->second);
    }
    return gm20b::storeKeyOf(k, r, air);
}

}  // namespace harness
