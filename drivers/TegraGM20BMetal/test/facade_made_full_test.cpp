// SPDX-License-Identifier: MIT
// A render state the shader compiler made gets what a pack build gives a state of its own, its fragment program's split and
// its program variants: its full form (RequestForm::Full) asked once its primary record is placed, and attached to its
// pipeline while it may be drawing (Device::attachFullForm), or at its creation where the store's primary entry already
// is that full record. A full form of another layout than the placed record attaches nothing.
#include "facade_variant_compiler.h"
#include "bazel/testing/fresh_process.h"

#include <chrono>
#include <thread>

namespace {

using namespace harness;

constexpr uint32_t kResidualWord = 16;

id<MTLLibrary> served(RenderCompiler &compiler, const std::string &name, id<MTLDevice> __strong &device)
{
    const std::string directory = dir() + "/" + name + "-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    if (!compiler.serve(directory))
        return nil;
    NSString *why = nil;
    device = metal_device_open(writePack(packKey(MTLPixelFormatRGBA16Float)).c_str(), 0, 0, &why);
    if (!device)
        return nil;
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    return [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
}

// A fragment uniform buffer read through residual push word kResidualWord: what a program variant assumes is 0
void residualUniform(gm20b::RuntimeRecord &r)
{
    gm20b::PackPipeline &pp = r.pipeline;
    gm20b::setEntryCount(pp, 3);
    pp.entry[2] = {32, gm20b::EntryUbo, 2, 1, uint8_t((kResidualWord + 1) << 2), 0, 0};
    pp.setBytes = 48;
    pp.memoryEntries |= 4;
    pp.pushMask |= 1u << kResidualWord;
    pp.uniform[0] = {2, 1, 0, 0};
}

// What the full form adds: the fragment program's aligned variant and its split (split_record_test.cpp's batch split)
void fullForm(gm20b::RuntimeRecord &r)
{
    gm20b::CapturedProgram aligned = r.programs[1];
    aligned.image[84] = 1;
    aligned.id = "fs-aligned";
    r.variants.push_back({gm20b::AssumeUniformAligned, aligned});
    r.pipeline.variantCount[1] = 1;
    r.splitTried = r.split = true;
    gm20b::PackSplit &s = r.splitRecord;
    s.frontierBytes = 64;
    s.frontierCbuf = 2;
    s.prologueCbufs = 1;
    s.batchVersion = 1;
    s.batchThreads = 32;
    s.qmd[18] = 32u << 16;
    s.qmd[19] = 0x10001;
    r.splitPrograms[0] = r.programs[1];
    r.splitPrograms[0].image[85] = 1;
    r.splitPrograms[0].id = "fs-split";
    r.splitPrograms[0].cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet, gm20b::CbufFrontier};
    r.splitPrograms[1].id = "prologue";
    r.splitPrograms[1].image.assign(80 + 64, 0);
}

// A primary record as compilerd makes one: its split not tried
void primaryForm(gm20b::RuntimeRecord &r)
{
    r.splitTried = false;
}

gm20b::Pipeline *made(RenderCompiler &compiler, const char *name, id<MTLDevice> __strong &device,
                      id<MTLRenderPipelineState> __strong &state, std::string &why)
{
    id<MTLLibrary> library = served(compiler, name, device);
    if (!library)
        return why = "no device or library", nullptr;
    NSError *error = nil;
    state = [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    if (!state)
        return why = error.localizedDescription.UTF8String ?: "no state", nullptr;
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    return pipeline->_raw;
}

// What draws of `pl` read of its variants and split while its full form may be landing (RenderEncoder::prepare,
// splitTwin), read as they read them, through what they load, until both are there or 2 s pass: the pipeline an aligned
// draw takes
gm20b::Pipeline *drawnUntilFull(gm20b::Pipeline *pl)
{
    gm20b::Device &dev = *gRM->raw;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        gm20b::Pipeline *chosen = dev.variant(*pl, gm20b::AssumeUniformAligned);
        const gm20b::SplitSource *source = __atomic_load_n(&pl->splitSource, __ATOMIC_ACQUIRE);
        const bool full = chosen != pl && chosen->rec == pl->rec && source && source->pp == pl->rec;
        if (full || std::chrono::steady_clock::now() >= until)
            return chosen;
        std::this_thread::yield();
    }
}

std::vector<gm20b::RequestForm> forms(RenderCompiler &compiler)
{
    std::vector<gm20b::RequestForm> out;
    for (const gm20b::CompileRequest &r : compiler.captured())
        out.push_back(r.form);
    return out;
}

void aFullFormAfterPlacementGivesItsVariantsAndSplit()
{
    gm20b::RuntimeRecord full;
    RenderCompiler compiler;
    compiler.shapes = [&full](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        residualUniform(record);
        if (request.form != gm20b::RequestForm::Full)
            return primaryForm(record);
        fullForm(record);
        full = record;
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = made(compiler, "late-full", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    gm20b::Pipeline *aligned = drawnUntilFull(pl);
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler),
              (std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary, gm20b::RequestForm::Full}));
    const gm20b::VariantTable *table = __atomic_load_n(&pl->variantTable, __ATOMIC_ACQUIRE);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->chosen.size(), 2u);
    EXPECT_NE(aligned, pl);
    EXPECT_EQ(aligned, table->chosen[0]);
    EXPECT_EQ(aligned->rec, pl->rec);
    gm20b::Device &dev = *gRM->raw;
    EXPECT_EQ(dev.variant(*pl, 0), pl);
    const gm20b::SplitSource *source = __atomic_load_n(&pl->splitSource, __ATOMIC_ACQUIRE);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->pp, pl->rec);
    // a second full form of the state gives it nothing again: a draw may hold the split and variants it loaded
    full.pipeline.keyHash = pl->rec->keyHash;
    EXPECT_TRUE(dev.attachFullForm(full));
    EXPECT_EQ(__atomic_load_n(&pl->splitSource, __ATOMIC_ACQUIRE), source);
    EXPECT_EQ(__atomic_load_n(&pl->variantTable, __ATOMIC_ACQUIRE), table);
}

void aStoredFullRecordGivesBothAtCreation()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &, gm20b::RuntimeRecord &record) {
        residualUniform(record);
        fullForm(record);
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = made(compiler, "stored-full", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    const gm20b::VariantTable *table = __atomic_load_n(&pl->variantTable, __ATOMIC_ACQUIRE);
    ASSERT_NE(table, nullptr);
    EXPECT_NE(gRM->raw->variant(*pl, gm20b::AssumeUniformAligned), pl);
    EXPECT_NE(__atomic_load_n(&pl->splitSource, __ATOMIC_ACQUIRE), nullptr);
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(2) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler), std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary});
}

void aFullFormOfAnotherLayoutAttachesNothing()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        residualUniform(record);
        if (request.form != gm20b::RequestForm::Full)
            return primaryForm(record);
        fullForm(record);
        record.blob = {0x1a00, 0x2222};
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = made(compiler, "other-layout", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_TRUE(compiler.wait(2));
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler),
              (std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary, gm20b::RequestForm::Full}));
    EXPECT_EQ(__atomic_load_n(&pl->variantTable, __ATOMIC_ACQUIRE), nullptr);
    EXPECT_EQ(__atomic_load_n(&pl->splitSource, __ATOMIC_ACQUIRE), nullptr);
}

// A device-made state's turned draws take its turned variant (TurnedSource), never a program variant made unturned
void aTurnedDrawOfADeviceMadeStateKeepsItsOwnPipeline()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &, gm20b::RuntimeRecord &record) {
        residualUniform(record);
        fullForm(record);
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = made(compiler, "turned", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    gm20b::Device &dev = *gRM->raw;
    ASSERT_NE(dev.variant(*pl, gm20b::AssumeUniformAligned), pl);
    EXPECT_EQ(dev.variant(*pl, gm20b::AssumeUniformAligned | gm20b::AssumeTurned), pl);
}

// The forms the device asked of a state whose primary record `shapes` makes, once every background ask settled
std::vector<gm20b::RequestForm> askedOf(const char *name, std::function<void(gm20b::RuntimeRecord &)> shapes)
{
    RenderCompiler compiler;
    compiler.shapes = [shapes](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        if (request.form != gm20b::RequestForm::Full)
            shapes(record);
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    EXPECT_NE(made(compiler, name, device, state, why), nullptr) << why;
    EXPECT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    return forms(compiler);
}

void aRecordWhoseSplitWasTriedAndOfNoResidualAsksNoFullForm()
{
    EXPECT_EQ(askedOf("tried", [](gm20b::RuntimeRecord &) {}),
              std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary});
}

void aRecordWhoseSplitWasNotTriedAsksItsFullForm()
{
    EXPECT_EQ(askedOf("untried", primaryForm),
              (std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary, gm20b::RequestForm::Full}));
}

void aRecordOfAResidualUniformAsksItsFullForm()
{
    EXPECT_EQ(askedOf("residual", residualUniform),
              (std::vector<gm20b::RequestForm>{gm20b::RequestForm::Primary, gm20b::RequestForm::Full}));
}

}  // namespace

TEST(GM20BFacadeMadeFull, ARecordWhoseSplitWasTriedAndOfNoResidualAsksNoFullForm)
{
    switch_ios::test::ExpectPassesInFreshProcess(aRecordWhoseSplitWasTriedAndOfNoResidualAsksNoFullForm);
}

TEST(GM20BFacadeMadeFull, ARecordWhoseSplitWasNotTriedAsksItsFullForm)
{
    switch_ios::test::ExpectPassesInFreshProcess(aRecordWhoseSplitWasNotTriedAsksItsFullForm);
}

TEST(GM20BFacadeMadeFull, ARecordOfAResidualUniformAsksItsFullForm)
{
    switch_ios::test::ExpectPassesInFreshProcess(aRecordOfAResidualUniformAsksItsFullForm);
}

TEST(GM20BFacadeMadeFull, AFullFormAfterPlacementGivesItsVariantsAndSplit)
{
    switch_ios::test::ExpectPassesInFreshProcess(aFullFormAfterPlacementGivesItsVariantsAndSplit);
}

TEST(GM20BFacadeMadeFull, AStoredFullRecordGivesBothAtCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess(aStoredFullRecordGivesBothAtCreation);
}

TEST(GM20BFacadeMadeFull, AFullFormOfAnotherLayoutAttachesNothing)
{
    switch_ios::test::ExpectPassesInFreshProcess(aFullFormOfAnotherLayoutAttachesNothing);
}

TEST(GM20BFacadeMadeFull, ATurnedDrawOfADeviceMadeStateKeepsItsOwnPipeline)
{
    switch_ios::test::ExpectPassesInFreshProcess(aTurnedDrawOfADeviceMadeStateKeepsItsOwnPipeline);
}
