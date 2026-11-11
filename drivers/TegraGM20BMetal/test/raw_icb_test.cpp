// SPDX-License-Identifier: MIT
// raw/runtime/raw_icb.h: an indirect command buffer's command words, run through the host FIFO's method decoding and the
// Fermi MME simulator with the draw macros the channel loads: each command sets its topology, restart and index buffer,
// then draws as an indirect draw of its arguments does (root draw parameters, base registers, the instance loop); every
// word past it, and every word of a command with nothing encoded, is a no-op; a command fits its slot.
#include <gtest/gtest.h>
#include "../raw/runtime/raw_icb.h"
#include "../raw/pack/mesa/mme_fermi.h"
#include "../raw/pack/mesa/mme_fermi_sim.h"
#include <utility>
#include <vector>

namespace {
using Method = std::pair<uint16_t, uint32_t>;

struct Fifo {
    std::vector<Method> methods;
    std::vector<uint32_t> arguments;
    size_t consumed = 0, nops = 0;

    static uint32_t load(void *opaque)
    {
        auto &f = *static_cast<Fifo *>(opaque);
        return f.consumed < f.arguments.size() ? f.arguments[f.consumed++] : 0;
    }

    static uint32_t state(void *, uint16_t)
    {
        ADD_FAILURE() << "a draw macro reads no state";
        return 0;
    }

    static void emit(void *opaque, uint16_t method, uint32_t value)
    {
        static_cast<Fifo *>(opaque)->methods.emplace_back(method, value);
    }

    static void barrier(void *)
    {}

    static uint32_t *dram(void *, uint32_t)
    {
        return nullptr;
    }

    void macro(uint32_t slot)
    {
        const raw::mme::Program &p = slot == raw::mme::kIndirect          ? raw::mme::kIndirectProgram
                                     : slot == raw::mme::kIndexedIndirect ? raw::mme::kIndexedIndirectProgram
                                     : slot == raw::mme::kDraw            ? raw::mme::kDrawProgram
                                                                          : raw::mme::kIndexedProgram;
        std::vector<mme_fermi_inst> decoded(p.size);
        mme_fermi_decode(decoded.data(), p.words, p.size);
        consumed = 0;
        const mme_sim_state_ops ops = {load, state, emit, barrier, dram};
        mme_fermi_sim_core(uint32_t(decoded.size()), decoded.data(), &ops, this);
        EXPECT_EQ(consumed, arguments.size());
        arguments.clear();
    }

    // Method headers as the host FIFO reads them (raw_push.h): 1 INC, 3 NON_INC, 4 IMMD, 5 ONE_INC; a macro call's data
    // feeds the macro
    void run(const uint32_t *w, size_t n)
    {
        for (size_t i = 0; i < n;) {
            const uint32_t header = w[i++], op = header >> 29, count = header >> 16 & 0x1fff,
                           method = (header & 0xfff) << 2;
            EXPECT_EQ(header >> 13 & 7, uint32_t(raw::SUBC_3D));
            if (method >= raw::mme::kMacroCall) {
                ASSERT_EQ(op, 5u);
                ASSERT_LE(i + count, n);
                arguments.assign(w + i, w + i + count);
                i += count;
                macro((method - raw::mme::kMacroCall) / 8);
                continue;
            }
            if (op == 4) {
                if (method == NVB197_NO_OPERATION)
                    nops++;
                else
                    methods.emplace_back(uint16_t(method), count);
                continue;
            }
            ASSERT_TRUE(op == 1 || op == 3);
            ASSERT_LE(i + count, n);
            for (uint32_t k = 0; k < count; k++)
                methods.emplace_back(uint16_t(op == 1 ? method + 4 * k : method), w[i++]);
        }
    }
};

std::vector<Method> drawn(uint16_t first, uint32_t start, uint32_t count, uint32_t instances)
{
    std::vector<Method> m;
    for (uint32_t i = 0; i < instances && count; i++) {
        m.emplace_back(NVB197_BEGIN, i ? 1u << 26 : 0);
        m.emplace_back(first, start);
        m.emplace_back(first + 4, count);
        m.emplace_back(NVB197_END, 0);
    }
    return m;
}

TEST(RawIcb, ADrawSetsItsTopologyThenDrawsAsAnIndirectDraw)
{
    for (uint32_t primitive : {0u, 2u, 3u, 4u}) {
        uint32_t slot[raw::icb::kSlotWords];
        raw::icb::draw(slot, {primitive, 6, 3, 10, 7});
        Fifo f;
        f.run(slot, raw::icb::kSlotWords);
        std::vector<Method> want = {{NVB197_SET_PRIMITIVE_TOPOLOGY, raw::icb::topology(primitive)},
                                    {NVB197_SET_DA_PRIMITIVE_RESTART, primitive == 2 || primitive == 4},
                                    {NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0},
                                    {NVB197_LOAD_CONSTANT_BUFFER(0), 10},
                                    {NVB197_LOAD_CONSTANT_BUFFER(1), 7},
                                    {NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, 7},
                                    {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, 0},
                                    {NVB197_SET_VERTEX_ID_BASE, 0}};
        const auto loop = drawn(NVB197_SET_VERTEX_ARRAY_START, 10, 6, 3);
        want.insert(want.end(), loop.begin(), loop.end());
        EXPECT_EQ(f.methods, want) << primitive;
        EXPECT_EQ(f.nops, raw::icb::kSlotWords - 7);
    }
}

TEST(RawIcb, AnIndexedDrawBindsItsIndicesThenDrawsAsAnIndexedIndirectDraw)
{
    for (uint32_t size : {2u, 4u}) {
        uint32_t slot[raw::icb::kSlotWords];
        const uint64_t va = 0x1234567800ull, last = va + 0xfff;
        raw::icb::indexed(slot, {4, size, va, last, 9, 2, 0, -5, 3});
        Fifo f;
        f.run(slot, raw::icb::kSlotWords);
        std::vector<Method> want = {{NVB197_SET_INDEX_BUFFER_A, uint32_t(va >> 32)},
                                    {NVB197_SET_INDEX_BUFFER_B, uint32_t(va)},
                                    {NVB197_SET_INDEX_BUFFER_C, uint32_t(last >> 32)},
                                    {NVB197_SET_INDEX_BUFFER_D, uint32_t(last)},
                                    {NVB197_SET_DA_PRIMITIVE_RESTART_INDEX, size == 2 ? 0xffffu : 0xffffffffu},
                                    {NVB197_SET_INDEX_BUFFER_E, size == 2 ? 1u : 2u},
                                    {NVB197_SET_PRIMITIVE_TOPOLOGY, raw::icb::topology(4)},
                                    {NVB197_SET_DA_PRIMITIVE_RESTART, 1},
                                    {NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0},
                                    {NVB197_LOAD_CONSTANT_BUFFER(0), uint32_t(-5)},
                                    {NVB197_LOAD_CONSTANT_BUFFER(1), 3},
                                    {NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX, 3},
                                    {NVB197_SET_GLOBAL_BASE_VERTEX_INDEX, uint32_t(-5)},
                                    {NVB197_SET_VERTEX_ID_BASE, uint32_t(-5)}};
        const auto loop = drawn(NVB197_SET_INDEX_BUFFER_F, 0, 9, 2);
        want.insert(want.end(), loop.begin(), loop.end());
        EXPECT_EQ(f.methods, want) << size;
        EXPECT_EQ(f.nops, 0u) << "an indexed draw fills its slot";
    }
}

TEST(RawIcb, AnEmptyCommandAndARangeOfThemDoNothing)
{
    std::vector<uint32_t> range(3 * raw::icb::kSlotWords);
    raw::icb::empty(range.data());
    raw::icb::draw(range.data() + raw::icb::kSlotWords, {3, 3, 1, 0, 0});
    raw::icb::empty(range.data() + raw::icb::kSlotWords);
    raw::icb::empty(range.data() + 2 * raw::icb::kSlotWords);
    Fifo f;
    f.run(range.data(), range.size());
    EXPECT_TRUE(f.methods.empty());
    EXPECT_EQ(f.nops, range.size());
}

TEST(RawIcb, CommandsRunBackToBackInOneRange)
{
    std::vector<uint32_t> range(2 * raw::icb::kSlotWords);
    raw::icb::draw(range.data(), {3, 3, 1, 0, 0});
    raw::icb::draw(range.data() + raw::icb::kSlotWords, {1, 4, 2, 8, 0});
    Fifo f;
    f.run(range.data(), range.size());
    size_t begins = 0;
    for (const Method &m : f.methods)
        begins += m.first == NVB197_BEGIN;
    EXPECT_EQ(begins, 3u) << "one instance of the first, two of the second";
    EXPECT_EQ(f.methods.back(), Method(NVB197_END, 0));
}

}  // namespace
