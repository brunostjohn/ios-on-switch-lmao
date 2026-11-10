// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include "bazel/testing/require.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"
#include "drivers/TegraGM20B/SwitchGpuBindings.h"

using namespace std::chrono_literals;
constexpr uint32_t kMaxChannels = 8;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = -2;

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
	uint64_t generation = 1, fault = 0;
	int error = 0;
};

uint64_t tgm_chan_fault_va(tgm_chan *chan)
{
	return chan->fault;
}

std::vector<std::string> logs;

void IOLog(const char *format, ...)
{
	char line[256];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	logs.emplace_back(line);
}

IOReturn ioreturn(int error)
{
	return error;
}

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
	uint64_t va = 0, bytes = 0;
};

struct tgm_mem {};

// A mapping cut as SwitchGpuVmMap.h cuts one: the parts below, in and above the range, each NULL when empty.
std::vector<std::pair<uint64_t, uint64_t>> splits;
int splitError;

int switch_gpu_vm_split(void *, switch_gpu_mapping **mapping, tgm_mem *, uint8_t, int, int,
			uint64_t va, uint64_t range, switch_gpu_mapping **below,
			switch_gpu_mapping **taken, switch_gpu_mapping **above)
{
	if (splitError)
		return splitError;
	const auto *old = *mapping;
	const uint64_t end = va + range, oldEnd = old->va + old->bytes;
	const uint64_t low = std::max(old->va, va), high = std::min(oldEnd, end);
	*below = old->va < va ? new switch_gpu_mapping{old->id, old->va, va - old->va} : nullptr;
	*taken = new switch_gpu_mapping{old->id, low, high - low};
	*above = oldEnd > end ? new switch_gpu_mapping{old->id, end, oldEnd - end} : nullptr;
	splits.push_back({va, range});
	delete *mapping;
	*mapping = nullptr;
	return 0;
}

int switch_gpu_mapping_span(const switch_gpu_mapping *mapping, uint64_t *va, uint64_t *bytes)
{
	if (!mapping)
		return -22;
	*va = mapping->va;
	*bytes = mapping->bytes;
	return 0;
}

void switch_gpu_mapping_describe(const switch_gpu_mapping *mapping, char *out, size_t size)
{
	snprintf(out, size, "%#llx+%#llx/12", (unsigned long long)mapping->va,
		 (unsigned long long)mapping->bytes);
}

std::function<void(switch_gpu_mapping *)> onUnmap;
std::vector<unsigned> unmapped;

void switch_gpu_vm_unmap(void *, switch_gpu_mapping **mapping)
{
	if (!*mapping)
		return;
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
		bool chunked = false, readOnly = false;
		tgm_mem *mem = nullptr;
		uint32_t bindings = 0, flags = 0;
		Bo **zombieLink = nullptr;
	};

	struct RetiredBinding {
		RetiredBinding *next;
		uint64_t va, range;
		Bo *bo;
		switch_gpu_mapping *mapping;
		switch_gpu_backing_token snapshots[kMaxChannels];
		uint8_t kind, vol;
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

	struct Service {
		void stop()
		{
		}
	} events_, resources_;

	struct EventChannel {
		void reset()
		{
		}
	} eventChannels_[kMaxChannels];

	struct Admission {
		void end()
		{
		}
	} admissions_[kMaxChannels];

	uint64_t admissionCookies_[kMaxChannels] = {};
	IOLock storage;
	IOLock *lock_ = &storage;
	void *vm_ = nullptr;
	tgm_chan *chans_[kMaxChannels] = {};
	Bo *bos_ = nullptr, *zombies_ = nullptr, *readyBos_ = nullptr;
	RetiredBinding *retired_ = nullptr, **retiredTail_ = &retired_, *unmapping_ = nullptr;
	IOLock unmapStorage;
	IOLock *unmapLock_ = &unmapStorage;
	bool faultLogged_[kMaxChannels] = {};
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
	void takeRetiredOverlaps(uint64_t, uint64_t);
	void retire(RetiredBinding *);
	IOReturn cutOverlaps(uint64_t, uint64_t, bool, RetiredBinding **);
	void logFaultBindings();
	void reap(bool);
	void drainResources();
	IOReturn boFree(const TegraGM20BAbiBoFree *);
	void unbindOverlaps(uint64_t, uint64_t, Bo *);

	Bo *add(unsigned id, uint64_t va = 0x10000)
	{
		return add(id, va, 4096);
	}

	Bo *add(unsigned id, uint64_t va, uint64_t bytes)
	{
		auto *bo = new Bo;
		bo->handle = id;
		bo->bytes = bytes;
		bo->bindings = 1;
		bo->next = bos_;
		bos_ = bo;
		auto *record = new RetiredBinding{
			nullptr, va, bytes, bo, new switch_gpu_mapping{id, va, bytes}, {}, 0, 0};
		binds_[nbinds_++] = {va, bytes, bo, 0, record};
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
		nullptr, 0x20000, 4096, bo, new switch_gpu_mapping{5, 0x20000, 4096}, {}, 0, 0};
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

TEST_F(ClientRetirement, NonoverlappingVaReuseLeavesOldPtesToRetirement)
{
	client.add(7);
	free(7);
	IOLockLock(client.lock_);
	client.takeRetiredOverlaps(0x20000, 4096);
	IOLockUnlock(client.lock_);
	EXPECT_EQ(client.waits, 0u);
	EXPECT_TRUE(unmapped.empty());
}

// A Map over a range whose old binding's GPU work is still pending takes its PTEs at once and never waits for the GPU
// (VmBind holds the connection's mutation gate); the old pages stay until that work completes.
TEST_F(ClientRetirement, OverlappingVaReuseTakesOldPtesWithoutWaitingForTheGpu)
{
	client.add(8);
	free(8);
	IOLockLock(client.lock_);
	client.takeRetiredOverlaps(0x10000, 4096);
	IOLockUnlock(client.lock_);
	EXPECT_EQ(client.waits, 0u);
	EXPECT_EQ(unmapped, (std::vector<unsigned>{8}));
	client.retireBatch();
	EXPECT_EQ(client.releases, 0u);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_EQ(client.releases, 1u);
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
		client.takeRetiredOverlaps(0x10000, 4096);
		IOLockUnlock(client.lock_);
		return 0;
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

namespace
{
using RetiredBinding = AGXDeviceUserClient::RetiredBinding;

RetiredBinding *spareRecord()
{
	return new RetiredBinding{};
}
}  // namespace

// Unmapping a page out of a three-page binding leaves the two others bound and mapped.
TEST_F(ClientRetirement, SubRangeUnmapKeepsTheRestOfTheBindingMapped)
{
	auto *bo = client.add(20, 0x10000, 0x3000);
	RetiredBinding *spare[2] = {spareRecord(), spareRecord()};
	IOLockLock(client.lock_);
	EXPECT_EQ(client.cutOverlaps(0x11000, 0x1000, false, spare), 0);
	IOLockUnlock(client.lock_);
	for (auto *each : spare)
		delete each;
	ASSERT_EQ(client.nbinds_, 2u);
	EXPECT_EQ(client.binds_[0].va, 0x10000u);
	EXPECT_EQ(client.binds_[0].range, 0x1000u);
	EXPECT_EQ(client.binds_[1].va, 0x12000u);
	EXPECT_EQ(client.binds_[1].range, 0x1000u);
	EXPECT_EQ(client.binds_[1].bo_offset, 0x2000u);
	EXPECT_EQ(bo->bindings, 3u);
	EXPECT_TRUE(unmapped.empty());
	ASSERT_NE(client.retired_, nullptr);
	EXPECT_EQ(client.retired_->va, 0x11000u);
	EXPECT_EQ(client.retired_->range, 0x1000u);
	chan.completed = chan.submitted;
	client.retireBatch();
	EXPECT_EQ(unmapped.size(), 1u);
	EXPECT_EQ(bo->bindings, 2u);
	EXPECT_EQ(client.releases, 0u);
}

// A Map over the middle of a binding takes that part's PTEs now, keeps both sides, and needs both spare records.
TEST_F(ClientRetirement, MapOverTheMiddleOfABindingTakesOnlyThatPart)
{
	client.add(21, 0x10000, 0x4000);
	RetiredBinding *spare[2] = {spareRecord(), spareRecord()};
	IOLockLock(client.lock_);
	EXPECT_EQ(client.cutOverlaps(0x11000, 0x2000, true, spare), 0);
	IOLockUnlock(client.lock_);
	EXPECT_EQ(spare[0], nullptr);
	EXPECT_EQ(spare[1], nullptr);
	EXPECT_EQ(client.nbinds_, 2u);
	EXPECT_EQ(unmapped.size(), 1u);
	EXPECT_EQ(client.waits, 0u);
}

TEST_F(ClientRetirement, FailedCutChangesNothing)
{
	client.add(22, 0x10000, 0x3000);
	RetiredBinding *spare[2] = {spareRecord(), spareRecord()};
	splitError = -22;
	IOLockLock(client.lock_);
	EXPECT_EQ(client.cutOverlaps(0x11000, 0x1000, true, spare), -22);
	IOLockUnlock(client.lock_);
	splitError = 0;
	for (auto *each : spare)
		delete each;
	ASSERT_EQ(client.nbinds_, 1u);
	EXPECT_EQ(client.binds_[0].range, 0x3000u);
	EXPECT_EQ(client.retired_, nullptr);
}

TEST_F(ClientRetirement, KilledChannelLogsTheBindingOverItsFaultOnce)
{
	client.add(23, 0x10000, 0x3000);
	chan.error = 1;
	chan.fault = 0x11800;
	logs.clear();
	IOLockLock(client.lock_);
	client.logFaultBindings();
	client.logFaultBindings();
	IOLockUnlock(client.lock_);
	ASSERT_EQ(logs.size(), 1u);
	EXPECT_NE(logs[0].find("fault at 0x11800: binding 0x10000+0x3000 (handle 23, pages "
			       "0x10000+0x3000/12)"),
		  std::string::npos);
	chan.error = 0;
}
