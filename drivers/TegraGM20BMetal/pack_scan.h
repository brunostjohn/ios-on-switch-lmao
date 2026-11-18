// SPDX-License-Identifier: MIT
// What every pack the generator writes holds to, which the runtime reads unchecked (Pack::load reads its header alone, a
// record is drawn as the pack holds it): the generator's tests scan the built pack (test/directcheck_test.py) and the
// packs host tests write with this.
#pragma once
#include "dynamic_push.h"
#include "pack.h"
#include "set_entries.h"
#include "storage.h"

#include <cstdio>
#include <string>
#include <vector>

namespace gm20b {

// Why the runtime could not draw record `pp` of `pack` as the pack holds it; nullptr: it can
inline const char *packRecordRefusal(const Pack &pack, const PackPipeline &pp)
{
    if (const char *why = entriesRefusal(pp, pack.far(pp), pack.header()->samplers))
        return why;
    if (!usableDynamicPush(pp, pack.string(pp.keyString)))
        return "its dynamic push words are not this runtime's";
    if (!usableStorageMetadata(pp, pack.section<uint32_t>(PackBlob) + pp.blob, pp.blobDwords, pack.far(pp)))
        return "its storage metadata is not this runtime's";
    return nullptr;
}

// Each way `pack` is not what the runtime reads: its pipelines out of keyHash order, a record's far block or a program's
// far table past the blob section, a record of more entries than any holds, a record refused (packRecordRefusal), a fast
// key out of order or not naming the first of its state's records and every one of them
inline std::vector<std::string> packScan(const Pack &pack)
{
    std::vector<std::string> out;
    const auto say = [&out](const char *what, uint64_t which, const char *why) {
        char line[96];
        snprintf(line, sizeof line, "%s %016llx: ", what, (unsigned long long)which);
        out.push_back(line + std::string(why));
    };
    const PackHeader &h = *pack.header();
    const PackPipeline *p = pack.section<PackPipeline>(PackPipelines);
    const uint64_t blobWords = pack.bytes(PackBlob) / sizeof(uint32_t);
    for (uint32_t i = 0; i < h.pipelines; i++) {
        if (i && p[i].keyHash < p[i - 1].keyHash)
            say("pipeline", p[i].keyHash, "out of keyHash order");
        const uint32_t count = entryCount(p[i]);
        if (count > kSetEntriesMax)
            say("pipeline", p[i].keyHash, "of more set-0 entries than a record holds");
        else if (count > kPackEntries && uint64_t(farEntriesAt(p[i])) + farWords(count) > blobWords)
            say("pipeline", p[i].keyHash, "its far entries past the blob section");
        else if (const char *why = packRecordRefusal(pack, p[i]))
            say("pipeline", p[i].keyHash, why);
    }
    const PackProgram *programs = pack.section<PackProgram>(PackPrograms);
    for (uint32_t i = 0; i < h.programs; i++)
        if (programFarAt(programs[i]) && programFarAt(programs[i]) - 1 + kProgramCbufs / 2 > blobWords)
            say("program", i, "its far uniform buffers past the blob section");
    const PackFastKey *fast = pack.section<PackFastKey>(PackFastKeys);
    for (uint64_t i = 0; i < pack.bytes(PackFastKeys) / sizeof(PackFastKey); i++) {
        const PackFastKey &k = fast[i];
        if (i && k.fast <= fast[i - 1].fast)
            say("fast key", k.fast, "out of order");
        if (!k.records || uint64_t(k.first) + k.records > h.pipelines) {
            say("fast key", k.fast, "its records past the pipelines");
            continue;
        }
        const uint64_t state = p[k.first].keyHash;
        const bool whole = (!k.first || p[k.first - 1].keyHash != state) &&
                           (uint64_t(k.first) + k.records == h.pipelines || p[k.first + k.records].keyHash != state);
        bool one = true;
        for (uint32_t r = 1; r < k.records; r++)
            one &= p[k.first + r].keyHash == state;
        if (!whole || !one)
            say("fast key", k.fast, "not its state's records alone and all of them");
    }
    return out;
}

}  // namespace gm20b
