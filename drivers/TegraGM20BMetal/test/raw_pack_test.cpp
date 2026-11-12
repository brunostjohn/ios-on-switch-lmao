// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_variants.h"

namespace {

// A pack of one pipeline whose fragment stage has `variants` variants, its header saying `version`
std::string writePack(uint32_t version, uint32_t variants, const char *name)
{
    std::vector<uint8_t> file(0x400, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = version;
    h.pipelines = 1;
    h.section[raw::PackPipelines] = {0x100, sizeof(raw::PackPipeline)};
    h.section[raw::PackVariants] = {0x300, variants * sizeof(raw::PackVariant)};
    h.chipFacts = raw::AssumeTexsF16;
    memcpy(file.data(), &h, sizeof h);
    raw::PackPipeline pp = {};
    pp.keyHash = 7;
    pp.variantCount[1] = uint8_t(variants);
    memcpy(file.data() + 0x100, &pp, sizeof pp);
    for (uint32_t i = 0; i < variants; i++) {
        const raw::PackVariant v = {raw::AssumeUniformAligned << i, 70000 + i};
        memcpy(file.data() + 0x300 + i * sizeof v, &v, sizeof v);
    }
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

// A pack of a state's own record and two of its variants' (" | tests=2" and a cubic one), under one key
std::string writeVariantPack(const char *name)
{
    std::vector<uint8_t> file(0x1000, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    h.pipelines = 3;
    h.section[raw::PackPipelines] = {0x100, 3 * sizeof(raw::PackPipeline)};
    const char strings[] = "\0 | tests=2\0 | cubic s0:0000n t0:80";
    h.section[raw::PackStrings] = {0xc00, sizeof strings};
    memcpy(file.data(), &h, sizeof h);
    memcpy(file.data() + 0xc00, strings, sizeof strings);
    raw::PackPipeline pp[3] = {};
    for (uint32_t i = 0; i < 3; i++) {
        pp[i].keyHash = 7;
        pp[i].vs = 70000 + i;
        pp[i].flags = i ? raw::PipelineVariantRecord : 0;
    }
    pp[1].variantName = 1;
    pp[2].variantName = 12;
    memcpy(file.data() + 0x100, pp, sizeof pp);
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(RawPack, AStatesVariantRecordsAreFoundByNameNeverAsTheState)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeVariantPack("variants.bin").c_str(), 0, err)) << err;
    ASSERT_NE(pack.find(7), nullptr);
    EXPECT_EQ(pack.find(7)->vs, 70000u);
    EXPECT_EQ(pack.find(7, raw::PackForm::Barrier), nullptr);
    EXPECT_EQ(pack.variantRecords(7).size(), 2u);
    ASSERT_NE(pack.variantRecord(7, " | tests=2"), nullptr);
    EXPECT_EQ(pack.variantRecord(7, " | tests=2")->vs, 70001u);
    EXPECT_EQ(pack.variantRecord(7, " | cubic s0:0000n t0:80")->vs, 70002u);
    EXPECT_EQ(pack.variantRecord(7, " | tests=3"), nullptr);
    EXPECT_EQ(pack.variantRecord(8, " | tests=2"), nullptr);
    const std::string key = "c0=80:15:0:1:0:0:1:0:0 | v a | f b | l RenderBox | cubic s0:0000n t0:80 | tests=4";
    EXPECT_EQ(key.substr(raw::variantNameAt(key)), " | cubic s0:0000n t0:80 | tests=4");
    EXPECT_EQ(raw::variantNameAt("c0=80 | v a | f b"), 17u);
}

TEST(RawPack, RefusesAPackOfAnotherVersion)
{
    raw::Pack pack;
    std::string err;
    EXPECT_FALSE(pack.load(writePack(18, 0, "v18.bin").c_str(), 0, err));
    EXPECT_EQ(err, "a version-18 pack: this runtime reads version 19 only");
}

TEST(RawPack, ReadsAStagesVariantsInOrder)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writePack(raw::kPackVersion, 2, "current.bin").c_str(), 0, err)) << err;
    EXPECT_EQ(pack.header()->chipFacts, raw::AssumeTexsF16);
    const raw::PackPipeline *pp = pack.find(7);
    ASSERT_NE(pp, nullptr);
    uint32_t count = 0;
    EXPECT_EQ(pack.variants(*pp, 0, count), nullptr);
    EXPECT_EQ(count, 0u);
    const raw::PackVariant *v = pack.variants(*pp, 1, count);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(v[0].program, 70000u);
    EXPECT_EQ(v[1].assume, raw::AssumeUniformAligned << 1);
}

TEST(RawPack, RefusesVariantsPastTheirSection)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writePack(raw::kPackVersion, 0, "short.bin").c_str(), 0, err)) << err;
    raw::PackPipeline pp = *pack.find(7);
    pp.variantCount[1] = 1;
    uint32_t count = 1;
    EXPECT_EQ(pack.variants(pp, 1, count), nullptr);
    EXPECT_EQ(count, 0u);
}

TEST(RawVariants, TheFirstVariantWhoseAssumptionsHoldIsDrawn)
{
    const raw::PackVariant v[2] = {{raw::AssumeUniformAligned | raw::AssumeTexsF16, 1}, {raw::AssumeUniformAligned, 2}};
    EXPECT_EQ(raw::chooseVariant(v, 2, raw::AssumeUniformAligned | raw::AssumeTexsF16), 0u);
    EXPECT_EQ(raw::chooseVariant(v, 2, raw::AssumeUniformAligned | raw::AssumeUniformAligned << 1), 1u);
    EXPECT_EQ(raw::chooseVariant(v, 2, raw::AssumeUniformAligned << 1), 2u);
    EXPECT_EQ(raw::chooseVariant(nullptr, 0, ~0u), 0u);
}

TEST(RawVariants, FloatAndExtendedRangeTexelsAreNotFixedPoint)
{
    EXPECT_TRUE(raw::fixedPointTexels(80));    // BGRA8Unorm
    EXPECT_TRUE(raw::fixedPointTexels(71));    // RGBA8Unorm_sRGB
    EXPECT_FALSE(raw::fixedPointTexels(115));  // RGBA16Float
    EXPECT_FALSE(raw::fixedPointTexels(552));  // BGRA10_XR
    EXPECT_FALSE(raw::fixedPointTexels(0));
}

// A pack listing two archives (keys {3, 9} and {5}) and the compute states it holds kernels of ({11})
std::string writeArchivePack(const char *name)
{
    std::vector<uint8_t> file(0x400, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    const uint32_t counts[2] = {3, 4};
    const raw::PackArchive archives[3] = {{raw::kPackHeldKernels, 0, 1}, {0x20, 1, 2}, {0x40, 3, 1}};
    const uint64_t keys[4] = {11, 3, 9, 5};
    h.section[raw::PackArchives] = {0x100, sizeof counts + sizeof archives + sizeof keys};
    memcpy(file.data(), &h, sizeof h);
    memcpy(file.data() + 0x100, counts, sizeof counts);
    memcpy(file.data() + 0x100 + sizeof counts, archives, sizeof archives);
    memcpy(file.data() + 0x100 + sizeof counts + sizeof archives, keys, sizeof keys);
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(RawPack, AnArchiveDeclaresItsOwnKeysAlone)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeArchivePack("archives.bin").c_str(), 0, err)) << err;
    const raw::PackArchive *first = pack.archive(0x20), *second = pack.archive(0x40);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(pack.archive(0x30), nullptr) << "an archive the pack does not list";
    EXPECT_TRUE(pack.declares(*first, 3));
    EXPECT_TRUE(pack.declares(*first, 9));
    EXPECT_FALSE(pack.declares(*first, 5)) << "the next archive's key";
    EXPECT_FALSE(pack.declares(*first, 11)) << "a held kernel's key";
    EXPECT_TRUE(pack.declares(*second, 5));
    const raw::PackArchive *held = pack.archive(raw::kPackHeldKernels);
    ASSERT_NE(held, nullptr);
    EXPECT_TRUE(pack.declares(*held, 11));
    EXPECT_FALSE(pack.declares(*held, 3));
}

// wide_index_pack.py's pack, written by the pack writer's codec: every program index past 65,535 reads back whole
TEST(RawPack, ProgramIndicesPast16BitsReadBackAsThePackWriterWroteThem)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load("drivers/TegraGM20BMetal/test/wide_index.bin", 0, err)) << err;
    const std::string key = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v wide_vertex  | f wide_fragment";
    const raw::PackPipeline *pp = pack.find(raw::Pack::fnv(key.data(), key.size()));
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(pp->vs, 70000u);
    EXPECT_EQ(pp->fs, 70001u);
    uint32_t count = 0;
    const raw::PackVariant *v = pack.variants(*pp, 1, count);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(v[0].program, 70002u);
    const std::string kernel = "kernel | wide_kernel";
    const raw::PackKernel *k = pack.findKernel(raw::Pack::fnv(kernel.data(), kernel.size()));
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->program, 70003u);
    EXPECT_EQ(k->regionWord, -1);
    const raw::PackSplit *split = pack.split(70001);
    ASSERT_NE(split, nullptr);
    EXPECT_EQ(split->fs, 70004u);
    EXPECT_EQ(split->prologue, 70005u);
    EXPECT_EQ(split->frontierBytes, 16u);
    EXPECT_EQ(split->frontierCbuf, 2u);
    EXPECT_EQ(pack.split(70001 & 0xFFFF), nullptr);
}

TEST(RawPack, APackWithoutArchiveListsListsNone)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeVariantPack("no-archives.bin").c_str(), 0, err)) << err;
    EXPECT_EQ(pack.archive(raw::kPackHeldKernels), nullptr);
    EXPECT_EQ(pack.archive(0x20), nullptr);
}

}  // namespace
