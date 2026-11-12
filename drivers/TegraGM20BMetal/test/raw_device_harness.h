// SPDX-License-Identifier: MIT
// raw::Device on the host over the kernel model (raw_kernel_model.h, whose IOKit calls replace the framework's: include
// this in one unit of a test), with a pack of no states. Nothing runs the pushes: the model completes jobs as they are
// submitted, or holds them until a kill, and the test reads what the device submitted.
#pragma once
#include "raw_kernel_model.h"
#include "../raw/runtime/raw_gpu.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace harness {

// A pack of this build's version holding no states (raw_pack.h), in the test's temporary directory
inline std::string emptyPack()
{
    std::vector<uint8_t> file(0x400, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/empty-pack.bin";
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

class DeviceTest : public testing::Test {
protected:
    raw::Device dev;

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.vaEnd = uint64_t(1) << 40;  // the program window's quarter of the client VA
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
    }

    // One command buffer of `copies` copies of `bytes` from `from` to `to`: its seqno, 0 when it was not submitted
    uint32_t copy(const raw::Buffer &from, const raw::Buffer &to, uint32_t bytes, uint32_t copies = 1)
    {
        raw::CommandBuffer *cb = dev.begin();
        raw::BlitEncoder *e = cb ? cb->blit() : nullptr;
        if (!e)
            return 0;
        for (uint32_t i = 0; i < copies; i++)
            e->copyBytes(from.va, to.va, bytes);
        e->end();
        uint32_t seqno = 0;
        return cb->commit(seqno) ? seqno : 0;
    }

    kernel_model::Slot *liveSlot()
    {
        for (kernel_model::Slot &s : kernel_model::model.slots)
            if (s.uid && !s.foreign)
                return &s;
        return nullptr;
    }

    uint32_t liveChannel()
    {
        kernel_model::Slot *s = liveSlot();
        return s ? uint32_t(s - kernel_model::model.slots) + 1 : 0;
    }

    bool freed(uint64_t handle) const
    {
        const auto &f = kernel_model::model.freed;
        return std::find(f.begin(), f.end(), handle) != f.end();
    }
};

}  // namespace harness
