// SPDX-License-Identifier: MIT
// raw_counters.h and raw_counter_kernel.h: a counter sample buffer's checks and their errors as the identity's AGX driver
// words them, a sample's index and a resolve's range, PTIMER placed on the CPU's clock by an anchor (and an anchor a power-up
// moved PTIMER away from), the CPU's resolve and the GPU kernel's arithmetic in 32-bit words equal to it, and the words each
// boundary's report emits.
#include <cstdint>
#include <cstring>
#include <random>
#include <gtest/gtest.h>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_counter_kernel.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_counters.h"

namespace {
using namespace raw::counters;

TEST(RawCounters, TheDescriptorIsCheckedInTheDriversOrder)
{
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 1), Check::Ok);
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 4096), Check::Ok);
    EXPECT_EQ(checkDescriptor(0, "", "", 4), Check::SetSize);
    EXPECT_EQ(checkDescriptor(2, "timestamp", "GPUTimestamp", 4), Check::SetSize);
    EXPECT_EQ(checkDescriptor(1, "stage", "GPUTimestamp", 4), Check::SetName);
    EXPECT_EQ(checkDescriptor(1, "timestamp", "Cycles", 0), Check::CounterName);
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 0), Check::Length);
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 4097), Check::Length);
    // sampleCount << 3 wraps as the driver's shift does: 2^61 samples are 0 bytes
    EXPECT_EQ(checkDescriptor(1, "timestamp", "GPUTimestamp", 1ull << 61), Check::Length);
}

TEST(RawCounters, ALengthIsOneByteToThirtyTwoKiBInSamples)
{
    EXPECT_EQ(checkLength(0), Check::Length);
    EXPECT_EQ(checkLength(1), Check::Alignment);
    EXPECT_EQ(checkLength(8), Check::Ok);
    EXPECT_EQ(checkLength(12), Check::Alignment);
    EXPECT_EQ(checkLength(0x8000), Check::Ok);
    EXPECT_EQ(checkLength(0x8001), Check::Length);
    EXPECT_EQ(checkLength(~0ull), Check::Length);
}

TEST(RawCounters, ErrorsAreTheDriversWords)
{
    EXPECT_EQ(checkMessage(Check::SetSize, "", 0), "Invalid counter set size. Expecting 1 counter");
    EXPECT_EQ(checkMessage(Check::SetName, "stage", 0), "Invalid counter set name. Expecting: timestamp. Found: stage");
    EXPECT_EQ(checkMessage(Check::CounterName, "(null)", 0),
              "Invalid counter name. Expecting: GPUTimestamp. Found: (null)");
    EXPECT_EQ(checkMessage(Check::Length, "", 32776),
              "Invalid sample buffer length: 32776 B. Expected range: 8 -> 32768");
    EXPECT_EQ(checkMessage(Check::Alignment, "", 12),
              "Invalid sample buffer length: 12 B. Length must be aligned to 8 B");
    EXPECT_EQ(checkMessage(Check::Ok, "", 8), "");
}

TEST(RawCounters, AnIndexPastTheSamplesSamplesNothing)
{
    EXPECT_EQ(sampleIndex(~0ull, 4), Index::None);  // MTLCounterDontSample
    EXPECT_EQ(sampleIndex(0xffffffffull, 4), Index::None);
    EXPECT_EQ(sampleIndex(0, 4), Index::Sample);
    EXPECT_EQ(sampleIndex(3, 4), Index::Sample);
    EXPECT_EQ(sampleIndex(4, 4), Index::Invalid);
}

TEST(RawCounters, ResolveRangesAreTheDriversBounds)
{
    EXPECT_TRUE(resolvable(0, 4, 4));
    EXPECT_TRUE(resolvable(3, 1, 4));
    EXPECT_FALSE(resolvable(0, 0, 4));
    EXPECT_FALSE(resolvable(3, 2, 4));
    EXPECT_FALSE(resolvable(5, 1, 4));
    EXPECT_FALSE(resolvable(~0ull, 2, 4));
    EXPECT_TRUE(withinSamples(4, 0, 4));
    EXPECT_TRUE(withinSamples(0, 4, 4));
    EXPECT_FALSE(withinSamples(1, 4, 4));
    EXPECT_FALSE(withinSamples(2, ~0ull, 4));
}

TEST(RawCounters, PtimerUnitsAre625Over384Nanoseconds)
{
    EXPECT_EQ(unitsToNs(0), 0);
    EXPECT_EQ(unitsToNs(384), 625);
    EXPECT_EQ(unitsToNs(32), 52);  // one tick of the 19.2 MHz oscillator: 52.083 ns
    EXPECT_EQ(unitsToNs(19'200'000ll * 32), 1'000'000'000);
    EXPECT_EQ(unitsToNs(-384), -625);
    EXPECT_EQ(unitsToNs(-1), -2);
    EXPECT_EQ(unitsToNs(1), 1);
}

TEST(RawCounters, AnAnchorPlacesUnitsOnTheCpusClock)
{
    const Anchor a = anchorOf({1'000'000'000, 600'000'000, 1'000'004'000});
    ASSERT_TRUE(a.valid);
    EXPECT_EQ(a.units, 600'000'000u);
    EXPECT_EQ(a.ns, 1'000'002'000u);
    EXPECT_EQ(nanoseconds(a, 600'000'000 + 384 * 1000), 1'000'002'000u + 625'000u);
    EXPECT_EQ(nanoseconds(a, 600'000'000 - 384 * 1000), 1'000'002'000u - 625'000u);
    EXPECT_FALSE(anchorOf({1, 0, 2}).valid);
}

TEST(RawCounters, ALaterReadingKeepsTheAnchorUntilAPowerUpMovesPtimer)
{
    const Anchor a = anchorOf({1'000'000'000, 600'000'000, 1'000'004'000});
    // a second later on both clocks: 19.2e6 ticks, 32 units each
    const uint64_t second = 19'200'000ull * 32;
    EXPECT_TRUE(agrees(a, {2'000'001'000, 600'000'000 + second, 2'000'003'500}));
    EXPECT_TRUE(agrees(a, {2'000'000'000, 600'000'000 + second, 2'010'000'000}));
    // the GPU powered down and up: PTIMER restarted from the kernel's clock at the wake, 2.0 s, 0.1 s before
    EXPECT_FALSE(agrees(a, {2'100'000'000, 2'061'440'000, 2'100'003'000}));
    EXPECT_FALSE(agrees(Anchor{}, {1, 2, 3}));
    EXPECT_FALSE(agrees(a, {2'000'000'000, 0, 2'000'001'000}));
}

TEST(RawCounters, APoweredDownGpusReadingIsTheCpusClock)
{
    // the kernel's clock while the GPU is powered down: within the reading's window
    EXPECT_TRUE(poweredDown({5'000'000'000, 5'000'000'400, 5'000'001'000}));
    EXPECT_TRUE(poweredDown({5'000'000'000, 4'999'999'500, 5'000'001'000}));
    // PTIMER, restarted from it at a wake 10 ms before and counting 0.6144 of it since: 3.9 ms below
    EXPECT_FALSE(poweredDown({5'000'000'000, 4'990'000'000 + 6'144'000, 5'000'001'000}));
    EXPECT_FALSE(poweredDown({5'000'000'000, 0, 5'000'001'000}));
}

TEST(RawCounters, TheNarrowestReadingAnchors)
{
    const Reading r[3] = {{100, 10, 900}, {1000, 20, 1010}, {2000, 0, 2001}};
    EXPECT_EQ(narrowest(r, 3).units, 20u);
    const Reading none[2] = {{100, 0, 101}, {200, 0, 201}};
    EXPECT_FALSE(anchorOf(narrowest(none, 2)).valid);
}

TEST(RawCounters, AnUnwrittenRecordResolvesToZeroAndNoAnchorToTheErrorValue)
{
    const Anchor a = anchorOf({1'000'000, 3'840'000, 1'000'000});
    EXPECT_EQ(resolved(Record{0, 0, 3'840'384}, a), 0u);
    EXPECT_EQ(resolved(Record{kPayload, 0, 3'840'384}, a), 1'000'625u);
    EXPECT_EQ(resolved(Record{kPayload, 0, 3'840'384}, Anchor{}), kErrorValue);
}

TEST(RawCounters, TheKernelsWordArithmeticIsTheCpusResolve)
{
    std::mt19937_64 random(0x5eed);
    const uint64_t anchors[] = {
        0, 1, 0xffffffffull, 0x1'0000'0000ull, 600'000'000, 0x7fff'ffff'0000'0000ull, 0x0000'1234'ffff'ffffull};
    uint64_t compared = 0;
    for (const uint64_t base : anchors)
        for (const uint64_t ns : {uint64_t(0), uint64_t(0xffff'fff0), uint64_t(123'456'789'012'345)})
            for (int i = 0; i < 2000; i++) {
                const Anchor a{base, ns, true};
                const int64_t span = int64_t(random() % (1ull << 52)) - int64_t(1ull << 51);
                const uint64_t units = i < 8 ? base + uint64_t(int64_t(i) - 4) : base + uint64_t(span);
                const Record r{kPayload, 0, units};
                ASSERT_EQ(resolvedInWords(r, resolveParams(a, 1)), resolved(r, a))
                    << "anchor " << base << " ns " << ns << " units " << units;
                compared++;
            }
    EXPECT_EQ(compared, 7u * 3 * 2000);
    const Record unwritten{0, 0, 5};
    EXPECT_EQ(resolvedInWords(unwritten, resolveParams(Anchor{1, 2, true}, 1)), 0u);
    EXPECT_EQ(resolvedInWords(Record{kPayload, 0, 5}, resolveParams(Anchor{}, 1)), kErrorValue);
}

// What the GPU does with a report: its payload and PTIMER at the report's address, the record a sample is resolved from
Record released(const uint32_t *words, size_t addressWord, uint64_t units)
{
    return Record{words[addressWord + 2], 0, units};
}

TEST(RawCounters, ThreeDReportsAreNvksTimestampAtEachStagesLocation)
{
    const uint64_t va = 0x12'3456'7890ull;
    // SET_REPORT_SEMAPHORE_D: OPERATION RELEASE (1:0 = 0), RELEASE AFTER_ALL_PRECEEDING_WRITES_COMPLETE (4:4 = 1),
    // PIPELINE_LOCATION (15:12), STRUCTURE_SIZE FOUR_WORDS (28:28 = 0) (clb197.h)
    const std::pair<Point, uint32_t> locations[] = {
        {Point::VertexStart, 0x1},    // DATA_ASSEMBLER
        {Point::VertexEnd, 0x6},      // GEOMETRY_SHADER
        {Point::FragmentStart, 0xA},  // PIXEL_SHADER
        {Point::FragmentEnd, 0xF},    // ALL
    };
    for (const auto &[point, location] : locations) {
        uint32_t words[kTimestampWords] = {};
        raw::Push p{words, words + kTimestampWords};
        emit(p, {va, point});
        ASSERT_EQ(p.at - words, 6) << int(point);
        EXPECT_EQ(words[0], raw::Push::hdr(4, raw::SUBC_3D, 0x1144, 0));  // FLUSH_PENDING_WRITES
        EXPECT_EQ(words[1], raw::Push::hdr(1, raw::SUBC_3D, 0x1b00, 4));
        EXPECT_EQ(words[2], 0x12u);
        EXPECT_EQ(words[3], 0x34567890u);
        EXPECT_EQ(words[4], kPayload);
        EXPECT_EQ(words[5], 1u << 4 | location << 12) << int(point);
        const Record r = released(words, 2, 77);
        EXPECT_EQ(r.payload, kPayload);
    }
}

TEST(RawCounters, ComputeReportsAreTheClasssFourWordRelease)
{
    uint32_t words[kTimestampWords] = {};
    raw::Push p{words, words + kTimestampWords};
    emit(p, {0x1'0000'0040ull, Point::ComputeStart});
    ASSERT_EQ(p.at - words, 5);
    EXPECT_EQ(words[0], raw::Push::hdr(1, raw::SUBC_COMPUTE, 0x1b00, 4));
    EXPECT_EQ(words[1], 0x1u);
    EXPECT_EQ(words[2], 0x40u);
    EXPECT_EQ(words[3], kPayload);
    EXPECT_EQ(words[4], 0u);  // RELEASE, FOUR_WORDS (clb1c0.h)
    uint32_t end[kTimestampWords] = {};
    raw::Push e{end, end + kTimestampWords};
    emit(e, {0x1'0000'0040ull, Point::ComputeEnd});
    EXPECT_EQ(memcmp(words, end, sizeof words), 0);
}

TEST(RawCounters, BlitReportsAreTheCopyEnginesSemaphoreOfAnEmptyLaunch)
{
    const uint64_t va = 0xab'cdef'0010ull;
    uint32_t start[kTimestampWords] = {}, end[kTimestampWords] = {};
    raw::Push ps{start, start + kTimestampWords}, pe{end, end + kTimestampWords};
    emit(ps, {va, Point::BlitStart});
    emit(pe, {va, Point::BlitEnd});
    // SET_SEMAPHORE_A..PAYLOAD; LAUNCH_DMA: DATA_TRANSFER_TYPE (1:0) NONE or NON_PIPELINED (2), FLUSH_ENABLE (2:2),
    // SEMAPHORE_TYPE (4:3) RELEASE_FOUR_WORD_SEMAPHORE (2) (clb0b5.h)
    ASSERT_EQ(ps.at - start, 5);
    EXPECT_EQ(start[0], raw::Push::hdr(1, raw::SUBC_COPY, 0x240, 3));
    EXPECT_EQ(start[1], 0xabu);
    EXPECT_EQ(start[2], 0xcdef0010u);
    EXPECT_EQ(start[3], kPayload);
    EXPECT_EQ(start[4], raw::Push::hdr(4, raw::SUBC_COPY, 0x300, 1u << 2 | 2u << 3));
    ASSERT_EQ(pe.at - end, 8);
    EXPECT_EQ(memcmp(start, end, 4 * sizeof(uint32_t)), 0);
    EXPECT_EQ(end[4], raw::Push::hdr(1, raw::SUBC_COPY, 0x418, 2));  // LINE_LENGTH_IN, LINE_COUNT 0
    EXPECT_EQ(end[5], 0u);
    EXPECT_EQ(end[6], 0u);
    EXPECT_EQ(end[7], raw::Push::hdr(4, raw::SUBC_COPY, 0x300, 2u | 1u << 2 | 2u << 3));
}

TEST(RawCounters, StartsAreTheEncodersFirstBoundaries)
{
    for (const Point p : {Point::VertexStart, Point::FragmentStart, Point::ComputeStart, Point::BlitStart})
        EXPECT_TRUE(isStart(p));
    for (const Point p : {Point::VertexEnd, Point::FragmentEnd, Point::ComputeEnd, Point::BlitEnd})
        EXPECT_FALSE(isStart(p));
}

}  // namespace
