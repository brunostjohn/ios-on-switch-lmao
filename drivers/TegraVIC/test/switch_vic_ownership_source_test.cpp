// SPDX-License-Identifier: BSD-2-Clause-Patent
// What VIC hands another engine, and what it writes, goes through the GPU's ownership layer: a copy-engine copy is only
// the checked one, whose completion reports the copy's own failure to the job that queued it.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(VicOwnershipSource, CopyEngineCopiesAreCheckedAndReportTheirFailure)
{
    EXPECT_THAT(SourceText("../../TegraGM20B/TegraGM20BCopy.h"), Not(HasSubstr("int tegra_gm20b_ce_copy_plane(")));
    const std::string bounce = SourceText("../SwitchVICBounceSubmit.cpp");
    EXPECT_THAT(bounce, HasSubstr("const tegra_gm20b_ce_done done = {TEGRA_GM20B_CE_NO_SYNCPT, completed, work};"));
    EXPECT_THAT(bounce, HasSubstr("nullptr, &done,"));
}
