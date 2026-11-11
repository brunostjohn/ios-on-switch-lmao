// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "../raw/runtime/raw_fresh_queue.h"

namespace {

struct Texture {
    std::string name;
};

struct Entry {
    std::weak_ptr<Texture> texture;
};

// Two channels of one GPU as the raw path orders them (RawMetalSubmit.mm): each runs its submissions in order; an m2v
// submission made after raw work first acquires the raw channel's last seqno (the kernel's Order), and nothing else orders the
// two. A command is "fill:<texture>" or "write:<texture>".
struct Gpu {
    struct Submission {
        bool raw;
        uint32_t acquires;  // raw seqno an m2v submission waits for (0: none)
        std::vector<std::string> commands;
    };

    std::vector<Submission> raw, m2v;
    bool rawSinceM2v = false;

    uint32_t submitRaw(std::vector<std::string> commands)
    {
        raw.push_back({true, 0, std::move(commands)});
        rawSinceM2v = true;
        return uint32_t(raw.size());
    }

    void submitM2v(std::vector<std::string> commands)
    {
        m2v.push_back({false, rawSinceM2v ? uint32_t(raw.size()) : 0u, std::move(commands)});
        rawSinceM2v = false;
    }

    // Whether some order the GPU may run both channels in puts a write of `texture` ahead of its fill: the m2v channel run as
    // early as its acquires allow, before the raw channel's later submissions
    bool writeMayPrecedeFill(const std::string &texture) const
    {
        std::vector<std::string> order;
        size_t r = 0;
        for (const Submission &s : m2v) {
            while (r < s.acquires)
                order.insert(order.end(), raw[r].commands.begin(), raw[r].commands.end()), r++;
            order.insert(order.end(), s.commands.begin(), s.commands.end());
        }
        for (; r < raw.size(); r++)
            order.insert(order.end(), raw[r].commands.begin(), raw[r].commands.end());
        const auto fill = std::find(order.begin(), order.end(), "fill:" + texture);
        const auto write = std::find(order.begin(), order.end(), "write:" + texture);
        return write != order.end() && (fill == order.end() || write < fill);
    }
};

// The runtime's use of the queue: a raw submission takes every pending texture and writes it ahead of its own commands
// (FreshWrites); an m2v submission is preceded by a raw one of the pending textures alone (writeFreshAhead) when `ahead`
struct Runtime {
    raw::FreshQueue<Entry> fresh;
    Gpu gpu;
    bool ahead = true;

    std::shared_ptr<Texture> make(const std::string &name)
    {
        auto t = std::make_shared<Texture>(Texture{name});
        fresh.add({t});
        return t;
    }

    std::vector<std::string> takeFills()
    {
        std::vector<std::string> fills;
        for (Entry &e : fresh.take())
            if (auto t = e.texture.lock())
                fills.push_back("fill:" + t->name);
        return fills;
    }

    void raw(std::vector<std::string> commands)
    {
        std::vector<std::string> all = takeFills();
        all.insert(all.end(), commands.begin(), commands.end());
        gpu.submitRaw(std::move(all));
    }

    void m2v(std::vector<std::string> commands)
    {
        if (ahead && fresh.any())
            gpu.submitRaw(takeFills());
        gpu.submitM2v(std::move(commands));
    }
};

}  // namespace

TEST(RawFreshQueue, TheFirstRawSubmissionWritesATextureAheadOfItsCommands)
{
    Runtime rt;
    auto a = rt.make("a");
    rt.raw({"write:a"});
    ASSERT_EQ(rt.gpu.raw.size(), 1u);
    EXPECT_EQ(rt.gpu.raw[0].commands, (std::vector<std::string>{"fill:a", "write:a"}));
    EXPECT_FALSE(rt.fresh.any());
    rt.raw({"write:a"});
    EXPECT_EQ(rt.gpu.raw[1].commands, (std::vector<std::string>{"write:a"}));
}

TEST(RawFreshQueue, AnM2vSubmissionGoesAfterTheRawFillOfItsTextures)
{
    Runtime rt;
    auto a = rt.make("a");
    rt.m2v({"write:a"});
    EXPECT_FALSE(rt.gpu.writeMayPrecedeFill("a"));
    rt.raw({"write:a"});
    EXPECT_FALSE(rt.gpu.writeMayPrecedeFill("a"));
}

// Without the raw submission ahead, the next raw submission fills the texture after m2v wrote it, on another channel
TEST(RawFreshQueue, WithoutTheFillAheadAnM2vWriteIsOverwritten)
{
    Runtime rt;
    rt.ahead = false;
    auto a = rt.make("a");
    rt.m2v({"write:a"});
    rt.raw({"draw"});
    EXPECT_TRUE(rt.gpu.writeMayPrecedeFill("a"));
}

TEST(RawFreshQueue, AnM2vSubmissionAfterRawWorkWaitsForIt)
{
    Runtime rt;
    auto a = rt.make("a");
    rt.raw({"draw"});
    auto b = rt.make("b");
    rt.m2v({"write:b", "write:a"});
    EXPECT_FALSE(rt.gpu.writeMayPrecedeFill("a"));
    EXPECT_FALSE(rt.gpu.writeMayPrecedeFill("b"));
    EXPECT_EQ(rt.gpu.raw.size(), 2u);
}

TEST(RawFreshQueue, AFailedSubmissionGivesItsTexturesBack)
{
    raw::FreshQueue<Entry> q;
    auto a = std::make_shared<Texture>(Texture{"a"});
    auto b = std::make_shared<Texture>(Texture{"b"});
    q.add({a});
    auto taken = q.take();
    q.add({b});
    q.settle(std::move(taken), false);
    auto again = q.take();
    ASSERT_EQ(again.size(), 2u);
    EXPECT_EQ(again[0].texture.lock(), a);
    EXPECT_EQ(again[1].texture.lock(), b);
    q.settle(std::move(again), true);
    EXPECT_FALSE(q.any());
    EXPECT_TRUE(q.take().empty());
}

TEST(RawFreshQueue, ATextureReleasedBeforeAnySubmissionIsNeverFilled)
{
    Runtime rt;
    rt.make("gone");
    rt.raw({"draw"});
    EXPECT_EQ(rt.gpu.raw[0].commands, (std::vector<std::string>{"draw"}));
}
