// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::SourceText;

TEST(ExternalOwnership, FrontReadLivesUntilReplacementAndGenerationChangesRefuseReuse)
{
    const auto source = SourceText("../SwitchExternalScanout.cpp");
    const auto latch = Block(source, "bool SwitchExternalScreen::frameEnd()");
    EXPECT_TRUE(latch.has("front != drawing"));
    EXPECT_TRUE(latch.has("releaseRead(static_cast<unsigned>(front));"));
    EXPECT_TRUE(latch.has("switch_native_scanout_deliver(&slots[front].read);"));
    EXPECT_FALSE(latch.has("switch_native_scanout_release"));

    const auto reuse = Block(source, "if (slot.read.lease)");
    EXPECT_TRUE(reuse.has("slot.read.allocation.content_generation != snapshot.allocation.content_generation"));
    EXPECT_TRUE(reuse.has("switch_native_scanout_release(&snapshot); return -1;"));
    EXPECT_FALSE(reuse.has("releaseRead"));
    EXPECT_FALSE(reuse.has("slot.read ="));

    const auto lifecycle = SourceText("../SwitchExternalScreen.cpp");
    const auto off = Block(lifecycle, "void SwitchExternalScreen::linkDown()");
    EXPECT_TRUE(off.has("releaseRead(static_cast<unsigned>(front));"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::detach()").has("releaseSlot(i);"));
    EXPECT_TRUE(Block(lifecycle, "void SwitchExternalScreen::linkUp(").has("mode.hactive != timing.hactive"));
    EXPECT_TRUE(Block(source, "int SwitchExternalScreen::prepareSlot(").has("native_legacy_surface_bounds("));
}
