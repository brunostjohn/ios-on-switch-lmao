// SPDX-License-Identifier: MIT
#include <string.h>
#include "SwitchGpuBackingInternal.h"

namespace {
constexpr uint32_t kBuckets = 256;
IOLock *registry;
switch_gpu_backing *buckets[kBuckets];
switch_gpu_backing *retiring;
switch_gpu_backing_engine engines[SWITCH_GPU_ENGINE_COUNT];
uint32_t engine_ready[SWITCH_GPU_ENGINE_COUNT];
uint64_t next_id, next_generation;

IOLock *registry_lock()
{
	IOLock *lock = __atomic_load_n(&registry, __ATOMIC_ACQUIRE);
	if (lock)
		return lock;
	IOLock *made = IOLockAlloc();
	if (!made)
		return nullptr;
	if (!__atomic_compare_exchange_n(&registry, &lock, made, false, __ATOMIC_RELEASE,
					 __ATOMIC_ACQUIRE)) {
		IOLockFree(made);
		return lock;
	}
	return made;
}

uint32_t bucket(uint64_t pa)
{
	return uint32_t((pa >> 12) ^ (pa >> 20)) & (kBuckets - 1);
}

switch_gpu_backing_pages *copy_pages(const uint64_t *pages, uint32_t count)
{
	if (!pages || !count)
		return nullptr;
	for (uint32_t i = 0; i < count; ++i)
		if (!pages[i] || (pages[i] & 4095))
			return nullptr;
	auto *copy = IONew(switch_gpu_backing_pages, 1);
	if (!copy)
		return nullptr;
	copy->older = nullptr;
	copy->count = count;
	copy->addresses = IONew(uint64_t, count);
	if (!copy->addresses) {
		IODelete(copy, switch_gpu_backing_pages, 1);
		return nullptr;
	}
	memcpy(copy->addresses, pages, uint64_t(count) * sizeof(*pages));
	return copy;
}

void free_pages(switch_gpu_backing_pages *pages)
{
	while (pages) {
		auto *older = pages->older;
		IODelete(pages->addresses, uint64_t, pages->count);
		IODelete(pages, switch_gpu_backing_pages, 1);
		pages = older;
	}
}

void destroy(switch_gpu_backing *backing)
{
	for (uint32_t i = 0; i < backing->access_count; ++i)
		switch_gpu_backing_drop_token(backing->accesses[i].token);
	IODelete(backing->accesses, switch_gpu_backing_access, backing->access_capacity);
	if (backing->owner && backing->release_owner)
		backing->release_owner(backing->owner);
	free_pages(backing->pages);
	IOLockFree(backing->lock);
	IODelete(backing, switch_gpu_backing, 1);
}
}  // namespace

extern "C" switch_gpu_backing *switch_gpu_backing_register(const uint64_t *pages, uint32_t count,
		uint32_t surface_id, void *owner, void (*release_owner)(void *))
{
	if ((owner != nullptr) != (release_owner != nullptr))
		return nullptr;
	auto *copy = copy_pages(pages, count);
	IOLock *lock = registry_lock();
	if (!copy || !lock) {
		free_pages(copy);
		return nullptr;
	}
	auto *made = IONew(switch_gpu_backing, 1);
	if (!made) {
		free_pages(copy);
		return nullptr;
	}
	bzero(made, sizeof(*made));
	made->lock = IOLockAlloc();
	if (!made->lock) {
		free_pages(copy);
		IODelete(made, switch_gpu_backing, 1);
		return nullptr;
	}
	IOLockLock(lock);
	if (switch_gpu_backing_prepare_retirement()) {
		IOLockUnlock(lock);
		free_pages(copy);
		IOLockFree(made->lock);
		IODelete(made, switch_gpu_backing, 1);
		return nullptr;
	}
	switch_gpu_backing *found = buckets[bucket(pages[0])];
	while (found && found->pages->addresses[0] != pages[0])
		found = found->next;
	if (found) {
		IOLockLock(found->lock);
		const uint32_t common = count < found->pages->count ? count : found->pages->count;
		if (memcmp(pages, found->pages->addresses, uint64_t(common) * sizeof(*pages)) ||
		    (count > found->pages->count && found->owner && !owner) ||
		    (count > found->pages->count && (found->cpu_readers || found->cpu_writers)) ||
		    (surface_id && found->surface_id && surface_id != found->surface_id)) {
			IOLockUnlock(found->lock);
			IOLockUnlock(lock);
			free_pages(copy);
			IOLockFree(made->lock);
			IODelete(made, switch_gpu_backing, 1);
			return nullptr;
		}
		void *old_owner = nullptr;
		void (*old_release)(void *) = nullptr;
		if (count > found->pages->count) {
			if (found->initial_clean)
				found->clean_start = uint64_t(found->pages->count) << 12;
			copy->older = found->pages;
			found->pages = copy;
			copy = nullptr;
			found->generation = ++next_generation;
			found->dirty |= SWITCH_GPU_BACKING_CPU_DIRTY;
			found->initial_clean = false;
			old_owner = found->owner;
			old_release = found->release_owner;
			found->owner = owner;
			found->release_owner = release_owner;
			owner = nullptr;
		} else if (!found->owner && owner && count == found->pages->count) {
			found->owner = owner;
			found->release_owner = release_owner;
			owner = nullptr;
		}
		if (surface_id)
			found->surface_id = surface_id;
		++found->refs;
		IOLockUnlock(found->lock);
		IOLockUnlock(lock);
		if (old_owner && old_release)
			old_release(old_owner);
		if (owner)
			release_owner(owner);
		free_pages(copy);
		IOLockFree(made->lock);
		IODelete(made, switch_gpu_backing, 1);
		return found;
	}
	made->id = ++next_id;
	made->generation = ++next_generation;
	made->refs = 1;
	made->pages = copy;
	made->surface_id = surface_id;
	made->owner = owner;
	made->release_owner = release_owner;
	made->dirty = SWITCH_GPU_BACKING_CPU_DIRTY;
	made->next = buckets[bucket(pages[0])];
	buckets[bucket(pages[0])] = made;
	IOLockUnlock(lock);
	return made;
}

extern "C" switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t first_page)
{
	IOLock *lock = registry_lock();
	if (!lock)
		return nullptr;
	IOLockLock(lock);
	auto *found = buckets[bucket(first_page)];
	while (found && found->pages->addresses[0] != first_page)
		found = found->next;
	if (found)
		++found->refs;
	IOLockUnlock(lock);
	return found;
}

extern "C" switch_gpu_backing *switch_gpu_backing_find_surface(uint32_t surface_id)
{
	IOLock *lock = registry_lock();
	if (!surface_id || !lock)
		return nullptr;
	IOLockLock(lock);
	switch_gpu_backing *found = nullptr;
	for (uint32_t i = 0; i < kBuckets; ++i)
		for (auto *at = buckets[i]; at; at = at->next)
			if (at->surface_id == surface_id && (!found || at->pages->count > found->pages->count))
				found = at;
	if (found)
		++found->refs;
	IOLockUnlock(lock);
	return found;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
	if (!backing)
		return;
	IOLockLock(registry);
	++backing->refs;
	IOLockUnlock(registry);
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
	if (!backing)
		return;
	IOLockLock(registry);
	const bool last = --backing->refs == 0;
	if (last) {
		backing->refs = 1;
		backing->retiring = true;
		backing->retiring_next = retiring;
		retiring = backing;
	}
	IOLockUnlock(registry);
	if (last)
		switch_gpu_backing_schedule_retirement();
}

extern "C" void switch_gpu_backing_retire(void)
{
	for (;;) {
		IOLockLock(registry);
		auto *backing = retiring;
		if (!backing) {
			IOLockUnlock(registry);
			return;
		}
		IOLockLock(backing->lock);
		switch_gpu_backing_token wait = {};
		for (uint32_t i = 0; backing->refs == 1 && i < backing->access_count; ++i) {
			if (switch_gpu_backing_token_status(&backing->accesses[i].token) == 0) {
				wait = backing->accesses[i].token;
				break;
			}
		}
		if (wait.engine) {
			const auto *ops = switch_gpu_backing_engine_ops(wait.engine);
			const int retained = ops->retain_token(&wait);
			IOLockUnlock(backing->lock);
			IOLockUnlock(registry);
			if (retained)
				return;
			const int status = switch_gpu_backing_token_wait(&wait);
			ops->release_token(&wait);
			if (!status)
				return;
			continue;
		}
		retiring = backing->retiring_next;
		backing->retiring_next = nullptr;
		backing->retiring = false;
		const bool last = --backing->refs == 0;
		if (last) {
			auto **at = &buckets[bucket(backing->pages->addresses[0])];
			while (*at && *at != backing)
				at = &(*at)->next;
			if (*at)
				*at = backing->next;
		}
		IOLockUnlock(backing->lock);
		IOLockUnlock(registry);
		if (last)
			destroy(backing);
	}
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
	if (!backing || !info)
		return;
	IOLockLock(backing->lock);
	*info = {backing->id, backing->generation, backing->content,
		 backing->pages->addresses, backing->pages->count, backing->surface_id,
		 backing->cpu_readers + backing->cpu_writers, backing->dirty, backing->layout};
	IOLockUnlock(backing->lock);
}

extern "C" int switch_gpu_backing_set_layout(switch_gpu_backing *backing,
					    const switch_gpu_backing_layout *layout)
{
	if (!backing || !layout || layout->plane_count > SWITCH_GPU_BACKING_MAX_PLANES)
		return -22;
	IOLockLock(backing->lock);
	int result = 0;
	const uint64_t bytes = uint64_t(backing->pages->count) << 12;
	for (uint32_t i = 0; i < layout->plane_count; ++i) {
		const auto &plane = layout->planes[i];
		if (!plane.width || !plane.height || !plane.pitch || !plane.bytes_per_element ||
		    plane.offset > bytes || plane.bytes > bytes - plane.offset ||
		    plane.layout > SWITCH_GPU_BACKING_BLOCK_LINEAR || plane.block_height_log2 > 5 ||
		    uint64_t(plane.width) * plane.bytes_per_element > plane.pitch ||
		    uint64_t(plane.pitch) * plane.height > plane.bytes ||
		    (plane.layout == SWITCH_GPU_BACKING_BLOCK_LINEAR &&
		     ((plane.pitch & 63) || uint64_t(plane.pitch) *
		      ((uint64_t(plane.height) + (8u << plane.block_height_log2) - 1) &
		       ~uint64_t((8u << plane.block_height_log2) - 1)) > plane.bytes)))
			result = -22;
	}
	if (!result && memcmp(layout, &backing->layout, sizeof(*layout))) {
		result = switch_gpu_backing_prune_locked(backing);
		if (!result && (backing->access_count || backing->cpu_readers || backing->cpu_writers))
			result = -16;
		if (!result)
			backing->layout = *layout;
	}
	IOLockUnlock(backing->lock);
	return result;
}

const switch_gpu_backing_engine *switch_gpu_backing_engine_ops(uint32_t engine)
{
	return engine < SWITCH_GPU_ENGINE_COUNT && __atomic_load_n(&engine_ready[engine], __ATOMIC_ACQUIRE)
		       ? &engines[engine] : nullptr;
}

extern "C" int switch_gpu_backing_register_engine(uint32_t engine,
					 const switch_gpu_backing_engine *operations)
{
	if (!engine || engine >= SWITCH_GPU_ENGINE_COUNT || !operations || !operations->status ||
	    !operations->wait || !operations->retain_token || !operations->release_token ||
	    (operations->value_bits != 32 && operations->value_bits != 64))
		return -22;
	IOLock *lock = registry_lock();
	if (!lock)
		return -12;
	IOLockLock(lock);
	const int result = engine_ready[engine] ? -16 : 0;
	if (!result) {
		engines[engine] = *operations;
		__atomic_store_n(&engine_ready[engine], 1u, __ATOMIC_RELEASE);
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *token)
{
	const auto *ops = token ? switch_gpu_backing_engine_ops(token->engine) : nullptr;
	return ops ? ops->status(token) : -19;
}

extern "C" int switch_gpu_backing_token_wait(const switch_gpu_backing_token *token)
{
	const auto *ops = token ? switch_gpu_backing_engine_ops(token->engine) : nullptr;
	return ops ? ops->wait(token) : -19;
}
