// SPDX-License-Identifier: MIT
#include "switch_gpu_backing_fixture.h"
#include "switch_gpu_allocated_memory_peer.h"
#include "drivers/TegraGM20B/SwitchGpuAllocatedMemory.h"

struct tgm_mem {
	bool smmu;
	bool compressible;
	tgm_mem *later = nullptr;
};

namespace
{
std::atomic<unsigned> directNewCount, exclusiveNewCount, smmuNewCount, bigSmmuNewCount, delCount;
std::atomic<unsigned> laterCount, finishCount, ownerReleases, ownerReleasesAtFinish;
std::atomic<bool> bigSmmuFallbackToDirect{false};
std::mutex hookMutex;
std::function<void()> smmuHook;
}  // namespace

extern "C" int tgm_mem_new(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **pmem)
{
	++directNewCount;
	*pmem = new tgm_mem{false, false};
	return 0;
}

extern "C" int tgm_mem_new_exclusive(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **pmem)
{
	++exclusiveNewCount;
	*pmem = new tgm_mem{false, false};
	return 0;
}

extern "C" int tgm_mem_new_exclusive_smmu(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **pmem)
{
	++smmuNewCount;
	std::function<void()> hook;
	{
		std::lock_guard<std::mutex> guard(hookMutex);
		hook = smmuHook;
	}
	if (hook)
		hook();
	*pmem = new tgm_mem{true, true};
	return 0;
}

extern "C" int tgm_mem_new_smmu_big(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **pmem)
{
	++bigSmmuNewCount;
	std::function<void()> hook;
	{
		std::lock_guard<std::mutex> guard(hookMutex);
		hook = smmuHook;
	}
	if (hook)
		hook();
	*pmem = new tgm_mem{!bigSmmuFallbackToDirect.load(std::memory_order_acquire), false};
	return 0;
}

extern "C" int tgm_mem_can_compress(const tgm_mem *mem)
{
	return mem && mem->compressible ? 1 : 0;
}

extern "C" int tgm_mem_is_smmu(const tgm_mem *mem)
{
	return mem && mem->smmu ? 1 : 0;
}

extern "C" void tgm_mem_del(tgm_mem **pmem)
{
	if (!pmem || !*pmem)
		return;
	++delCount;
	delete *pmem;
	*pmem = nullptr;
}

extern "C" void tgm_mem_del_later(tgm_mem **pmem, tgm_mem **later)
{
	if (!*pmem || !(*pmem)->smmu) {
		tgm_mem_del(pmem);
		return;
	}
	++laterCount;
	(*pmem)->later = *later;
	*later = *pmem;
	*pmem = nullptr;
}

extern "C" void tgm_mem_del_finish(tgm_mem *later)
{
	if (!later)
		return;
	++finishCount;
	ownerReleasesAtFinish = ownerReleases.load();
	while (later) {
		tgm_mem *next = later->later;
		++delCount;
		delete later;
		later = next;
	}
}

namespace
{
class AllocatedMemoryCrossTu : public Backing
{
protected:
	void SetUp() override
	{
		Backing::SetUp();
		directNewCount = exclusiveNewCount = smmuNewCount = bigSmmuNewCount = delCount = 0;
		laterCount = finishCount = ownerReleases = ownerReleasesAtFinish = 0;
		bigSmmuFallbackToDirect.store(false, std::memory_order_release);
		std::lock_guard<std::mutex> guard(hookMutex);
		smmuHook = {};
	}

	void TearDown() override
	{
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = {};
		}
		Backing::TearDown();
		EXPECT_EQ(directNewCount.load() + exclusiveNewCount.load() + smmuNewCount.load() +
				  bigSmmuNewCount.load(),
			  delCount.load());
	}
};

// Backings retired together release their SMMU memories after one L2 invalidate (gpu_mem.c tgm_mem_del_finish), not
// one per backing, and their owners' pinned pages only after it: its flush and the SMMU unmaps must not reach a page
// back with the VM. A canonical memory released outside retirement goes at once.
TEST_F(AllocatedMemoryCrossTu, OneRetirementBatchSharesOneL2Invalidate)
{
	int owners[3] = {};
	switch_gpu_backing *made[3] = {};
	for (uint32_t i = 0; i < 3; ++i) {
		std::vector<uint64_t> pages;
		for (uint32_t p = 0; p < 16; ++p)
			pages.push_back(next_page += 4096);
		made[i] = switch_gpu_backing_register(pages.data(), 16, 0, &owners[i], [](void *) {
			++ownerReleases;
		});
		switch_gpu_backing *b = made[i];
		ASSERT_NE(b, nullptr);
		tgm_mem *mem = nullptr;
		ASSERT_EQ(allocatedMemorySmmu(b, 16, &mem), 0);
		ASSERT_TRUE(tgm_mem_is_smmu(mem));
	}
	for (switch_gpu_backing *b : made)
		switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(laterCount.load(), 3u);
	EXPECT_EQ(finishCount.load(), 1u);
	EXPECT_EQ(delCount.load(), 3u);
	EXPECT_EQ(ownerReleasesAtFinish.load(), 0u)
		<< "a pinned page went back before the batch's flush and unmaps";
	EXPECT_EQ(ownerReleases.load(), 3u);

	tgm_mem *alone = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(nullptr, nullptr, 16, &alone), 0);
	switch_gpu_allocated_memory_destroy(alone);
	EXPECT_EQ(laterCount.load(), 3u);
	EXPECT_EQ(delCount.load(), 4u);
}

TEST_F(AllocatedMemoryCrossTu, CrossTuCanonicalLookupReturnsSameObjectNeverEopnotsupp)
{
	EXPECT_EQ(peer_destroy_address(), &switch_gpu_allocated_memory_destroy);
	auto *b = backing(16);
	tgm_mem *fromPeer = nullptr;
	ASSERT_EQ(peer_allocated_memory_smmu(b, 16, &fromPeer), 0);
	ASSERT_NE(fromPeer, nullptr);
	EXPECT_TRUE(tgm_mem_is_smmu(fromPeer));
	EXPECT_TRUE(tgm_mem_can_compress(fromPeer));

	tgm_mem *fromMain = nullptr, *fromHeader = nullptr, *fromDirectCheck = nullptr;
	ASSERT_EQ(switch_gpu_backing_tgm_mem_existing(b, 16, &fromMain), 0);
	ASSERT_EQ(allocatedMemoryExisting(b, 16, &fromHeader), 0);
	ASSERT_EQ(switch_gpu_backing_tgm_mem_existing_or_direct(b, 16, &fromDirectCheck), 0);
	EXPECT_EQ(fromMain, fromPeer);
	EXPECT_EQ(fromHeader, fromPeer);
	EXPECT_EQ(fromDirectCheck, fromPeer);
	EXPECT_EQ(smmuNewCount.load(), 1u);
	EXPECT_EQ(directNewCount.load(), 0u);
	const auto write = token(701, 1);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, write), 0);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(delCount.load(), 0u);
	timelines[key(&write)].completed = 1;
	switch_gpu_backing_completed();
	switch_gpu_backing_retire();
	EXPECT_EQ(delCount.load(), 1u);
}

TEST_F(AllocatedMemoryCrossTu, DirectPaTouchRefusesSubsequentSmmuUpgradeAcrossTus)
{
	auto *b = backing(16);
	tgm_mem *existing = nullptr;
	EXPECT_EQ(peer_allocated_memory_existing_or_direct(b, 16, &existing), -2);
	EXPECT_EQ(existing, nullptr);

	tgm_mem *mem = nullptr;
	ASSERT_EQ(allocatedMemorySmmu(b, 16, &mem), 0);
	ASSERT_NE(mem, nullptr);
	EXPECT_FALSE(tgm_mem_is_smmu(mem));
	EXPECT_FALSE(tgm_mem_can_compress(mem));
	EXPECT_EQ(smmuNewCount.load(), 0u);
	EXPECT_EQ(directNewCount.load(), 1u);

	tgm_mem *fromPeer = nullptr;
	ASSERT_EQ(peer_allocated_memory_existing(b, 16, &fromPeer), 0);
	EXPECT_EQ(fromPeer, mem);
	switch_gpu_backing_release(b);
}

TEST_F(AllocatedMemoryCrossTu, ConcurrentDirectPaAndSmmuRaceNeverExposesBothAddressSpaces)
{
	// Deterministic race: peer latches direct PA while SMMU creation is paused mid-flight.
	{
		auto *b = backing(16);
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = [&] {
				tgm_mem *existing = nullptr;
				const int err =
					peer_allocated_memory_existing_or_direct(b, 16, &existing);
				EXPECT_EQ(err, -2);
				EXPECT_EQ(existing, nullptr);
			};
		}
		tgm_mem *mem = nullptr;
		ASSERT_EQ(allocatedMemorySmmu(b, 16, &mem), 0);
		ASSERT_NE(mem, nullptr);
		EXPECT_FALSE(tgm_mem_is_smmu(mem));
		EXPECT_FALSE(tgm_mem_can_compress(mem));
		EXPECT_EQ(smmuNewCount.load(), 1u);
		EXPECT_EQ(directNewCount.load(), 1u);
		EXPECT_EQ(delCount.load(), 1u);
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = {};
		}
		switch_gpu_backing_release(b);
	}

	// Parallel stress: two threads racing SMMU acquisition against direct-PA fallback.
	for (unsigned round = 0; round < 64; ++round) {
		auto *b = backing(16);
		std::atomic<bool> go{false};
		tgm_mem *smmuOut = nullptr, *directOut = nullptr;
		int smmuRet = -1, directRet = -1;
		std::thread t1([&] {
			while (!go.load(std::memory_order_acquire))
				;
			smmuRet = allocatedMemorySmmu(b, 16, &smmuOut);
		});
		std::thread t2([&] {
			while (!go.load(std::memory_order_acquire))
				;
			directRet = peer_allocated_memory_existing_or_direct(b, 16, &directOut);
		});
		go.store(true, std::memory_order_release);
		t1.join();
		t2.join();
		ASSERT_EQ(smmuRet, 0);
		ASSERT_NE(smmuOut, nullptr);
		if (directRet == 0) {
			EXPECT_EQ(directOut, smmuOut);
		} else {
			EXPECT_EQ(directRet, -2);
			EXPECT_FALSE(tgm_mem_is_smmu(smmuOut));
		}
		switch_gpu_backing_release(b);
	}
}

TEST_F(AllocatedMemoryCrossTu, CrossTuBigSmmuCanonicalLookupReturnsSameUncompressedObject)
{
	auto *b = backing(900);
	tgm_mem *fromPeer = nullptr;
	ASSERT_EQ(peer_allocated_memory_big(b, 900, &fromPeer), 0);
	ASSERT_NE(fromPeer, nullptr);
	EXPECT_TRUE(tgm_mem_is_smmu(fromPeer));
	EXPECT_FALSE(tgm_mem_can_compress(fromPeer));

	tgm_mem *fromMain = nullptr, *fromSubrange = nullptr, *fromDirectCheck = nullptr;
	ASSERT_EQ(allocatedMemoryBig(b, 900, &fromMain), 0);
	ASSERT_EQ(switch_gpu_backing_tgm_mem_existing(b, 16, &fromSubrange), 0);
	ASSERT_EQ(switch_gpu_backing_tgm_mem_existing_or_direct(b, 900, &fromDirectCheck), 0);
	EXPECT_EQ(fromMain, fromPeer);
	EXPECT_EQ(fromSubrange, fromPeer);
	EXPECT_EQ(fromDirectCheck, fromPeer);
	EXPECT_EQ(bigSmmuNewCount.load(), 1u);

	const auto write = token(702, 1);
	ASSERT_EQ(publish(b, SWITCH_GPU_BACKING_WRITE, write), 0);
	switch_gpu_backing_release(b);
	switch_gpu_backing_retire();
	EXPECT_EQ(delCount.load(), 0u);
	timelines[key(&write)].completed = 1;
	switch_gpu_backing_completed();
	switch_gpu_backing_retire();
	EXPECT_EQ(delCount.load(), 1u);
}

TEST_F(AllocatedMemoryCrossTu, BigSmmuReserveFallbackLatchesDirectPaAndNeverUpgrades)
{
	auto *b = backing(900);
	bigSmmuFallbackToDirect.store(true, std::memory_order_release);
	tgm_mem *first = nullptr;
	ASSERT_EQ(allocatedMemoryBig(b, 900, &first), 0);
	ASSERT_NE(first, nullptr);
	EXPECT_FALSE(tgm_mem_is_smmu(first));
	EXPECT_FALSE(tgm_mem_can_compress(first));

	bigSmmuFallbackToDirect.store(false, std::memory_order_release);
	tgm_mem *second = nullptr;
	ASSERT_EQ(peer_allocated_memory_big(b, 900, &second), 0);
	EXPECT_EQ(second, first);
	EXPECT_EQ(bigSmmuNewCount.load(), 1u);
	switch_gpu_backing_release(b);
}

TEST_F(AllocatedMemoryCrossTu, ConcurrentDirectPaAndBigSmmuRaceNeverExposesBothAddressSpaces)
{
	{
		auto *b = backing(900);
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = [&] {
				tgm_mem *existing = nullptr;
				EXPECT_EQ(
					peer_allocated_memory_existing_or_direct(b, 900, &existing),
					-2);
				EXPECT_EQ(existing, nullptr);
			};
		}
		tgm_mem *mem = nullptr;
		ASSERT_EQ(allocatedMemoryBig(b, 900, &mem), 0);
		ASSERT_NE(mem, nullptr);
		EXPECT_FALSE(tgm_mem_is_smmu(mem));
		EXPECT_EQ(bigSmmuNewCount.load(), 1u);
		EXPECT_EQ(directNewCount.load(), 1u);
		EXPECT_EQ(delCount.load(), 1u);
		{
			std::lock_guard<std::mutex> guard(hookMutex);
			smmuHook = {};
		}
		switch_gpu_backing_release(b);
	}

	for (unsigned round = 0; round < 64; ++round) {
		auto *b = backing(900);
		std::atomic<bool> go{false};
		tgm_mem *bigOut = nullptr, *directOut = nullptr;
		int bigRet = -1, directRet = -1;
		std::thread t1([&] {
			while (!go.load(std::memory_order_acquire))
				;
			bigRet = allocatedMemoryBig(b, 900, &bigOut);
		});
		std::thread t2([&] {
			while (!go.load(std::memory_order_acquire))
				;
			directRet = peer_allocated_memory_existing_or_direct(b, 900, &directOut);
		});
		go.store(true, std::memory_order_release);
		t1.join();
		t2.join();
		ASSERT_EQ(bigRet, 0);
		ASSERT_NE(bigOut, nullptr);
		if (directRet == 0) {
			EXPECT_EQ(directOut, bigOut);
		} else {
			EXPECT_EQ(directRet, -2);
			EXPECT_FALSE(tgm_mem_is_smmu(bigOut));
		}
		switch_gpu_backing_release(b);
	}
}
}  // namespace
