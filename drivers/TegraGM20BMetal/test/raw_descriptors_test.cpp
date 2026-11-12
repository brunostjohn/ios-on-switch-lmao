// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_descriptors.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

struct Uploads {
    std::vector<raw::DescriptorWords> images;

    uint64_t operator()(const raw::DescriptorWords &words)
    {
        images.push_back(words);
        return 0x100000000ULL + images.size() * sizeof(words);
    }
};

TEST(RawDescriptors, UnchangedDrawsShareOneImmutableUpload)
{
    raw::DescriptorSnapshot snapshot;
    raw::DescriptorWords words = {};
    words[0] = 0x12345678;
    words[1] = 3;
    words[2] = 256;
    words[4] = 17 | 29 << 20;
    Uploads uploads;
    const auto upload = [&](const raw::DescriptorWords &value) {
        return uploads(value);
    };
    const uint64_t first = snapshot.resolve(words, upload);
    ASSERT_NE(first, 0u);
    for (unsigned draw = 0; draw < 1000; ++draw)
        EXPECT_EQ(snapshot.resolve(words, upload), first);
    ASSERT_EQ(uploads.images.size(), 1u);
    EXPECT_EQ(uploads.images.front(), words);
}

TEST(RawDescriptors, EveryDescriptorWordCanInvalidateTheSnapshot)
{
    raw::DescriptorSnapshot snapshot;
    raw::DescriptorWords words = {};
    Uploads uploads;
    const auto upload = [&](const raw::DescriptorWords &value) {
        return uploads(value);
    };
    uint64_t previous = snapshot.resolve(words, upload);
    for (unsigned word = 0; word < words.size(); ++word) {
        words[word] = word + 1;
        const uint64_t next = snapshot.resolve(words, upload);
        EXPECT_NE(next, previous) << word;
        EXPECT_EQ(uploads.images.back(), words);
        previous = next;
    }
    EXPECT_EQ(uploads.images.size(), words.size() + 1);
    EXPECT_EQ(uploads.images.front(), raw::DescriptorWords{});
}

TEST(RawDescriptors, ReturningToEarlierBindingsDoesNotOverwriteAnInflightImage)
{
    raw::DescriptorSnapshot snapshot;
    raw::DescriptorWords first = {}, second = {};
    first[0] = 42;
    second[0] = 43;
    Uploads uploads;
    const auto upload = [&](const raw::DescriptorWords &value) {
        return uploads(value);
    };
    const uint64_t a = snapshot.resolve(first, upload);
    const uint64_t b = snapshot.resolve(second, upload);
    const uint64_t c = snapshot.resolve(first, upload);
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    ASSERT_EQ(uploads.images.size(), 3u);
    EXPECT_EQ(uploads.images[0], first);
    EXPECT_EQ(uploads.images[1], second);
    EXPECT_EQ(uploads.images[2], first);
}

TEST(RawDescriptors, FailedUploadDoesNotPublishOrReplaceCachedBytes)
{
    raw::DescriptorSnapshot snapshot;
    raw::DescriptorWords first = {}, changed = {};
    changed[63] = 11;
    Uploads uploads;
    const auto upload = [&](const raw::DescriptorWords &value) {
        return uploads(value);
    };
    const auto failure = [](const raw::DescriptorWords &) {
        return uint64_t{0};
    };
    EXPECT_EQ(snapshot.resolve(first, failure), 0u);
    const uint64_t address = snapshot.resolve(first, upload);
    EXPECT_EQ(snapshot.resolve(changed, failure), 0u);
    EXPECT_EQ(snapshot.resolve(first, upload), address);
    EXPECT_EQ(uploads.images.size(), 1u);
    EXPECT_NE(snapshot.resolve(changed, upload), address);
}

TEST(RawDescriptors, NewEncoderCannotReuseAnotherEncodersUpload)
{
    raw::DescriptorSnapshot first, second;
    raw::DescriptorWords words = {};
    Uploads uploads;
    const auto upload = [&](const raw::DescriptorWords &value) {
        return uploads(value);
    };
    EXPECT_NE(first.resolve(words, upload), second.resolve(words, upload));
    EXPECT_EQ(uploads.images.size(), 2u);
}

TEST(RawDescriptors, ConstantBufferBindingsTrackBothPipelineAndSnapshot)
{
    raw::DescriptorBinding binding;
    const int first = 1, second = 2;
    EXPECT_TRUE(binding.changed(&first, 256));
    EXPECT_FALSE(binding.changed(&first, 256));
    EXPECT_TRUE(binding.changed(&first, 512));
    EXPECT_TRUE(binding.changed(&second, 512));
    EXPECT_TRUE(binding.changed(&first, 512));
    EXPECT_FALSE(binding.changed(&first, 512));
}

TEST(RawDescriptors, UnchangedGenerationDoesNotRebuildOrUpload)
{
    raw::PreparedDescriptors cache;
    int pipeline = 0;
    unsigned builds = 0;
    Uploads uploads;
    const auto build = [&](raw::PreparedDescriptorWords &prepared) {
        ++builds;
        prepared.words[4] = 123;
        prepared.residuals[7] = 12;
        prepared.residualMask = 1u << 7;
        return true;
    };
    const auto upload = [&](const raw::DescriptorWords &words) {
        return uploads(words);
    };
    const auto *first = cache.resolve(&pipeline, 5, build, upload);
    ASSERT_NE(first, nullptr);
    const uint64_t address = first->address;
    for (unsigned draw = 0; draw < 10000; ++draw) {
        const auto *next = cache.resolve(&pipeline, 5, build, upload);
        ASSERT_NE(next, nullptr);
        EXPECT_EQ(next->address, address);
        EXPECT_EQ(next->residuals[7], 12u);
        EXPECT_EQ(next->residualMask, 1u << 7);
    }
    EXPECT_EQ(builds, 1u);
    EXPECT_EQ(uploads.images.size(), 1u);
}

TEST(RawDescriptors, PipelineAndBindingRevisionRebuildIndependently)
{
    raw::PreparedDescriptors cache;
    int first = 0, second = 0;
    unsigned builds = 0;
    Uploads uploads;
    const auto upload = [&](const raw::DescriptorWords &words) {
        return uploads(words);
    };
    const auto build = [&](raw::PreparedDescriptorWords &prepared) {
        prepared.words[0] = 33;
        prepared.residuals[2] = ++builds;
        prepared.residualMask = 4;
        return true;
    };
    ASSERT_NE(cache.resolve(&first, 1, build, upload), nullptr);
    EXPECT_EQ(cache.resolve(&first, 2, build, upload)->residuals[2], 2u);
    EXPECT_EQ(cache.resolve(&second, 2, build, upload)->residuals[2], 3u);
    EXPECT_EQ(cache.resolve(&first, 2, build, upload)->residuals[2], 4u);
    EXPECT_EQ(builds, 4u);
    EXPECT_EQ(uploads.images.size(), 1u);
}

TEST(RawDescriptors, FailedPreparationOrUploadDoesNotPoisonTheGeneration)
{
    raw::PreparedDescriptors cache;
    int pipeline = 0;
    unsigned builds = 0;
    Uploads uploads;
    const auto upload = [&](const raw::DescriptorWords &words) {
        return uploads(words);
    };
    const auto build = [&](raw::PreparedDescriptorWords &prepared) {
        ++builds;
        prepared.words[0] = 22;
        return true;
    };
    const auto failedBuild = [](raw::PreparedDescriptorWords &) {
        return false;
    };
    const auto failedUpload = [](const raw::DescriptorWords &) {
        return uint64_t(0);
    };
    EXPECT_EQ(cache.resolve(&pipeline, 1, failedBuild, upload), nullptr);
    EXPECT_TRUE(uploads.images.empty());
    EXPECT_EQ(cache.resolve(&pipeline, 1, build, failedUpload), nullptr);
    ASSERT_NE(cache.resolve(&pipeline, 1, build, upload), nullptr);
    EXPECT_EQ(builds, 2u);
    EXPECT_EQ(uploads.images.size(), 1u);
    EXPECT_EQ(cache.resolve(&pipeline, 2, failedBuild, upload), nullptr);
    ASSERT_NE(cache.resolve(&pipeline, 1, failedBuild, failedUpload), nullptr);
}

TEST(RawDescriptors, ADrawOfTheSamePipelineRebindsOnlyTheUniformBuffersThatMoved)
{
    raw::DescriptorBinding binding;
    int a = 0, b = 0;
    const uint32_t first[3] = {0x1000, 0, 256}, second[3] = {0x1100, 0, 256}, other[3] = {0x2000, 0, 64};
    EXPECT_TRUE(binding.changed(&a, 0x9000));
    EXPECT_TRUE(binding.uboMoved(0, first));
    EXPECT_TRUE(binding.uboMoved(1, other));
    EXPECT_FALSE(binding.changed(&a, 0x9000));
    EXPECT_FALSE(binding.uboMoved(0, first));
    EXPECT_FALSE(binding.uboMoved(1, other));
    EXPECT_TRUE(binding.changed(&a, 0x9100));
    EXPECT_TRUE(binding.uboMoved(0, second));
    EXPECT_FALSE(binding.uboMoved(1, other));
    EXPECT_TRUE(binding.changed(&b, 0x9100));
    EXPECT_TRUE(binding.uboMoved(0, second));
    EXPECT_TRUE(binding.uboMoved(1, other));
    EXPECT_TRUE(binding.changed(&a, 0x9100));
    EXPECT_TRUE(binding.uboMoved(0, second));
}

}  // namespace
