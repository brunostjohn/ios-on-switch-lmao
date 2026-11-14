// SPDX-License-Identifier: MIT
// Guards on a render pipeline whose functions call linked functions by name (iOS 27's RenderBox: each icon effect is made with
// MTLFunctionDescriptor.specializedName custom_fn or custom_blend and linked through fragmentLinkedFunctions.privateFunctions),
// which no host model runs: the raw path's key names each linked function, so the pack holds such a state, and the pack
// generator files a state under the key the runtime computes.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {

TEST(RawLinkedSource, TheRawRecordIsTheLinkedFunctionsToo)
{
    const std::string states = SourceText("../raw/runtime/RawMetalStates.mm");
    EXPECT_THAT(Body(states, "RMPipeline *rmPipelineShell("), HasSubstr("raw_metal_linked_packable(d)"));
    EXPECT_THAT(Body(SourceText("../raw/runtime/RawMetalLibrary.mm"), "NSString *raw_metal_pipeline_key("),
                HasSubstr("raw::pipelineKey(d, vf ? &vertex : nullptr, ff ? &fragment : nullptr, linkedKey(d),"));
    EXPECT_THAT(Body(SourceText("../raw/runtime/RawPipelineKey.h"), "inline std::string pipelineKey("),
                HasSubstr("key += linked;"));

    EXPECT_THAT(Body(SourceText("../raw/runtime/RawComplete.mm"), "bool Completion::keyOf(Ask &a, CacheKey &k)"),
                HasSubstr("renderCacheKey(a.facade ?: a.descriptor, a.key, a.own, stack_, k, a.facade ? "
                          "rmFunctionSource : nullptr)"));
    const std::string key = SourceText("../raw/runtime/RawRenderKey.mm");
    EXPECT_THAT(key, HasSubstr("descriptor.vertexLinkedFunctions"));
    EXPECT_THAT(key, HasSubstr("descriptor.fragmentLinkedFunctions"));
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_render_key.h"), "inline CacheKey renderCacheKey("),
                HasSubstr("key += \" | linked\";"));
}

// The pack generator files a state under the key the runtime computes of the descriptor it made, never under its corpus
// line's text, so a line written otherwise than the device journals it (a constant's width, a "spec" mark) still hits.
TEST(RawLinkedSource, ThePacksKeysAreTheRuntimes)
{
    const std::string parse = Body(SourceText("../raw/pack/probe_states.h"), "inline bool parseState(");
    EXPECT_THAT(parse,
                HasSubstr("s.key = raw::pipelineKey(d, &vertex, d.fragmentFunction ? &fragment : nullptr, linked,"));
    EXPECT_THAT(Body(SourceText("../raw/pack/probe_states.h"), "inline id<MTLFunction> function("),
                HasSubstr("key.constants = raw::constantsText(values);"));
    EXPECT_THAT(Body(SourceText("../raw/runtime/RawMetalLibrary.mm"), "- (RMFunction *)wrap:"),
                HasSubstr("f->_constants = raw::constantsText(values);"));
}

}  // namespace
