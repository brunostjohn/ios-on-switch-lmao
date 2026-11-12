// SPDX-License-Identifier: MIT
// The macros Mesa's builder makes (gen/SwitchGM20BMacros.h) against the ones raw_mme.h hand-encoded before it installed
// the builder's (their words kept here), both run by Mesa's Fermi MME simulator (the in-tree copy) on the same
// arguments: the same methods with the same values, every argument consumed, no shadow state read. The one difference
// allowed: the builder sends the root table's two words to LOAD_CONSTANT_BUFFER(0) and (1), raw both to (0); the
// hardware takes either as the next word of the load.
#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../../SwitchGM20B/SwitchGM20B3D.h"
#include "SwitchGM20BMacros.h"
#include "mme_fermi.h"
#include "mme_fermi_sim.h"

namespace {

using Method = std::pair<uint32_t, uint32_t>;

constexpr uint32_t kHandDraw[] = {0x00000201, 0x00000301, 0x00000411, 0x00030827, 0x01618021, 0x00002041,
                                  0x04d74021, 0x00001041, 0x00001841, 0x01614021, 0x00000041, 0x00004411,
                                  0xd0410413, 0xffffc911, 0xfffd8837, 0x00000091, 0x00000011};
constexpr uint32_t kHandDrawIndexed[] = {0x00000201, 0x00000301, 0x00000411, 0x00030827, 0x01618021, 0x00002041,
                                         0x057dc021, 0x00001041, 0x00001841, 0x01614021, 0x00000041, 0x00004411,
                                         0xd0410413, 0xffffc911, 0xfffd8837, 0x00000091, 0x00000011};
constexpr uint32_t kHandIndirect[] = {0x00000201, 0x00000301, 0x00000401, 0x0238c021, 0x00000041, 0x02390021,
                                      0x00001841, 0x00002041, 0x01438021, 0x00002041, 0x01434021, 0x00000041,
                                      0x01118021, 0x00000041, 0x00000611, 0x00034827, 0x00031027, 0x01618021,
                                      0x00003041, 0x04d74021, 0x00001841, 0x00000841, 0x01614021, 0x00000041,
                                      0x00004611, 0xd0418613, 0xffffd211, 0xfffd9037, 0x00000091, 0x00000011};
constexpr uint32_t kHandIndexedIndirect[] = {
    0x00000201, 0x00000301, 0x00000401, 0x00000501, 0x0238c021, 0x00000041, 0x02390021, 0x00002041,
    0x00002841, 0x01438021, 0x00002841, 0x01434021, 0x00002041, 0x01118021, 0x00002041, 0x00000611,
    0x00034827, 0x00031027, 0x01618021, 0x00003041, 0x057dc021, 0x00001841, 0x00000841, 0x01614021,
    0x00000041, 0x00004611, 0xd0418613, 0xffffd211, 0xfffd9037, 0x00000091, 0x00000011};

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
        ADD_FAILURE() << "a draw macro reads no shadow state";
        return 0;
    }

    static void emit(void *opaque, uint16_t method, uint32_t value)
    {
        const uint32_t load = 0x2390;  // LOAD_CONSTANT_BUFFER(0): (0..15) all load the next word
        static_cast<Simulation *>(opaque)->methods.emplace_back(method >= load && method < load + 64 ? load : method,
                                                                value);
    }

    static void barrier(void *)
    {
        ADD_FAILURE() << "a draw macro needs no barrier";
    }

    static uint32_t *dram(void *, uint32_t)
    {
        ADD_FAILURE() << "a draw macro needs no MME memory";
        return nullptr;
    }
};

std::vector<Method> run(const uint32_t *words, size_t size, const std::vector<uint32_t> &arguments)
{
    Simulation s;
    s.arguments = arguments;
    std::vector<mme_fermi_inst> decoded(size);
    mme_fermi_decode(decoded.data(), words, uint32_t(size));
    const mme_sim_state_ops ops = {Simulation::load, Simulation::state, Simulation::emit, Simulation::barrier,
                                   Simulation::dram};
    mme_fermi_sim_core(uint32_t(size), decoded.data(), &ops, &s);
    EXPECT_EQ(s.consumed, arguments.size());
    return s.methods;
}

template <size_t N> size_t count(const uint32_t (&)[N])
{
    return N;
}

TEST(EmitMme, DirectDrawsAsRawMacrosDrawThem)
{
    for (uint32_t instances : {0u, 1u, 2u, 5u}) {
        const std::vector<uint32_t> args = {instances, 7, 300};
        EXPECT_EQ(run(switch_gm20b_macro_draw, count(switch_gm20b_macro_draw), args),
                  run(kHandDraw, count(kHandDraw), args))
            << instances;
        EXPECT_EQ(run(switch_gm20b_macro_draw_indexed, count(switch_gm20b_macro_draw_indexed), args),
                  run(kHandDrawIndexed, count(kHandDrawIndexed), args))
            << instances;
    }
}

TEST(EmitMme, IndirectDrawsAsRawMacrosDrawThem)
{
    for (uint32_t vertices : {0u, 6u}) {
        for (uint32_t instances : {0u, 1u, 3u}) {
            const std::vector<uint32_t> draw = {vertices, instances, 11, 4};
            EXPECT_EQ(run(switch_gm20b_macro_indirect, count(switch_gm20b_macro_indirect), draw),
                      run(kHandIndirect, count(kHandIndirect), draw))
                << vertices << " " << instances;
            const std::vector<uint32_t> indexed = {vertices, instances, 11, 0xfffffff0u, 4};
            EXPECT_EQ(run(switch_gm20b_macro_indexed_indirect, count(switch_gm20b_macro_indexed_indirect), indexed),
                      run(kHandIndexedIndirect, count(kHandIndexedIndirect), indexed))
                << vertices << " " << instances;
        }
    }
}

// The draw parameters as the raw path wrote them from the CPU before each draw (raw_gpu.cpp prepare), then the draw loop
std::vector<Method> withParams(bool indexed, uint32_t instances, uint32_t first, uint32_t count, uint32_t baseVertex,
                               uint32_t baseInstance)
{
    std::vector<Method> m = {{NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0},
                             {NVB197_LOAD_CONSTANT_BUFFER(0), indexed ? baseVertex : first},
                             {NVB197_LOAD_CONSTANT_BUFFER(0), baseInstance},
                             {NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, baseInstance},
                             {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, indexed ? baseVertex : 0},
                             {NVB197_SET_VERTEX_ID_BASE, indexed ? baseVertex : 0}};
    const uint16_t start = indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START;
    for (uint32_t i = 0; i < instances; ++i) {
        m.push_back({NVB197_BEGIN, i ? 1u << 26 : 0});
        m.push_back({start, first});
        m.push_back({uint16_t(start + 4), count});
        m.push_back({NVB197_END, 0});
    }
    return m;
}

TEST(EmitMme, ParameterDrawsWriteTheParametersThenDraw)
{
    for (uint32_t instances : {0u, 1u, 3u}) {
        EXPECT_EQ(run(switch_gm20b_macro_draw_params, count(switch_gm20b_macro_draw_params), {instances, 40, 6, 9}),
                  withParams(false, instances, 40, 6, 0, 9))
            << instances;
        EXPECT_EQ(run(switch_gm20b_macro_draw_indexed_params, count(switch_gm20b_macro_draw_indexed_params),
                      {instances, 12, 30, 0xfffffffcu, 2}),
                  withParams(true, instances, 12, 30, 0xfffffffcu, 2))
            << instances;
    }
}

TEST(EmitMme, TheMacrosTakeNvksDrawSlots)
{
    EXPECT_EQ(SWITCH_GM20B_MACRO_DRAW, 11);
    EXPECT_EQ(SWITCH_GM20B_MACRO_DRAW_INDEXED_PARAMS, 16);
}

TEST(EmitMme, TheLayeredClearTakesNvksClearSlot)
{
    EXPECT_EQ(SWITCH_GM20B_MACRO_CLEAR_LAYERS, 2);
}

TEST(EmitMme, TheMacrosFitTheRawPathsCodeSpace)
{
    const size_t words = count(switch_gm20b_macro_draw) + count(switch_gm20b_macro_draw_indexed) +
                         count(switch_gm20b_macro_indirect) + count(switch_gm20b_macro_indexed_indirect) +
                         count(switch_gm20b_macro_draw_params) + count(switch_gm20b_macro_draw_indexed_params);
    EXPECT_LE(words, 2048u);
}

}  // namespace
