// SPDX-License-Identifier: MIT
// SwitchGpuBoLimits.h and its wiring in BoImport, BoAlloc, spares and GetInfo: large private textures (such as a
// 16384x16384x6 R8Unorm cube, 1536 MiB) pass the cached BoAlloc and BoImport caps, while pool/coherent/compressible
// allocations and MTLDevice.maxBufferLength keep the 256 MiB bound and oversized cached BOs bypass the spare pool.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "../SwitchGpuBoLimits.h"

namespace
{
using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

constexpr uint64_t kCubeBytes = 16384ULL * 16384ULL * 6ULL;  // 1536 MiB

TEST(SwitchGpuBoLimits, ImportAdmitsLargeTexturesUpToTwoGiBAndRefusesBadArgs)
{
	EXPECT_TRUE(switch_gpu_bo_import_args_ok(0xd2ac00000ULL, kCubeBytes));
	EXPECT_TRUE(switch_gpu_bo_import_args_ok(0x100000000ULL, SWITCH_GPU_MAX_BO_BYTES));
	EXPECT_FALSE(switch_gpu_bo_import_args_ok(0x100000000ULL, SWITCH_GPU_MAX_BO_BYTES + 4096));
	EXPECT_FALSE(switch_gpu_bo_import_args_ok(0x100000000ULL, 0));
	EXPECT_FALSE(switch_gpu_bo_import_args_ok(0x100000001ULL, 4096));
	EXPECT_FALSE(switch_gpu_bo_import_args_ok(0x100000000ULL, 4097));
	EXPECT_FALSE(switch_gpu_bo_import_args_ok(UINT64_MAX - 4095ULL, 8192));
}

TEST(SwitchGpuBoLimits, AllocAdmitsLargePlainCachedBosAndKeepsFlaggedBosAt256MiB)
{
	EXPECT_EQ(switch_gpu_bo_alloc_limit(0u), SWITCH_GPU_MAX_BO_BYTES);
	EXPECT_TRUE(switch_gpu_bo_alloc_args_ok(kCubeBytes, 16, 0u));
	EXPECT_TRUE(switch_gpu_bo_alloc_args_ok(SWITCH_GPU_MAX_BO_BYTES, 12, 0u));
	EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(SWITCH_GPU_MAX_BO_BYTES + 4096, 12, 0u));

	for (uint32_t flags : {uint32_t(TegraGM20BBoCoherent), uint32_t(TegraGM20BBoContiguous),
			       uint32_t(TegraGM20BBoCompressible),
			       uint32_t(TegraGM20BBoCoherent | TegraGM20BBoControl)}) {
		EXPECT_EQ(switch_gpu_bo_alloc_limit(flags), SWITCH_GPU_MAX_BUFFER_BYTES) << flags;
		EXPECT_TRUE(switch_gpu_bo_alloc_args_ok(SWITCH_GPU_MAX_BUFFER_BYTES, 16, flags))
			<< flags;
		EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(
			SWITCH_GPU_MAX_BUFFER_BYTES + SWITCH_GPU_BO_CHUNK_BYTES, 16, flags))
			<< flags;
	}

	EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(0, 12, 0u));
	EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(4097, 12, 0u));
	EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(4096, 11, 0u));
	EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(4096, 22, 0u));
	EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(4096, 12, TegraGM20BBoControl));
	EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(4096, 12, TegraGM20BBoCompressible));
	EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(SWITCH_GPU_BO_CHUNK_BYTES, 12,
						 TegraGM20BBoCompressible | TegraGM20BBoCoherent));
	EXPECT_FALSE(switch_gpu_bo_alloc_args_ok(4096, 12, 1u << 31));
}

TEST(SwitchGpuBoLimits, UserClientCallsPolicyWithExplicitRefusalsAndGuardsSpares)
{
	const CodeText import = Block(SourceText("../TegraGM20BUserClientImport.cpp"),
				      "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_TRUE(import.has("if (!switch_gpu_bo_import_args_ok(in->address, in->bytes))"));
	EXPECT_TRUE(import.has("switch_refusal( \"bo-import\", \"arguments\","));

	const CodeText alloc = Block(SourceText("../TegraGM20BUserClientBo.cpp"),
				     "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_TRUE(alloc.has(
		"if (!switch_gpu_bo_alloc_args_ok(in->bytes, in->align_log2, in->flags))"));
	EXPECT_TRUE(alloc.has("switch_refusal(\"bo-alloc\", \"arguments\","));

	const CodeText keep = Block(SourceText("../TegraGM20BUserClientSpares.cpp"),
				    "bool spareKeep(const SwitchGpuCachedBuffer &buffer,");
	EXPECT_TRUE(keep.has("if (bytes > kSpareCapBytes || classUp(bytes) >= kClasses)"));

	const CodeText info = Block(SourceText("../TegraGM20BUserClient.cpp"),
				    "IOReturn AGXDeviceUserClient::getInfo(");
	EXPECT_TRUE(info.has("out->max_bo_bytes = kMaxBoBytes;"));
}
}  // namespace
