// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "switch_gpu_relayout_control_stubs.h"
#include "drivers/TegraGM20B/TegraGM20BRelayout.cpp"
#include "gm20b_ce_model.h"

struct tgm_vm {};

struct tgm_mem {};

struct tgm_chan {};

namespace
{
std::vector<uint8_t> pushBytes(4096);
Memory memory;
tgm_vm space;
tgm_mem pushMemory, fenceMemory;
volatile uint32_t fence;
unsigned madeMemory;
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
	*out = &space;
	return 0;
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
	memory.regions[va] = &pushBytes;
	if (readOnly)
		memory.readOnly.insert(va);
	return 0;
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

TEST_F(RelayoutControl, ActualMappingAllowsGpuCheckpointWithoutChangingCommands)
{
	const uint32_t n =
		tgm_relayout_checkpoint(gRelayout.pushCpu, kPushVa + uint64_t(kCheckpointWord) * 4);
	const std::vector<uint8_t> commands(pushBytes.begin(), pushBytes.begin() + n * 4);
	CopyEngine ce(memory);
	ce.run(gRelayout.pushCpu, n);
	ASSERT_FALSE(ce.fault);
	EXPECT_EQ(gRelayout.pushCpu[kCheckpointWord], 1u);
	EXPECT_TRUE(std::equal(commands.begin(), commands.end(), pushBytes.begin()));
}

TEST_F(RelayoutControl, ReadOnlyMappingReproducesHostSemaphoreWriteFault)
{
	memory.readOnly.insert(kPushVa);
	const uint32_t n =
		tgm_relayout_checkpoint(gRelayout.pushCpu, kPushVa + uint64_t(kCheckpointWord) * 4);
	CopyEngine ce(memory);
	ce.run(gRelayout.pushCpu, n);
	EXPECT_TRUE(ce.fault);
	EXPECT_EQ(gRelayout.pushCpu[kCheckpointWord], 0u);
}
}  // namespace
