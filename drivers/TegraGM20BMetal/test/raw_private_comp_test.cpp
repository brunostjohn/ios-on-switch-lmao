// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.h: which private textures bind compressed memory, and of which kind.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_private_comp.h"

namespace {

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

}  // namespace
