// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include <memory>
#include "switch_gpu_copy_stubs.h"
#include "drivers/TegraGM20B/TegraGM20BCopy.cpp"

struct tgm_vm {};

struct tgm_mem {
	const uint64_t *pages;
};

struct tgm_chan {
	uint32_t submitted = 0, completed = 0;
	bool failed = false;
};

struct switch_gpu_mapping {
	uint64_t address;
};

struct switch_gpu_backing {
	std::vector<uint64_t> pages;
	switch_gpu_backing_layout layout{};
	uint64_t id = 0, generation = 1;
	unsigned refs = 1;
};

namespace
{
tgm_vm space;
tgm_chan gpu;
volatile uint32_t fence;
unsigned maps, unmaps, execs, commits, aborts, tokenRefs, roomWaits, adoptions, binds;
int nextError, guardError, semanticStatus;
unsigned memoryCalls, memoryFailure, poolFrees;
bool locked, entered, early, capture, covered;
uint32_t leaseCount;
void (*irq)(void *);
void *irqContext;
std::map<uint64_t, switch_gpu_backing *> allocations;

void pump()
{
	if (queue.retirement && queue.retirement->pending) {
		queue.retirement->pending = false;
		queue.retirement->fn(nullptr, nullptr);
	}
}

void finish(int semantic = 1)
{
	gpu.completed = gpu.submitted;
	semanticStatus = semantic;
	if (irq)
		irq(irqContext);
	pump();
}
}

extern "C" void *tgm_pool_alloc(uint64_t bytes, uint64_t, uint64_t *pa)
{
	*pa = 0x100000;
	return calloc(1, size_t(bytes));
}

extern "C" void tgm_pool_free(void *va, uint64_t, uint64_t)
{
	++poolFrees;
	free(va);
}

extern "C" void tgm_vm_del(tgm_vm **out)
{
	*out = nullptr;
}

extern "C" int tgm_fence_kernel(unsigned, volatile uint32_t **cpu, uint64_t *page, uint32_t *offset)
{
	*cpu = &fence;
	*page = 0x200000;
	*offset = 0;
	return 0;
}

extern "C" int tgm_vm_new(tgm_vm **out)
{
	*out = &space;
	return 0;
}

extern "C" void tgm_vm_claim(tgm_vm *, const void *)
{
}

extern "C" int tgm_mem_new(tgm_vm *, const uint64_t *pages, uint32_t, tgm_mem **out)
{
	if (++memoryCalls == memoryFailure)
		return -12;
	*out = new tgm_mem{pages};
	return 0;
}

extern "C" void tgm_mem_del(tgm_mem **p)
{
	delete *p;
	*p = nullptr;
}

extern "C" int tgm_vm_map_ordered(tgm_vm *, uint64_t, uint64_t, tgm_mem *, uint64_t, uint8_t, int,
				  uint8_t *)
{
	return 0;
}

extern "C" int tgm_vm_kmap(tgm_vm *, tgm_mem *, uint64_t, uint64_t *va)
{
	*va = 0x300000;
	return 0;
}

extern "C" int switch_gpu_vm_map(tgm_vm *, uint64_t va, uint64_t, tgm_mem *, uint64_t, uint8_t, int,
				 switch_gpu_mapping **out)
{
	++maps;
	*out = new switch_gpu_mapping{va};
	return 0;
}

extern "C" void switch_gpu_vm_unmap(tgm_vm *, switch_gpu_mapping **p)
{
	if (*p) {
		++unmaps;
		delete *p;
		*p = nullptr;
	}
}

extern "C" int tgm_chan_new_copy(tgm_vm *, const void *, volatile uint32_t *, uint64_t,
				 const char *, int, tgm_chan **out)
{
	gpu = {};
	*out = &gpu;
	return 0;
}

extern "C" void tgm_chan_del(tgm_chan **p)
{
	*p = nullptr;
}

extern "C" uint32_t tgm_chan_error(tgm_chan *p)
{
	return p->failed;
}

extern "C" void tgm_chan_on_fence(tgm_chan *, void (*fn)(void *), void *p)
{
	irq = fn;
	irqContext = p;
}

extern "C" void switch_gpu_chan_submit_enter(tgm_chan *)
{
	EXPECT_FALSE(entered);
	entered = true;
}

extern "C" void switch_gpu_chan_submit_leave()
{
	EXPECT_FALSE(locked);
	entered = false;
}

extern "C" void switch_gpu_chan_next_token(tgm_chan *, switch_gpu_backing_token *out)
{
	*out = {gpu.submitted + 1, 1, SWITCH_GPU_ENGINE_GM20B, 1};
}

extern "C" int switch_gpu_chan_exec_visibility(tgm_chan *, const tgm_push *, uint32_t,
					       const switch_gpu_backing_token *, uint32_t,
					       uint32_t *sequence, const tgm_prof *, uint32_t flags)
{
	EXPECT_TRUE(entered);
	EXPECT_TRUE(locked);
	EXPECT_GT(binds, 0u);
	EXPECT_TRUE(flags & SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY);
	++execs;
	if (nextError) {
		int result = nextError;
		nextError = 0;
		return result;
	}
	*sequence = ++gpu.submitted;
	if (early) {
		gpu.completed = gpu.submitted;
		irq(irqContext);
	}
	return 0;
}

extern "C" int switch_gpu_chan_wait_submission_room(tgm_chan *, const tgm_push *, uint32_t,
						    uint32_t, const tgm_prof *)
{
	EXPECT_FALSE(locked);
	++roomWaits;
	return 0;
}

extern "C" switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t pa)
{
	auto i = allocations.find(pa);
	if (i == allocations.end())
		return nullptr;
	++i->second->refs;
	return i->second;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *p)
{
	if (p)
		++p->refs;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *p)
{
	if (p)
		--p->refs;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *p, switch_gpu_backing_info *out)
{
	*out = {};
	out->allocation_id = p->id;
	out->allocation_generation = p->generation;
	out->page_count = uint32_t(p->pages.size());
	out->pages = p->pages.data();
	out->layout = p->layout;
}

extern "C" switch_gpu_backing *switch_gpu_backing_register(const uint64_t *pages, uint32_t,
							   uint32_t, void *owner,
							   void (*release)(void *))
{
	auto *p = switch_gpu_backing_find_pa(pages[0]);
	if (p)
		release(owner);
	return p;
}

extern "C" int switch_gpu_backing_token_retain(const switch_gpu_backing_token *)
{
	++tokenRefs;
	return 0;
}

extern "C" void switch_gpu_backing_token_release(const switch_gpu_backing_token *)
{
	--tokenRefs;
}

extern "C" int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *p)
{
	return p->value > gpu.completed ? 0 : semanticStatus;
}

extern "C" int switch_gpu_backing_begin_checked(switch_gpu_backing_use *uses, uint32_t count,
						const switch_gpu_backing_guard *, uint32_t,
						const switch_gpu_backing_token *token,
						switch_gpu_backing_transaction *out)
{
	if (guardError)
		return guardError;
	EXPECT_TRUE(entered);
	EXPECT_FALSE(locked);
	locked = true;
	*out = {uses, count, count, count, 0, *token};
	return 0;
}

extern "C" int switch_gpu_backing_begin_conversion_checked(
	switch_gpu_backing_use *uses, uint32_t count, switch_gpu_cpu_lease *const *,
	uint32_t leases, const switch_gpu_backing_guard *guards, uint32_t ng,
	const switch_gpu_backing_token *token, switch_gpu_backing_transaction *out)
{
	leaseCount = leases;
	return switch_gpu_backing_begin_checked(uses, count, guards, ng, token, out);
}

extern "C" int switch_gpu_backing_dependencies(const switch_gpu_backing_transaction *,
					       switch_gpu_backing_token *out, uint32_t,
					       uint32_t *count)
{
	EXPECT_TRUE(locked);
	*count = 1;
	out[0] = {1, 2, SWITCH_GPU_ENGINE_VIC, 9};
	return 0;
}

extern "C" void switch_gpu_backing_commit(switch_gpu_backing_transaction *p)
{
	EXPECT_TRUE(locked);
	EXPECT_EQ(p->count, 2u);
	++commits;
	locked = false;
	*p = {};
}

extern "C" void switch_gpu_backing_abort(switch_gpu_backing_transaction *p)
{
	if (p->locked) {
		++aborts;
		locked = false;
	}
	*p = {};
}

extern "C" tgm_coh *tgm_coh_find_pa(uint64_t)
{
	return nullptr;
}

extern "C" void tgm_coh_lock(tgm_coh *)
{
}

extern "C" void tgm_coh_unlock(tgm_coh *)
{
}

extern "C" tgm_tiled **tgm_coh_tiled(tgm_coh *)
{
	static tgm_tiled *p = nullptr;
	return &p;
}

extern "C" void tgm_coherency_put(tgm_coh *)
{
}

bool SwitchGpuEventChannel::initialize()
{
	return true;
}

void SwitchGpuEventChannel::reset()
{
}

int SwitchGpuEventJob::prepareKernel(const switch_gpu_event_submission *p)
{
	clear();
	borrowedSignals_ = p ? p->signals : nullptr;
	signalCount_ = p ? p->signal_count : 0;
	return 0;
}

int SwitchGpuEventJob::sequence(SwitchGpuEventChannel &)
{
	EXPECT_FALSE(locked);
	++adoptions;
	return 0;
}

int SwitchGpuEventJob::append(switch_gpu_backing_token *, uint32_t, uint32_t *) const
{
	return 0;
}

int SwitchGpuEventJob::captureDependencies(switch_gpu_backing_token *, uint32_t)
{
	return capture && !covered ? 1 : 0;
}

int SwitchGpuEventJob::inheritDependencies()
{
	EXPECT_FALSE(locked);
	covered = true;
	return 0;
}

int SwitchGpuEventJob::bind(const switch_gpu_backing_token *)
{
	EXPECT_TRUE(locked);
	++binds;
	return 0;
}

void SwitchGpuEventJob::commit()
{
	for (uint32_t i = 0; i < signalCount_; ++i)
		borrowedSignals_[i] = nullptr;
}

void SwitchGpuEventChannel::destroy()
{
}

void SwitchGpuEventJob::clear()
{
	borrowedSignals_ = nullptr;
	signalCount_ = 0;
}

namespace
{
class CheckedCopy : public testing::Test
{
protected:
	std::vector<std::unique_ptr<switch_gpu_backing>> owners;
	std::vector<std::unique_ptr<IOMemoryDescriptor>> descriptors;

	void SetUp() override
	{
		maps = unmaps = execs = commits = aborts = tokenRefs = roomWaits = adoptions =
			binds = leaseCount = 0;
		nextError = guardError = 0;
		memoryCalls = memoryFailure = poolFrees = 0;
		semanticStatus = 1;
		locked = entered = early = capture = covered = false;
		gpu = {};
		allocations.clear();
	}

	void TearDown() override
	{
		finish();
		switch_gpu_copy_mappings_clear(queue.vm, &queue.mappings);
		for (const auto &p : owners)
			EXPECT_EQ(p->refs, 1u);
		for (const auto &p : descriptors) {
			EXPECT_EQ(p->refs, 1u);
			EXPECT_EQ(p->prepared, 1u);
		}
		EXPECT_EQ(tokenRefs, 0u);
		EXPECT_EQ(maps, unmaps);
		tgm_mem_del(&queue.push);
		tgm_mem_del(&queue.fence);
		free(queue.words);
		free(queue.dependencies);
		if (queue.retirement)
			thread_call_free(queue.retirement);
		if (queue.lock)
			IOLockFree(queue.lock);
		queue.~Copier();
		new (&queue) Copier{};
	}

	std::pair<IOMemoryDescriptor *, switch_gpu_backing *> allocation(uint32_t prefix = 0)
	{
		auto b = std::make_unique<switch_gpu_backing>();
		b->id = owners.size() + 1;
		for (unsigned i = 0; i < 4; i++)
			b->pages.push_back((b->id << 28) + i * 4096);
		auto d = std::make_unique<IOMemoryDescriptor>();
		for (uint32_t i = 0; i < prefix; i++)
			d->pages.push_back(0x90000000 + i * 4096);
		d->pages.insert(d->pages.end(), b->pages.begin(), b->pages.end());
		d->length = d->pages.size() * 4096;
		auto result = std::pair(d.get(), b.get());
		allocations[b->pages[0]] = b.get();
		owners.push_back(std::move(b));
		descriptors.push_back(std::move(d));
		return result;
	}

	unsigned calls = 0;
	int callbackStatus = 42;

	static void callback(void *p, int status)
	{
		auto &f = *static_cast<CheckedCopy *>(p);
		++f.calls;
		f.callbackStatus = status;
	}

	int copy(std::pair<IOMemoryDescriptor *, switch_gpu_backing *> a,
		 std::pair<IOMemoryDescriptor *, switch_gpu_backing *> b,
		 uint64_t descriptorOffset = 0,
		 const switch_gpu_copy_ownership *ownership = nullptr)
	{
		tegra_gm20b_ce_plane src = {a.first, descriptorOffset, 256},
				     dst = {b.first, 0, 256};
		tegra_gm20b_ce_done done = {TEGRA_GM20B_CE_NO_SYNCPT, callback, this};
		return tegra_gm20b_ce_copy_plane_checked(&src, &dst, 256, 8, nullptr, &done,
							 a.second, 0, b.second, 0, ownership,
							 nullptr);
	}
};

TEST_F(CheckedCopy, WarmCopiesKeepPreparedDescriptorsAndMappings)
{
	auto a = allocation(), b = allocation();
	for (unsigned i = 0; i < 1000; i++) {
		ASSERT_EQ(copy(a, b), 0);
		finish();
	}
	EXPECT_EQ(maps, 2u);
	EXPECT_EQ(a.first->prepares, 1u);
	EXPECT_EQ(b.first->prepares, 1u);
	EXPECT_EQ(a.first->walks, 4u);
	EXPECT_EQ(calls, 1000u);
}

TEST_F(CheckedCopy, PhysicalCompletionWaitsForSemanticAuthority)
{
	auto a = allocation(), b = allocation();
	ASSERT_EQ(copy(a, b), 0);
	finish(0);
	EXPECT_EQ(calls, 0u);
	EXPECT_EQ(tokenRefs, 1u);
	EXPECT_EQ(queue.mappings.descriptors[0].users, 1u);
	semanticStatus = -5;
	switch_gpu_copy_completed();
	pump();
	EXPECT_EQ(calls, 1u);
	EXPECT_EQ(callbackStatus, -5);
	EXPECT_EQ(tokenRefs, 0u);
}

TEST_F(CheckedCopy, EarlyInterruptCannotLoseAcceptedLifetime)
{
	auto a = allocation(), b = allocation();
	early = true;
	ASSERT_EQ(copy(a, b), 0);
	EXPECT_EQ(calls, 0u);
	EXPECT_EQ(tokenRefs, 1u);
	pump();
	EXPECT_EQ(calls, 1u);
	EXPECT_EQ(tokenRefs, 0u);
}

TEST_F(CheckedCopy, RingWaitAndSemanticCaptureOccurOutsideOwnershipLocks)
{
	auto a = allocation(), b = allocation();
	nextError = -11;
	capture = true;
	ASSERT_EQ(copy(a, b), 0);
	EXPECT_EQ(roomWaits, 1u);
	EXPECT_TRUE(covered);
	EXPECT_EQ(commits, 1u);
	EXPECT_EQ(execs, 2u);
}

TEST_F(CheckedCopy, GuardFailurePublishesNothingAndCallsNobody)
{
	auto a = allocation(), b = allocation();
	guardError = SWITCH_GPU_BACKING_STALE;
	EXPECT_EQ(copy(a, b), SWITCH_GPU_BACKING_STALE);
	EXPECT_EQ(execs, 0u);
	EXPECT_EQ(calls, 0u);
	EXPECT_EQ(tokenRefs, 0u);
}

TEST_F(CheckedCopy, DescriptorBaseOffsetIsCheckedAgainstAllocationOrigin)
{
	auto a = allocation(1), b = allocation();
	ASSERT_EQ(copy(a, b, 4096), 0);
	finish();
	EXPECT_EQ(copy(a, b, 0), -22);
	EXPECT_EQ(execs, 1u);
}

TEST_F(CheckedCopy, OwnedConversionReservationReachesExactTransaction)
{
	auto a = allocation(), b = allocation();
	auto *lease = reinterpret_cast<switch_gpu_cpu_lease *>(uintptr_t(1));
	const switch_gpu_copy_ownership ownership = {&lease, 1, nullptr, 0};
	ASSERT_EQ(copy(a, b, 0, &ownership), 0);
	EXPECT_EQ(leaseCount, 1u);
}

TEST_F(CheckedCopy, RejectedLaterStageKeepsAcceptedPrefixAlive)
{
	auto a = allocation(), b = allocation();
	ASSERT_EQ(copy(a, b), 0);
	nextError = -5;
	EXPECT_EQ(copy(a, b), -5);
	EXPECT_EQ(tokenRefs, 1u);
	EXPECT_EQ(calls, 0u);
	finish();
	EXPECT_EQ(calls, 1u);
}
}

TEST_F(CheckedCopy, PartialInitializationReturnsItsControlAllocation)
{
	auto a = allocation(), b = allocation();
	memoryFailure = 2;
	EXPECT_EQ(copy(a, b), -12);
	EXPECT_EQ(poolFrees, 1u);
	EXPECT_EQ(queue.vm, nullptr);
	EXPECT_EQ(queue.push, nullptr);
	EXPECT_EQ(queue.fence, nullptr);
	EXPECT_EQ(queue.words, nullptr);
	EXPECT_EQ(queue.dependencies, nullptr);
	EXPECT_EQ(execs, 0u);
	EXPECT_EQ(copy(a, b), -12);
	EXPECT_EQ(poolFrees, 1u);
}
