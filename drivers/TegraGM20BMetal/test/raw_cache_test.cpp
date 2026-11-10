// SPDX-License-Identifier: MIT
// raw/runtime/raw_cache.h and raw_metallib.h on made-up records and a made-up universal metallib (raw/test/cachecheck runs the
// same cache checks on a bucket's real records, and the AIR hashes on QuartzCore's own library).
#include "../raw/runtime/raw_cache.h"
#include "../raw/runtime/raw_metallib.h"
#include "../raw/runtime/raw_render_key.h"
#include "../raw/test/cache_checks.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

TEST(RenderCacheKey, PreservesFrameworkKeyAndFullPrimaryHashes)
{
    raw::AirHash vertex{}, fragment{};
    vertex[0] = 1;
    fragment[0] = 2;
    const auto key = raw::renderCacheKey(0x53, "descriptor", vertex, fragment, {}, false);
    EXPECT_EQ(key.key, "descriptor");
    EXPECT_EQ(key.stack, 0x53u);
    EXPECT_EQ(std::memcmp(key.vertexAir, vertex.data(), vertex.size()), 0);
    EXPECT_EQ(std::memcmp(key.fragmentAir, fragment.data(), fragment.size()), 0);
    vertex[31] = 7;
    EXPECT_NE(key.name(), raw::renderCacheKey(0x53, "descriptor", vertex, fragment, {}, false).name());
}

TEST(RenderCacheKey, PreservesOwnLibrarySuffix)
{
    raw::AirHash vertex{}, fragment{};
    for (unsigned i = 0; i < 32; ++i) {
        vertex[i] = i;
        fragment[i] = i + 32;
    }
    const auto key = raw::renderCacheKey(0x53, "descriptor", vertex, fragment, {}, true);
    EXPECT_EQ(key.key, "descriptor | air 0706050403020100 2726252423222120");
    EXPECT_NE(key.name(), raw::renderCacheKey(0x53, "descriptor", vertex, fragment, {}, false).name());
}

TEST(RenderCacheKey, LinkedStageAndDescriptorOrderRemainSignificant)
{
    raw::AirHash vertex{}, fragment{}, first{}, second{};
    first[0] = 1;
    second[0] = 2;
    const std::vector<raw::LinkedStageAir> linked{{'v', first}, {'f', second}};
    const auto key = raw::renderCacheKey(0x53, "descriptor", vertex, fragment, linked, false);
    EXPECT_EQ(key.key, "descriptor | linked v0000000000000001 f0000000000000002");
    EXPECT_NE(key.name(), raw::renderCacheKey(0x53, "descriptor", vertex, fragment,
                                              {{'f', second}, {'v', first}}, false).name());
    EXPECT_NE(key.name(), raw::renderCacheKey(0x53, "descriptor", vertex, fragment,
                                              {{'f', first}, {'f', second}}, false).name());
}

TEST(RenderCacheKey, ConstructionDoesNotAppendSuffixesToTheInput)
{
    const std::string base = "descriptor";
    raw::AirHash vertex{}, fragment{};
    const auto first = raw::renderCacheKey(0x53, base, vertex, fragment, {{'f', vertex}}, true);
    const auto second = raw::renderCacheKey(0x53, base, vertex, fragment, {{'f', vertex}}, true);
    EXPECT_EQ(base, "descriptor");
    EXPECT_EQ(first.key, second.key);
    EXPECT_EQ(first.name(), second.name());
    EXPECT_NE(first.name(), raw::renderCacheKey(0x54, base, vertex, fragment, {{'f', vertex}}, true).name());
}

raw::RuntimeRecord madeUp(uint32_t n)
{
    raw::RuntimeRecord r;
    r.pipeline.keyHash = 0x1000 + n;
    r.pipeline.vs = 0;
    r.pipeline.fs = 1;
    r.pipeline.entries = 2;
    r.pipeline.entry[0] = {0, raw::EntryUbo, 1, uint8_t(n), 0, 16, 0};
    r.pipeline.entry[1] = {16, raw::EntryTexture, 2, 3, raw::SamplerSlot, 3, 0};
    r.pipeline.pushMask = 1u << (n % 32);
    r.pipeline.pushWords[n % 32] = n;
    if (n % 2) {
        r.pipeline.flags |= raw::PipelinePushMetadata;
        r.pipeline.blendColorWord = 5;
        r.pipeline.halfBlendColorWord = 29;
        r.pipeline.pushMask |= 0xf0000ff0u;
    }
    r.keyString = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 " + std::to_string(n);
    for (int k = 0; k < 2; k++) {
        r.programs[k].id = std::to_string(n * 2 + k);
        r.programs[k].image.assign(80 + 64 * (n + 1), uint8_t(n + k));
        r.programs[k].cbufTypes = {1, 3};
    }
    r.programs[1].data.assign(256, 0x5A);
    r.programs[1].cbufTypes = {1, 2, 3};
    r.blob = {0x1A00, n, 0x1E08, n + 1};
    if (n % 3 == 0) {
        r.extras = {{0x1340, n}};
        r.restore = {{0x1340, 0}};
        r.samplers.push_back({n, 1, 2, 3, 4, 5, 6, 7});
    }
    r.splitTried = n % 2 == 0;
    if (n % 4 == 0) {
        r.split = true;
        r.splitRecord.thresholdPixels = 900 + n;
        r.splitRecord.frontierBytes = 48;
        r.splitRecord.frontierCbuf = 3;
        r.splitRecord.uboCount = 1;
        r.splitRecord.uboCbuf[0] = 1;
        r.splitRecord.prologueCbufs = 3;
        r.splitRecord.qmd[n % 64] = n;
        r.splitRecord.cost = {1, 0, 4000, 4000, 91 + n, 72 + n, 8 + n, 3 + n, 12};
        for (int k = 0; k < 2; k++) {
            r.splitPrograms[k].id = "split" + std::to_string(n * 2 + k);
            r.splitPrograms[k].image.assign(80 + 32 * (n + 1), uint8_t(n + 7 * k));
            r.splitPrograms[k].cbufTypes = {4, 1};
        }
    }
    return r;
}

void tag(std::vector<uint8_t> &out, const char *fourcc, const void *data, uint16_t length)
{
    out.insert(out.end(), fourcc, fourcc + 4);
    out.push_back(uint8_t(length));
    out.push_back(uint8_t(length >> 8));
    const auto *b = static_cast<const uint8_t *>(data);
    out.insert(out.end(), b, b + length);
}

template <class T> void put(std::vector<uint8_t> &out, size_t at, T v)
{
    memcpy(&out[at], &v, sizeof v);
}

void bigEndian(std::vector<uint8_t> &out, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        out[at + i] = uint8_t(v >> (24 - 8 * i));
}

// A universal file of an Apple GPU slice and an AIR64 slice whose function list names `functions`, each HASH a pattern of its
// index
std::vector<uint8_t> universalMetallib(const std::vector<std::string> &functions)
{
    std::vector<uint8_t> slice(0x58);
    memcpy(slice.data(), "MTLB", 4);
    std::vector<uint8_t> list(4);
    put(list, 0, uint32_t(functions.size()));
    for (size_t i = 0; i < functions.size(); i++) {
        std::vector<uint8_t> e(4);
        tag(e, "NAME", functions[i].c_str(), uint16_t(functions[i].size() + 1));
        const uint8_t type = 1;
        tag(e, "TYPE", &type, 1);
        raw::AirHash hash;
        hash.fill(uint8_t(0xA0 + i));
        tag(e, "HASH", hash.data(), uint16_t(hash.size()));
        e.insert(e.end(), {'E', 'N', 'D', 'T'});
        put(e, 0, uint32_t(e.size()));
        list.insert(list.end(), e.begin(), e.end());
    }
    put(slice, 24, uint64_t(slice.size()));
    put(slice, 32, uint64_t(list.size() - 4));
    slice.insert(slice.end(), list.begin(), list.end());
    std::vector<uint8_t> file(0x100, 0);
    bigEndian(file, 0, 0xCAFEBABE);
    bigEndian(file, 4, 2);
    bigEndian(file, 8, 0x01000013);  // an Apple GPU binary first, as the frameworks ship them
    bigEndian(file, 16, 0x100);
    bigEndian(file, 20, 16);
    bigEndian(file, 28, 0x01000017);
    bigEndian(file, 36, 0x110);
    bigEndian(file, 40, uint32_t(slice.size()));
    file.resize(0x110, 0xEE);
    file.insert(file.end(), slice.begin(), slice.end());
    return file;
}

TEST(RawMetallib, ReadsTheAirSlicesFunctionHashes)
{
    const std::vector<std::string> names = {"fixed_vert_lph_spc", "fixed_frag_lph_cph", "downsample_4_frag_lph"};
    const std::vector<uint8_t> file = universalMetallib(names);
    std::map<std::string, raw::AirHash> hashes;
    ASSERT_TRUE(raw::metallibAirHashes(file.data(), file.size(), hashes));
    ASSERT_EQ(hashes.size(), names.size());
    for (size_t i = 0; i < names.size(); i++) {
        raw::AirHash want;
        want.fill(uint8_t(0xA0 + i));
        EXPECT_EQ(hashes[names[i]], want) << names[i];
    }
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/universal.metallib";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    std::map<std::string, raw::AirHash> fromFile;
    ASSERT_TRUE(raw::metallibAirHashes(path, fromFile));
    EXPECT_EQ(fromFile, hashes);
    std::vector<uint8_t> cut(file.begin(), file.end() - 10);
    EXPECT_FALSE(raw::metallibAirHashes(cut.data(), cut.size(), hashes));
    EXPECT_TRUE(hashes.empty());
}

TEST(RawCache, KeepsRecordsAcrossProcessesAndMakesEachStateOnce)
{
    std::vector<std::pair<std::string, raw::RuntimeRecord>> records;
    for (uint32_t n = 0; n < 12; n++)
        records.push_back(
            {"samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v v" + std::to_string(n) + " | f f", madeUp(n)});
    cachechecks::run(records, "samples=1 | v two_draws | f f", "2 draws", getenv("TEST_TMPDIR") ?: "/tmp");
    EXPECT_EQ(cachechecks::failures, 0);
}

TEST(RawCache, KeepsKernelsAndTheirRefusals)
{
    const std::string dir = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/kernels";
    mkdir(dir.c_str(), 0700);
    raw::RecordCache cache;
    ASSERT_TRUE(cache.open(dir, 0x77, 1 << 20));
    raw::CacheKey made, refused;
    made.stack = refused.stack = 0x77;
    made.key = "kernel | 16ff | max=0 tgm=0 | tg=96,1,1 | tgmem= | tex= | smp=";
    refused.key = "kernel | 16ff | max=0 tgm=0 | tg=32,32,1 | tgmem= | tex= | smp=";
    raw::ComputeRecord r;
    r.kernel.keyHash = 0xABCD;
    r.kernel.regionWord = 4;
    r.kernel.entries = 2;
    r.kernel.entry[0] = {0, raw::KernelBuffer, 2, 0x3000040};
    r.kernel.entry[1] = {16, raw::KernelSampler, 0x80, 0};
    r.kernel.qmd[20] = 0x7;
    r.code.assign(320, 0x11);
    r.data.assign(256, 0x22);
    r.cbufType[0] = raw::CbufRoot;
    r.cbufType[1] = raw::CbufSet;
    r.cbufs = 2;
    r.keyString = made.key;
    r.samplers.push_back({1, 2, 3, 4, 5, 6, 7, 8});
    ASSERT_TRUE(cache.storeKernel(made, &r, ""));
    ASSERT_TRUE(cache.storeKernel(refused, nullptr, "a texture slot the dispatch leaves unbound at 0"));
    raw::ComputeRecord back;
    std::string why;
    ASSERT_EQ(cache.loadKernel(made, back, why), raw::RecordCache::Found::Record);
    EXPECT_EQ(memcmp(&back.kernel, &r.kernel, sizeof r.kernel), 0);
    EXPECT_EQ(back.code, r.code);
    EXPECT_EQ(back.data, r.data);
    EXPECT_EQ(memcmp(back.cbufType, r.cbufType, sizeof r.cbufType), 0);
    EXPECT_EQ(back.cbufs, r.cbufs);
    EXPECT_EQ(back.keyString, r.keyString);
    EXPECT_EQ(back.samplers, r.samplers);
    EXPECT_EQ(cache.loadKernel(refused, back, why), raw::RecordCache::Found::Refusal);
    EXPECT_EQ(why, "a texture slot the dispatch leaves unbound at 0");
    raw::RuntimeRecord state;
    EXPECT_EQ(cache.load(made, state, why), raw::RecordCache::Found::None);
}

// Cold work is deliberately held at a barrier: unrelated warm work and callbacks must progress before it is released.
TEST(RawCompiler, IndependentColdKeysOverlapWhileWarmKeysRemainReady)
{
    std::mutex lock;
    std::condition_variable changed;
    unsigned entered = 0, active = 0, maximum = 0;
    bool release = false;
    std::atomic<unsigned> makes{0}, callbacks{0};
    const std::string dir = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/parallel-cache";
    mkdir(dir.c_str(), 0700);
    raw::RecordCache cache;
    ASSERT_TRUE(cache.open(dir, 0x98, 1 << 20));
    raw::CacheKey warm;
    warm.stack = 0x98;
    warm.key = "warm";
    auto original = madeUp(1);
    ASSERT_TRUE(cache.store(warm, &original, ""));
    raw::Compiler compiler(&cache, [&](const raw::CacheKey &, raw::RuntimeRecord &record, std::string &) {
        makes++;
        std::unique_lock<std::mutex> guard(lock);
        ++entered;
        maximum = std::max(maximum, ++active);
        changed.notify_all();
        changed.wait(guard, [&] {
            return release;
        });
        --active;
        record = madeUp(2);
        return raw::Compiler::Outcome::Made;
    });
    raw::CacheKey cold[3];
    for (unsigned i = 0; i < 3; ++i) {
        cold[i].stack = 0x98;
        cold[i].key = "cold-" + std::to_string(i);
    }
    for (unsigned i = 0; i < 2; ++i)
        EXPECT_EQ(compiler.ask(cold[i], nullptr), raw::Compiler::State::Pending);
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return entered == 2;
        }));
    }
    // A warm lookup and an asynchronous warm ask both bypass the occupied compile slots.
    raw::RuntimeRecord loaded;
    std::string why;
    EXPECT_EQ(compiler.lookup(warm, loaded, why), raw::RecordCache::Found::Record);
    EXPECT_EQ(loaded.keyString, original.keyString);
    raw::CacheKey warmAsync = warm;
    warmAsync.key = "warm-async";
    EXPECT_TRUE(cache.store(warmAsync, &original, ""));
    std::atomic<bool> warmDone{false};
    compiler.ask(warmAsync, [&](const raw::RuntimeRecord *record, const std::string &) {
        EXPECT_NE(record, nullptr);
        std::lock_guard<std::mutex> guard(lock);
        warmDone = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return warmDone.load();
        }));
    }
    // Repeated requests share the running key, and a third cold key cannot exceed the worker bound.
    for (unsigned i = 0; i < 32; ++i)
        compiler.ask(cold[0], [&](const raw::RuntimeRecord *record, const std::string &) {
            EXPECT_NE(record, nullptr);
            callbacks++;
        });
    compiler.ask(cold[2], nullptr);
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
        changed.notify_all();
    }
    compiler.drain();
    EXPECT_EQ(makes.load(), 3u);
    EXPECT_EQ(callbacks.load(), 32u);
    EXPECT_EQ(maximum, 2u);
    // Drain includes background persistence, not just callback publication.
    raw::RecordCache reopened;
    ASSERT_TRUE(reopened.open(dir, 0x98, 1 << 20));
    for (const auto &key : cold)
        EXPECT_EQ(reopened.load(key, loaded, why), raw::RecordCache::Found::Record);
}

TEST(RawCompiler, ComputeTasksShareTheRenderWorkerLimitAndDrain)
{
    std::mutex lock;
    std::condition_variable changed;
    bool release = false;
    unsigned entered = 0, active = 0, maximum = 0;
    auto work = [&] {
        std::unique_lock<std::mutex> guard(lock);
        maximum = std::max(maximum, ++active);
        ++entered;
        changed.notify_all();
        changed.wait(guard, [&] { return release; });
        --active;
    };
    raw::Compiler compiler(nullptr, [&](const raw::CacheKey &, raw::RuntimeRecord &record, std::string &) {
        work();
        record = madeUp(1);
        return raw::Compiler::Outcome::Made;
    });
    compiler.enqueue(work);
    compiler.enqueue(work);
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] { return entered == 2; }));
    }
    raw::CacheKey key;
    key.key = "render-after-compute";
    compiler.ask(key, nullptr);
    compiler.enqueue(work);
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_FALSE(changed.wait_for(guard, std::chrono::milliseconds(50), [&] { return entered > 2; }));
        release = true;
        changed.notify_all();
    }
    compiler.drain();
    EXPECT_EQ(entered, 4u);
    EXPECT_EQ(active, 0u);
    EXPECT_EQ(maximum, 2u);
}

}  // namespace
