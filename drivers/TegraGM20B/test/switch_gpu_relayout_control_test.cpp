// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "switch_gpu_relayout_control_stubs.h"
#include "drivers/TegraGM20B/TegraGM20BRelayout.cpp"
#include "gm20b_ce_model.h"

struct tgm_vm {};

struct tgm_mem {};

struct tgm_chan {};

struct switch_gpu_mapping {};

namespace
{
std::vector<uint8_t> pushBytes(4096);
Memory memory;
tgm_vm space;
tgm_mem pushMemory, fenceMemory;
volatile uint32_t fence;
unsigned madeMemory, vmFailures, poolFrees;
bool pushVolatile;
switch_gpu_mapping pushWindow;
}

extern "C" void *tgm_pool_alloc(uint64_t bytes, uint64_t, uint64_t *pa)
{
	EXPECT_EQ(bytes, pushBytes.size());
	*pa = 0x100000;
	return pushBytes.data();
}

extern "C" int tgm_fence_kernel(unsigned, volatile uint32_t **cpu, uint64_t *page, uint32_t *offset)
{
	*cpu = &fence;
	*page = 0x200000;
	*offset = 128;
	return 0;
}

extern "C" int tgm_vm_new(tgm_vm **out)
{
	if (vmFailures) {
		--vmFailures;
		return -12;
	}
	*out = &space;
	return 0;
}

extern "C" void tgm_vm_del(tgm_vm **out)
{
	*out = nullptr;
}

extern "C" void tgm_mem_del(tgm_mem **out)
{
	*out = nullptr;
}

extern "C" void tgm_pool_free(void *, uint64_t, uint64_t)
{
	++poolFrees;
}

extern "C" void tgm_vm_claim(tgm_vm *, const void *)
{
}

extern "C" int tgm_mem_new(tgm_vm *, const uint64_t *, uint32_t, tgm_mem **out)
{
	*out = madeMemory++ ? &fenceMemory : &pushMemory;
	return 0;
}

extern "C" int tgm_vm_map_ordered(tgm_vm *, uint64_t va, uint64_t bytes, tgm_mem *mem,
				  uint64_t offset, uint8_t kind, int readOnly, uint8_t *)
{
	EXPECT_EQ(mem, &pushMemory);
	EXPECT_EQ(bytes, pushBytes.size());
	EXPECT_EQ(offset, 0u);
	EXPECT_EQ(kind, 0u);
	memory.map(va, pushBytes, readOnly != 0);
	return 0;
}

extern "C" int switch_gpu_vm_map_volatile(tgm_vm *vm, uint64_t va, uint64_t bytes, tgm_mem *mem,
					  uint64_t offset, uint8_t kind, int readOnly, int vol,
					  switch_gpu_mapping **out)
{
	pushVolatile = vol != 0;
	*out = &pushWindow;
	return tgm_vm_map_ordered(vm, va, bytes, mem, offset, kind, readOnly, nullptr);
}

extern "C" void switch_gpu_vm_unmap(tgm_vm *, switch_gpu_mapping **mapping)
{
	*mapping = nullptr;
}

extern "C" int tgm_vm_kmap(tgm_vm *, tgm_mem *, uint64_t, uint64_t *va)
{
	*va = 0x200000;
	return 0;
}

extern "C" uint32_t tgm_chan_error(tgm_chan *)
{
	return 0;
}

extern "C" void tgm_chan_del(tgm_chan **)
{
}

extern "C" int tgm_chan_new_copy(tgm_vm *, const void *, volatile uint32_t *, uint64_t,
				 const char *, int, tgm_chan **)
{
	return -12;
}

extern "C" uint32_t tgm_chan_uid(tgm_chan *)
{
	return 1;
}

extern "C" int tgm_chan_exec(tgm_chan *, const tgm_push *, uint32_t, uint32_t *, const tgm_prof *)
{
	return -12;
}

extern "C" void tgm_copy_wait(uint32_t, uint32_t, const tgm_relayout_writer *, uint32_t)
{
}

extern "C" int tgm_writer_done(uint32_t, uint32_t)
{
	return 1;
}

extern "C" int tgm_writer_fence_va(tgm_vm *, uint32_t, uint64_t *)
{
	return -22;
}

extern "C" void tgm_writer_wait(uint32_t, uint32_t)
{
}

extern "C" void tgm_cpu_clean_pages(const uint64_t *, uint32_t)
{
}

extern "C" vm_offset_t phystokv(uint64_t)
{
	return 0;
}

extern "C" void switch_refusal(const char *, const char *, const char *, ...)
{
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *)
{
}

switch_gpu_backing *switch_gpu_pin_memory(const uint64_t *, uint32_t, uint32_t,
					  IOMemoryDescriptor *, bool)
{
	return nullptr;
}

int switch_gpu_relayout_mapping(tgm_vm *, SwitchGpuRelayoutCache *, const uint64_t *, uint32_t,
				bool, SwitchGpuRelayoutMapping **)
{
	return -12;
}

void switch_gpu_relayout_mapping_put(SwitchGpuRelayoutMapping *)
{
}

void switch_gpu_relayout_cache_clear(tgm_vm *, SwitchGpuRelayoutCache *)
{
}

switch_scratch *switch_scratch_get(uint64_t, task_t)
{
	return nullptr;
}

void switch_scratch_put(switch_scratch *)
{
}

void switch_scratch_discard(switch_scratch *)
{
}

void switch_scratch_stats(uint64_t *, uint64_t *)
{
}

namespace
{
class RelayoutControl : public testing::Test
{
	void SetUp() override
	{
		gRelayout = {};
		memory = {};
		madeMemory = 0;
		std::fill(pushBytes.begin(), pushBytes.end(), 0);
		ASSERT_TRUE(openSpace());
	}
};

TEST_F(RelayoutControl, FailedOpenIsUndoneAndTheNextMoveOpensAgain)
{
	gRelayout = {};
	madeMemory = 0;
	vmFailures = 1;
	const unsigned frees = poolFrees, logged = relayoutLogs;
	EXPECT_EQ(channel(), nullptr);
	EXPECT_EQ(poolFrees, frees + 1);
	EXPECT_EQ(gRelayout.pushCpu, nullptr);
	EXPECT_EQ(relayoutLogs, logged + 1);
	EXPECT_EQ(channel(), nullptr);
	EXPECT_EQ(gRelayout.vm, &space);
	EXPECT_EQ(relayoutLogs, logged + 1);
}

TEST_F(RelayoutControl, ActualMappingAllowsGpuCheckpointWithoutChangingCommands)
{
	EXPECT_TRUE(pushVolatile);
	const uint32_t n =
		tgm_relayout_checkpoint(gRelayout.pushCpu, kPushVa + uint64_t(kCheckpointWord) * 4);
	const std::vector<uint8_t> commands(pushBytes.begin(), pushBytes.begin() + n * 4);
	CopyEngine ce(memory);
	ce.runAt(kPushVa, n);
	ASSERT_FALSE(ce.fault);
	EXPECT_EQ(gRelayout.pushCpu[kCheckpointWord], 1u);
	EXPECT_TRUE(std::equal(commands.begin(), commands.end(), pushBytes.begin()));
}

TEST_F(RelayoutControl, ReadOnlyMappingReproducesHostSemaphoreWriteFault)
{
	memory.map(kPushVa, pushBytes, true);
	const uint32_t n =
		tgm_relayout_checkpoint(gRelayout.pushCpu, kPushVa + uint64_t(kCheckpointWord) * 4);
	CopyEngine ce(memory);
	ce.runAt(kPushVa, n);
	EXPECT_TRUE(ce.fault);
	EXPECT_EQ(gRelayout.pushCpu[kCheckpointWord], 0u);
}

TEST_F(RelayoutControl, PushTheDriverNeverMappedFaultsBeforeItsFirstMethod)
{
	memory.unmap(kPushVa, pushBytes.size());
	const uint32_t n =
		tgm_relayout_checkpoint(gRelayout.pushCpu, kPushVa + uint64_t(kCheckpointWord) * 4);
	CopyEngine ce(memory);
	ce.runAt(kPushVa, n);
	EXPECT_TRUE(ce.fault);
	EXPECT_TRUE(ce.m.empty());
}
}  // namespace

namespace
{
TEST_F(RelayoutControl, MovesTheCpuFinishesAreCountedWithTheStats)
{
	uint64_t moves = 0, bytes = 0;
	tgm_relayout_cpu_stats(&moves, &bytes);
	const uint64_t pas[2] = {0x1000, 0x2000};
	const tgm_relayout_pages surface = {pas, 2};
	const tgm_layout layout = {};
	tgm_relayout_finish_cpu(&surface, &surface, &layout, 0, TGM_RELAYOUT_TO_ROWS);
	tgm_relayout_cpu_stats(&moves, &bytes);
	EXPECT_EQ(moves, TGM_STATS ? 1u : 0u);
	EXPECT_EQ(bytes, TGM_STATS ? 2u * 4096u : 0u);
}
}  // namespace
