// SPDX-License-Identifier: MIT
// A render state's creation on the raw facade over the kernel model: one the pack lacks is handed back only once it is
// usable, and where it cannot be (no shader compiler comes this boot, its compile could not run) the creation fails with an
// NSError, as Metal's does, instead of handing back a state whose first draws go elsewhere (state.making). The
// completion-handler form fails the same way, and no m2v state stands in for a failed one.
#include "raw_facade_harness.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm, kUnpacked = MTLPixelFormatRGBA8Unorm;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Opened {
    id<MTLDevice> device;
    id<MTLLibrary> library;
};

// The facade completing the states its pack lacks, with the broker answering as set
Opened opened(uint32_t flags, int32_t result)
{
    kernel_model::model.compilerFlags = flags;
    kernel_model::model.compileResult = result;
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(writePack(packKey(kPacked)));
    if (!device)
        return {};
    const std::string cache = dir() + "/cache";
    mkdir(cache.c_str(), 0700);
    raw_metal_complete_states(cache.c_str(), nullptr);
    NSError *error = nil;
    return
    {
        device, [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error]
    };
}

// An unpacked state's creation with the broker answering as set; its error's text, "" for a state made
std::string created(uint32_t flags, int32_t result)
{
    const auto [device, library] = opened(flags, result);
    if (!library)
        return "no device or library";
    NSError *error = nil;
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, kUnpacked) error:&error];
    if (state)
        return "";
    return error ? text(error.localizedDescription) : "nil with no error";
}

constexpr uint32_t kServing = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered;

TEST(RawFacadeCreation, NoCompilerComingFailsTheCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const std::string why = created(TegraGM20BCompilerCapable | TegraGM20BCompilerDeclined, 0);
        EXPECT_NE(why.find("no shader compiler comes this boot"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 0u);
    });
}

TEST(RawFacadeCreation, ACompileTheCompilerCouldNotRunFailsTheCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const std::string why = created(kServing, TegraGM20BCompileFailed);
        EXPECT_NE(why.find("the state's compile could not run"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 1u);
    });
}

TEST(RawFacadeCreation, ACompilerThatWentIsNotTheProcessesToCompile)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const std::string why = created(kServing | TegraGM20BCompilerDeclined, TegraGM20BCompileUnavailable);
        EXPECT_NE(why.find("no shader compiler comes this boot"), std::string::npos) << why;
    });
}

TEST(RawFacadeCreation, ACompletionHandlersCreationFailsAsTheSynchronousOneDoes)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(TegraGM20BCompilerCapable | TegraGM20BCompilerDeclined, 0);
        ASSERT_TRUE(library != nil);
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        __block id<MTLRenderPipelineState> state = nil;
        __block NSError *error = nil;
        [device newRenderPipelineStateWithDescriptor:descriptor(library, kUnpacked)
                                   completionHandler:^(id<MTLRenderPipelineState> made, NSError *failed) {
                                       state = made;
                                       error = failed;
                                       dispatch_semaphore_signal(answered);
                                   }];
        ASSERT_EQ(dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
        EXPECT_EQ(state, nil);
        EXPECT_NE(text(error.localizedDescription).find("no shader compiler comes this boot"), std::string::npos)
            << text(error);
        EXPECT_FALSE([m2vRecord(device)[@"opened"] boolValue]) << text(m2vRecord(device));
    });
}

}  // namespace
