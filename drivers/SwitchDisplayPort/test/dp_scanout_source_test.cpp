// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::SourceText;

TEST(ExternalOwnership, FrontReadLivesUntilReplacementAndGenerationChangesRefuseReuse)
{
    const auto source = SourceText("../SwitchExternalScanout.cpp");
    const auto latch = Block(source, "void SwitchExternalScreen::frameEnd()");
    EXPECT_TRUE(latch.Has("front != drawing"));
    EXPECT_TRUE(latch.Has("releaseRead(static_cast<unsigned>(front));"));
    EXPECT_TRUE(latch.Has("switch_native_scanout_deliver(&slots[front].read);"));
    EXPECT_FALSE(latch.Has("switch_native_scanout_release"));

    const auto reuse = Block(source, "if (slot.read.lease)");
    EXPECT_TRUE(reuse.Has("slot.read.allocation.content_generation != snapshot.allocation.content_generation"));
    EXPECT_TRUE(reuse.Has("switch_native_scanout_release(&snapshot); return -1;"));
    EXPECT_FALSE(reuse.Has("releaseRead"));
    EXPECT_FALSE(reuse.Has("slot.read ="));

    const auto lifecycle = SourceText("../SwitchExternalScreen.cpp");
    const auto off = Block(lifecycle, "void SwitchExternalScreen::linkDown()");
    EXPECT_TRUE(off.Has("releaseRead(static_cast<unsigned>(front));"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::detach()").Has("releaseSlot(i);"));
}
