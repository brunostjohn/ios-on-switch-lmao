// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.h: which private textures bind compressed memory, and of which kind.
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_fetch_order.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_private_comp.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_storage_form.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

// An attachment as the storage view sees it
struct Target {
    uint32_t bpp = 4, samples = 1;
    bool comptags = false;
};

// A list of two triangles, which a barrier form draws a triangle at a time
constexpr raw::FetchDraw kList = {3, 6, 1, false, false};

// What a draw of shape `draw` of a state with `storageBits` (and a barrier form that fetches the colour it writes or not)
// takes over `t`: its storage form ("raw"), its barrier form ("twin"), or the refusal that sends it to m2v ("enc.storage",
// "enc.geometry"), once the plain move ran before the encode
std::string route(uint8_t storageBits, bool twin, Target &t, const raw::FetchDraw &draw = kList)
{
    const bool twinDraws = raw::barrierFormDraws(0x1, 0x1, 0, false, draw);
    if (raw::storageSlotsToPlain(storageBits, twin, twinDraws) & 1)
        t.comptags = false;
    const auto reason = raw::storageViewMismatch(4, 1, t);
    if (reason == raw::StorageFallback::None)
        return "raw";
    if (!twin)
        return "enc.storage";
    return twinDraws ? "twin" : "enc.geometry";
}

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

TEST(RawPrivateComp, AFetchingStateWithNoBarrierFormDrawsAPlainTargetOnTheRawPath)
{
    // gpu.raw.fetch_blend_order strip16 (rgba8, 64x64 private RenderTarget|ShaderRead): boot 11's sto=compressed,notwin
    Target strip{4, 1, true};
    EXPECT_EQ(raw::storageViewMismatch(4, 1, strip), raw::StorageFallback::Compressed);
    EXPECT_EQ(route(0x1, false, strip), "raw");
    EXPECT_FALSE(strip.comptags);
    // gpu.raw.pass_fusion private_color_adjacent: a clear pass and a load pass on one target, the fetch in both
    Target adjacent{4, 1, true};
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
}

TEST(RawPrivateComp, AStateWithABarrierFormKeepsTheCompressedTarget)
{
    Target t{4, 1, true};
    EXPECT_EQ(raw::storageSlotsToPlain(0x3, true, true), 0);
    EXPECT_EQ(route(0x1, true, t), "twin");
    EXPECT_TRUE(t.comptags);
    EXPECT_EQ(raw::storageSlotsToPlain(0x6, false, true), 0x6);
}

// Boot 21 iconservicesagent (RenderBox filter_color 2218a4f4d2231d46, uber 43017f3f15e41157, both drawn in the pixel
// interlock elsewhere): a quad drawn as an indexed strip of four over a compressed private target took the barrier
// form, which cannot order an indexed strip's triangles (restart), and the encode refused it as enc.geometry. The
// target moves plain and the storage form draws it, whatever the shape.
TEST(RawPrivateComp, AShapeTheBarrierFormCannotDrawTakesTheStorageFormOverAPlainTarget)
{
    const raw::FetchDraw indexedStrip = {4, 4, 1, false, true};
    EXPECT_FALSE(raw::barrierFormDraws(0x1, 0x1, 0, false, indexedStrip));
    Target t{4, 1, true};
    EXPECT_EQ(route(0x1, true, t, indexedStrip), "raw");
    EXPECT_FALSE(t.comptags);
    const raw::FetchDraw shapes[] = {
        {3, 6, 2, false, false}, {3, 0, 1, true, false}, {1, 4, 1, false, false}, {3, 3 * 97, 1, false, false}};
    for (const raw::FetchDraw &d : shapes) {
        Target each{4, 1, true};
        EXPECT_EQ(route(0x1, true, each, d), "raw") << d.primitive << " " << d.count << " " << d.instances;
    }
    Target strip{4, 1, true};
    EXPECT_EQ(route(0x1, true, strip, {4, 4, 1, false, false}), "twin") << "a vertex strip is drawn a triangle a time";
    EXPECT_TRUE(strip.comptags);
    EXPECT_TRUE(raw::barrierFormDraws(0x1, 0x2, 0, false, indexedStrip)) << "it fetches nothing it writes: drawn whole";
    EXPECT_FALSE(raw::barrierFormDraws(0x1, 0x1, 0, true, {3, 6, 1, false, false}))
        << "its fragments read the primitive";
}

// A compositor drawing a no-twin fetch pass over a private target every frame, keeping the target or making a new one
// of the same shape each frame: the move runs once, and the target, or every later one, is plain from then on
uint32_t movesOver(uint32_t frames, bool newTargetEachFrame)
{
    raw::PlainShapes plain;
    const raw::PrivateShape shape{70, 64, 64, 5};
    uint32_t moves = 0;
    Target target;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (!frame || newTargetEachFrame)
            target = Target{4, 1, !plain.holds(shape)};
        if (raw::storageSlotsToPlain(0x1, false, true) & 1 && target.comptags) {
            plain.add(shape);
            target.comptags = false;
            ++moves;
        }
        EXPECT_EQ(raw::storageViewMismatch(4, 1, target), raw::StorageFallback::None) << "frame " << frame;
    }
    return moves;
}

TEST(RawPrivateComp, AStorageTargetMovesOnceEverAndItsShapeIsMadePlainFromThen)
{
    EXPECT_EQ(movesOver(1, false), 1u);
    EXPECT_EQ(movesOver(2, false), 1u) << "the second frame does no move";
    EXPECT_EQ(movesOver(100, false), 1u);
    EXPECT_EQ(movesOver(2, true), 1u) << "the second frame's new target is made plain";
    EXPECT_EQ(movesOver(100, true), 1u);
    raw::PlainShapes plain;
    plain.add({70, 64, 64, 5});
    EXPECT_FALSE(plain.holds({70, 64, 32, 5}));
    EXPECT_FALSE(plain.holds({80, 64, 64, 5}));
}

TEST(RawPrivateComp, TheCheckIsAFlagTestPerPassAtRecordAndTheMoveRunsBeforeTheRawEncode)
{
    const std::string record = SourceText("../raw/runtime/RawMetalCommandBuffer.mm");
    EXPECT_THAT(record, HasSubstr("if (cmd.op == OpPipeline && _recordedPass < _passes.size() && "
                                  "_passes[_recordedPass].compressed)\n        rmNoteStorageTargets(self, "
                                  "_passes[_recordedPass], cmd);"));
    const std::string pass = SourceText("../raw/runtime/RawMetalEncoder.mm");
    EXPECT_THAT(pass, HasSubstr("facts.compressed |= uint8_t(1u << c);"));
    const std::string submit = SourceText("../raw/runtime/RawMetalSubmit.mm");
    EXPECT_THAT(submit, HasSubstr("rmStorageTargetsPlain(cb, range.from, range.to);\n        const bool encoded = "
                                  "encodeRaw(cb, range.from, range.to, &why, &refusedEncoder);"));
    const std::string facade = SourceText("../raw/runtime/RawMetalPrivateComp.mm");
    const std::string note = Body(facade, "static void notePipeline(");
    EXPECT_THAT(note,
                HasSubstr("raw::storageSlotsToPlain(pl->storageBits, twin != nullptr, twinDraws) & pass.compressed"));
    EXPECT_THAT(note, HasSubstr("raw::barrierFormDraws(twin->fetchBits, twin->writeBits, twin->storageBits"));
    EXPECT_THAT(Body(facade, "static void noteRangeDraws("), HasSubstr("false, &shape);"))
        << "a variant made late is judged by its draws' shapes";
    EXPECT_THAT(SourceText("../raw/runtime/RawMetalRenderEncoder.mm"),
                HasSubstr("if (const RMPass &pass = e->_cb->_passes[e->_passAt]; pass.compressed)\n        "
                          "rmNoteStorageDraw(e->_cb, pass, pl, e->_drawnVariant, shape);"))
        << "each draw recorded in a compressed pass, by its shape";
    EXPECT_THAT(SourceText("../raw/runtime/RawMetalParallel.mm"), HasSubstr("cb->_plainForStorage.insert("))
        << "a parallel encoder's sub-encoders' targets";
    EXPECT_THAT(note, HasSubstr("cb->_scanStorageTargets = true;")) << "a variant still being made is looked at later";
    const std::string move = Body(facade, "void rmStorageTargetsPlain(");
    EXPECT_THAT(move, HasSubstr("if (cb->_scanStorageTargets)"));
    EXPECT_THAT(move, HasSubstr("gPlainShapes.add("));
    EXPECT_THAT(move, HasSubstr("rmTexturePlain(t)"));
    EXPECT_THAT(Body(facade, "uint32_t privateCompressedKind("), HasSubstr("gPlainShapes.holds("));
    EXPECT_THAT(SourceText("../raw/runtime/RawMetalEncode.mm"), HasSubstr("tRawEncoding = true;"));
}

}  // namespace
