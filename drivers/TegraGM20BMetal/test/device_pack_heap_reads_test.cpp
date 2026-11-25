// SPDX-License-Identifier: MIT
// A pack hit reads none of PackHeap's pages on the CPU: no process keeps them resident (the GPU draws them from the
// kernel's image), so a read is an SD page-in on the creating thread. The test protects the mapping's heap pages after
// the device opens and counts each first touch.
#include "facade_harness.h"
#include "pack_fixture_image.h"
#include "../pack_window.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::atomic<uintptr_t> gBegin{0}, gEnd{0};
std::atomic<uint32_t> gTouches{0};
uintptr_t gPage = 0;
struct sigaction gPreviousBus, gPreviousSegv;

void onFault(int sig, siginfo_t *info, void *)
{
    const uintptr_t at = uintptr_t(info->si_addr);
    if (at >= gBegin.load() && at < gEnd.load()) {
        gTouches.fetch_add(1);
        mprotect(reinterpret_cast<void *>(at & ~(gPage - 1)), gPage, PROT_READ);
        return;
    }
    sigaction(sig, sig == SIGBUS ? &gPreviousBus : &gPreviousSegv, nullptr);
}

// Counts the CPU's first touch of each page of [begin, end) from its making to its end
class HeapTouches {
public:
    HeapTouches(const uint8_t *begin, const uint8_t *end)
    {
        gPage = uintptr_t(getpagesize());
        const uintptr_t from = (uintptr_t(begin) + gPage - 1) & ~(gPage - 1);
        const uintptr_t to = uintptr_t(end) & ~(gPage - 1);
        from_ = from;
        bytes_ = to > from ? to - from : 0;
        gTouches = 0;
        gBegin = from;
        gEnd = from + bytes_;
        struct sigaction a = {};
        a.sa_sigaction = onFault;
        a.sa_flags = SA_SIGINFO;
        sigemptyset(&a.sa_mask);
        sigaction(SIGBUS, &a, &gPreviousBus);
        sigaction(SIGSEGV, &a, &gPreviousSegv);
        if (bytes_)
            mprotect(reinterpret_cast<void *>(from_), bytes_, PROT_NONE);
    }

    ~HeapTouches()
    {
        if (bytes_)
            mprotect(reinterpret_cast<void *>(from_), bytes_, PROT_READ);
        gBegin = gEnd = 0;
        sigaction(SIGBUS, &gPreviousBus, nullptr);
        sigaction(SIGSEGV, &gPreviousSegv, nullptr);
    }

    uint64_t bytes() const
    {
        return bytes_;
    }

    uint32_t touches() const
    {
        return gTouches.load();
    }

private:
    uintptr_t from_ = 0;
    uint64_t bytes_ = 0;
};

constexpr std::array<const char *, 6> kTier0Keys = {
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v v  | f f",
    "samples=2 topo=0 ds=0/0 c0=80:15:1:4:5:0:4:5:0  | v blend_v  | f blend_f",
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v tess_v  | f tess_f",
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v fetch_v  | f fetch_f",
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v var_v  | f var_f",
    "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v stride_v  | f stride_f",
};

TEST(PackHeapReads, ATier0FirstHitAndItsVariantsReadNoHeapPage)
{
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    GTEST_SKIP() << "the fault counter's signal handler is not TSan's to run";
#endif
#endif
    const std::vector<uint8_t> file = pack_fixture::readFileBytes(pack_fixture::kPath);
    ASSERT_FALSE(file.empty());
    pack_fixture::configurePackImageFromBytes(file, true);
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(pack_fixture::kPath, 0, err, gm20b::Device::Role::Client)) << err;
    ASSERT_GT(dev.packWindowBytes(), 0u);

    const gm20b::Pack &pack = dev.pack();
    const uint8_t *heap = pack.section<uint8_t>(gm20b::PackHeap);
    std::vector<gm20b::Pipeline *> made;
    uint32_t touches = 0;
    {
        HeapTouches watch(heap, heap + pack.bytes(gm20b::PackHeap));
        ASSERT_GT(watch.bytes(), 0u);
        for (const char *key : kTier0Keys) {
            const uint64_t h = gm20b::Pack::fnv(key, strlen(key));
            const gm20b::PackRun run = pack.fastRun(h);
            ASSERT_GT(run.records, 0u) << key;
            made.push_back(dev.pipeline(run, h, gm20b::PackForm::Default, false));
            (void)dev.variantPipelines(run, false);
        }
        touches = watch.touches();
    }
    EXPECT_EQ(touches, 0u) << "heap pages the creations read on the CPU";
    for (size_t i = 0; i < made.size(); i++)
        EXPECT_NE(made[i], nullptr) << kTier0Keys[i];
    ASSERT_NE(made[4], nullptr);
    EXPECT_FALSE(made[4]->chosen.empty()) << "the variants' state places its program variants";
}

TEST(PackHeapReads, TheRenderServerWiresTheSegmentsWords)
{
    const std::vector<uint8_t> file = pack_fixture::readFileBytes(pack_fixture::kPath);
    ASSERT_FALSE(file.empty());
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(pack_fixture::kPath, 0, err)) << err;
    uint32_t wordsAt = 0, wordsBytes = 0;
    ASSERT_TRUE(pack.segmentWords(wordsAt, wordsBytes));
    ASSERT_GT(wordsBytes, 0u);
    const gm20b::PackHeader &h = *pack.header();
    const auto *base = reinterpret_cast<const uint8_t *>(pack.header());
    TegraGM20BAbiPackCpuRange ranges[gm20b::kPackWindowMaxCpuRanges] = {};
    const uint32_t count = gm20b::packWindowCollectCpuRanges(base, h, uint64_t(getpagesize()), ranges);
    ASSERT_GT(count, 0u);
    const uint64_t from = uint64_t(uintptr_t(base)) + h.section[gm20b::PackHeap].offset + wordsAt;
    const uint64_t to = from + wordsBytes;
    bool covered = false;
    for (uint32_t i = 0; i < count; i++)
        covered |= ranges[i].address <= from && to <= ranges[i].address + ranges[i].bytes;
    EXPECT_TRUE(covered) << "the segments' words block is among the wired ranges";
}

}  // namespace
