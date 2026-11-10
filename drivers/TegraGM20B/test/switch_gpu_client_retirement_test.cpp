// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include "bazel/testing/require.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"

using namespace std::chrono_literals;
constexpr uint32_t kMaxChannels = 8;
constexpr int THREAD_UNINT = 0;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNotOpen = -1, kIOReturnBadArgument = -2;

struct IOLock {
	std::mutex mutex;
	std::condition_variable changed;
};

void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.lock();
}

void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.unlock();
}

void IOLockSleep(IOLock *lock, void *, int) __attribute__((no_thread_safety_analysis))
{
	std::unique_lock<std::mutex> held(lock->mutex, std::adopt_lock);
	lock->changed.wait(held);
	held.release();
}

void IOLockWakeup(IOLock *lock, void *, bool)
{
	lock->changed.notify_all();
}

#define IODelete(p, type, count) \
	do {                     \
		(void)(count);   \
		delete (p);      \
		(p) = nullptr;   \
	} while (0)
using thread_call_param_t = void *;

struct Call {
	std::atomic<unsigned> entries{0};
	bool cancelled = false;
	std::mutex completionLock;
	std::condition_variable completion;
	unsigned running = 0;
};

using thread_call_t = Call *;
std::function<void()> onCallEnter;

void thread_call_enter(Call *call)
{
	REQUIRE(call != nullptr);
	if (onCallEnter)
		onCallEnter();
	++call->entries;
}

void thread_call_cancel_wait(Call *call)
{
	std::unique_lock held(call->completionLock);
	call->completion.wait(held, [&] {
		return !call->running;
	});
	call->cancelled = true;
}

struct tgm_chan {
	uint32_t completed = 0, submitted = 1, queue = 1;
	uint64_t generation = 1;
	int error = 0;
};

void switch_gpu_chan_next_token(tgm_chan *chan, switch_gpu_backing_token *out)
{
	*out = {uint32_t(chan->submitted + 1), chan->generation, SWITCH_GPU_ENGINE_GM20B,
		chan->queue};
}

int tgm_chan_error(tgm_chan *chan)
{
	return chan->error;
}

uint32_t tgm_chan_completed(tgm_chan *chan)
{
	return chan->completed;
}

void tgm_chan_del(tgm_chan **chan)
{
	*chan = nullptr;
}

struct switch_gpu_mapping {
	unsigned id;
};

std::function<void(switch_gpu_mapping *)> onUnmap;
std::vector<unsigned> unmapped;

void switch_gpu_vm_unmap(void *, switch_gpu_mapping **mapping)
{
	if (onUnmap)
		onUnmap(*mapping);
	unmapped.push_back((*mapping)->id);
	delete *mapping;
	*mapping = nullptr;
}

uint64_t nowNs()
{
	return 0;
}

void tgm_stats_bo_free(uint64_t)
{
}

void scheduleOrphanRelease()
{
}

using thread_t = void *;

thread_t current_thread()
{
	return nullptr;
}

class AGXDeviceUserClient
{
public:
	struct Bo {
		Bo *next = nullptr;
		uint64_t handle = 1, bytes = 4096, controlCharge = 0;
		void *pool_va = nullptr;
		bool chunked = false;
		uint32_t bindings = 0, flags = 0;
		Bo **zombieLink = nullptr;
	};

	struct RetiredBinding {
		RetiredBinding *next;
		uint64_t va, range;
		Bo *bo;
		switch_gpu_mapping *mapping;
		switch_gpu_backing_token snapshots[kMaxChannels];
		uint8_t kind;
	};

	struct Binding {
		uint64_t va, range;
		Bo *bo;
		uint64_t bo_offset;
		RetiredBinding *retirement;
	};

	struct Gate {
		bool value = false;

		bool closed() const
		{
			return value;
		}

		void close(IOLock *)
		{
			value = true;
		}
	} submitGate_;

	struct Surfaces {
		void clear()
		{
		}

		void remove(uint64_t)
		{
		}
	} surfaces_;

	IOLock storage;
	IOLock *lock_ = &storage;
	void *vm_ = nullptr;
	tgm_chan *chans_[kMaxChannels] = {};
	Bo *bos_ = nullptr, *zombies_ = nullptr, *readyBos_ = nullptr;
	RetiredBinding *retired_ = nullptr, **retiredTail_ = &retired_, *unmapping_ = nullptr;
	Call call;
	thread_call_t retireCall_ = &call;
	uint32_t retiredCount_ = 0, retireRemaining_ = 0, retirementPending_ = 0,
		 retirementStopped_ = 0;
	uint64_t retirementEpoch_ = 0, retireScanEpoch_ = 0, pool_bytes_ = 0, control_bytes_ = 0;
	Binding binds_[256] = {};
	uint32_t nbinds_ = 0;
	thread_t hangHolder_ = nullptr;
	std::atomic<unsigned> releases{0};
	unsigned waits = 0;
	std::function<void()> onWait, onRelease;

	void releaseMappings()
	{
	}

	void releaseCpuLeases()
	{
	}

	void releaseBo(Bo *bo)
	{
		if (onRelease)
			onRelease();
		++releases;
		delete bo;
	}

	IOReturn waitFence(const TegraGM20BAbiWaitFence *wait)
	{
		++waits;
		if (onWait)
			onWait();
		chans_[wait->channel - 1]->completed = wait->seqno;
		retirementFence(this);
		retireBatch();
		return 0;
	}

	void accountRelease(Bo *);
	void readyBo(Bo *);
	void queueZombie(Bo *);
	void scheduleRetirement();
	static void retirementFence(void *);
	static void retirementWork(thread_call_param_t, thread_call_param_t);
	bool bindingIdle(const RetiredBinding *) const;
	void retireBatch();
	IOReturn waitRetiredOverlaps(uint64_t, uint64_t);
	void reap(bool);
	void drainResources();
	IOReturn boFree(const TegraGM20BAbiBoFree *);
	void unbindOverlaps(uint64_t, uint64_t, Bo *);

	Bo *add(unsigned id, uint64_t va = 0x10000)
	{
		auto *bo = new Bo;
		bo->handle = id;
		bo->bindings = 1;
		bo->next = bos_;
		bos_ = bo;
		auto *record = new RetiredBinding{nullptr, va, 4096, bo, new switch_gpu_mapping{id},
						  {},      0};
		binds_[nbinds_++] = {va, 4096, bo, 0, record};
		return bo;
	}
};

#include "drivers/TegraGM20B/SwitchGpuClientRetirement.h"

class ClientRetirement : public testing::Test
{
protected:
	AGXDeviceUserClient client;
	tgm_chan chan;

	void SetUp() override
	{
		unmapped.clear();
		onUnmap = {};
		onCallEnter = {};
		client.chans_[0] = &chan;
	}

	void TearDown() override
	{
		onUnmap = {};
		onCallEnter = {};
		client.onRelease = {};
		client.drainResources();
	}

	void free(unsigned id)
	{
		IOLockLock(client.lock_);
		const TegraGM20BAbiBoFree request{id};
		ASSERT_EQ(client.boFree(&request), 0);
		IOLockUnlock(client.lock_);
	}
};

TEST_F(ClientRetirement, FreeKeepsMappingAndPagesUntilExactFenceCompletes)
{
	client.add(1);
	free(1);
	EXPECT_EQ(client.nbinds_, 0u);
	EXPECT_TRUE(unmapped.empty());
	client.retireBatch();
	EXPECT_TRUE(unmapped.empty());
	EXPECT_EQ(client.releases, 0u);
	chan.completed = chan.submitted;
	AGXDeviceUserClient::retirementFence(&client);
	client.retireBatch();
	EXPECT_EQ(unmapped, (std::vector<unsigned>{1}));
	EXPECT_EQ(client.releases, 1u);
}

TEST_F(ClientRetirement, ChannelReplacementCannotHoldAnOldMappingForever)
{
	client.add(2);
	free(2);
	++chan.generation;
	client.retireBatch();
	EXPECT_EQ(unmapped.size(), 1u);
	EXPECT_EQ(client.releases, 1u);
}

TEST_F(ClientRetirement, QuiescedFailureReleasesTheMapping)
{
	client.add(3);
	free(3);
	chan.error = 3;
	client.retireBatch();
	EXPECT_EQ(client.releases, 1u);
}

TEST_F(ClientRetirement, MultipleBindingsKeepTheirBoUntilLastRetirement)
{
	auto *bo = client.add(4);
	auto *record = new AGXDeviceUserClient::RetiredBinding{
		nullptr, 0x20000, 4096, bo, new switch_gpu_mapping{5}, {}, 0};
	client.binds_[client.nbinds_++] = {0x20000, 4096, bo, 0, record};
	++bo->bindings;
	free(4);
	chan.completed = 1;
	client.retireBatch();
	EXPECT_EQ(unmapped.size(), 2u);
	EXPECT_EQ(client.releases, 1u);
}

TEST_F(ClientRetirement, UnmapAndDestructionReleaseTheConnectionMutex)
{
	client.add(6);
	free(6);
	chan.completed = 1;
	unsigned checked = 0;
	const auto check = [&] {
		REQUIRE(client.lock_->mutex.try_lock());
		++checked;
		client.lock_->mutex.unlock();
	};
	onUnmap = [&](auto *) {
		check();
	};
	client.onRelease = check;
	client.retireBatch();
	EXPECT_EQ(checked, 2u);
}

TEST_F(ClientRetirement, NonoverlappingVaReuseDoesNotWaitForOldGpuWork)
{
	client.add(7);
	free(7);
	IOLockLock(client.lock_);
	EXPECT_EQ(client.waitRetiredOverlaps(0x20000, 4096), 0);
	IOLockUnlock(client.lock_);
	EXPECT_EQ(client.waits, 0u);
	EXPECT_TRUE(unmapped.empty());
}

TEST_F(ClientRetirement, OverlappingVaReuseWaitsOutsideTheConnectionMutex)
{
	client.add(8);
	free(8);
	client.onWait = [&] {
		REQUIRE(client.lock_->mutex.try_lock());
		client.lock_->mutex.unlock();
	};
	IOLockLock(client.lock_);
	EXPECT_EQ(client.waitRetiredOverlaps(0x10000, 4096), 0);
	IOLockUnlock(client.lock_);
	EXPECT_EQ(client.waits, 1u);
	EXPECT_EQ(unmapped.size(), 1u);
}

TEST_F(ClientRetirement, PhysicalUnmapRetainsVaQuarantineUntilItFinishes)
{
	client.add(9);
	free(9);
	chan.completed = 1;
	std::promise<void> started, resume;
	auto released = resume.get_future();
	onUnmap = [&](auto *) {
		started.set_value();
		released.wait();
	};
	auto worker = std::async(std::launch::async, [&] {
		client.retireBatch();
	});
	started.get_future().wait();
	auto reuse = std::async(std::launch::async, [&] {
		IOLockLock(client.lock_);
		const auto result = client.waitRetiredOverlaps(0x10000, 4096);
		IOLockUnlock(client.lock_);
		return result;
	});
	EXPECT_EQ(reuse.wait_for(10ms), std::future_status::timeout);
	resume.set_value();
	worker.get();
	EXPECT_EQ(reuse.get(), 0);
}

TEST_F(ClientRetirement, BoundedScanRotatesPendingHeadPastReadyTail)
{
	for (unsigned i = 0; i < 130; ++i) {
		client.add(10 + i, 0x10000 + i * 4096);
		free(10 + i);
	}
	unsigned at = 0;
	for (auto *record = client.retired_; record; record = record->next)
		if (++at == 130)
			record->snapshots[0].value = 0;
	client.retireBatch();
	EXPECT_EQ(client.retireRemaining_, 66u);
	EXPECT_EQ(client.releases, 0u);
	client.retireBatch();
	EXPECT_EQ(client.retireRemaining_, 2u);
	client.retireBatch();
	EXPECT_EQ(client.releases, 1u);
	EXPECT_EQ(client.retiredCount_, 129u);
}

TEST_F(ClientRetirement, CloseStopsCallbacksBeforeCancellingWorkAndReleasingMappings)
{
	client.add(150);
	free(150);
	client.drainResources();
	EXPECT_TRUE(client.call.cancelled);
	EXPECT_EQ(client.releases, 1u);
	const auto scheduled = client.call.entries.load();
	AGXDeviceUserClient::retirementFence(&client);
	EXPECT_EQ(client.call.entries.load(), scheduled);
}

TEST_F(ClientRetirement, CloseCannotPassTheWorkerReentryPublication)
{
	for (unsigned i = 0; i < 65; ++i) {
		client.add(200 + i, 0x10000 + i * 4096);
		free(200 + i);
	}
	std::promise<void> entering, resume;
	auto resumed = resume.get_future();
	onCallEnter = [&] {
		entering.set_value();
		resumed.wait();
	};
	auto worker = std::async(std::launch::async, [&] {
		client.retireBatch();
	});
	entering.get_future().wait();
	auto close = std::async(std::launch::async, [&] {
		client.drainResources();
	});
	EXPECT_EQ(close.wait_for(10ms), std::future_status::timeout);
	resume.set_value();
	worker.get();
	close.get();
	const auto entries = client.call.entries.load();
	client.retireBatch();
	EXPECT_EQ(client.call.entries.load(), entries);
	EXPECT_TRUE(client.call.cancelled);
}

TEST_F(ClientRetirement, CloseJoinsAnUnmapOutsideTheConnectionLock)
{
	client.add(31);
	free(31);
	chan.completed = chan.submitted;
	std::promise<void> entered, leave;
	auto mayLeave = leave.get_future().share();
	onUnmap = [&](auto *) {
		entered.set_value();
		mayLeave.wait();
	};
	auto worker = std::async(std::launch::async, [&] {
		{
			std::lock_guard held(client.call.completionLock);
			++client.call.running;
		}
		client.retireBatch();
		{
			std::lock_guard held(client.call.completionLock);
			--client.call.running;
		}
		client.call.completion.notify_all();
	});
	entered.get_future().wait();
	auto closing = std::async(std::launch::async, [&] {
		client.drainResources();
	});
	EXPECT_EQ(closing.wait_for(20ms), std::future_status::timeout);
	EXPECT_EQ(client.releases.load(), 0u);
	leave.set_value();
	worker.get();
	closing.get();
	EXPECT_EQ(client.releases.load(), 1u);
	EXPECT_EQ(unmapped, (std::vector<unsigned>{31}));
	EXPECT_EQ(client.unmapping_, nullptr);
	EXPECT_TRUE(client.call.cancelled);
}

TEST(ClientControlRetirement, ControlQuotaRetiresWithActualPoolOwnership)
{
    AGXDeviceUserClient client;
    AGXDeviceUserClient::Bo bo;
    bo.pool_va = reinterpret_cast<void *>(1);
    bo.flags = TegraGM20BBoControl;
    client.pool_bytes_ = bo.bytes;
    client.control_bytes_ = bo.controlCharge = 16384;
    client.accountRelease(&bo);
    EXPECT_EQ(client.pool_bytes_, 0u);
    EXPECT_EQ(client.control_bytes_, 0u);
}
