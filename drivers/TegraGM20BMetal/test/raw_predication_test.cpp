// SPDX-License-Identifier: MIT
// raw's command predication run on a model of the units it drives: the copy engine's copies and reducing semaphore
// releases, the compute class's render enable (two 64-bit operands, read when the mode is set), its inline writes and
// launches gated by it. Each program's launches must be the ones Metal runs.
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_predicate_records.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_predication.h"
#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

constexpr uint64_t kBase = 0x0310000000ull;
constexpr uint32_t kCompute = 1, kCopy = 4;
constexpr uint32_t kEdges[] = {0, 1, 2, 7, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu};

// NVB0B5_LAUNCH_DMA's fields: DATA_TRANSFER_TYPE 1:0, SEMAPHORE_TYPE 4:3, SEMAPHORE_REDUCTION 17:14, its SIGN 18:18 and
// ENABLE 19:19
constexpr uint32_t bits(uint32_t v, uint32_t hi, uint32_t lo)
{
    return v >> lo & ((1u << (hi - lo + 1)) - 1);
}

struct Gpu {
    std::vector<uint8_t> memory = std::vector<uint8_t>(1 << 16);
    std::vector<uint32_t> launched;
    bool enabled = true;
    uint64_t enableVa = 0, ceIn = 0, ceOut = 0, ceSemaphore = 0, i2mOut = 0;
    uint32_t ceLength = 0, cePayload = 0, i2mLength = 0, pcas = 0;

    uint8_t *at(uint64_t va)
    {
        EXPECT_GE(va, kBase);
        EXPECT_LT(va - kBase, memory.size());
        return memory.data() + (va - kBase);
    }

    uint32_t word(uint64_t va)
    {
        uint32_t v;
        memcpy(&v, at(va), 4);
        return v;
    }

    uint64_t dword(uint64_t va)
    {
        return uint64_t(word(va)) | uint64_t(word(va + 4)) << 32;
    }

    void put(uint64_t va, uint32_t v)
    {
        memcpy(at(va), &v, 4);
    }

    void copyEngine(uint32_t m, uint32_t v)
    {
        switch (m) {
        case NVB0B5_OFFSET_IN_UPPER:
            ceIn = uint64_t(v) << 32 | (ceIn & 0xffffffff);
            break;
        case NVB0B5_OFFSET_IN_LOWER:
            ceIn = (ceIn & ~0xffffffffull) | v;
            break;
        case NVB0B5_OFFSET_OUT_UPPER:
            ceOut = uint64_t(v) << 32 | (ceOut & 0xffffffff);
            break;
        case NVB0B5_OFFSET_OUT_LOWER:
            ceOut = (ceOut & ~0xffffffffull) | v;
            break;
        case NVB0B5_LINE_LENGTH_IN:
            ceLength = v;
            break;
        case NVB0B5_SET_SEMAPHORE_A:
            ceSemaphore = uint64_t(v) << 32 | (ceSemaphore & 0xffffffff);
            break;
        case NVB0B5_SET_SEMAPHORE_B:
            ceSemaphore = (ceSemaphore & ~0xffffffffull) | v;
            break;
        case NVB0B5_SET_SEMAPHORE_PAYLOAD:
            cePayload = v;
            break;
        case NVB0B5_LAUNCH_DMA: {
            if (bits(v, 1, 0) != NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NONE)
                memmove(at(ceOut), at(ceIn), ceLength);
            if (bits(v, 4, 3) != NVB0B5_LAUNCH_DMA_SEMAPHORE_TYPE_NONE) {
                uint32_t next = cePayload;
                if (bits(v, 19, 19)) {
                    EXPECT_EQ(bits(v, 18, 18), uint32_t(NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_SIGN_UNSIGNED));
                    const uint32_t old = word(ceSemaphore);
                    next = bits(v, 17, 14) == NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMIN ? std::min(old, cePayload)
                                                                                         : std::max(old, cePayload);
                }
                put(ceSemaphore, next);
            }
            break;
        }
        default:
            break;
        }
    }

    void compute(uint32_t m, uint32_t v)
    {
        switch (m) {
        case NVB1C0_SET_GLOBAL_RENDER_ENABLE_A:
            enableVa = uint64_t(v) << 32 | (enableVa & 0xffffffff);
            break;
        case NVB1C0_SET_GLOBAL_RENDER_ENABLE_B:
            enableVa = (enableVa & ~0xffffffffull) | v;
            break;
        case NVB1C0_SET_GLOBAL_RENDER_ENABLE_C:
            if (v == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE)
                enabled = true;
            else if (v == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_EQUAL)
                enabled = dword(enableVa) == dword(enableVa + 16);
            else if (v == NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_NOT_EQUAL)
                enabled = dword(enableVa) != dword(enableVa + 16);
            else
                ADD_FAILURE() << "render enable mode " << v;
            break;
        case NVB1C0_LINE_LENGTH_IN:
            i2mLength = v;
            break;
        case NVB1C0_OFFSET_OUT_UPPER:
            i2mOut = uint64_t(v) << 32 | (i2mOut & 0xffffffff);
            break;
        case NVB1C0_OFFSET_OUT:
            i2mOut = (i2mOut & ~0xffffffffull) | v;
            break;
        case NVB1C0_LOAD_INLINE_DATA:
            EXPECT_EQ(i2mLength, 4u);
            if (enabled)
                put(i2mOut, v);
            break;
        case NVB1C0_SEND_PCAS_A:
            pcas = v;
            break;
        case NVB1C0_SEND_SIGNALING_PCAS_B:
            if (enabled)
                launched.push_back(pcas);
            break;
        default:
            break;
        }
    }

    void run(const std::vector<uint32_t> &words)
    {
        std::vector<switch_gm20b_method> m(words.size() * 2 + 1);
        const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
        ASSERT_GE(n, 0);
        for (long i = 0; i < n; i++) {
            if (m[size_t(i)].subchannel == kCopy)
                copyEngine(m[size_t(i)].method, m[size_t(i)].value);
            else if (m[size_t(i)].subchannel == kCompute)
                compute(m[size_t(i)].method, m[size_t(i)].value);
            else
                ADD_FAILURE() << "subchannel " << m[size_t(i)].subchannel;
        }
    }
};

// A command buffer's compute words: the predication and launches named 1, 2, ... in encode order
struct Program {
    Gpu gpu;
    raw::Predication predication;
    std::vector<uint32_t> words = std::vector<uint32_t>(1 << 14);
    raw::Push p{words.data(), words.data() + words.size()};
    uint64_t nextSlot = kBase + 0x1000;
    uint32_t launches = 0;

    raw::PredicateSlot slot()
    {
        const raw::PredicateSlot s{gpu.at(nextSlot), nextSlot};
        nextSlot += raw::Predication::kSlotBytes;
        return s;
    }

    uint64_t wordVa(uint32_t i) const
    {
        return kBase + 4 * i;
    }

    void startIf(uint32_t word, raw::PredicateCompare c, uint32_t reference)
    {
        const uint32_t i = uint32_t(predication.depth());
        gpu.put(wordVa(i), word);
        uint32_t *before = p.at;
        EXPECT_EQ(predication.startIf(p, slot(), wordVa(i), raw::predicateTest(c, reference)),
                  raw::Predication::Answer::Done);
        EXPECT_LE(uint32_t(p.at - before), raw::Predication::kStartWords);
    }

    void startElse()
    {
        uint32_t *before = p.at;
        EXPECT_EQ(predication.startElse(p, slot()), raw::Predication::Answer::Done);
        EXPECT_LE(uint32_t(p.at - before), raw::Predication::kStartWords);
    }

    void dispatch()
    {
        predication.guard(p);
        p.m(NVB1C0_SEND_PCAS_A, ++launches, kCompute);
        p.m(NVB1C0_SEND_SIGNALING_PCAS_B, 0, kCompute);
        predication.release(p);
    }

    std::vector<uint32_t> run()
    {
        gpu.run(std::vector<uint32_t>(words.data(), p.at));
        return gpu.launched;
    }
};

TEST(RawPredication, MetalsCodesAboveFiveCompareEqual)
{
    EXPECT_EQ(raw::predicateCompare(5), raw::PredicateCompare::LessEqual);
    for (uint64_t code : {6ull, 255ull, (1ull << 32) + 2, ~0ull})
        EXPECT_EQ(raw::predicateCompare(code), raw::PredicateCompare::Equal);
}

TEST(RawPredication, EveryCompareReducesToTheUnitsEquality)
{
    const auto holds = [](raw::PredicateCompare c, uint32_t w, uint32_t r) {
        switch (c) {
        case raw::PredicateCompare::Equal:
            return w == r;
        case raw::PredicateCompare::NotEqual:
            return w != r;
        case raw::PredicateCompare::Greater:
            return w > r;
        case raw::PredicateCompare::GreaterEqual:
            return w >= r;
        case raw::PredicateCompare::Less:
            return w < r;
        case raw::PredicateCompare::LessEqual:
            return w <= r;
        }
        return false;
    };
    std::vector<uint32_t> values(std::begin(kEdges), std::end(kEdges));
    for (uint32_t v = 0; v < 64; ++v)
        values.push_back(v);
    for (uint32_t c = 0; c < 6; ++c)
        for (uint32_t r : values)
            for (uint32_t w : values)
                ASSERT_EQ(raw::predicateRuns(raw::predicateTest(raw::PredicateCompare(c), r), w),
                          holds(raw::PredicateCompare(c), w, r))
                    << "compare " << c << " word " << w << " reference " << r;
}

TEST(RawPredication, OneBlockRunsItsDispatchExactlyWhereTheWordCompares)
{
    for (uint32_t c = 0; c < 6; ++c)
        for (uint32_t r : kEdges)
            for (uint32_t w : kEdges) {
                Program prog;
                prog.dispatch();
                prog.startIf(w, raw::PredicateCompare(c), r);
                prog.dispatch();
                prog.startElse();
                prog.dispatch();
                EXPECT_EQ(prog.predication.endIf(), raw::Predication::Answer::Done);
                prog.dispatch();
                const bool holds = raw::predicateRuns(raw::predicateTest(raw::PredicateCompare(c), r), w);
                EXPECT_EQ(prog.run(), (std::vector<uint32_t>{1, holds ? 2u : 3u, 4}))
                    << "compare " << c << " word " << w << " reference " << r;
            }
}

TEST(RawPredication, NestedBlocksRunWhereEveryEnclosingBranchHolds)
{
    std::mt19937 rng(1234);
    for (int trial = 0; trial < 4000; ++trial) {
        Program prog;
        std::vector<uint32_t> expected;
        std::vector<bool> live{true};
        const int depth = 1 + int(rng() % 4);
        auto dispatchAndExpect = [&] {
            prog.dispatch();
            if (live.back())
                expected.push_back(prog.launches);
        };
        std::vector<bool> conditions;
        for (int d = 0; d < depth; ++d) {
            dispatchAndExpect();
            const auto c = raw::PredicateCompare(rng() % 6);
            const uint32_t r = rng() % 3 ? rng() % 8 : kEdges[rng() % 8];
            const uint32_t w = rng() % 3 ? rng() % 8 : kEdges[rng() % 8];
            prog.startIf(w, c, r);
            conditions.push_back(raw::predicateRuns(raw::predicateTest(c, r), w));
            live.push_back(live.back() && conditions.back());
            dispatchAndExpect();
        }
        for (int d = depth - 1; d >= 0; --d) {
            if (rng() % 2) {
                prog.startElse();
                live.pop_back();
                live.push_back(live.back() && !conditions[size_t(d)]);
                dispatchAndExpect();
            }
            EXPECT_EQ(prog.predication.endIf(), raw::Predication::Answer::Done);
            live.pop_back();
            dispatchAndExpect();
        }
        ASSERT_EQ(prog.run(), expected) << "trial " << trial;
        ASSERT_TRUE(prog.gpu.enabled);
    }
}

TEST(RawPredication, MisusedRecordsAreRefusedWithoutWords)
{
    Program prog;
    uint32_t *before = prog.p.at;
    EXPECT_EQ(prog.predication.startElse(prog.p, prog.slot()), raw::Predication::Answer::Misused);
    EXPECT_EQ(prog.predication.endIf(), raw::Predication::Answer::Misused);
    prog.startIf(1, raw::PredicateCompare::Equal, 1);
    prog.startElse();
    before = prog.p.at;
    EXPECT_EQ(prog.predication.startElse(prog.p, prog.slot()), raw::Predication::Answer::Misused);
    EXPECT_EQ(prog.p.at, before);
}

TEST(RawPredication, ADispatchOutsideABlockCarriesNoRenderEnable)
{
    Program prog;
    prog.dispatch();
    EXPECT_EQ(prog.p.at - prog.words.data(), 2);
    prog.startIf(3, raw::PredicateCompare::Greater, 2);
    prog.predication.endUnit();
    prog.dispatch();
    EXPECT_EQ(prog.run(), (std::vector<uint32_t>{1, 2}));
}

}  // namespace

namespace {

using Records = raw::PredicateRecords;

TEST(RawPredicateRecords, EndsAnswerYesForTheirOwnBlockOnly)
{
    Records r;
    r.computeEncoder(0);
    EXPECT_TRUE(r.record(Records::StartIf));
    EXPECT_TRUE(r.record(Records::StartElse));
    EXPECT_TRUE(r.record(Records::StartIf));
    EXPECT_TRUE(r.record(Records::EndIf));
    EXPECT_TRUE(r.record(Records::EndIf));
    EXPECT_FALSE(r.misused());
    EXPECT_FALSE(r.loops());
    EXPECT_TRUE(r.record(Records::StartWhile));
    EXPECT_FALSE(r.record(Records::EndIf));
    EXPECT_TRUE(r.misused());
    EXPECT_TRUE(r.loops());
}

TEST(RawPredicateRecords, ASecondElseOrAnElseOnALoopIsMisuse)
{
    Records a;
    a.computeEncoder(0);
    a.record(Records::StartIf);
    a.record(Records::StartElse);
    EXPECT_FALSE(a.record(Records::StartElse));
    EXPECT_TRUE(a.misused());
    Records b;
    b.computeEncoder(0);
    b.record(Records::StartWhile);
    EXPECT_FALSE(b.record(Records::StartElse));
    EXPECT_TRUE(b.misused());
}

TEST(RawPredicateRecords, AUnitEndsAtOtherWorkOrAnotherDispatchType)
{
    Records r;
    r.computeEncoder(0);
    r.record(Records::StartIf);
    r.computeEncoder(0);
    EXPECT_EQ(r.depth(), 1u);
    r.computeEncoder(1);
    EXPECT_EQ(r.depth(), 0u);
    r.record(Records::StartIf);
    r.otherWork();
    EXPECT_FALSE(r.record(Records::EndIf));
    EXPECT_TRUE(r.recorded());
}

}  // namespace
