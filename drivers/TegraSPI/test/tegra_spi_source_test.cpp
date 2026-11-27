// SPDX-License-Identifier: BSD-2-Clause-Patent
// Source guards for TegraSPI's hot path: no redundant irqLock, no full-system dmb sy, no hard-IRQ CAS, and
// skip clearStatus() on clean FIFOs.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(TegraSPISource, HotPathHasNoIrqLockDmbSyOrHardIrqCas)
{
    const std::string spi = SourceText("../TegraSPI.cpp");
    EXPECT_THAT(spi, Not(HasSubstr("irqLock")));
    EXPECT_THAT(spi, Not(HasSubstr("dmb sy")));
    EXPECT_THAT(spi, HasSubstr("switch_mmio_write32_publish(b.regs, COMMAND1, cmd | CMD1_PIO);"));
    EXPECT_THAT(spi, HasSubstr("ensureCleanFifos(b)"));

    const std::string filter = Body(spi, "bool filterInterrupt(");
    ASSERT_FALSE(filter.empty());
    EXPECT_THAT(filter, Not(HasSubstr("__atomic_compare_exchange")));
    EXPECT_LT(filter.find("TRANS_STATUS, trans"), filter.find("kDone, __ATOMIC_RELEASE"));

    const std::string sleep = Body(spi, "int sleepForAction(");
    ASSERT_FALSE(sleep.empty());
    EXPECT_THAT(sleep, HasSubstr("b.irq->disable();"));
    EXPECT_THAT(sleep, Not(HasSubstr("__atomic_compare_exchange")));
}
