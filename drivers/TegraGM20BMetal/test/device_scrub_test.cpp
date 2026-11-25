// SPDX-License-Identifier: MIT
// Zero-on-free of Shared buffers on gm20b::Device (scrubs.cpp), off the copy engine's methods the device submitted: a
// destroyed Shared buffer's span is filled with zeros by the next scrub job, a create before then does not get the span
// back, and one after the job completed does, with no CPU write to it. Metal clears every buffer newBufferWithLength: makes;
// review MR4: the CPU memset every reused span at create.
#include "device_harness.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include <cstring>
#include <vector>

namespace {

struct Fill {
    uint64_t va = 0;
    uint32_t constA = 0;
};

class DeviceScrub : public harness::DeviceTest {
protected:
    static std::vector<Fill> fills(size_t before)
    {
        std::vector<uint32_t> words;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
            words.insert(words.end(), kernel_model::model.jobs[j].words.begin(),
                         kernel_model::model.jobs[j].words.end());
        std::vector<switch_gm20b_method> m(words.size() * 2 + 1);
        const long n = switch_gm20b_push_decode(words.data(), words.size(), m.data(), m.size());
        EXPECT_GE(n, 0);
        std::vector<Fill> out;
        uint64_t va = 0;
        uint32_t constA = 0;
        for (long i = 0; i < n; i++) {
            if (m[i].subchannel != gm20b::SUBC_COPY)
                continue;
            if (m[i].method == NVB0B5_OFFSET_OUT_UPPER)
                va = uint64_t(m[i].value) << 32 | (va & 0xffffffffu);
            else if (m[i].method == NVB0B5_OFFSET_OUT_LOWER)
                va = (va & ~0xffffffffull) | m[i].value;
            else if (m[i].method == NVB0B5_SET_REMAP_CONST_A)
                constA = m[i].value;
            else if (m[i].method == NVB0B5_LAUNCH_DMA &&
                     (m[i].value & 3) != NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NONE && (m[i].value & (1u << 10)))
                out.push_back({va, constA});
        }
        return out;
    }

    static bool filledAt(const std::vector<Fill> &fills, uint64_t va)
    {
        for (const Fill &f : fills)
            if (f.va == va && f.constA == 0)
                return true;
        return false;
    }
};

TEST_F(DeviceScrub, AFreedSharedBufferIsZeroedByTheCopyEngineBeforeItsSpanIsReused)
{
    constexpr uint64_t kBytes = 64 * 1024;
    gm20b::Buffer first;
    ASSERT_TRUE(dev.createBuffer(kBytes, first, true, true, true));
    ASSERT_TRUE(first.gpuVolatile) << "a Shared buffer lives in the VOL=1 write-combined chunks";
    const uint64_t va = first.va;
    uint8_t *const cpu = first.cpu;
    memset(cpu, 0xab, kBytes);
    dev.destroyBuffer(first);
    EXPECT_TRUE(dev.scrubDue());

    gm20b::Buffer early;
    ASSERT_TRUE(dev.createBuffer(kBytes, early, true, true, true));
    EXPECT_NE(early.va, va) << "a create before the span was zeroed got it back";

    const size_t before = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.scrubFreed(false));
    EXPECT_TRUE(filledAt(fills(before), va)) << "no zero fill at the freed span's address";
    dev.destroyBuffer(early);

    // the model completed the job as it was submitted: the next look retires it
    ASSERT_TRUE(dev.scrubFreed(false));
    gm20b::Buffer later;
    ASSERT_TRUE(dev.createBuffer(kBytes, later, true, true, true));
    EXPECT_EQ(later.va, va) << "the zeroed span did not come back";
    // nothing runs the model's pushes, so the bytes the copy engine would zero are what the last owner left: a CPU
    // memset at the create would have cleared them
    EXPECT_EQ(later.cpu[0], 0xab);
    EXPECT_EQ(later.cpu[kBytes - 1], 0xab);
    dev.destroyBuffer(later);
}

TEST_F(DeviceScrub, NoFreedSpanNoScrubJob)
{
    const size_t before = kernel_model::model.jobs.size();
    ASSERT_TRUE(dev.scrubFreed(false));
    EXPECT_EQ(kernel_model::model.jobs.size(), before);
    EXPECT_FALSE(dev.scrubDue());
}

}  // namespace
