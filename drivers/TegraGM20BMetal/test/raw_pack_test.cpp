// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../raw/runtime/raw_pack.h"

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
        const raw::PackVariant v = {raw::AssumeUniformAligned << i, uint16_t(10 + i), 0};
        memcpy(file.data() + 0x300 + i * sizeof v, &v, sizeof v);
    }
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(RawPack, RefusesAPackOfAnotherVersion)
{
    raw::Pack pack;
    std::string err;
    EXPECT_FALSE(pack.load(writePack(14, 0, "v14.bin").c_str(), 0, err));
    EXPECT_EQ(err, "not a version-15 pack");
}

TEST(RawPack, ReadsAStagesVariantsInOrder)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writePack(raw::kPackVersion, 2, "v15.bin").c_str(), 0, err)) << err;
    EXPECT_EQ(pack.header()->chipFacts, raw::AssumeTexsF16);
    const raw::PackPipeline *pp = pack.find(7);
    ASSERT_NE(pp, nullptr);
    uint32_t count = 0;
    EXPECT_EQ(pack.variants(*pp, 0, count), nullptr);
    EXPECT_EQ(count, 0u);
    const raw::PackVariant *v = pack.variants(*pp, 1, count);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(v[0].program, 10);
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

}  // namespace
