// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuSmmu.h"
#include "drivers/TegraGM20B/TegraGM20BComp.h"

namespace
{
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using dma_addr_t = uint64_t;

constexpr u8 PAGE_SHIFT = 12;
constexpr int GFP_KERNEL = 0;
constexpr int NVIF_CLASS_MEM_GF100 = 1;

struct nvif_mem_ram_v0 {
	u8 version;
	dma_addr_t *dma;
	void *sgl;
};

struct nvif_mem {
	u8 shift;
	u64 bytes;
	dma_addr_t *dma;
};

struct tgm_vm {};

struct tgm_mem {
	nvif_mem mem;
	u64 bytes;
	u8 page;
	const u64 *pages;
	u32 npages;
	u64 smmu_iova;
	u32 smmu_npages;
	u64 *smmu_pages;
	tgm_mem *later;
};

struct FakeLtc {
	u32 num_tags = 1024;
};

struct FakeDevice {
	FakeLtc ltcStorage;
	FakeLtc *ltc = &ltcStorage;
};

struct FakeGpu {
	int stage = 3;
	FakeDevice deviceStorage;
	FakeDevice *device = &deviceStorage;
	int mmu = 0;
	int mem_type = 0;
	int lock = 0;
} tgm_gpu;

unsigned gSmmuMaps, gSmmuUnmaps, gCtorCalls, gDtorCalls, gLtcInvalidates;
unsigned gLtcInvalidatesBeforeFirstUnmap, gLtcInvalidatesBeforeLastUnmap, gDtorsBeforeInvalidate;
uint64_t gNextIova = 0x10000000ULL;
uint64_t gSmmuFreeBytes = SWITCH_GPU_SMMU_MAX_WINDOW_BYTES;
uint64_t gLastReserveBytes;
uint32_t gLastMappedPages, gLastUnmappedPages;
int gCtorError;
bool gSmmuFail;
u8 gLastCtorPage;
const u64 *gLastCtorDma;

struct AnyPtr {
	void *p;

	template <typename T> operator T *() const
	{
		return static_cast<T *>(p);
	}

	explicit operator bool() const
	{
		return p != nullptr;
	}
};

AnyPtr kzalloc(size_t size, int)
{
	return {calloc(1, size)};
}

AnyPtr kmalloc_array(size_t n, size_t size, int)
{
	return {calloc(n, size)};
}

void kfree(void *ptr)
{
	free(ptr);
}

int stats_on()
{
	return 0;
}

u64 nvxnu_nanotime()
{
	return 0;
}

void tgm_gpu_lock()
{
}

void mutex_unlock(int *)
{
}

int nvif_mem_ctor_type(int *, const char *, int, int, u8 page, u64 bytes, nvif_mem_ram_v0 *args,
		       size_t, nvif_mem *out)
{
	++gCtorCalls;
	gLastCtorPage = page;
	gLastCtorDma = args->dma;
	if (gCtorError)
		return gCtorError;
	*out = {page, bytes, args->dma};
	return 0;
}

int l2_on()
{
	return 1;
}

// nvkm_ltc_invalidate (nouveau/nvkm/subdev/ltc/base.c) flushes before it invalidates
void nvkm_ltc_invalidate(FakeLtc *)
{
	if (!gLtcInvalidates++)
		gDtorsBeforeInvalidate = gDtorCalls;
}

void nvif_mem_dtor(nvif_mem *)
{
	++gDtorCalls;
}

void mem_prepare_big_views(tgm_mem *)
{
}

void mem_big_views_drop(tgm_mem *)
{
}

extern "C" uint64_t switch_gpu_smmu_map_reserved(const uint64_t *, uint32_t npages,
						 uint64_t reserve_bytes)
{
	++gSmmuMaps;
	gLastReserveBytes = reserve_bytes;
	gLastMappedPages = npages;
	const uint64_t bytes = ((uint64_t(npages) << 12) + SWITCH_GPU_SMMU_GRANULE - 1u) &
			       ~(SWITCH_GPU_SMMU_GRANULE - 1u);
	if (gSmmuFail || gSmmuFreeBytes < bytes || gSmmuFreeBytes - bytes < reserve_bytes)
		return 0;
	gSmmuFreeBytes -= bytes;
	const uint64_t iova = gNextIova;
	gNextIova += bytes + SWITCH_GPU_SMMU_GRANULE;
	return iova;
}

extern "C" uint64_t switch_gpu_smmu_map(const uint64_t *pas, uint32_t npages)
{
	return switch_gpu_smmu_map_reserved(pas, npages, 0);
}

extern "C" void switch_gpu_smmu_unmap(uint64_t, uint32_t npages)
{
	if (!gSmmuUnmaps)
		gLtcInvalidatesBeforeFirstUnmap = gLtcInvalidates;
	gLtcInvalidatesBeforeLastUnmap = gLtcInvalidates;
	++gSmmuUnmaps;
	gLastUnmappedPages = npages;
	const uint64_t bytes = ((uint64_t(npages) << 12) + SWITCH_GPU_SMMU_GRANULE - 1u) &
			       ~(SWITCH_GPU_SMMU_GRANULE - 1u);
	gSmmuFreeBytes += bytes;
}

#include "drivers/TegraGM20B/test/switch_gpu_mem_smmu_source.h"

class GpuMemSmmuTest : public testing::Test
{
protected:
	tgm_vm vm;

	void SetUp() override
	{
		tgm_gpu = {};
		tgm_gpu.device = &tgm_gpu.deviceStorage;
		tgm_gpu.deviceStorage.ltc = &tgm_gpu.deviceStorage.ltcStorage;
		gSmmuMaps = gSmmuUnmaps = gCtorCalls = gDtorCalls = gLtcInvalidates = 0;
		gLtcInvalidatesBeforeFirstUnmap = gLtcInvalidatesBeforeLastUnmap = gDtorsBeforeInvalidate =
			0;
		gNextIova = 0x10000000ULL;
		gSmmuFreeBytes = SWITCH_GPU_SMMU_MAX_WINDOW_BYTES;
		gLastReserveBytes = 0;
		gLastMappedPages = gLastUnmappedPages = 0;
		gCtorError = 0;
		gSmmuFail = false;
		gLastCtorPage = 0;
		gLastCtorDma = nullptr;
	}

	static std::vector<uint64_t> scattered(uint32_t npages)
	{
		std::vector<uint64_t> pas(npages);
		for (uint32_t i = 0; i < npages; ++i)
			pas[i] = 0x80001000ULL + uint64_t(i) * 0x2000ULL;
		return pas;
	}

	static std::vector<uint64_t> contiguousAligned(uint64_t base, uint32_t npages)
	{
		std::vector<uint64_t> pas(npages);
		for (uint32_t i = 0; i < npages; ++i)
			pas[i] = base + (uint64_t(i) << 12);
		return pas;
	}
};

TEST_F(GpuMemSmmuTest, NonExclusiveAndDirectExclusiveNeverAllocateSmmuIovas)
{
	const auto pages = scattered(32);
	tgm_mem *a = nullptr, *b = nullptr, *c = nullptr;
	uint64_t waited = 0;

	ASSERT_EQ(tgm_mem_new(&vm, pages.data(), 32, &a), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_EQ(a->smmu_iova, 0u);
	EXPECT_EQ(a->pages, pages.data());
	EXPECT_FALSE(tgm_mem_can_compress(a));
	EXPECT_FALSE(tgm_mem_is_smmu(a));

	ASSERT_EQ(tgm_mem_new_waited(&vm, pages.data(), 32, &b, &waited), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_EQ(b->smmu_iova, 0u);
	EXPECT_EQ(b->pages, pages.data());

	ASSERT_EQ(tgm_mem_new_exclusive(&vm, pages.data(), 32, &c), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_EQ(c->smmu_iova, 0u);
	EXPECT_FALSE(tgm_mem_can_compress(c));

	tgm_mem_del(&a);
	tgm_mem_del(&b);
	tgm_mem_del(&c);
	EXPECT_EQ(gSmmuUnmaps, 0u);
}

TEST_F(GpuMemSmmuTest, DirectExclusiveCompressesPhysicallyAlignedRunsWithoutSmmu)
{
	const auto aligned = contiguousAligned(0xe4000000ULL, 32);
	tgm_mem *mem = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive(&vm, aligned.data(), 32, &mem), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_EQ(mem->smmu_iova, 0u);
	EXPECT_TRUE(tgm_mem_can_compress(mem));
	EXPECT_EQ(mem->page, 16u);
	tgm_mem_del(&mem);
	EXPECT_EQ(gSmmuUnmaps, 0u);
}

TEST_F(GpuMemSmmuTest, ExclusiveSmmuCompactsOnly64KMultipleScatteredAndCleansUp)
{
	const auto panel = scattered(900);
	tgm_mem *panelMem = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, panel.data(), 900, &panelMem), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_FALSE(tgm_mem_can_compress(panelMem));
	tgm_mem_del(&panelMem);

	const auto aligned = contiguousAligned(0xe4000000ULL, 32);
	tgm_mem *directMem = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, aligned.data(), 32, &directMem), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_TRUE(tgm_mem_can_compress(directMem));
	EXPECT_FALSE(tgm_mem_is_smmu(directMem));
	tgm_mem_del(&directMem);

	const auto pages = scattered(32);
	tgm_mem *compMem = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, pages.data(), 32, &compMem), 0);
	EXPECT_EQ(gSmmuMaps, 1u);
	EXPECT_NE(compMem->smmu_iova, 0u);
	EXPECT_TRUE(tgm_mem_can_compress(compMem));
	EXPECT_TRUE(tgm_mem_is_smmu(compMem));
	EXPECT_EQ(compMem->page, 16u);
	EXPECT_TRUE(switch_gpu_smmu_is_iova(compMem->pages[0]));
	EXPECT_EQ(compMem->pages[16] - compMem->pages[0], 0x10000ULL);
	tgm_mem_del(&compMem);
	EXPECT_EQ(gSmmuUnmaps, 1u);
	EXPECT_EQ(gLtcInvalidatesBeforeLastUnmap, 1u);
}

// A retirement batch's memories: every GMMU mapping goes, one L2 invalidate, then every IOVA goes back. Memory without
// an IOVA is deleted at once.
TEST_F(GpuMemSmmuTest, ABatchSharesOneL2InvalidateBeforeItsIovasGoBack)
{
	const auto pages = scattered(32);
	tgm_mem *batch[3] = {};
	for (tgm_mem *&mem : batch)
		ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, pages.data(), 32, &mem), 0);
	tgm_mem *direct = nullptr;
	ASSERT_EQ(tgm_mem_new(&vm, pages.data(), 32, &direct), 0);
	ASSERT_FALSE(tgm_mem_is_smmu(direct));
	tgm_mem *later = nullptr;
	tgm_mem_del_later(&direct, &later);
	EXPECT_EQ(direct, nullptr);
	EXPECT_EQ(later, nullptr);
	EXPECT_EQ(gDtorCalls, 1u);
	for (tgm_mem *&mem : batch)
		tgm_mem_del_later(&mem, &later);
	EXPECT_EQ(gDtorCalls, 4u);
	EXPECT_EQ(gSmmuUnmaps, 0u);
	EXPECT_EQ(gLtcInvalidates, 0u);
	tgm_mem_del_finish(later);
	EXPECT_EQ(gLtcInvalidates, 1u);
	EXPECT_EQ(gDtorsBeforeInvalidate, 4u);
	EXPECT_EQ(gSmmuUnmaps, 3u);
	EXPECT_EQ(gLtcInvalidatesBeforeFirstUnmap, 1u);
	tgm_mem_del_finish(nullptr);
	EXPECT_EQ(gLtcInvalidates, 1u);
}

TEST_F(GpuMemSmmuTest, ExclusiveSmmuUnmapsOnCtorFailureAndSkipsWhenCompDisabled)
{
	const auto pages = scattered(16);
	tgm_mem *mem = nullptr;

	gCtorError = -ENOMEM;
	EXPECT_EQ(tgm_mem_new_exclusive_smmu(&vm, pages.data(), 16, &mem), -ENOMEM);
	EXPECT_EQ(mem, nullptr);
	EXPECT_EQ(gSmmuMaps, 1u);
	EXPECT_EQ(gSmmuUnmaps, 1u);

	gCtorError = 0;
	tgm_gpu.deviceStorage.ltcStorage.num_tags = 0;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, pages.data(), 16, &mem), 0);
	EXPECT_EQ(gSmmuMaps, 1u);
	EXPECT_FALSE(tgm_mem_can_compress(mem));
	tgm_mem_del(&mem);
}

TEST_F(GpuMemSmmuTest, SmmuBigCompactsScatteredPrefixAndKeepsDirectPaTail)
{
	// 720x1280 CA Framebuffer (900 pages = 56*16 + 4), 360 KiB (90 = 5*16 + 10),
	// 540 KiB (135 = 8*16 + 7), and an exact 64 KiB-multiple uncompressed buffer (32 pages).
	for (uint32_t npages : {900u, 90u, 135u, 32u}) {
		const uint32_t prefix = switch_gpu_smmu_prefix_pages(npages);
		const auto pages = scattered(npages);
		tgm_mem *mem = nullptr;
		ASSERT_EQ(tgm_mem_new_smmu_big(&vm, pages.data(), npages, &mem), 0) << npages;
		EXPECT_TRUE(tgm_mem_is_smmu(mem)) << npages;
		EXPECT_FALSE(tgm_mem_can_compress(mem)) << npages;
		EXPECT_EQ(mem->page, 12u) << npages;
		EXPECT_EQ(mem->smmu_npages, prefix) << npages;
		EXPECT_EQ(gLastMappedPages, prefix) << npages;
		EXPECT_EQ(gLastReserveBytes, SWITCH_GPU_SMMU_UNCOMPRESSED_RESERVE) << npages;
		for (uint32_t i = 0; i < prefix; ++i) {
			EXPECT_TRUE(switch_gpu_smmu_is_iova(mem->pages[i])) << npages << " @" << i;
			EXPECT_EQ(mem->pages[i],
				  switch_gpu_smmu_pa(mem->smmu_iova + (uint64_t(i) << 12)));
		}
		for (uint32_t i = prefix; i < npages; ++i) {
			EXPECT_FALSE(switch_gpu_smmu_is_iova(mem->pages[i])) << npages << " @" << i;
			EXPECT_EQ(mem->pages[i], pages[i]) << npages << " @" << i;
		}
		tgm_mem_del(&mem);
		EXPECT_EQ(gLastUnmappedPages, prefix) << npages;
	}
	EXPECT_EQ(gSmmuFreeBytes, SWITCH_GPU_SMMU_MAX_WINDOW_BYTES);
}

TEST_F(GpuMemSmmuTest, SmmuBigSkipsAlreadyAlignedRunsEvenWithCollidingBit16Parity)
{
	auto sameParity = contiguousAligned(0xe4000000ULL, 16);
	auto secondEven = contiguousAligned(0xe4040000ULL, 16);
	sameParity.insert(sameParity.end(), secondEven.begin(), secondEven.end());
	tgm_mem *mem = nullptr;
	ASSERT_EQ(tgm_mem_new_smmu_big(&vm, sameParity.data(), uint32_t(sameParity.size()), &mem),
		  0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_FALSE(tgm_mem_is_smmu(mem));
	EXPECT_FALSE(tgm_mem_can_compress(mem));
	tgm_mem_del(&mem);
	EXPECT_EQ(gSmmuUnmaps, 0u);
}

TEST_F(GpuMemSmmuTest, SmmuBigRespectsReserveHeadroomWithoutStarvingCompressible)
{
	gSmmuFreeBytes = SWITCH_GPU_SMMU_UNCOMPRESSED_RESERVE + SWITCH_GPU_SMMU_GRANULE;
	const auto twoBlocks = scattered(32);
	tgm_mem *fallback = nullptr;
	ASSERT_EQ(tgm_mem_new_smmu_big(&vm, twoBlocks.data(), 32, &fallback), 0);
	EXPECT_FALSE(tgm_mem_is_smmu(fallback)) << "128 KiB would dip below 128 MiB reserve";
	EXPECT_EQ(fallback->pages, twoBlocks.data());
	tgm_mem_del(&fallback);

	const auto oneBlock = scattered(16);
	tgm_mem *atBoundary = nullptr;
	ASSERT_EQ(tgm_mem_new_smmu_big(&vm, oneBlock.data(), 16, &atBoundary), 0);
	EXPECT_TRUE(tgm_mem_is_smmu(atBoundary)) << "64 KiB leaves exact 128 MiB reserve";
	EXPECT_EQ(gSmmuFreeBytes, SWITCH_GPU_SMMU_UNCOMPRESSED_RESERVE);

	tgm_mem *comp = nullptr;
	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, twoBlocks.data(), 32, &comp), 0);
	EXPECT_TRUE(tgm_mem_is_smmu(comp)) << "compressible still uses reserved headroom";
	EXPECT_TRUE(tgm_mem_can_compress(comp));

	tgm_mem_del(&comp);
	tgm_mem_del(&atBoundary);
}

TEST_F(GpuMemSmmuTest, SmmuRejectsDuplicatePhysicalPagesAndUnmapsPrefixOnCtorFailure)
{
	auto dupPrefix = scattered(32);
	dupPrefix[18] = dupPrefix[2];
	tgm_mem *mem = nullptr;
	ASSERT_EQ(tgm_mem_new_smmu_big(&vm, dupPrefix.data(), 32, &mem), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_FALSE(tgm_mem_is_smmu(mem));
	tgm_mem_del(&mem);

	ASSERT_EQ(tgm_mem_new_exclusive_smmu(&vm, dupPrefix.data(), 32, &mem), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_FALSE(tgm_mem_is_smmu(mem));
	tgm_mem_del(&mem);

	auto dupTail = scattered(900);
	dupTail[899] = dupTail[17];
	ASSERT_EQ(tgm_mem_new_smmu_big(&vm, dupTail.data(), 900, &mem), 0);
	EXPECT_EQ(gSmmuMaps, 0u);
	EXPECT_FALSE(tgm_mem_is_smmu(mem));
	tgm_mem_del(&mem);

	const auto panel = scattered(900);
	gCtorError = -ENOMEM;
	EXPECT_EQ(tgm_mem_new_smmu_big(&vm, panel.data(), 900, &mem), -ENOMEM);
	EXPECT_EQ(mem, nullptr);
	EXPECT_EQ(gSmmuMaps, 1u);
	EXPECT_EQ(gSmmuUnmaps, 1u);
	EXPECT_EQ(gLastMappedPages, 896u);
	EXPECT_EQ(gLastUnmappedPages, 896u);
	EXPECT_EQ(gSmmuFreeBytes, SWITCH_GPU_SMMU_MAX_WINDOW_BYTES);
}
}  // namespace
