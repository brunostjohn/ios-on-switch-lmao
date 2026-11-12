// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.h: which private textures bind compressed memory, and of which kind.
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_private_comp.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_storage_form.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

// An attachment as the storage view sees it
struct Target {
    uint32_t bpp = 4, samples = 1;
    bool comptags = false;
};

// What a draw of a state with `storageBits` (and a barrier form or not) takes over `t`: its storage form ("raw"), its
// barrier form ("twin"), or the refusal that sends it to m2v ("enc.storage"), once the plain move ran before the encode
std::string route(uint8_t storageBits, bool twin, Target &t)
{
    if (raw::storageSlotsToPlain(storageBits, twin) & 1)
        t.comptags = false;
    const auto reason = raw::storageViewMismatch(4, 1, t);
    if (reason == raw::StorageFallback::None)
        return "raw";
    return twin ? "twin" : "enc.storage";
}

raw::PrivateTextureFacts target(uint32_t bpp)
{
    raw::PrivateTextureFacts f;
    f.privateStorage = f.oneLevel = f.oneSample = f.twoD = f.renderedAndSampledOnly = f.plainColour = true;
    f.compression = true;
    f.bpp = bpp;
    return f;
}

TEST(RawPrivateComp, FourAndEightByteTargetsTakeTheProvenColourKinds)
{
    EXPECT_EQ(raw::privateCompressedKind(target(4)), uint32_t(raw::kCompKindC32));
    EXPECT_EQ(raw::privateCompressedKind(target(8)), uint32_t(raw::kCompKindC64));
    EXPECT_EQ(raw::privateCompressedKind(target(16)), 0u) << "C128: the copy engine was not asked";
    EXPECT_EQ(raw::privateCompressedKind(target(2)), 0u);
}

TEST(RawPrivateComp, AnythingElseStaysPlain)
{
    bool raw::PrivateTextureFacts::*fields[] = {
        &raw::PrivateTextureFacts::privateStorage,
        &raw::PrivateTextureFacts::oneLevel,
        &raw::PrivateTextureFacts::oneSample,
        &raw::PrivateTextureFacts::twoD,
        &raw::PrivateTextureFacts::renderedAndSampledOnly,
        &raw::PrivateTextureFacts::plainColour,
        &raw::PrivateTextureFacts::compression,
    };
    for (auto field : fields) {
        raw::PrivateTextureFacts f = target(4);
        f.*field = false;
        EXPECT_EQ(raw::privateCompressedKind(f), 0u);
    }
}

TEST(RawPrivateComp, AFetchingStateWithNoBarrierFormDrawsAPlainTargetOnTheRawPath)
{
    // gpu.raw.fetch_blend_order strip16 (rgba8, 64x64 private RenderTarget|ShaderRead): boot 11's sto=compressed,notwin
    Target strip{4, 1, true};
    EXPECT_EQ(raw::storageViewMismatch(4, 1, strip), raw::StorageFallback::Compressed);
    EXPECT_EQ(route(0x1, false, strip), "raw");
    EXPECT_FALSE(strip.comptags);
    // gpu.raw.pass_fusion private_color_adjacent: a clear pass and a load pass on one target, the fetch in both
    Target adjacent{4, 1, true};
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
}

TEST(RawPrivateComp, AStateWithABarrierFormKeepsTheCompressedTarget)
{
    Target t{4, 1, true};
    EXPECT_EQ(raw::storageSlotsToPlain(0x3, true), 0);
    EXPECT_EQ(route(0x1, true, t), "twin");
    EXPECT_TRUE(t.comptags);
    EXPECT_EQ(raw::storageSlotsToPlain(0x6, false), 0x6);
}

TEST(RawPrivateComp, TheMoveRunsBeforeTheRawEncodeWhichMayNotAskForIt)
{
    const std::string submit = SourceText("../raw/runtime/RawMetalSubmit.mm");
    EXPECT_THAT(submit, HasSubstr("rmStorageTargetsPlain(cb, range.from, range.to);\n        const bool encoded = "
                                  "encodeRaw(cb, range.from, range.to, &why, &refusedEncoder);"));
    const std::string scan = Body(SourceText("../raw/runtime/RawMetalPrivateComp.mm"), "void rmStorageTargetsPlain(");
    EXPECT_THAT(scan, HasSubstr("raw::storageSlotsToPlain(pl->storageBits, pl->barrierTwin != nullptr)"));
    EXPECT_THAT(scan, HasSubstr("rmTexturePlain(t)"));
    const std::string encode = SourceText("../raw/runtime/RawMetalEncode.mm");
    EXPECT_THAT(encode, HasSubstr("tRawEncoding = true;"));
}

}  // namespace
