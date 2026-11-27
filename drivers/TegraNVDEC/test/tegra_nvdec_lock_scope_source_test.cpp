// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(NVDECLockScopeWiring, SubmitBufferFreeAndCloseDropClientLockAcrossSleeps)
{
    const std::string client = SourceText("../TegraNVDECUserClient.cpp");
    const std::string ext = Body(client, "IOReturn AppleD5500UserClient::externalMethod(");
    EXPECT_THAT(ext, HasSubstr("else if (method == TegraNVDECMethodBufferFree)"));
    EXPECT_THAT(ext, HasSubstr("else if (method == TegraNVDECMethodSubmit)"));

    const std::string submit = Body(client, "IOReturn AppleD5500UserClient::submitMethod(");
    const size_t submitUnlock = submit.find("IOLockUnlock(lock_);");
    const size_t runCall = submit.find("tegra_nvdec_run(");
    ASSERT_NE(submitUnlock, std::string::npos);
    ASSERT_NE(runCall, std::string::npos);
    EXPECT_LT(submitUnlock, runCall);
    EXPECT_THAT(submit, HasSubstr("snapshotSubmitLocked();"));

    const std::string bufferFree = Body(client, "IOReturn AppleD5500UserClient::bufferFreeMethod(");
    const size_t freeUnlock = bufferFree.find("IOLockUnlock(lock_);");
    const size_t freeQuiesce = bufferFree.find("quiesceFence(");
    ASSERT_NE(freeUnlock, std::string::npos);
    ASSERT_NE(freeQuiesce, std::string::npos);
    EXPECT_LT(freeUnlock, freeQuiesce);

    const std::string close = Body(client, "IOReturn AppleD5500UserClient::clientClose()");
    const size_t closeUnlock = close.find("IOLockUnlock(lock_);");
    const size_t closeQuiesce = close.find("quiesceFence(");
    ASSERT_NE(closeUnlock, std::string::npos);
    ASSERT_NE(closeQuiesce, std::string::npos);
    EXPECT_LT(closeUnlock, closeQuiesce);
}
}  // namespace
