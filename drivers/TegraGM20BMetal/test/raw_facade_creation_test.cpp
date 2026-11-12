// SPDX-License-Identifier: MIT
// A render state's synchronous creation on the raw facade over the kernel model: one the pack lacks is handed back only
// once it is usable, and where it cannot be (no shader compiler comes this boot, its compile could not run) the creation
// fails with an NSError, as Metal's does, instead of handing back a state whose first draws go elsewhere (state.making).
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

// An unpacked state's creation with the broker answering as set; its error's text, "" for a state made
std::string created(uint32_t flags, int32_t result)
{
    kernel_model::model.compilerFlags = flags;
    kernel_model::model.compileResult = result;
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(writePack(packKey(kPacked)));
    if (!device)
        return "no device";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    if (!library)
        return "no library: " + text(error);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, kUnpacked) error:&error];
    if (state)
        return "";
    return error ? text(error.localizedDescription) : "nil with no error";
}

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
        const std::string why =
            created(TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered,
                    TegraGM20BCompileFailed);
        EXPECT_NE(why.find("the state's compile could not run"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 1u);
    });
}

TEST(RawFacadeCreation, ACompilerThatWentIsNotTheProcessesToCompile)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const std::string why =
            created(TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerDeclined,
                    TegraGM20BCompileUnavailable);
        EXPECT_NE(why.find("no shader compiler comes this boot"), std::string::npos) << why;
    });
}

}  // namespace
