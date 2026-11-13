// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.h: which private textures bind compressed memory, and of which kind.
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_private_comp.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_storage_form.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// An attachment as the storage view sees it
struct Target {
    uint32_t bpp = 4, samples = 1;
    bool comptags = false;
};

// What a draw of a state with `storageBits` (and a barrier form or not) takes over `t`: its storage form ("raw"), its
// barrier form ("twin"), or the refusal that sends it to m2v ("enc.storage"), once the plain move ran before the encode
std::string route(uint8_t storageBits, bool twin, Target &t)
{
    if (raw::storageSlotsToPlain(storageBits, twin, raw::storageReadsCompressed()) & 1)
        t.comptags = false;
    const auto reason = raw::storageViewMismatch(4, 1, t);
    if (reason == raw::StorageFallback::None)
        return "raw";
    return twin ? "twin" : "enc.storage";
}

raw::PrivateTextureFacts target(uint32_t bpp)
{
    raw::PrivateTextureFacts f;
    f.privateStorage = f.renderedAndSampledOnly = f.plainColour = true;
    f.shape = raw::PrivateShape2D::Plain;
    f.compression = true;
    f.bpp = bpp;
    return f;
}

TEST(RawPrivateComp, TargetsTakeTheColourKindsTheCopyEngineReads)
{
    EXPECT_EQ(raw::privateCompressedKind(target(4)), uint32_t(raw::kCompKindC32));
    EXPECT_EQ(raw::privateCompressedKind(target(8)), uint32_t(raw::kCompKindC64));
    EXPECT_EQ(raw::privateCompressedKind(target(16)), uint32_t(raw::kCompKindC128)) << "RGBA32Float";
    EXPECT_EQ(raw::privateCompressedKind(target(2)), 0u);
}

TEST(RawPrivateComp, AnythingElseStaysPlain)
{
    bool raw::PrivateTextureFacts::*fields[] = {
        &raw::PrivateTextureFacts::privateStorage,
        &raw::PrivateTextureFacts::renderedAndSampledOnly,
        &raw::PrivateTextureFacts::plainColour,
        &raw::PrivateTextureFacts::compression,
    };
    for (auto field : fields) {
        raw::PrivateTextureFacts f = target(4);
        f.*field = false;
        EXPECT_EQ(raw::privateCompressedKind(f), 0u);
    }
    raw::PrivateTextureFacts other = target(4);
    other.shape = raw::PrivateShape2D::Other;
    EXPECT_EQ(raw::privateCompressedKind(other), 0u) << "a cube, a 3D or a 1D texture";
}

// The copy engine moves a multisampled texture as fx::msLayout's grid of its samples
TEST(RawPrivateComp, AMultisampledTextureMovesAsItsSampleGrid)
{
    uint32_t sx = 9, sy = 9;
    raw::sampleGrid(1, sx, sy);
    EXPECT_EQ(sx, 0u);
    EXPECT_EQ(sy, 0u);
    raw::sampleGrid(2, sx, sy);
    EXPECT_EQ(sx, 1u);
    EXPECT_EQ(sy, 0u);
    raw::sampleGrid(4, sx, sy);
    EXPECT_EQ(sx, 1u);
    EXPECT_EQ(sy, 1u);
}

TEST(RawPrivateComp, EveryLaidOutShapeMovesWholeAndKeepsWhatWorkSubmittedReads)
{
    const std::string source = SourceText("../raw/runtime/raw_private_comp.cpp");
    const std::string move = Body(source, "bool Device::plainFromCompressed(");
    EXPECT_THAT(move, HasSubstr("createLaidOut(ask, plain, nullptr)")) << "a chain, array or multisampled texture";
    EXPECT_THAT(move, HasSubstr("copyImages(*b, t, plain);"));
    EXPECT_THAT(move, HasSubstr("p.staleChain = t.chain;"));
    const std::string copy = Body(source, "void copyImages(");
    EXPECT_THAT(copy, HasSubstr("b.copyTexels(from, 0, 0, 0, to, 0, 0, 0, from.width, from.height);")) << "level 0";
    EXPECT_THAT(copy, HasSubstr("chainLevelImage(from, level, a)")) << "every later level";
    EXPECT_THAT(copy, HasSubstr("layer < std::max(from.layers, 1u)")) << "every layer";
    EXPECT_THAT(copy, HasSubstr("sampleGrid(from.samples, sx, sy)")) << "every sample";
    EXPECT_THAT(Body(source, "void Device::releasePrivateCompressed("), HasSubstr("delete stale;"));
    const std::string laid = Body(SourceText("../raw/runtime/raw_gpu.cpp"), "bool Device::createLaidOut(");
    EXPECT_THAT(laid, HasSubstr("ask.compressKind && !at && !zeta && takeCompressed(bytes, ask.compressKind, comp)"));
    EXPECT_THAT(laid, HasSubstr("noteCompressed(ask, comp, bytes, t);"));
}

TEST(RawPrivateComp, AFetchingStateWithNoBarrierFormDrawsItsCompressedTargetOnTheRawPath)
{
    // gpu.raw.fetch_blend_order strip16 (rgba8, 64x64 private RenderTarget|ShaderRead): boot 11's sto=compressed,notwin,
    // which rule storage.compressed now draws in place
    Target strip{4, 1, true};
    EXPECT_EQ(raw::storageViewMismatch(4, 1, strip), raw::StorageFallback::None);
    EXPECT_EQ(route(0x1, false, strip), "raw");
    EXPECT_TRUE(strip.comptags) << "no move to plain memory";
    // gpu.raw.pass_fusion private_color_adjacent: a clear pass and a load pass on one target, the fetch in both
    Target adjacent{4, 1, true};
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
}

TEST(RawPrivateComp, AStateWithABarrierFormKeepsTheCompressedTarget)
{
    Target t{4, 1, true};
    EXPECT_EQ(raw::storageSlotsToPlain(0x3, true, false), 0);
    EXPECT_EQ(route(0x1, true, t), "raw") << "rule storage.compressed: its storage form, in place";
    EXPECT_TRUE(t.comptags);
    EXPECT_EQ(raw::storageSlotsToPlain(0x6, false, false), 0x6);
}

// gpu.raw.storage_compressed's storage arm turns storage views of compressed memory on (switchRawProbeStorageCompressed:):
// its no-twin fetching draw must keep the compressed target it judges, not move it to plain memory first
TEST(RawPrivateComp, StorageViewsOfCompressedMemoryMoveNothing)
{
    EXPECT_EQ(raw::storageSlotsToPlain(0x3, false, true), 0);
    Target t{4, 1, true};
    EXPECT_EQ(raw::storageViewMismatch(4, 1, t, true), raw::StorageFallback::None);
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
        if (raw::storageSlotsToPlain(0x1, false, false) & 1 && target.comptags) {
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
    EXPECT_THAT(note, HasSubstr("raw::storageSlotsToPlain(pl->storageBits, pl->barrierTwin != nullptr, "
                                "gRM->raw->storageCompressedOk()) &\n        pass.compressed"));
    EXPECT_THAT(note, HasSubstr("cb->_scanStorageTargets = true;")) << "a variant still being made is looked at later";
    const std::string move = Body(facade, "void rmStorageTargetsPlain(");
    EXPECT_THAT(move, HasSubstr("if (cb->_scanStorageTargets)"));
    EXPECT_THAT(move, HasSubstr("gPlainShapes.add("));
    EXPECT_THAT(move, HasSubstr("rmTexturePlain(t)"));
    EXPECT_THAT(Body(facade, "uint32_t privateCompressedKind("), HasSubstr("gPlainShapes.holds("));
    EXPECT_THAT(SourceText("../raw/runtime/RawMetalEncode.mm"), HasSubstr("tRawEncoding = true;"));
}

TEST(RawPrivateComp, AStoredZetaTakesCompressedMemoryInEveryBuildAndKeepsItsTwinMove)
{
    const std::string zeta = Body(SourceText("../raw/runtime/raw_multisample.cpp"), "bool Device::createOwnedZeta(");
    EXPECT_THAT(zeta, HasSubstr("storedZetaKind("));
    EXPECT_THAT(zeta, HasSubstr("createCompressedZeta(bytes, kind, sampled, t)"));
    EXPECT_THAT(zeta, Not(HasSubstr("#if"))) << "the compressed route is the release one, not a probe's";
    const std::string move = Body(SourceText("../raw/runtime/raw_multisample.cpp"), "bool Device::moveOwnedZeta(");
    EXPECT_THAT(move, HasSubstr("compZeta_.count(t.va)")) << "a compressed zeta still moves into its m2v twin's image";
    const std::string made =
        Body(SourceText("../raw/runtime/raw_private_comp.cpp"), "bool Device::createCompressedZeta(");
    EXPECT_THAT(made, HasSubstr("takeCompressed(bytes, kind, bo)")) << "memory made off the render thread, as colour's";
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_private_comp.cpp"), "bool Device::takeCompressed("),
                HasSubstr("comp_.fit(need, kind)"));
    EXPECT_THAT(made, Not(HasSubstr("allocCompressible")));
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_gpu.h"), "    void freeOwnedZeta(uint64_t va)"),
                HasSubstr("freeCompressedZeta(va);"));
}

}  // namespace
