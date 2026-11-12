// SPDX-License-Identifier: MIT
// raw/runtime/raw_icb_compute.h and raw_mme.h's gate macro: an indirect command buffer's dispatch commands, run through the
// host FIFO's method decoding and the Fermi MME simulator with the macros the channel loads, after the head an execute sends
// (the CPU's range as scratch words, or kIcbRange reading one from memory) and before its tail. The model of the compute
// class keeps the global render enable the gate sets and counts the launches it lets through: exactly the regions of the
// commands inside the range launch, a barrier flushes before its command whatever the range, an empty command is no-ops,
// and the tail leaves launches enabled. A command's stage-in words (raw_stage_in.h) are its own region and bind strides.
#include <gtest/gtest.h>
#include "../raw/runtime/raw_icb_compute.h"
#include "../raw/runtime/raw_stage_in.h"
#include "../raw/pack/mesa/mme_fermi.h"
#include "../raw/pack/mesa/mme_fermi_sim.h"
#include <utility>
#include <vector>

namespace {

struct Method {
    uint32_t subc;
    uint16_t method;
    uint32_t value;
};

struct Channel {
    std::vector<Method> methods;
    std::vector<uint32_t> arguments, launched;  // the QMD addresses (>> 8) launched while enabled
    size_t consumed = 0;
    uint32_t scratch[256] = {};
    uint32_t enable = NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE, pendingQmd = 0, flushes = 0;

    static bool isScratch(uint16_t method)
    {
        return method >= NVB197_SET_MME_SHADOW_SCRATCH(0) && method < NVB197_CALL_MME_MACRO(0);
    }

    static uint32_t load(void *opaque)
    {
        auto &c = *static_cast<Channel *>(opaque);
        return c.consumed < c.arguments.size() ? c.arguments[c.consumed++] : 0;
    }

    static uint32_t state(void *opaque, uint16_t method)
    {
        EXPECT_TRUE(isScratch(method)) << "the gate reads no state but its scratch";
        return isScratch(method)
                   ? static_cast<Channel *>(opaque)->scratch[(method - NVB197_SET_MME_SHADOW_SCRATCH(0)) / 4]
                   : 0;
    }

    static void emit(void *opaque, uint16_t m, uint32_t value)
    {
        static_cast<Channel *>(opaque)->method(raw::SUBC_3D, m, value);
    }

    static void barrier(void *)
    {}

    static uint32_t *dram(void *, uint32_t)
    {
        return nullptr;
    }

    void method(uint32_t subc, uint16_t m, uint32_t value)
    {
        if (subc == raw::SUBC_3D && isScratch(m)) {
            scratch[(m - NVB197_SET_MME_SHADOW_SCRATCH(0)) / 4] = value;
            return;
        }
        methods.push_back({subc, m, value});
        if (m == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C)
            enable = value;
        if (subc != raw::SUBC_COMPUTE)
            return;
        if (m == NVB1C0_SEND_PCAS_A)
            pendingQmd = value;
        if (m == NVB1C0_SEND_SIGNALING_PCAS_B && enable == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE)
            launched.push_back(pendingQmd);
        if (m == NVB1C0_INVALIDATE_SHADER_CACHES)
            flushes++;
    }

    void macro(uint32_t slot)
    {
        const raw::mme::Program &p =
            slot == raw::mme::kIcbRange ? raw::mme::kIcbRangeProgram : raw::mme::kIcbGateProgram;
        ASSERT_TRUE(slot == raw::mme::kIcbRange || slot == raw::mme::kIcbGate) << slot;
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
            const uint32_t header = w[i++], op = header >> 29, count = header >> 16 & 0x1fff, subc = header >> 13 & 7,
                           method = (header & 0xfff) << 2;
            if (method >= raw::mme::kMacroCall) {
                ASSERT_EQ(subc, uint32_t(raw::SUBC_3D));
                ASSERT_EQ(op, 5u);
                ASSERT_LE(i + count, n);
                arguments.assign(w + i, w + i + count);
                i += count;
                macro((method - raw::mme::kMacroCall) / 8);
                continue;
            }
            if (op == 4) {
                if (method != NVB197_NO_OPERATION)
                    this->method(subc, uint16_t(method), count);
                continue;
            }
            ASSERT_TRUE(op == 1 || op == 3);
            ASSERT_LE(i + count, n);
            for (uint32_t k = 0; k < count; k++)
                this->method(subc, uint16_t(op == 1 ? method + 4 * k : method), w[i++]);
        }
    }

    void head(bool written, bool fromMemory, uint32_t first, uint32_t count, const uint32_t *memoryRange = nullptr)
    {
        uint32_t words[16];
        raw::Push p{words, words + 16};
        raw::icb::computeExecuteHead(p, written, fromMemory, first, count);
        if (!fromMemory) {
            run(words, size_t(p.at - words));
            return;
        }
        // the call's header ends the head; its two data words are the GPFIFO continuation at the range buffer
        std::vector<uint32_t> all(words, p.at);
        all.insert(all.end(), memoryRange, memoryRange + 2);
        run(all.data(), all.size());
    }

    void tail()
    {
        uint32_t words[4];
        raw::Push p{words, words + 4};
        raw::icb::computeExecuteTail(p);
        run(words, size_t(p.at - words));
    }
};

// Five commands: command i has i % 3 + 1 regions at QMDs 0x10000 * (i + 1) + 0x100 * r; command 2 also has a barrier
std::vector<uint32_t> commands(uint32_t n = 5)
{
    std::vector<uint32_t> slots(n * raw::icb::kComputeSlotWords);
    for (uint32_t i = 0; i < n; i++) {
        raw::icb::ComputeCommand c{};
        c.regions = i % 3 + 1;
        c.barrier = i == 2;
        for (uint32_t r = 0; r < c.regions; r++)
            c.qmd[r] = 0x10000ull * (i + 1) + 0x100 * r;
        raw::icb::dispatch(slots.data() + i * raw::icb::kComputeSlotWords, i, c);
    }
    return slots;
}

std::vector<uint32_t> launches(uint32_t location, uint32_t length, uint32_t n = 5)
{
    std::vector<uint32_t> out;
    for (uint32_t i = location; i < location + length && i < n; i++)
        for (uint32_t r = 0; r < i % 3 + 1; r++)
            out.push_back(uint32_t((0x10000ull * (i + 1) + 0x100 * r) >> 8));
    return out;
}

TEST(RawIcbCompute, ACommandIsItsBarrierItsGateThenALaunchPerRegion)
{
    const std::vector<uint32_t> slots = commands();
    for (uint32_t i = 0; i < 5; i++) {
        const uint32_t *slot = slots.data() + i * raw::icb::kComputeSlotWords;
        EXPECT_EQ(slot[raw::icb::kComputeCallWord],
                  raw::Push::hdr(5, raw::SUBC_3D, raw::mme::kMacroCall + 8 * raw::mme::kIcbGate, 1));
        EXPECT_EQ(slot[raw::icb::kComputeIndexWord], i);
        Channel c;
        c.head(false, false, i, 1);
        c.run(slot, raw::icb::kComputeSlotWords);
        EXPECT_EQ(c.launched, launches(i, 1));
        EXPECT_EQ(c.flushes, i == 2 ? 1u : 0u);
    }
}

TEST(RawIcbCompute, ACpuRangeLaunchesItsCommandsAlone)
{
    const std::vector<uint32_t> slots = commands();
    const std::pair<uint32_t, uint32_t> ranges[] = {{0, 5}, {1, 2}, {3, 1}, {2, 0}, {4, 3}};
    for (const auto &[location, length] : ranges) {
        Channel c;
        c.head(false, false, location, length);
        c.run(slots.data(), slots.size());
        EXPECT_EQ(c.launched, launches(location, length)) << location << "+" << length;
        EXPECT_EQ(c.flushes, 1u) << "a barrier flushes whether or not its command launches";
        c.tail();
        EXPECT_EQ(c.enable, uint32_t(NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE));
    }
}

TEST(RawIcbCompute, ARangeFromMemoryGatesEveryCommand)
{
    const std::vector<uint32_t> slots = commands();
    const std::pair<uint32_t, uint32_t> ranges[] = {{0, 4}, {1, 2}, {3, 1}, {2, 0}, {4, 3}, {3, 9}};
    for (const auto &[location, length] : ranges) {
        Channel c;
        const uint32_t range[2] = {location, length};
        c.head(false, true, 0, 0, range);
        EXPECT_EQ(c.scratch[raw::mme::kScratchLocation], location);
        EXPECT_EQ(c.scratch[raw::mme::kScratchLength], length);
        c.run(slots.data(), slots.size());
        EXPECT_EQ(c.launched, launches(location, length)) << location << "+" << length;
        c.tail();
        EXPECT_EQ(c.enable, uint32_t(NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE));
    }
}

TEST(RawIcbCompute, WordsTheGpuWroteAreWaitedForBeforeTheyAreFetched)
{
    for (const bool fromMemory : {false, true}) {
        Channel c;
        const uint32_t range[2] = {0, 1};
        c.head(true, fromMemory, 0, 1, range);
        ASSERT_GE(c.methods.size(), 3u);
        EXPECT_EQ(c.methods[0].method, NVB1C0_WAIT_FOR_IDLE);
        EXPECT_EQ(c.methods[0].subc, uint32_t(raw::SUBC_COMPUTE));
        EXPECT_EQ(c.methods[1].method, NVB1C0_INVALIDATE_SHADER_CACHES);
        EXPECT_EQ(c.methods[2].method, 0x0050u);
    }
    Channel plain;
    plain.head(false, false, 0, 1);
    EXPECT_TRUE(plain.methods.empty()) << "a CPU-written buffer's CPU range is scratch words alone";
}

TEST(RawIcbCompute, AnEmptyCommandIsNoOperations)
{
    uint32_t slot[raw::icb::kComputeSlotWords];
    raw::icb::computeEmpty(slot);
    Channel c;
    c.run(slot, raw::icb::kComputeSlotWords);
    EXPECT_TRUE(c.methods.empty());
}

TEST(RawIcbCompute, ACopyToAnotherIndexDiffersInItsIndexWordAlone)
{
    raw::icb::ComputeCommand c{};
    c.regions = raw::icb::kComputeRegions;
    c.barrier = true;
    for (uint32_t r = 0; r < c.regions; r++)
        c.qmd[r] = 0x7700000000ull + 0x100 * r;
    uint32_t at3[raw::icb::kComputeSlotWords], at9[raw::icb::kComputeSlotWords];
    raw::icb::dispatch(at3, 3, c);
    raw::icb::dispatch(at9, 9, c);
    for (uint32_t w = 0; w < raw::icb::kComputeSlotWords; w++)
        EXPECT_EQ(at3[w] == at9[w], w != raw::icb::kComputeIndexWord) << w;
    Channel all;
    all.head(false, false, 3, 1);
    all.run(at3, raw::icb::kComputeSlotWords);
    EXPECT_EQ(all.launched.size(), size_t(raw::icb::kComputeRegions));
}

TEST(RawIcbCompute, ARegionsBlockHoldsItsQmdRootTableAndSetOnTheirAlignment)
{
    EXPECT_EQ(raw::icb::computeRegionBytes(0), 256u + 0x800u + 256u);
    EXPECT_EQ(raw::icb::computeRegionBytes(0x1f0), 256u + 0x800u + 512u);
    EXPECT_EQ(raw::icb::computeRegionBytes(0x1000), 256u + 0x800u + 0x1000u);
    for (const uint32_t set : {0u, 16u, 0x1f0u, 0x1000u})
        EXPECT_EQ(raw::icb::computeRegionBytes(set) % raw::icb::kComputeBlockAlign, 0u);
}

TEST(RawIcbCompute, ACommandsStageInWordsAreItsRegionAndItsBindStrides)
{
    raw::PackKernel k{};
    k.stageInRegionWord = 4;
    k.stageInStride[0][0] = 1;
    k.stageInStride[0][1] = 13;
    k.stageInStride[1][0] = 2;
    k.stageInStride[1][1] = 14;
    raw::StageIn s;
    s.setRegion({3, 5, 7, 0x100000040ull, 2, 1});
    s.strides[1] = 24;
    uint32_t push[32];
    std::fill(std::begin(push), std::end(push), 0xeeeeeeeeu);
    raw::stageInWords(k, s, push);
    const uint32_t region[8] = {3, 5, 7, 0, 0x40, 2, 1, 0};
    for (uint32_t w = 0; w < 8; w++)
        EXPECT_EQ(push[3 + w], region[w]) << w;
    EXPECT_EQ(push[12], 24u);
    EXPECT_EQ(push[13], 0u) << "a bind of no stride reads at 0";
    EXPECT_EQ(push[2], 0xeeeeeeeeu);
    EXPECT_EQ(push[14], 0xeeeeeeeeu);
}

TEST(RawIcbCompute, AKernelOfNoStageInTakesNoStageInWords)
{
    const raw::PackKernel k{};
    raw::StageIn s;
    s.setRegion({1, 2, 3, 4, 5, 6});
    uint32_t push[32];
    std::fill(std::begin(push), std::end(push), 0x5au);
    raw::stageInWords(k, s, push);
    for (uint32_t w = 0; w < 32; w++)
        EXPECT_EQ(push[w], 0x5au) << w;
}

}  // namespace
