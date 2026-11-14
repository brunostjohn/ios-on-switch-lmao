// SPDX-License-Identifier: MIT
// Where the user client's memory work runs, which no host model of it runs: spares of 128 KiB units are made by the
// refill thread alone.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace
{
std::string unit(const char *name)
{
	return SourceText(std::string("../") + name);
}
}  // namespace

TEST(GM20BMemSource, OnlyTheRefillThreadMakesSparesOfUnits)
{
	const std::string pool = unit("TegraGM20BUserClientPool.cpp");
	const CodeText make = Block(pool, "Spare *spareMake(SparePool *p, uint64_t bytes)");
	EXPECT_TRUE(make.Before("switch_gpu_cached_buffer_units(bytes, &buffer)",
				"switch_gpu_cached_buffer_pages(bytes, &buffer)"));
	EXPECT_EQ(CodeText(pool).Count("spareMake("), 2u) << "its definition and spareWork's call";
	EXPECT_TRUE(Block(pool, "void spareWork(thread_call_param_t param, thread_call_param_t)")
			    .Has("spareMake(p, classBytes(c))"));
	const CodeText alloc =
		Block(unit("TegraGM20BUserClientBo.cpp"), "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_FALSE(alloc.Has("switch_gpu_cached_buffer_units"));
	EXPECT_TRUE(alloc.Has("if (in->flags & TegraGM20BBoContiguous) options |= "
			      "kIOMemoryPhysicallyContiguous;"));
	EXPECT_EQ(alloc.Count("kIOMemoryPhysicallyContiguous"), 1u);
}

TEST(GM20BMemSource, AUnitIsOneAlignedContiguousRun)
{
	const std::string buffer = unit("SwitchGpuCachedBuffer.cpp");
	const CodeText part = Block(buffer, "IOMemoryDescriptor *part(uint64_t bytes, bool unit)");
	EXPECT_TRUE(part.Has("unit ? kOptions | kIOMemoryPhysicallyContiguous : kOptions"));
	EXPECT_TRUE(part.Has("unit ? kReach & ~(SWITCH_GPU_SPARE_UNIT_BYTES - 1) : kReach"));
	EXPECT_TRUE(Block(buffer, "bool switch_gpu_cached_buffer_alone(")
			    .Has("getRetainCount() == (buffer.map ? 2 : 1)"));
}
