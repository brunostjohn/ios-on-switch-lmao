// SPDX-License-Identifier: MIT
// Where the user client's memory work runs, which no host model of it runs: spares of 128 KiB units are made by the
// refill thread alone; BoAlloc and BoImport make their BO without the connection lock and publish it without closing the
// submit gate, a VmBind closes it only to cut, SubmitJob reads only published tables, and retirement frees what left
// them only after taking every submit lock once.
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

TEST(GM20BMemSource, BoAllocAndBoImportBuildOutsideTheConnectionLock)
{
	const CodeText run = Block(unit("TegraGM20BUserClientDispatch.cpp"),
				   "IOReturn AGXDeviceUserClient::run(");
	EXPECT_TRUE(run.Before("return boAlloc(", "switch_gpu_method_closes_gate(method)"));
	EXPECT_TRUE(run.Before("return boImport(", "switch_gpu_method_closes_gate(method)"));
	EXPECT_EQ(run.Count("boAlloc("), 1u);
	EXPECT_EQ(run.Count("boImport("), 1u);
	const CodeText import = Block(unit("TegraGM20BUserClientImport.cpp"),
				      "IOReturn AGXDeviceUserClient::boImport(");
	EXPECT_EQ(import.Count("IOLockLock(lock_);"), 1u);
	EXPECT_TRUE(import.Before("bo->wired->prepare()", "IOLockLock(lock_);"));
	EXPECT_TRUE(import.Before("tgm_mem_new_waited(", "IOLockLock(lock_);"));
	EXPECT_TRUE(import.Before("lap(3);", "IOLockLock(lock_);"));
	EXPECT_TRUE(import.Between("IOLockLock(lock_);", "IOLockUnlock(lock_);")
			    .Has("publishBo(bo, false)"));
	EXPECT_FALSE(import.Has("beginMutation"));
	const std::string bo = unit("TegraGM20BUserClientBo.cpp");
	const CodeText alloc = Block(bo, "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_EQ(alloc.Count("IOLockLock(lock_);"), 2u);
	EXPECT_TRUE(alloc.Between("IOLockLock(lock_);", "IOLockUnlock(lock_);")
			    .Has("const BoCharge charge = chargeBo(in);"));
	EXPECT_TRUE(alloc.Before("const BoCharge charge = chargeBo(in);", "spareTake(in->bytes)"));
	EXPECT_TRUE(alloc.Before("cleanZeros(va, backing);",
				 "ret = settleBo(in, charge, bo, ret, chunksRefused, system);"));
	EXPECT_FALSE(alloc.Has("beginMutation"));
	EXPECT_TRUE(
		Block(bo, "IOReturn AGXDeviceUserClient::settleBo(").Has("publishBo(bo, true)"));
	const CodeText publish = Block(bo, "IOReturn AGXDeviceUserClient::publishBo(");
	EXPECT_TRUE(publish.Before("submitGate_.closed()", "boIndex_.insert(bo->handle, bo);"));
	EXPECT_FALSE(publish.Has("beginMutation"));
}

TEST(GM20BMemSource, AVmBindClosesTheGateOnlyToCut)
{
	const std::string channel = unit("TegraGM20BUserClientChannel.cpp");
	const CodeText bind = Block(channel, "IOReturn AGXDeviceUserClient::vmBind(");
	EXPECT_TRUE(bind.Has("if (cuts && !submitGate_.beginMutation(lock_))"));
	EXPECT_TRUE(bind.Has("if (cuts) submitGate_.endMutation(lock_);"));
	EXPECT_TRUE(bind.Before("BindingTable::prepare(nbinds_ + 2 * count)",
				"bindOp(in->ops[i], changed)"));
	EXPECT_TRUE(bind.Before("bindOp(in->ops[i], changed)", "publishBindings(snapshot);"));
	EXPECT_TRUE(Block(channel, "const uint32_t *AGXDeviceUserClient::pushView(")
			    .Has("hit = bindTable_.find(va, bytes);"));
}

TEST(GM20BMemSource, SubmitJobReadsOnlyPublishedTables)
{
	const CodeText uses = Block(unit("TegraGM20BUserClientJobSubmit.cpp"),
				    "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(uses.Has("bindTable_.find(use.handle, bytes ? bytes : 1)"));
	EXPECT_TRUE(uses.Has("findBo(use.handle)"));
	EXPECT_FALSE(uses.Has("binds_"));
	EXPECT_FALSE(uses.Has("bos_"));
	const std::string scan = unit("TegraGM20BPushScanRun.cpp");
	EXPECT_FALSE(CodeText(scan).Has("binds_"));
}

TEST(GM20BMemSource, RetirementFreesWhatLeftTheTablesAfterEverySubmitLock)
{
	const std::string retirement = unit("SwitchGpuClientRetirement.h");
	const CodeText batch = Block(retirement, "void AGXDeviceUserClient::retireBatch()");
	EXPECT_TRUE(batch.Before("auto *indexes = boIndex_.takeRetired();", "passSubmitters();"));
	EXPECT_TRUE(
		batch.Before("auto *bindings = bindTable_.takeRetired();", "passSubmitters();"));
	EXPECT_TRUE(batch.Between("IOLockUnlock(lock_); if (release || indexes || bindings)",
				  "releaseBo(release);")
			    .Has("passSubmitters();"));
	EXPECT_TRUE(batch.Before("passSubmitters();", "boIndex_.releaseRetired(indexes);"));
	EXPECT_TRUE(batch.Before("passSubmitters();", "BindingTable::releaseRetired(bindings);"));
	EXPECT_FALSE(Block(retirement, "void AGXDeviceUserClient::passSubmitters()").Has("lock_"));
	const CodeText free = Block(retirement, "IOReturn AGXDeviceUserClient::boFree(");
	EXPECT_TRUE(free.Before("boIndex_.erase(bo->handle);", "queueZombie(bo);"));
	EXPECT_TRUE(free.Before("publishBindings(snapshot);", "queueZombie(bo);"));
	EXPECT_TRUE(free.Before("BindingTable::prepare(nbinds_)", "*p = bo->next;"));
	EXPECT_EQ(CodeText(retirement).Count("releaseBo("), 2u)
		<< "retireBatch's, after the pass, and reap's at close, with the gate closed and "
		   "drained";
}
