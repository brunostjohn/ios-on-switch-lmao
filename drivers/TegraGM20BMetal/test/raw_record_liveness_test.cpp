// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstring>
#include "../raw/runtime/raw_record_parts.h"

namespace {

raw::PackEntry fetch(uint16_t offset, uint8_t colour)
{
    return {offset, raw::EntryFetch, 2, colour, raw::SamplerNone, 0, 0};
}

// A storage form fetching colours 0 and 1 (entries 0 and 1), writing colour 0, its programs read `reads` of set 0
raw::RuntimeRecord storageForm(uint32_t reads)
{
    raw::RuntimeRecord rec;
    raw::PackPipeline &p = rec.pipeline;
    p.flags = raw::PipelineFetches | raw::PipelineStorageMetadata;
    p.entries = 2;
    p.entry[0] = fetch(32, 0);
    p.entry[1] = fetch(36, 1);
    p.storageMask = 3;
    p.storageWriteMask = 1;
    p.storageTic[0] = 0x11;
    p.storageTic[1] = 0x22;
    p.fetchRung[0] = raw::FetchInterlock;
    p.fetchRung[1] = raw::FetchBarrier;
    p.memoryEntries = reads;
    return rec;
}

TEST(RawRecordLiveness, AFetchNoProgramReadsLeavesTheRecordWithItsStorageView)
{
    raw::RuntimeRecord rec = storageForm(1u << 0);
    EXPECT_EQ(raw::dropUnreadFetches(rec), 1u << 1);
    const raw::PackPipeline &p = rec.pipeline;
    EXPECT_EQ(p.entry[1].kind, raw::EntrySamplerOnly);
    EXPECT_EQ(p.entry[1].offset, 36);
    EXPECT_EQ(p.entry[0].kind, raw::EntryFetch);
    EXPECT_EQ(p.storageMask, 1);
    EXPECT_EQ(p.storageWriteMask, 1);
    EXPECT_EQ(p.storageTic[1], 0u);
    EXPECT_EQ(p.fetchRung[1], raw::FetchNone);
    EXPECT_TRUE(p.flags & raw::PipelineFetches);
}

TEST(RawRecordLiveness, ARecordWhoseProgramsReadNoFetchFetchesNothing)
{
    raw::RuntimeRecord rec = storageForm(0);
    EXPECT_EQ(raw::dropUnreadFetches(rec), 3);
    EXPECT_FALSE(rec.pipeline.flags & raw::PipelineFetches);
    EXPECT_EQ(rec.pipeline.storageMask, 0);
}

TEST(RawRecordLiveness, ReadsThatNoAnalysisCouldNameKeepEveryFetch)
{
    raw::RuntimeRecord rec = storageForm(~0u);
    EXPECT_EQ(raw::dropUnreadFetches(rec), 0);
    EXPECT_EQ(rec.pipeline.storageMask, 3);
}

TEST(RawRecordLiveness, AColourOneOfWhoseEntriesIsReadStays)
{
    raw::RuntimeRecord rec = storageForm(1u << 2);
    rec.pipeline.entries = 3;
    rec.pipeline.entry[2] = fetch(40, 1);
    EXPECT_EQ(raw::dropUnreadFetches(rec), 1u << 0);
    EXPECT_EQ(rec.pipeline.storageMask, 1u << 1);
    EXPECT_EQ(rec.pipeline.entry[1].kind, raw::EntryFetch);
}

// A program of one instruction group after its 0x50-byte header, `word` at the group's slot `slot` (0: its scheduling
// word)
raw::CapturedProgram programWith(uint64_t word, size_t slot)
{
    raw::CapturedProgram p;
    p.image.assign(0x50 + 32, 0);
    memcpy(p.image.data() + 0x50 + 8 * slot, &word, sizeof word);
    return p;
}

constexpr uint64_t kSuld = 0xeb00ull << 48, kSust = 0xeb20ull << 48;

TEST(RawRecordLiveness, AStorageFormLoadsAColourOnlyWithASurfaceLoad)
{
    raw::RuntimeRecord rec;
    rec.programs[1] = programWith(kSust, 1);
    EXPECT_FALSE(raw::loadsSurface(rec));
    rec.programs[1] = programWith(kSuld, 0);
    EXPECT_FALSE(raw::loadsSurface(rec));
    rec.programs[1] = programWith(kSuld, 2);
    EXPECT_TRUE(raw::loadsSurface(rec));
}

TEST(RawRecordLiveness, AVariantsSurfaceLoadIsTheRecords)
{
    raw::RuntimeRecord rec;
    rec.programs[1] = programWith(kSust, 1);
    rec.variants.push_back({1, programWith(kSuld, 3)});
    EXPECT_TRUE(raw::loadsSurface(rec));
}

}  // namespace
