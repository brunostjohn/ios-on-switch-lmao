// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include <kern/thread.h>
#include "SwitchGpuBacking.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BViews.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

namespace {
struct Lease {
	Lease *next;
	IOUserClient *client;
	switch_gpu_backing *backing;
	uint64_t bytes;
	uint32_t id, flags;
	uint32_t pending[2], held[2];
};
struct Pending {
	Pending *next;
	Lease *lease;
	void *thread;
	uint32_t index;
};
IOLock *lock;
Lease *leases, *free_leases;
Pending *pending, *free_pending;

bool initialize()
{
	if (__atomic_load_n(&lock, __ATOMIC_ACQUIRE))
		return true;
	IOLock *made = IOLockAlloc(), *expected = nullptr;
	if (!made)
		return false;
	if (!__atomic_compare_exchange_n(&lock, &expected, made, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
		IOLockFree(made);
	return true;
}

Lease **find(void *client, uint32_t id)
{
	auto **at = &leases;
	while (*at && ((*at)->client != client || (*at)->id != id))
		at = &(*at)->next;
	return at;
}

uint32_t access(uint32_t index)
{
	return index ? SWITCH_GPU_BACKING_WRITE : SWITCH_GPU_BACKING_READ;
}

bool take_pending(Lease *lease, uint32_t index)
{
	auto **at = &pending;
	while (*at && ((*at)->lease != lease || (*at)->index != index || (*at)->thread != current_thread()))
		at = &(*at)->next;
	if (!*at)
		return false;
	auto *item = *at;
	*at = item->next;
	item->next = free_pending;
	free_pending = item;
	return true;
}

void recycle(Lease *lease)
{
	lease->client->release();
	switch_gpu_backing_release(lease->backing);
	IOLockLock(lock);
	lease->next = free_leases;
	free_leases = lease;
	IOLockUnlock(lock);
}

void release_one(void *client, uint32_t id, uint32_t options, bool failed)
{
	if (!lock)
		return;
	const uint32_t index = options & 1 ? 0 : 1;
	IOLockLock(lock);
	auto **at = find(client, id);
	auto *lease = *at;
	uint32_t *counts = lease ? (failed ? lease->pending : lease->held) : nullptr;
	if (!counts || !counts[index] || (failed && !take_pending(lease, index))) {
		IOLockUnlock(lock);
		return;
	}
	--counts[index];
	auto *backing = lease->backing;
	const uint64_t bytes = lease->bytes;
	const uint32_t flags = lease->flags | access(index) | SWITCH_GPU_BACKING_CPU_NO_CACHE;
	const bool last = !lease->pending[0] && !lease->pending[1] && !lease->held[0] && !lease->held[1];
	if (last)
		*at = lease->next;
	IOLockUnlock(lock);
	if (failed)
		switch_gpu_backing_cpu_cancel(backing, flags);
	else
		switch_gpu_backing_cpu_release(backing, 0, bytes, flags);
	if (last)
		recycle(lease);
}
} // namespace

extern "C" void switch_gpu_surface_sweep(void)
{
	if (!lock)
		return;
	IOLockLock(lock);
	Lease *dead = nullptr;
	for (auto **at = &leases; *at;) {
		auto *lease = *at;
		if (!lease->client->isInactive()) {
			at = &lease->next;
			continue;
		}
		*at = lease->next;
		lease->next = dead;
		dead = lease;
		for (auto **item = &pending; *item;) {
			if ((*item)->lease != lease) {
				item = &(*item)->next;
				continue;
			}
			auto *removed = *item;
			*item = removed->next;
			removed->next = free_pending;
			free_pending = removed;
		}
	}
	IOLockUnlock(lock);
	while (dead) {
		auto *lease = dead;
		dead = dead->next;
		switch_gpu_backing_info info = {};
		switch_gpu_backing_describe(lease->backing, &info);
		if (auto *coherency = tgm_coh_find_pa(info.pages[0])) {
			tgm_views_sweep(coherency);
			tgm_coherency_put(coherency);
		}
		for (uint32_t index = 0; index < 2; ++index) {
			while (lease->pending[index]) {
				--lease->pending[index];
				switch_gpu_backing_cpu_cancel(lease->backing, access(index));
			}
			while (lease->held[index]) {
				--lease->held[index];
				switch_gpu_backing_cpu_release(lease->backing, 0, lease->bytes, lease->flags | access(index));
			}
		}
		recycle(lease);
	}
}

extern "C" int switch_gpu_surface_lock_will(void *opaque, uint32_t id, uint32_t options)
{
	if (!id || !initialize())
		return id ? -12 : 0;
	switch_gpu_surface_sweep();
	auto *backing = switch_gpu_backing_find_surface(id);
	if (!backing)
		return 0;
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(backing, &info);
	uint32_t flags = SWITCH_GPU_BACKING_CPU_CONCURRENT;
	if (tegra_gm20b_surface_tiled(info.pages[0]) ||
	    (info.layout.plane_count && info.layout.planes[0].layout == SWITCH_GPU_BACKING_BLOCK_LINEAR))
		flags |= SWITCH_GPU_BACKING_CPU_NO_CACHE;
	const uint32_t index = options & 1 ? 0 : 1;
	const uint64_t bytes = uint64_t(info.page_count) << 12;
	int result = switch_gpu_backing_cpu_acquire(backing, 0, bytes, flags | access(index) |
		(options & 2 ? uint32_t(SWITCH_GPU_BACKING_CPU_AVOID_SYNC) : 0u));
	if (result) {
		switch_gpu_backing_release(backing);
		return result;
	}
	auto *client = static_cast<IOUserClient *>(opaque);
	IOLockLock(lock);
	auto *lease = *find(client, id);
	auto *item = free_pending;
	if (item)
		free_pending = item->next;
	else
		item = IONew(Pending, 1);
	if (!item)
		result = -12;
	if (client->isInactive() || (lease && lease->backing != backing))
		result = -19;
	if (!result && !lease) {
		lease = free_leases;
		if (lease)
			free_leases = lease->next;
		else
			lease = IONew(Lease, 1);
		if (!lease) {
			result = -12;
		} else {
			*lease = {leases, client, backing, bytes, id, flags, {}, {}};
			client->retain();
			switch_gpu_backing_retain(backing);
			leases = lease;
		}
	}
	if (!result) {
		++lease->pending[index];
		*item = {pending, lease, current_thread(), index};
		pending = item;
	} else if (item) {
		item->next = free_pending;
		free_pending = item;
	}
	IOLockUnlock(lock);
	if (result)
		switch_gpu_backing_cpu_cancel(backing, access(index));
	switch_gpu_backing_release(backing);
	return result;
}

extern "C" void switch_gpu_surface_lock_did(void *client, uint32_t id, uint32_t options)
{
	if (!lock)
		return;
	const uint32_t index = options & 1 ? 0 : 1;
	IOLockLock(lock);
	if (auto *lease = *find(client, id); lease && lease->pending[index] && take_pending(lease, index)) {
		--lease->pending[index];
		++lease->held[index];
	}
	IOLockUnlock(lock);
}

extern "C" void switch_gpu_surface_lock_failed(void *client, uint32_t id, uint32_t options)
{
	release_one(client, id, options, true);
}

extern "C" void switch_gpu_surface_unlock_will(void *client, uint32_t id, uint32_t options)
{
	if (!lock || (options & 1))
		return;
	IOLockLock(lock);
	const auto *lease = *find(client, id);
	switch_gpu_backing *backing = lease && lease->held[1] && !(lease->flags & SWITCH_GPU_BACKING_CPU_NO_CACHE) ?
		lease->backing : nullptr;
	const uint64_t bytes = backing ? lease->bytes : 0;
	if (backing)
		switch_gpu_backing_retain(backing);
	IOLockUnlock(lock);
	if (backing) {
		switch_gpu_backing_cpu_publish(backing, 0, bytes);
		switch_gpu_backing_release(backing);
	}
}

extern "C" void switch_gpu_surface_unlock_did(void *client, uint32_t id, uint32_t options)
{
	release_one(client, id, options, false);
}
