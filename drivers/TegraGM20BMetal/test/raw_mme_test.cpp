// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_mme.h"
#include "../raw/pack/mesa/mme_fermi.h"
#include "../raw/pack/mesa/mme_fermi_sim.h"
#include <utility>
#include <vector>

namespace {
using Method = std::pair<uint16_t, uint32_t>;

struct Simulation {
    std::vector<uint32_t> arguments;
    size_t consumed = 0;
    std::vector<Method> methods;

    static uint32_t load(void *opaque)
    {
        auto &s = *static_cast<Simulation *>(opaque);
        EXPECT_LT(s.consumed, s.arguments.size());
        return s.consumed < s.arguments.size() ? s.arguments[s.consumed++] : 0;
    }

    static uint32_t state(void *, uint16_t)
    {
        ADD_FAILURE() << "native draw macro must not read mutable shadow scratch or root state";
        return 0;
    }

    static void emit(void *opaque, uint16_t method, uint32_t value)
    {
        static_cast<Simulation *>(opaque)->methods.emplace_back(method, value);
    }

    static void barrier(void *)
    {
        ADD_FAILURE() << "an instancing macro requires no barrier";
    }

    static uint32_t *dram(void *, uint32_t)
    {
        ADD_FAILURE() << "an instancing macro requires no internal DRAM";
        return nullptr;
    }

    void runEncoded(const uint32_t *encoded, size_t size, std::vector<uint32_t> params)
    {
        arguments = std::move(params);
        consumed = 0;
        std::vector<mme_fermi_inst> decoded(size);
        mme_fermi_decode(decoded.data(), encoded, uint32_t(size));
        std::vector<uint32_t> roundTrip(size);
        mme_fermi_encode(roundTrip.data(), uint32_t(decoded.size()), decoded.data());
        EXPECT_EQ(roundTrip, (std::vector<uint32_t>(encoded, encoded + size)));
        const mme_sim_state_ops ops = {load, state, emit, barrier, dram};
        mme_fermi_sim_core(uint32_t(decoded.size()), decoded.data(), &ops, this);
        EXPECT_EQ(consumed, arguments.size());
    }

    void run(bool indexed, uint32_t first, uint32_t count, uint32_t instances)
    {
        const auto &encoded = indexed ? raw::mme::kIndexedProgram : raw::mme::kDrawProgram;
        runEncoded(encoded.data(), encoded.size(), {instances, first, count});
    }
};

std::vector<Method> expected(bool indexed, uint32_t first, uint32_t count, uint32_t instances)
{
    std::vector<Method> methods;
    const uint16_t start = indexed ? raw::mme::kIndexFirst : raw::mme::kVertexFirst;
    for (uint32_t i = 0; i < instances; ++i) {
        methods.emplace_back(raw::mme::kBegin, i ? 1u << 26 : 0);
        methods.emplace_back(start, first);
        methods.emplace_back(start + 4, count);
        methods.emplace_back(raw::mme::kEnd, 0);
    }
    return methods;
}

TEST(RawMme, ActualEncodedProgramsMatchDirectDrawSemantics)
{
    for (bool indexed : {false, true})
        for (uint32_t instances : {0u, 1u, 2u, 7u, 1000u}) {
            SCOPED_TRACE(::testing::Message() << indexed << ":" << instances);
            Simulation simulation;
            simulation.run(indexed, 0x81234567, 0x12345, instances);
            EXPECT_EQ(simulation.methods, expected(indexed, 0x81234567, 0x12345, instances));
        }
}

TEST(RawMme, MixedDirectAndMacroDrawsResetInstanceIdWithoutTouchingRootState)
{
    Simulation simulation;
    simulation.run(false, 8, 6, 3);
    const auto direct = expected(true, 0, 12, 1);
    simulation.methods.insert(simulation.methods.end(), direct.begin(), direct.end());
    simulation.run(true, 4, 9, 2);
    auto reference = expected(false, 8, 6, 3);
    reference.insert(reference.end(), direct.begin(), direct.end());
    const auto indexed = expected(true, 4, 9, 2);
    reference.insert(reference.end(), indexed.begin(), indexed.end());
    EXPECT_EQ(simulation.methods, reference);
}

TEST(RawMme, CallsHaveConstantCpuSizeAndUseTheCorrectArgumentMethod)
{
    for (bool indexed : {false, true})
        for (uint32_t instances : {2u, 1000u, UINT32_MAX}) {
            std::array<uint32_t, raw::mme::kCallWords> words{};
            raw::Push push{words.data(), words.data() + words.size()};
            raw::mme::draw(push, indexed, 12, 65537, instances);
            EXPECT_EQ(push.at, push.end);
            EXPECT_EQ(words[0], raw::Push::hdr(5, raw::SUBC_3D, 0x3800 + (indexed ? 8 : 0), 3));
            EXPECT_EQ(words[1], instances);
            EXPECT_EQ(words[2], 12u);
            EXPECT_EQ(words[3], 65537u);
        }
}

TEST(RawMme, IndirectArgumentsSetRootAndHardwareBaseParametersBeforeDrawing)
{
    for (bool indexed : {false, true})
        for (uint32_t count : {0u, 19u})
            for (uint32_t instances : {0u, 1u, 5u}) {
                Simulation simulation;
                const auto &code = indexed ? raw::mme::kIndexedIndirectProgram : raw::mme::kIndirectProgram;
                const uint32_t first = 7, baseVertex = uint32_t(-13), baseInstance = 0xf1234567;
                const std::vector<uint32_t> args =
                    indexed ? std::vector<uint32_t>{count, instances, first, baseVertex, baseInstance}
                            : std::vector<uint32_t>{count, instances, first, baseInstance};
                simulation.runEncoded(code.words.data(), code.size, args);
                std::vector<Method> reference = {{0x238c, 0},
                                                 {0x2390, indexed ? baseVertex : first},
                                                 {0x2390, baseInstance},
                                                 {0x1438, baseInstance},
                                                 {0x1434, indexed ? baseVertex : 0},
                                                 {0x1118, indexed ? baseVertex : 0}};
                auto draws = expected(indexed, first, count, count ? instances : 0);
                reference.insert(reference.end(), draws.begin(), draws.end());
                EXPECT_EQ(simulation.methods, reference);
            }
}

TEST(RawMme, IndirectHeaderDeclaresExactlyTheMetalArgumentWords)
{
    for (bool indexed : {false, true}) {
        uint32_t word;
        raw::Push push{&word, &word + 1};
        raw::mme::indirectHeader(push, indexed);
        EXPECT_EQ(push.at, push.end);
        EXPECT_EQ(word, raw::Push::hdr(5, raw::SUBC_3D, 0x3800 + (indexed ? 24 : 16), indexed ? 5 : 4));
    }
}

TEST(RawMme, ContextReservationCoversEveryProgram)
{
    std::array<uint32_t, raw::mme::kContextWords> words{};
    raw::Push push{words.data(), words.data() + words.size()};
    raw::mme::install(push);
    EXPECT_EQ(push.at, push.end);
    EXPECT_EQ(words[1], raw::mme::kDraw);
    EXPECT_EQ(words[2], 0u);
    const size_t second = 5 + raw::mme::kDrawProgram.size();
    EXPECT_EQ(words[second + 1], raw::mme::kDrawIndexed);
    EXPECT_EQ(words[second + 2], raw::mme::kDrawProgram.size());
}
}  // namespace
