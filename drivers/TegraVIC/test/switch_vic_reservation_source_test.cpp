// SPDX-License-Identifier: BSD-2-Clause-Patent
// A job without events reserves its destination for writing when it is queued, whatever it turns out to be, and a
// quarter turn the engine thread records instead of copying is made under that reservation. A rule in transform() that
// guessed which jobs might be recorded had to track runJob's precondition by hand, and a rebase let the two drift.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(TegraVICReservation, EveryJobWithoutEventsReservesAndARecordedTurnUsesIt)
{
    const std::string source = SourceText("../TegraVICUserClient.cpp");
    const std::string queue = Body(source, "IOReturn transform(Conn *conn, const void *in, uint32_t size)\n");
    EXPECT_THAT(queue, HasSubstr("if (!job->events)\n        switch_vic_surface_reserve_write(&job->dst, "
                                 "&job->dstReserve);"));
    EXPECT_THAT(queue, Not(HasSubstr("mayDeferTurn")));
    const std::string run = Body(source, "bool runJob(Job *job, IOReturn *status)\n");
    EXPECT_THAT(run, HasSubstr("&job->events, traits,\n                              &dstReserve.reservation)"));
}
