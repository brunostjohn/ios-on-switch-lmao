// SPDX-License-Identifier: MIT
// Multisampled passes' forms on gm20b::Device: a one-sample colour beside a four-sample depth (target-independent
// rasterisation) and how each state's draws keep Apple's colour rule there, a multisampled clear's reach, and which sample
// counts take a zcull region.
#include "device_forms.h"
#include "TegraGM20BZcullABI.h"
#include "../depth_stencil_state.h"
#include "../icb.h"
#include "../mme.h"
#include <functional>
#include <tuple>

namespace {

constexpr uint32_t kTriangle = 3;

gm20b::DepthStencilDesc depthWrites()
{
    gm20b::DepthStencilDesc d;
    d.depthCompare = 1;  // Less
    d.depthWrite = true;
    return d;
}

// A record of `keyString` whose state writes `ctWrite` to colour 0, its fragment program storing to memory where `stores`
gm20b::RuntimeRecord record(uint64_t key, const char *keyString, uint32_t ctWrite, bool stores)
{
    gm20b::RuntimeRecord r;
    r.keyString = keyString;
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = std::string("fs") + keyString + (stores ? "stores" : "");
    if (stores) {
        constexpr uint64_t kStg = uint64_t(0xeed8) << 48;  // program_stores.h STG, past the group's scheduling word
        memcpy(r.programs[1].image.data() + 80 + 8, &kStg, sizeof kStg);
    }
    r.pipeline.keyHash = key;
    r.blob = {NVB197_SET_CT_WRITE(0), ctWrite};
    return r;
}

gm20b::Pipeline *recordState(gm20b::Device &dev, uint64_t key, const char *keyString, uint32_t ctWrite, bool stores)
{
    std::string why;
    gm20b::Pipeline *p = dev.addRecord(record(key, keyString, ctWrite, stores), false, why);
    EXPECT_NE(p, nullptr) << why;
    return p;
}

// One pass of a one-sample colour beside a four-sample depth, both cleared, `body` encoding into it: what it submitted,
// empty with `why` where it was refused
struct MixedPass {
    gm20b::Device &dev;
    gm20b::Texture colour, zeta;
    gm20b::CommandBuffer *cb = nullptr;
    std::string why;

    explicit MixedPass(gm20b::Device &device) : dev(device)
    {
        EXPECT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
        EXPECT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
    }

    template <class Body> std::vector<Method> run(Body body)
    {
        cb = dev.begin();
        if (!cb) {
            why = "no command buffer";
            return {};
        }
        const size_t first = kernel_model::model.jobs.size();
        gm20b::PassDesc p;
        p.color[0].texture = &colour;
        p.color[0].load = gm20b::ColorAttachment::Clear;
        p.color[0].store = true;
        p.count = 1;
        p.depthStencil = {&zeta, true, false, 1.0f, 0};
        const char *rule = nullptr;
        gm20b::RenderEncoder *e = cb->render(p, &rule);
        if (!e) {
            cb->abandon();
            why = rule ? rule : "no pass";
            return {};
        }
        const bool ok = body(*e, &rule);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            why = rule ? rule : "not committed";
            return {};
        }
        return submitted(first);
    }
};

struct Step {
    char kind;  // 'M' a sample mask, 'C' colour target 0's CT_WRITE, 'D' a draw, 'F' a fetching state's ordered draw
    uint32_t value;
};

// What a mixed pass sends between the mode's enable and its disable, in order
std::vector<Step> modeSteps(const std::vector<Method> &ms)
{
    using namespace gm20b::mme;
    std::vector<Step> out;
    bool on = false;
    for (const Method &x : ms) {
        if (x.subc != gm20b::SUBC_3D)
            continue;
        if (x.method == NVB197_SET_TIR)
            on = x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M;
        if (!on)
            continue;
        if (x.method == NVB197_SET_SAMPLE_MASK_X0_Y0)
            out.push_back({'M', x.value});
        else if (x.method == NVB197_SET_CT_WRITE(0))
            out.push_back({'C', x.value});
        else if (x.method >= kMacroCall && (x.method - kMacroCall) % 8 == 0) {
            const uint32_t slot = (x.method - kMacroCall) / 8;
            if (slot == kFetchDraw || slot == kFetchDrawIndexed)
                out.push_back({'F', 0});
            else if (countedAsDraw(slot) || slot == kIcbDraw || slot == kIcbDrawIndexed || slot == kAmplify ||
                     slot == kAmplifyIndexed)
                out.push_back({'D', 0});
        }
    }
    return out;
}

std::string stepText(const Step &s)
{
    if (s.kind != 'M' && s.kind != 'C')
        return std::string(1, s.kind);
    char text[16];
    snprintf(text, sizeof text, "%c%x", s.kind, s.value);
    return text;
}

// The sample masks and draws, as "M1111 D Meeee D"
std::string masksAndDraws(const std::vector<Step> &steps)
{
    std::string out;
    for (const Step &s : steps)
        if (s.kind != 'C')
            out += (out.empty() ? "" : " ") + stepText(s);
    return out;
}

// The colour writes from samples 1-3's mask through the draw after it to the colour write after that, as "C0 D C1111"
std::string colourWritesAroundLaterSamples(const std::vector<Step> &steps)
{
    std::string out;
    auto at = std::find_if(steps.begin(), steps.end(), [](const Step &s) {
        return s.kind == 'M' && s.value == gm20b::kLaterSamplesMask;
    });
    bool drawn = false;
    for (at = at == steps.end() ? at : at + 1; at != steps.end() && at->kind != 'M'; ++at) {
        if (at->kind != 'C' && drawn)
            break;
        out += (out.empty() ? "" : " ") + stepText(*at);
        if (at->kind == 'C' && drawn)
            break;
        drawn |= at->kind != 'C';
    }
    return out;
}

// A one-sample colour beside a four-sample depth: the depth cleared under the zeta's own layout (SET_ANTI_ALIAS 2X2), then
// target-independent rasterisation on (the colour's 1X1, raster 2X2, no modulation), and off again at the pass's end
TEST_F(DevicePassForms, AOneSampleColourBesideAFourSampleDepthRasterisesAtTheDepthsSamples)
{
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    EXPECT_EQ(addressOf(ms, NVB197_SET_ZT_A), zeta.va);
    EXPECT_EQ(addressOf(ms, NVB197_SET_COLOR_TARGET_A(0)), colour.va);
    uint32_t antiAlias = ~0u, raster = ~0u, modulation = ~0u;
    bool zetaCleared = false, on = false;
    for (const Method &x : ms) {
        if (x.subc != gm20b::SUBC_3D)
            continue;
        if (x.method == NVB197_SET_ANTI_ALIAS)
            antiAlias = x.value;
        else if (x.method == NVB197_SET_ANTI_ALIAS_RASTER)
            raster = x.value;
        else if (x.method == NVB197_SET_TIR_MODULATION)
            modulation = x.value;
        else if (x.method == NVB197_CLEAR_SURFACE && (x.value & 1) && !on) {
            EXPECT_EQ(antiAlias, uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_2X2)) << "the zeta's own layout";
            zetaCleared = true;
        } else if (x.method == NVB197_SET_TIR && x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M && !on) {
            on = true;
            EXPECT_TRUE(zetaCleared) << "the depth is cleared before the mode is on";
            EXPECT_EQ(antiAlias, uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_1X1));
            EXPECT_EQ(raster, uint32_t(NVB197_SET_ANTI_ALIAS_RASTER_SAMPLES_MODE_2X2));
            EXPECT_EQ(modulation, uint32_t(NVB197_SET_TIR_MODULATION_COMPONENT_SELECT_NO_MODULATION));
        }
    }
    EXPECT_TRUE(on);
    EXPECT_EQ(lastOf(ms, NVB197_SET_TIR), uint32_t(NVB197_SET_TIR_MODE_DISABLED));
    EXPECT_EQ(lastOf(ms, NVB197_SET_ANTI_ALIAS_RASTER), uint32_t(NVB197_SET_ANTI_ALIAS_RASTER_SAMPLES_MODE_2X2));
    EXPECT_EQ(lastOf(ms, NVB197_SET_ANTI_ALIAS), uint32_t(NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_1X1));
}

// The colour of a mixed pass cleared over a loaded depth (gpu.gm20b.mixed_samples' pass) or a cleared one: its
// CLEAR_SURFACE comes with the zeta deselected and the mode off (the device: class error 0x124,
// INVALID_TIR_COMBINATION02, at that clear under the mode); the zeta then back, the mode on after every clear
TEST_F(DevicePassForms, AMixedPassClearsItsColourBeforeTheModeWithTheZetaDeselected)
{
    for (const bool depthCleared : {false, true}) {
        SCOPED_TRACE(depthCleared ? "depth cleared" : "depth loaded");
        gm20b::Texture colour, zeta;
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
        ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.count = 1;
        p.depthStencil = {&zeta, depthCleared, false, 0.25f, 0};
        std::string why;
        const auto ms = pass(p, &why);
        ASSERT_FALSE(ms.empty()) << why;
        uint32_t zetaSelected = ~0u;
        bool on = false, colourCleared = false;
        for (const Method &x : ms) {
            if (x.subc != gm20b::SUBC_3D)
                continue;
            if (x.method == NVB197_SET_ZT_SELECT) {
                zetaSelected = x.value;
            } else if (x.method == NVB197_SET_TIR && x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M) {
                on = true;
                EXPECT_EQ(zetaSelected, 1u) << "the zeta selected again for the draws";
            } else if (x.method == NVB197_CLEAR_SURFACE && (x.value & 0x3c)) {
                colourCleared = true;
                EXPECT_FALSE(on) << "a colour clear under the mode";
                EXPECT_EQ(zetaSelected, 0u) << "a colour clear beside the four-sample zeta";
                EXPECT_EQ(x.value & 3, 0u) << "the zeta cleared with the colour";
            } else if (x.method == NVB197_CLEAR_SURFACE) {
                EXPECT_FALSE(on) << "a zeta clear under the mode";
                EXPECT_EQ(zetaSelected, 1u);
            }
        }
        EXPECT_TRUE(colourCleared);
        EXPECT_TRUE(on);
    }
}

// The mode is on with REDUCE_COVERAGE (a colour pixel where any raster sample passes) for every state: disabling it under
// RASTER_N_TARGET_M is INVALID_TIR_COMBINATION05
TEST_F(DevicePassForms, AMixedPassKeepsReduceCoverageOnForEveryState)
{
    gm20b::Pipeline *one = state(0xb1, "samples=1 c0=70 ds=252/0 "), *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(one, nullptr);
    ASSERT_NE(four, nullptr);
    MixedPass m(dev);
    const auto ms = m.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(four);
        if (!e.draw(kTriangleStrip, 0, 4, 1, 0, refused))
            return false;
        e.setPipeline(one);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(ms.empty()) << m.why;
    const std::vector<uint32_t> controls = values(ms, NVB197_SET_TIR_CONTROL);
    ASSERT_GE(controls.size(), 1u);
    for (const uint32_t c : controls)
        EXPECT_EQ(c, uint32_t(NVB197_SET_TIR_CONTROL_REDUCE_COVERAGE_ENABLE << 1));
}

// A four-sample state writing colour and depth keeps Apple's colour of sample 0 by two draws: sample 0's, then samples
// 1-3's with colour writes off (the colour targets' CT_WRITE back after). A one-sample state after it draws once over
// every sample.
TEST_F(DevicePassForms, AFourSampleStateWritingDepthDrawsSampleZeroThenTheRestWithoutColour)
{
    gm20b::Pipeline *one = state(0xb1, "samples=1 c0=70 ds=252/0 "), *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(one, nullptr);
    ASSERT_NE(four, nullptr);
    const gm20b::DepthStencilState writes(depthWrites());
    MixedPass m(dev);
    const auto ms = m.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(four);
        e.setDepthStencil(&writes);
        if (!e.draw(kTriangleStrip, 0, 4, 1, 0, refused))
            return false;
        e.setPipeline(one);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(ms.empty()) << m.why;
    const std::vector<Step> steps = modeSteps(ms);
    EXPECT_EQ(masksAndDraws(steps), "M1111 D Meeee D Mffff D");
    EXPECT_EQ(colourWritesAroundLaterSamples(steps), "C0 D C1111");
}

// A four-sample state that writes no depth or stencil leaves samples 1-3 as they were: sample 0's draw alone. One that
// writes no colour draws every sample at once.
TEST_F(DevicePassForms, AFourSampleStateWritingNoDepthOrNoColourDrawsOnce)
{
    gm20b::Pipeline *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(four, nullptr);
    MixedPass noDepth(dev);
    const auto ms = noDepth.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(four);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(ms.empty()) << noDepth.why;
    EXPECT_EQ(masksAndDraws(modeSteps(ms)), "M1111 D");

    gm20b::Pipeline *depthOnly = recordState(dev, 0xb5, "samples=4 c0=70 ds=252/0 ", 0, false);
    ASSERT_NE(depthOnly, nullptr);
    const gm20b::DepthStencilState writes(depthWrites());
    MixedPass noColour(dev);
    const auto prepass = noColour.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(depthOnly);
        e.setDepthStencil(&writes);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(prepass.empty()) << noColour.why;
    const std::string drawn = masksAndDraws(modeSteps(prepass));
    EXPECT_TRUE(drawn == "D" || drawn == "Mffff D") << drawn;
}

// A four-sample state whose fragment program writes memory and which writes colour and depth has no draw of samples 1-3
// that runs those writes once: skipped and counted. Writing no depth, it is drawn once at sample 0.
TEST_F(DevicePassForms, AFourSampleStateThatStoresIsNeverRunTwice)
{
    gm20b::Pipeline *stores = recordState(dev, 0xb6, "samples=4 c0=70 ds=252/0 ", 0x1111, true);
    ASSERT_NE(stores, nullptr);
    ASSERT_TRUE(stores->writesMemory[1]);
    const gm20b::DepthStencilState writes(depthWrites());
    MixedPass split(dev);
    gm20b::Refusal skipped;
    const auto ms = split.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(stores);
        e.setDepthStencil(&writes);
        const bool done = e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
        skipped = split.cb->skipped;
        return done;
    });
    ASSERT_FALSE(ms.empty()) << split.why;
    EXPECT_EQ(skipped.id, gm20b::RefSamples);
    EXPECT_EQ(masksAndDraws(modeSteps(ms)).find('D'), std::string::npos);

    MixedPass once(dev);
    const auto drawn = once.run([&](gm20b::RenderEncoder &e, const char **refused) {
        e.setPipeline(stores);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(drawn.empty()) << once.why;
    EXPECT_EQ(masksAndDraws(modeSteps(drawn)), "M1111 D");
}

// Every way a draw reaches the GPU splits the same: indexed, indirect, indexed indirect, an indirect command buffer's
// commands, and a fetching state's ordered draw, its samples 1-3 drawn whole
TEST_F(DevicePassForms, EveryDrawPathSplitsAFourSampleStateWritingDepth)
{
    gm20b::Pipeline *four = state(0xb4, "samples=4 c0=70 ds=252/0 ");
    ASSERT_NE(four, nullptr);
    gm20b::RuntimeRecord fetching = record(0xb7, "samples=4 c0=70 ds=252/0 ", 0x1111, false);
    gm20b::setEntryCount(fetching.pipeline, 1);
    fetching.pipeline.entry[0] = {0, gm20b::EntryFetch, 2, 0, 0, 0, 0};
    fetching.pipeline.flags = gm20b::PipelineFetches;
    std::string added;
    gm20b::Pipeline *fetches = dev.addRecord(fetching, false, added);
    ASSERT_NE(fetches, nullptr) << added;
    gm20b::Buffer indices, arguments, slots;
    ASSERT_TRUE(dev.createBuffer(256, indices));
    ASSERT_TRUE(dev.createBuffer(256, arguments));
    ASSERT_TRUE(dev.createBuffer(gm20b::icb::kSlotWords * 4, slots));
    const uint16_t strip[4] = {0, 1, 2, 3};
    memcpy(indices.cpu, strip, sizeof strip);
    const uint32_t direct[5] = {4, 1, 0, 0, 0};
    memcpy(arguments.cpu, direct, sizeof direct);
    gm20b::icb::draw(reinterpret_cast<uint32_t *>(slots.cpu), 0, {kTriangleStrip, 4, 1, 0, 0});
    const gm20b::DepthStencilState writes(depthWrites());
    using Draw = std::function<bool(gm20b::RenderEncoder &, const char **)>;
    const std::vector<std::tuple<const char *, gm20b::Pipeline *, Draw, std::string>> paths = {
        {"indexed", four,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.drawIndexed(kTriangleStrip, 4, 2, &indices, 0, 1, 0, 0, why);
         },
         "M1111 D Meeee D"},
        {"indirect", four,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.drawIndirect(kTriangleStrip, &arguments, 0, why);
         },
         "M1111 D Meeee D"},
        {"indexed indirect", four,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.drawIndexedIndirect(kTriangleStrip, 2, &indices, 0, &arguments, 0, why);
         },
         "M1111 D Meeee D"},
        {"commands", four,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.executeSlots(
                 {&slots, 0, 1, gm20b::icb::kSlotWords, nullptr, 0, kTriangleStrip, false, false, nullptr, 0}, why);
         },
         "M1111 D Meeee D"},
        {"ordered fetch", fetches,
         [&](gm20b::RenderEncoder &e, const char **why) {
             return e.draw(kTriangle, 0, 6, 1, 0, why);
         },
         "M1111 F Meeee D"},
    };
    for (const auto &path : paths) {
        SCOPED_TRACE(std::get<0>(path));
        gm20b::Pipeline *pl = std::get<1>(path);
        const Draw &draw = std::get<2>(path);
        MixedPass m(dev);
        const auto ms = m.run([&](gm20b::RenderEncoder &e, const char **refused) {
            e.setPipeline(pl);
            e.setDepthStencil(&writes);
            return draw(e, refused);
        });
        ASSERT_FALSE(ms.empty()) << m.why;
        const std::vector<Step> steps = modeSteps(ms);
        EXPECT_EQ(masksAndDraws(steps), std::get<3>(path));
        EXPECT_EQ(colourWritesAroundLaterSamples(steps), "C0 D C1111");
    }
}

constexpr uint32_t kSetActiveZcullRegion = 0x1590;  // 0: the pass's region; 0x3f: none (zcull.h)
// The X1's zcull layout (one GPC, 2048 aliquots), as zcull_test.cpp holds it
constexpr TegraGM20BAbiZcullInfo kX1Zcull = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};

// The device over a kernel that reports zcull
class DeviceZcullPasses : public DevicePassForms {
protected:
    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.otherMethod = [](uint32_t method, void *output) -> kern_return_t {
            if (method != TegraGM20BMethodZcullInfo)
                return kIOReturnUnsupported;
            *static_cast<TegraGM20BAbiZcullInfo *>(output) = kX1Zcull;
            return KERN_SUCCESS;
        };
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    }

    // What one pass over a cleared depth of `zetaSamples` beside a memoryless colour of `samples` sets its active zcull
    // region to
    std::vector<uint32_t> regions(uint32_t samples, uint32_t zetaSamples)
    {
        gm20b::Texture colour, zeta;
        EXPECT_TRUE(dev.createMemoryless(32, 32, kRGBA8, colour, samples));
        EXPECT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, zetaSamples, true, zeta));
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.color[0].store = false;
        p.count = 1;
        p.depthStencil = {&zeta, true, false, 1.0f, 0};
        std::string why;
        const auto ms = pass(p, &why);
        EXPECT_FALSE(ms.empty()) << why;
        return values(ms, kSetActiveZcullRegion);
    }
};

// A pass takes a region at any sample count its colour and depth share, as NVK's begin does (the render area's pixels at
// any sample count); a mixed pass, whose depth has more samples than its colour, takes none
TEST_F(DeviceZcullPasses, APassOfOneSampleCountTakesARegionAndAMixedOneNone)
{
    for (const uint32_t samples : {1u, 4u}) {
        const auto set = regions(samples, samples);
        EXPECT_NE(std::find(set.begin(), set.end(), 0u), set.end()) << samples << " samples";
    }
    const auto mixed = regions(1, 4);
    EXPECT_EQ(std::find(mixed.begin(), mixed.end(), 0u), mixed.end());
    EXPECT_NE(std::find(mixed.begin(), mixed.end(), 0x3fu), mixed.end());
}

// The pass-end orders a mixed pass's probe rows write (mixed_samples.h TirEnd, Device::probeTir; HW Test
// gpu.gm20b.mixed_end_rows): the mode's, raster count's, zeta select's and raster enable's methods after the mode's last
// enable
TEST_F(DevicePassForms, AMixedPassEndsInEachProbeRowsOrder)
{
    if (!SWITCH_METAL_DIAGNOSTICS)
        GTEST_SKIP() << "the probe rows are debug builds'";
    using Tail = std::vector<std::pair<uint32_t, uint32_t>>;
    constexpr uint32_t kTir = NVB197_SET_TIR, kRaster = NVB197_SET_ANTI_ALIAS_RASTER, kZt = NVB197_SET_ZT_SELECT,
                       kEnable = NVB197_SET_RASTER_ENABLE;
    const auto row = [](gm20b::TirEnd end, uint32_t raster = 0) {
        return 1u | uint32_t(end) << 8 | raster << 12;
    };
    const std::vector<std::pair<uint32_t, Tail>> rows = {
        {row(gm20b::TirEnd::Mode), {{kTir, 0}}},
        {row(gm20b::TirEnd::RasterFirst), {{kRaster, 0}, {kTir, 0}}},
        {row(gm20b::TirEnd::ZetaOff), {{kZt, 0}, {kTir, 0}}},
        {row(gm20b::TirEnd::ModeOnly), {{kTir, 0}}},
        {row(gm20b::TirEnd::RasterDisabled), {{kEnable, 0}, {kTir, 0}, {kEnable, 1}}},
        {row(gm20b::TirEnd::Value, 6), {{kTir, 0}, {kRaster, 6}, {kZt, 0}, {kRaster, 0}}},
    };
    for (const auto &[probe, want] : rows) {
        dev.probeTir.store(probe);
        gm20b::Texture colour, zeta;
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
        ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.count = 1;
        p.depthStencil = {&zeta, true, false, 1.0f, 0};
        std::string why;
        const auto ms = pass(p, &why);
        ASSERT_FALSE(ms.empty()) << why;
        Tail got;
        for (const Method &x : ms) {
            if (x.subc != gm20b::SUBC_3D)
                continue;
            if (x.method == kTir && x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M)
                got.clear();
            else if (x.method == kTir || x.method == kRaster || x.method == kZt || x.method == kEnable)
                got.emplace_back(x.method, x.value);
        }
        EXPECT_EQ(got, want) << std::hex << probe;
    }
    dev.probeTir.store(0);
}

TEST_F(DevicePassForms, OtherMixedSampleCountsAreRefused)
{
    gm20b::Texture colour, zeta;
    ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
    ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 2, true, zeta));
    gm20b::PassDesc p;
    p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
    p.count = 1;
    p.depthStencil = {&zeta, true, false, 1.0f, 0};
    std::string why;
    EXPECT_TRUE(pass(p, &why).empty());
    EXPECT_NE(why.find("attachments of different sample counts"), std::string::npos) << why;
}

// A multisampled colour cleared over a render area smaller than it, neither stored nor memory of its own: the clear reaches
// the area rounded up to whole 32-pixel tiles, which its resolve reads (an M4 Max resolves a 64x64 Clear of a 40x24 area
// over [0, 64) x [0, 32), the clear's past the area: hwtests-r4 gpu.gm20b.resolve_codes_parts)
// A mixed pass's zcull probe rows (mixed_samples.h, Device::probeTir; HW Test gpu.gm20b.zc_mixed): a row asking for
// zcull takes a region, over the zeta's 2x2 sample grid where it asks, its words before the clears or after the mode's
// enable, and a row may leave the mode disabled after it
TEST_F(DeviceZcullPasses, AMixedPassTakesTheZcullItsProbeRowAsks)
{
    if (!SWITCH_METAL_DIAGNOSTICS)
        GTEST_SKIP() << "the probe rows are debug builds'";
    constexpr uint32_t kRegionSizeA = 0x07c0;
    const auto run = [&](uint32_t probe) {
        dev.probeTir.store(probe);
        gm20b::Texture colour, zeta;
        EXPECT_TRUE(dev.createMemoryless(32, 32, kRGBA8, colour, 1));
        EXPECT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 4, true, zeta));
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Clear);
        p.color[0].store = false;
        p.count = 1;
        p.depthStencil = {&zeta, true, false, 1.0f, 0};
        std::string why;
        const auto ms = pass(p, &why);
        EXPECT_FALSE(ms.empty()) << why;
        dev.probeTir.store(0);
        return ms;
    };

    // where the region (SET_ACTIVE_ZCULL_REGION 0) and the mode's enable fall, the region's width, and how often the
    // mode is disabled after its enable
    struct Seen {
        long region = -1, enable = -1;
        uint32_t width = 0, disables = 0;
    };

    const auto seen = [](const std::vector<Method> &ms) {
        Seen s;
        for (size_t i = 0; i < ms.size(); i++) {
            const Method &x = ms[i];
            if (x.subc != gm20b::SUBC_3D)
                continue;
            if (x.method == kSetActiveZcullRegion && x.value == 0 && s.region < 0)
                s.region = long(i);
            else if (x.method == kRegionSizeA && !s.width)
                s.width = x.value;
            else if (x.method == NVB197_SET_TIR && x.value == NVB197_SET_TIR_MODE_RASTER_N_TARGET_M && s.enable < 0)
                s.enable = long(i);
            s.disables += x.method == NVB197_SET_TIR && x.value == NVB197_SET_TIR_MODE_DISABLED && s.enable >= 0;
        }
        return s;
    };
    const Seen armed = seen(run(1));
    EXPECT_LT(armed.region, 0) << "an armed row without kTirProbeZcull takes none";
    EXPECT_EQ(armed.disables, 1u) << "the pass end's";
    const Seen before = seen(run(1 | gm20b::kTirProbeZcull));
    ASSERT_GE(before.region, 0);
    EXPECT_LT(before.region, before.enable);
    const Seen grid = seen(run(1 | gm20b::kTirProbeZcull | gm20b::kTirProbeSampleGrid));
    EXPECT_EQ(grid.width, 2 * before.width);
    const Seen after = seen(run(1 | gm20b::kTirProbeZcull | gm20b::kTirProbeZcullAfter));
    ASSERT_GE(after.region, 0);
    EXPECT_GT(after.region, after.enable);
    EXPECT_EQ(seen(run(1 | gm20b::kTirProbeModeOff)).disables, 2u) << "after its enable, and the pass end's";
}

TEST_F(DevicePassForms, AMultisampledClearReachesTheAreasWholeTiles)
{
    gm20b::Texture t;
    ASSERT_TRUE(dev.createMemoryless(64, 64, kRGBA8, t, 4));
    gm20b::PassDesc p;
    p.color[0] = kept(t, gm20b::ColorAttachment::Clear);
    p.color[0].store = false;
    p.count = 1;
    p.width = 40;
    p.height = 24;
    p.drawn = true;
    std::string why;
    const auto ms = pass(p, &why);
    ASSERT_FALSE(ms.empty()) << why;
    const auto h = values(ms, NVB197_SET_CLEAR_RECT_HORIZONTAL), v = values(ms, NVB197_SET_CLEAR_RECT_VERTICAL);
    EXPECT_NE(std::find(h.begin(), h.end(), 64u << 16), h.end());
    EXPECT_NE(std::find(v.begin(), v.end(), 32u << 16), v.end());
}

}  // namespace
