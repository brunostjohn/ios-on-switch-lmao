// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_compute_lookup.h"
#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>

namespace {

raw::BoundTexture texture(uint32_t slot = 0)
{
    return {slot, 80, 2, 1, 1, {0, 1, 2, 3}};
}

raw::BoundSampler sampler(uint32_t slot = 0)
{
    return {slot, 1, 1, 0, 0, 0, 0, 1, 7, 0, 0.0f, 16.0f, true, false};
}

TEST(RawComputeLookup, BindingOrderAndClearedSlotsHaveCanonicalKeys)
{
    raw::ComputeSpecialization a, b;
    auto low = texture(1), high = texture(7);
    a.bindTexture(7, &high);
    a.bindTexture(1, &low);
    b.bindTexture(1, &low);
    b.bindTexture(7, &high);
    a.bindMemory(4, 32);
    a.bindMemory(0, 16);
    b.bindMemory(0, 16);
    b.bindMemory(4, 32);
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a.bindTexture(1, &low));
    EXPECT_FALSE(a.bindMemory(4, 32));
    EXPECT_TRUE(a.bindTexture(7, nullptr));
    EXPECT_TRUE(b.bindTexture(7, nullptr));
    EXPECT_TRUE(a.bindMemory(4, 0));
    EXPECT_TRUE(b.bindMemory(4, 0));
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a.bindTexture(7, nullptr));
    EXPECT_FALSE(a.bindMemory(4, 0));
}

TEST(RawComputeLookup, TextPreservesPackAndJournalIdentity)
{
    raw::ComputeSpecialization key;
    auto t = texture(3);
    auto s = sampler(5);
    key.bindTexture(3, &t);
    key.bindSampler(5, &s);
    key.bindMemory(2, 48);
    const auto text = key.text();
    EXPECT_EQ(text.memory, "2:48");
    EXPECT_EQ(text.textures, "3:80:2:1:1:00010203");
    EXPECT_EQ(text.samplers, "5:1:1:0:0:0:0:0:41800000:1:7:0:1:0");
}

TEST(RawComputeLookup, EverySpecializationInputSelectsAnotherKernel)
{
    raw::ComputeSpecialization key;
    key.threads = {8, 4, 1};
    auto t = texture(3);
    auto s = sampler(5);
    key.bindTexture(3, &t);
    key.bindSampler(5, &s);
    key.bindMemory(2, 48);
    std::vector<raw::ComputeSpecialization> changed;
    for (size_t axis = 0; axis < 3; ++axis) {
        auto next = key;
        ++next.threads[axis];
        changed.push_back(next);
    }
    for (unsigned field = 0; field < 8; ++field) {
        auto next = key;
        auto v = t;
        switch (field) {
        case 0:
            ++v.pixelFormat;
            break;
        case 1:
            ++v.textureType;
            break;
        case 2:
            ++v.levels;
            break;
        case 3:
            ++v.arrayLength;
            break;
        default:
            ++v.swizzle[field - 4];
            break;
        }
        next.bindTexture(3, &v);
        changed.push_back(next);
    }
    for (unsigned field = 0; field < 13; ++field) {
        auto next = key;
        auto v = s;
        switch (field) {
        case 0:
            ++v.minFilter;
            break;
        case 1:
            ++v.magFilter;
            break;
        case 2:
            ++v.mipFilter;
            break;
        case 3:
            ++v.sAddress;
            break;
        case 4:
            ++v.tAddress;
            break;
        case 5:
            ++v.rAddress;
            break;
        case 6:
            ++v.maxAnisotropy;
            break;
        case 7:
            ++v.compare;
            break;
        case 8:
            ++v.border;
            break;
        case 9:
            v.lodMin = -0.0f;
            break;
        case 10:
            ++v.lodMax;
            break;
        case 11:
            v.normalized = !v.normalized;
            break;
        case 12:
            v.lodAverage = !v.lodAverage;
            break;
        }
        next.bindSampler(5, &v);
        changed.push_back(next);
    }
    auto memory = key;
    memory.bindMemory(2, 64);
    changed.push_back(memory);
    raw::ComputeKernelCache<int> cache;
    int state = 1, calls = 0;
    const auto make = [&] {
        ++calls;
        return raw::ComputeKernelResult<int>{&state, false};
    };
    cache.get(key, make);
    for (const auto &next : changed) {
        EXPECT_FALSE(next == key);
        cache.get(next, make);
    }
    EXPECT_EQ(calls, 1 + changed.size());
}

TEST(RawComputeLookup, WarmLookupDoesNotInvokeLoader)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
    int state = 1, calls = 0;
    for (int i = 0; i < 10000; ++i) {
        const auto result = cache.get(key, [&] {
            ++calls;
            return raw::ComputeKernelResult<int>{&state, false};
        });
        EXPECT_EQ(result.state, &state);
        EXPECT_FALSE(result.retry);
    }
    EXPECT_EQ(calls, 1);
}

TEST(RawComputeLookup, TransientFailureCanRecoverAndTerminalRefusalStaysCached)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
    int state = 1, calls = 0;
    auto make = [&] {
        ++calls;
        return raw::ComputeKernelResult<int>{calls == 1 ? nullptr : &state, calls == 1};
    };
    EXPECT_TRUE(cache.get(key, make).retry);
    EXPECT_EQ(cache.get(key, make).state, &state);
    EXPECT_EQ(cache.get(key, make).state, &state);
    EXPECT_EQ(calls, 2);
    key.threads[0] = 7;
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(cache
                      .get(key,
                           [&] {
                               ++calls;
                               return raw::ComputeKernelResult<int>{nullptr, false};
                           })
                      .state,
                  nullptr);
    EXPECT_EQ(calls, 3);
}

TEST(RawComputeLookup, SameKeySharesOneJobWhileAnotherKeyCanProgress)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
    int state = 1, other = 2;
    std::atomic<int> calls{0};
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto make = [&] {
        if (calls.fetch_add(1) == 0)
            entered.set_value();
        released.wait();
        return raw::ComputeKernelResult<int>{&state, false};
    };
    auto first = std::async(std::launch::async, [&] {
        return cache.get(key, make);
    });
    entered.get_future().wait();
    auto second = std::async(std::launch::async, [&] {
        return cache.get(key, make);
    });
    auto different = key;
    different.threads[0] = 2;
    EXPECT_EQ(cache
                  .get(different,
                       [&] {
                           return raw::ComputeKernelResult<int>{&other, false};
                       })
                  .state,
              &other);
    release.set_value();
    EXPECT_EQ(first.get().state, &state);
    EXPECT_EQ(second.get().state, &state);
    EXPECT_EQ(calls.load(), 1);
}

TEST(RawComputeLookup, UnwindingAJobDoesNotLeaveTheKeyPending)
{
    raw::ComputeKernelCache<int> cache;
    raw::ComputeSpecialization key;
    EXPECT_THROW(cache.get(key,
                           []() -> raw::ComputeKernelResult<int> {
                               throw std::runtime_error("compile");
                           }),
                 std::runtime_error);
    int state = 1;
    EXPECT_EQ(cache
                  .get(key,
                       [&] {
                           return raw::ComputeKernelResult<int>{&state, false};
                       })
                  .state,
              &state);
}

TEST(RawComputeLookup, RejectsEdgeAndOutOfRangeGroupsBeforeLookup)
{
    auto regular = raw::computeDispatch({64, 32, 1}, {8, 4, 1}, true);
    EXPECT_EQ(regular.reason, raw::ComputeDispatch::Ready);
    EXPECT_EQ(regular.groups, (std::array<uint64_t, 3>{8, 8, 1}));
    auto edge = raw::computeDispatch({65, 32, 1}, {8, 4, 1}, true);
    EXPECT_EQ(edge.reason, raw::ComputeDispatch::Edge);
    EXPECT_EQ(edge.axis, 0);
    EXPECT_EQ(raw::computeDispatch({1, 1, 1}, {0, 1, 1}, true).reason, raw::ComputeDispatch::Edge);
    EXPECT_EQ(raw::computeDispatch({1, 65536, 1}, {1, 1, 1}, false).reason, raw::ComputeDispatch::Range);
    EXPECT_EQ(raw::computeDispatch({uint64_t(1) << 31, 1, 1}, {1, 1, 1}, false).reason, raw::ComputeDispatch::Range);
    EXPECT_EQ(raw::computeDispatch({0, 0, 0}, {8, 4, 1}, true).reason, raw::ComputeDispatch::Ready);
}

}  // namespace
