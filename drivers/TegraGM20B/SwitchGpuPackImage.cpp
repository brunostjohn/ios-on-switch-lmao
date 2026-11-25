// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/resource.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include "SwitchGpuMapping.h"
#include "SwitchGpuPackImage.h"
#include "SwitchGpuPackTransfer.h"
#include "TegraGM20BGpu.h"

#ifndef O_NOFOLLOW_ANY
#define O_NOFOLLOW_ANY 0x20000000
#endif

extern "C" {
// NOLINTNEXTLINE(readability-redundant-declaration): not declared in every kernel config's headers
kern_return_t thread_terminate(thread_t thread);
// NOLINTNEXTLINE(readability-redundant-declaration)
vfs_context_t vfs_context_kernel(void);
}

namespace
{
struct PackImageStore {
	IOLock *lock;
	SwitchGpuPackState state;
	bool buildActive;
#ifdef SWITCH_GPU_PACK_IMAGE_HOST_TEST
	uint32_t refusedCount;
#endif
	SwitchGpuPackHeader header;
	uint64_t fileSize;
	uint64_t *pas;
	uint32_t npages;
	tgm_mem *mem;
	IOMemoryDescriptor *cpuMemory;
#if TGM_STATS
	char firstProc[17];
#endif
};

PackImageStore gPack = {};
volatile uint32_t gShutdown = 0;

IOLock *packLock()
{
	if (IOLock *lock = __atomic_load_n(&gPack.lock, __ATOMIC_SEQ_CST))
		return lock;
	IOLock *made = IOLockAlloc();
	if (!made)
		return nullptr;
	IOLock *expected = nullptr;
	if (!__atomic_compare_exchange_n(&gPack.lock, &expected, made, false, __ATOMIC_SEQ_CST,
					 __ATOMIC_SEQ_CST)) {
		IOLockFree(made);
		return expected;
	}
	return made;
}

void finishRefused(IOLock *lock, const char *why, int err, bool silent)
{
	if (!silent)
		IOLog("tegra-gm20b: pack image: refused (%s, err %d)\n", why, err);
	IOLockLock(lock);
	gPack.state = SwitchGpuPackState::Refused;
	gPack.buildActive = false;
#ifdef SWITCH_GPU_PACK_IMAGE_HOST_TEST
	if (!silent)
		++gPack.refusedCount;
#endif
	IOLockWakeup(lock, &gPack.state, false);
	IOLockWakeup(lock, &gPack.buildActive, false);
	IOLockUnlock(lock);
}

void buildThread(void *, wait_result_t)
{
	IOLock *lock = packLock();
#if TGM_STATS
	const uint64_t startedNs = switch_gpu_pack_now_ns();
#endif
	SwitchGpuPackStreamStats stats = {};
	if (switch_gpu_pack_shutdown_requested()) {
		finishRefused(lock, "shutdown", ECANCELED, true);
		thread_terminate(current_thread());
		return;
	}
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = nullptr;
	int err = vnode_open(TGM_PACK_PATH, FREAD | O_NOFOLLOW_ANY, 0, 0, &vp, ctx);
	if (err != 0 || !vp) {
		finishRefused(lock, "open failed", err, false);
		thread_terminate(current_thread());
		return;
	}
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	SwitchGpuPackHeader hdr = {};
	const char *why = nullptr;
	if (!vnode_isreg(vp)) {
		why = "not a regular file";
		err = EINVAL;
	} else if ((err = vnode_getattr(vp, &va, ctx)) != 0 ||
		   !VATTR_IS_SUPPORTED(&va, va_data_size)) {
		why = "getattr failed";
	} else if (va.va_data_size < sizeof(hdr)) {
		why = "file shorter than header";
		err = EINVAL;
	} else if ((err = switch_gpu_pack_read_exact(vp, &hdr, sizeof(hdr), 0, ctx)) != 0) {
		why = "header read failed";
	} else if (!switch_gpu_pack_validate_header(&hdr, va.va_data_size, &why)) {
		err = EINVAL;
	}
	if (why) {
		vnode_close(vp, FREAD, ctx);
		finishRefused(lock, why, err, false);
		thread_terminate(current_thread());
		return;
	}
	const uint64_t tier0Bytes = hdr.tier0[kSwitchGpuPackHeap];
	const uint64_t heapOff = hdr.section[kSwitchGpuPackHeap].offset;
	const uint32_t totalGpuPages = uint32_t(tier0Bytes >> 12);
	IOMemoryDescriptor *multi = nullptr;
	void **chunkCpu = nullptr;
	uint64_t *pas = nullptr;
	uint32_t chunkCount = 0;
	if (!switch_gpu_pack_alloc_tier0(tier0Bytes, &multi, &chunkCpu, &pas, &chunkCount)) {
		vnode_close(vp, FREAD, ctx);
		const bool shuttingDown = switch_gpu_pack_shutdown_requested();
		finishRefused(lock, shuttingDown ? "shutdown" : "tier-0 allocation failed",
			      shuttingDown ? ECANCELED : ENOMEM, shuttingDown);
		thread_terminate(current_thread());
		return;
	}
	const bool streamed = switch_gpu_pack_stream_tier0(vp, heapOff, tier0Bytes, pas, chunkCpu,
							   &why, &err, &stats);
	vnode_close(vp, FREAD, ctx);
	IODelete(chunkCpu, void *, chunkCount);
	if (!streamed) {
		multi->complete(kIODirectionInOut);
		multi->release();
		IODelete(pas, uint64_t, totalGpuPages);
		const bool shuttingDown = switch_gpu_pack_shutdown_requested();
		finishRefused(lock, why ? why : "stream failed", err, shuttingDown);
		thread_terminate(current_thread());
		return;
	}
	tgm_mem *mem = nullptr;
	err = tgm_mem_new(nullptr, pas, totalGpuPages, &mem);
	if (err != 0 || !mem) {
		multi->complete(kIODirectionInOut);
		multi->release();
		IODelete(pas, uint64_t, totalGpuPages);
		finishRefused(lock, "tgm_mem_new failed", err, false);
		thread_terminate(current_thread());
		return;
	}
	IOLockLock(lock);
	if (switch_gpu_pack_shutdown_requested()) {
		gPack.state = SwitchGpuPackState::Refused;
		gPack.buildActive = false;
		IOLockWakeup(lock, &gPack.state, false);
		IOLockWakeup(lock, &gPack.buildActive, false);
		IOLockUnlock(lock);
		tgm_mem_del(&mem);
		multi->complete(kIODirectionInOut);
		multi->release();
		IODelete(pas, uint64_t, totalGpuPages);
		thread_terminate(current_thread());
		return;
	}
	gPack.header = hdr;
	gPack.fileSize = va.va_data_size;
	gPack.pas = pas;
	gPack.npages = totalGpuPages;
	gPack.mem = mem;
	gPack.cpuMemory = multi;
	gPack.state = SwitchGpuPackState::Ready;
	gPack.buildActive = false;
#if TGM_STATS
	char firstProc[sizeof(gPack.firstProc)] = {};
	memcpy(firstProc, gPack.firstProc, sizeof(firstProc));
#endif
	IOLockWakeup(lock, &gPack.state, false);
	IOLockWakeup(lock, &gPack.buildActive, false);
	IOLockUnlock(lock);
#if TGM_STATS
	const uint64_t totalMs = (switch_gpu_pack_now_ns() - startedNs) / 1000000ULL;
	const uint64_t copyMs = stats.copyNs / 1000000ULL;
	const uint64_t bigBytes =
		switch_gpu_mapping_big_bytes(pas, totalGpuPages, TGM_PACK_WINDOW_VA, 0, tier0Bytes);
	IOLog("tegra-gm20b: pack image: tier 0 %llu MiB built in %llu ms for %s (pagein requests "
	      "%u, copy %llu ms, big-page bytes %llu)\n",
	      (unsigned long long)(tier0Bytes >> 20), (unsigned long long)totalMs, firstProc,
	      stats.pageinReqs, (unsigned long long)copyMs, (unsigned long long)bigBytes);
#endif
	thread_terminate(current_thread());
}

}  // namespace

bool switch_gpu_pack_shutdown_requested(void)
{
	return __atomic_load_n(&gShutdown, __ATOMIC_SEQ_CST) != 0u;
}

IOReturn switch_gpu_pack_image_acquire(const char *procName, SwitchGpuPackImageView *out,
				       uint64_t *waitedNs)
{
	if (!out)
		return kIOReturnBadArgument;
	*out = {};
	if (waitedNs)
		*waitedNs = 0;
	IOLock *lock = packLock();
	if (!lock)
		return kIOReturnNoMemory;
#if TGM_STATS
	uint64_t waitStart = 0;
#endif
	IOLockLock(lock);
	if (switch_gpu_pack_shutdown_requested()) {
		if (gPack.state == SwitchGpuPackState::None)
			gPack.state = SwitchGpuPackState::Refused;
		IOLockUnlock(lock);
		return kIOReturnSuccess;
	}
	if (gPack.state == SwitchGpuPackState::None) {
		gPack.state = SwitchGpuPackState::Building;
		gPack.buildActive = true;
#if TGM_STATS
		if (procName)
			strlcpy(gPack.firstProc, procName, sizeof(gPack.firstProc));
#else
		(void)procName;
#endif
		IOLockUnlock(lock);
		thread_t worker = nullptr;
		const kern_return_t kr = kernel_thread_start(buildThread, nullptr, &worker);
		if (kr != KERN_SUCCESS) {
			finishRefused(lock, "kernel_thread_start failed", int(kr), false);
			return kIOReturnSuccess;
		}
		thread_deallocate(worker);
		IOLockLock(lock);
	}
	if (gPack.state == SwitchGpuPackState::Building) {
#if TGM_STATS
		waitStart = switch_gpu_pack_now_ns();
#endif
		while (gPack.state == SwitchGpuPackState::Building) {
			if (IOLockSleep(lock, &gPack.state, THREAD_ABORTSAFE) ==
			    THREAD_INTERRUPTED) {
				IOLockUnlock(lock);
				return kIOReturnAborted;
			}
		}
#if TGM_STATS
		if (waitedNs && waitStart)
			*waitedNs = switch_gpu_pack_now_ns() - waitStart;
#endif
	}
	if (gPack.state == SwitchGpuPackState::Ready) {
		out->ready = true;
		out->mem = gPack.mem;
		out->cpuMemory = gPack.cpuMemory;
		out->tier0HeapBytes = gPack.header.tier0[kSwitchGpuPackHeap];
		out->heapBytes = gPack.header.section[kSwitchGpuPackHeap].bytes;
		out->stack = gPack.header.stack;
	}
	IOLockUnlock(lock);
	return kIOReturnSuccess;
}

namespace
{
uint64_t gPackCpuWiredBytes = 0;

bool reserveCpuWireBytes(uint64_t bytes)
{
	uint64_t cur = __atomic_load_n(&gPackCpuWiredBytes, __ATOMIC_RELAXED);
	for (;;) {
		if (bytes > kSwitchGpuPackMaxCpuWireBytes ||
		    cur > kSwitchGpuPackMaxCpuWireBytes - bytes)
			return false;
		if (__atomic_compare_exchange_n(&gPackCpuWiredBytes, &cur, cur + bytes, false,
						__ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
			return true;
	}
}

void releaseCpuWireBytes(uint64_t bytes)
{
	if (bytes)
		__atomic_fetch_sub(&gPackCpuWiredBytes, bytes, __ATOMIC_RELEASE);
}
}  // namespace

IOReturn switch_gpu_pack_client_wire_cpu(task_t task, const TegraGM20BAbiPackCpuRange *ranges,
					 uint32_t count, IOMemoryDescriptor **outDescriptors,
					 uint64_t *outBytes)
{
	uint64_t total = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (ranges[i].bytes > kSwitchGpuPackMaxCpuWireBytes - total)
			return kIOReturnNoResources;
		total += ranges[i].bytes;
	}
	if (!reserveCpuWireBytes(total))
		return kIOReturnNoResources;
	for (uint32_t i = 0; i < count; ++i) {
		IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
			ranges[i].address, ranges[i].bytes, kIODirectionOut, task);
		if (!md || md->prepare(kIODirectionOut) != kIOReturnSuccess) {
			if (md)
				md->release();
			for (uint32_t j = 0; j < i; ++j) {
				outDescriptors[j]->complete(kIODirectionOut);
				outDescriptors[j]->release();
				outDescriptors[j] = nullptr;
			}
			releaseCpuWireBytes(total);
			return kIOReturnBadArgument;
		}
		outDescriptors[i] = md;
	}
	if (outBytes)
		*outBytes = total;
	return kIOReturnSuccess;
}

void switch_gpu_pack_client_release_cpu(SwitchGpuPackClient *client)
{
	if (!client)
		return;
	for (uint32_t i = 0; i < client->cpuWiredCount; ++i) {
		if (client->cpuWired[i]) {
			client->cpuWired[i]->complete(kIODirectionOut);
			client->cpuWired[i]->release();
			client->cpuWired[i] = nullptr;
		}
	}
	client->cpuWiredCount = 0;
	releaseCpuWireBytes(client->cpuWiredBytes);
	client->cpuWiredBytes = 0;
}

void switch_gpu_pack_client_unmap_gpu(tgm_vm *vm, SwitchGpuPackClient *client)
{
	if (!client || !client->gpuMapping)
		return;
	switch_gpu_vm_unmap(vm, &client->gpuMapping);
	client->gpuBytes = 0;
}

extern "C" void switch_gpu_pack_shutdown(void)
{
	// Dekker seq_cst pair with packLock(): either shutdown observes the initialized lock and
	// drains active workers, or any late caller observes gShutdown before launching a worker.
	__atomic_store_n(&gShutdown, 1u, __ATOMIC_SEQ_CST);
	IOLock *lock = __atomic_load_n(&gPack.lock, __ATOMIC_SEQ_CST);
	if (!lock)
		return;
	IOLockLock(lock);
	if (gPack.state == SwitchGpuPackState::None)
		gPack.state = SwitchGpuPackState::Refused;
	IOLockWakeup(lock, &gPack.state, false);
	while (gPack.buildActive)
		IOLockSleep(lock, &gPack.buildActive, THREAD_UNINT);
	IOLockUnlock(lock);
}

#ifdef SWITCH_GPU_PACK_IMAGE_HOST_TEST
void switch_gpu_pack_image_reset_for_test(void)
{
	switch_gpu_pack_shutdown();
	if (gPack.mem)
		tgm_mem_del(&gPack.mem);
	if (gPack.cpuMemory) {
		gPack.cpuMemory->complete(kIODirectionInOut);
		gPack.cpuMemory->release();
		gPack.cpuMemory = nullptr;
	}
	if (gPack.pas) {
		IODelete(gPack.pas, uint64_t, gPack.npages);
		gPack.pas = nullptr;
	}
	if (gPack.lock) {
		IOLockFree(gPack.lock);
		gPack.lock = nullptr;
	}
	gPack = {};
	__atomic_store_n(&gPackCpuWiredBytes, 0u, __ATOMIC_SEQ_CST);
	__atomic_store_n(&gShutdown, 0u, __ATOMIC_SEQ_CST);
}

uint32_t switch_gpu_pack_image_refused_count_for_test(void)
{
	return gPack.refusedCount;
}
#endif
