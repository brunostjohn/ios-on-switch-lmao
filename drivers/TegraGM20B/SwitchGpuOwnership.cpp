// SPDX-License-Identifier: MIT
#include <string.h>
#include "SwitchGpuBackingInternal.h"
extern "C" void switch_gpu_surface_sweep(void) __attribute__((weak));

bool switch_gpu_backing_same_timeline(const switch_gpu_backing_token &a,
				      const switch_gpu_backing_token &b)
{
	return a.engine == b.engine && a.queue == b.queue &&
	       a.reset_generation == b.reset_generation;
}

bool switch_gpu_backing_later(const switch_gpu_backing_token &a, const switch_gpu_backing_token &b)
{
	const auto *ops = switch_gpu_backing_engine_ops(a.engine);
	return ops && (ops->value_bits == 32 ? int32_t(uint32_t(a.value - b.value)) > 0
					     : int64_t(a.value - b.value) > 0);
}

void switch_gpu_backing_drop_token(const switch_gpu_backing_token &token)
{
	const auto *ops = switch_gpu_backing_engine_ops(token.engine);
	if (ops)
		ops->release_token(&token);
}

void switch_gpu_backing_sort_uses(switch_gpu_backing_use *uses, uint32_t count)
{
    const auto swap = [&](uint32_t a, uint32_t b) {
        const auto value = uses[a]; uses[a] = uses[b]; uses[b] = value;
    };
    const auto sift = [&](uint32_t root, uint32_t end) {
        while (root < end / 2) {
            uint32_t child = root * 2 + 1;
            if (child + 1 < end && uses[child].backing->id < uses[child + 1].backing->id) ++child;
            if (uses[root].backing->id >= uses[child].backing->id) break;
            swap(root, child); root = child;
        }
    };
    for (uint32_t root = count / 2; root; --root) sift(root - 1, count);
    for (uint32_t end = count; end > 1; --end) { swap(0, end - 1); sift(0, end - 1); }
}

int switch_gpu_backing_prune_locked(switch_gpu_backing *backing)
{
	int result = 0;
	if (backing->host_acquire.engine) {
		const int acquired = switch_gpu_backing_token_status(&backing->host_acquire);
		if (acquired) {
			if (acquired == 1 && backing->host_acquire_epoch == backing->host_epoch)
				backing->host_visible_epoch = backing->host_epoch;
			switch_gpu_backing_drop_token(backing->host_acquire);
			backing->host_acquire = {};
		}
	}
	bool pending = false;
	for (uint32_t i = 0; i < backing->access_count; ++i)
		pending |= switch_gpu_backing_token_status(&backing->accesses[i].token) == 0;
	for (uint32_t i = 0; i < backing->access_count;) {
		const int status = switch_gpu_backing_token_status(&backing->accesses[i].token);
		const bool failed_writer = status < 0 &&
			(backing->accesses[i].access & SWITCH_GPU_BACKING_WRITE);
		// Pending consumers may still have a hardware acquire referencing a completed producer's fence allocation.
		const bool current_writer = (backing->accesses[i].access & SWITCH_GPU_BACKING_WRITE) &&
			backing->accesses[i].content == backing->content;
		if (status != 0 && !failed_writer && !pending && !current_writer) {
			switch_gpu_backing_drop_token(backing->accesses[i].token);
			backing->accesses[i] = backing->accesses[--backing->access_count];
		} else {
			if (failed_writer)
				result = status;
			++i;
		}
	}
	return result;
}

namespace
{
constexpr uint32_t kAccessMask = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_WRITE;

uint32_t access_kind(uint32_t access)
{
	return access & SWITCH_GPU_BACKING_WRITE ? SWITCH_GPU_BACKING_WRITE
						 : SWITCH_GPU_BACKING_READ;
}

bool conflicts(uint32_t old_access, uint32_t access)
{
	return (old_access | access) & SWITCH_GPU_BACKING_WRITE;
}

uint32_t access_slot(switch_gpu_backing *backing, const switch_gpu_backing_token &token,
		     uint32_t access)
{
	for (uint32_t i = 0; i < backing->access_count; ++i)
		if (backing->accesses[i].access == access_kind(access) &&
		    switch_gpu_backing_same_timeline(backing->accesses[i].token, token))
			return i;
	return backing->access_count;
}

int reserve_access(switch_gpu_backing *backing)
{
	if (backing->access_count < backing->access_capacity)
		return 0;
	const uint32_t capacity = backing->access_capacity ? backing->access_capacity * 2 : 4;
	if (capacity < backing->access_capacity)
		return -12;
	auto *accesses = IONew(switch_gpu_backing_access, capacity);
	if (!accesses)
		return -12;
	if (backing->access_count)
		memcpy(accesses, backing->accesses, sizeof(*accesses) * backing->access_count);
	IODelete(backing->accesses, switch_gpu_backing_access, backing->access_capacity);
	backing->accesses = accesses;
	backing->access_capacity = capacity;
	return 0;
}

void unlock_transaction(switch_gpu_backing_transaction *transaction)
{
	while (transaction->locked)
		IOLockUnlock(transaction->uses[--transaction->locked].backing->lock);
	for (uint32_t i = 0; i < transaction->count; ++i)
		switch_gpu_backing_release(transaction->uses[i].backing);
	*transaction = {};
}
}  // namespace

static int begin(switch_gpu_backing_use *uses, uint32_t count, switch_gpu_cpu_lease *const *leases,
		 uint32_t lease_count, const switch_gpu_backing_guard *guards, uint32_t guard_count,
		 const switch_gpu_backing_token *completion,
		 switch_gpu_backing_transaction *transaction)
{
	if (!transaction || !completion || (count && !uses) || (lease_count && !leases) ||
	    (guard_count && !guards))
		return -22;
	for (uint32_t i = 0; i < lease_count; ++i) {
		if (!leases[i])
			return -22;
		for (uint32_t j = 0; j < i; ++j)
			if (leases[i] == leases[j])
				return -22;
	}
	if (!lease_count && switch_gpu_surface_sweep)
		switch_gpu_surface_sweep();
	*transaction = {};
	const auto *ops = switch_gpu_backing_engine_ops(completion->engine);
	if (!ops)
		return -19;
	for (uint32_t i = 0; i < count; ++i) {
		if (!uses[i].backing || !(uses[i].access & kAccessMask) ||
		    (uses[i].access & ~kAccessMask) || uses[i].reserved)
			return -22;
	}
	switch_gpu_backing_sort_uses(uses, count);
	uint32_t unique = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (unique && uses[unique - 1].backing == uses[i].backing)
			uses[unique - 1].access |= uses[i].access;
		else
			uses[unique++] = uses[i];
	}
	for (uint32_t i = 0; i < guard_count; ++i) {
		if (!guards[i].backing || !guards[i].generation ||
		    (uintptr_t(guards[i].generation) & 7) || !guards[i].expected)
			return -22;
		uint32_t first = 0, last = unique;
		while (first < last) {
			const uint32_t middle = first + (last - first) / 2;
			if (uses[middle].backing->id < guards[i].backing->id)
				first = middle + 1;
			else
				last = middle;
		}
		if (first == unique || uses[first].backing != guards[i].backing)
			return -22;
	}
	transaction->uses = uses;
	transaction->count = unique;
	transaction->completion = *completion;
	for (uint32_t i = 0; i < unique; ++i)
		switch_gpu_backing_retain(uses[i].backing);
	int result = 0;
	uint32_t matched_leases = 0;
	for (uint32_t i = 0; i < unique; ++i) {
		IOLockLock(uses[i].backing->lock);
		++transaction->locked;
	}
	for (uint32_t i = 0; !result && i < guard_count; ++i)
		if (__atomic_load_n(guards[i].generation, __ATOMIC_ACQUIRE) != guards[i].expected)
			result = SWITCH_GPU_BACKING_STALE;
	for (uint32_t i = 0; !result && i < unique; ++i) {
		auto *backing = uses[i].backing;
		uint32_t readers = backing->cpu_readers, writers = backing->cpu_writers;
		uint32_t owned = 0;
		for (uint32_t j = 0; j < lease_count; ++j) {
			const auto *lease =
				switch_gpu_backing_find_lease_locked(backing, leases[j]);
			if (!lease)
				continue;
			++matched_leases;
			auto &owners = lease->access & SWITCH_GPU_BACKING_WRITE ? writers : readers;
			if ((lease->producer.engine && (uses[i].access & SWITCH_GPU_BACKING_WRITE)) ||
			    ++owned > 1 || !owners ||
			    !(lease->access & SWITCH_GPU_BACKING_CPU_NO_CACHE) || lease->offset ||
			    lease->bytes != (uint64_t(backing->pages->count) << 12))
				result = -22;
			else
				--owners;
		}
		if (!result &&
		    (writers || (readers && (uses[i].access & SWITCH_GPU_BACKING_WRITE))))
			result = -16;
		if (!result)
			result = switch_gpu_backing_prune_locked(backing);
		if (completion->engine == SWITCH_GPU_ENGINE_GM20B &&
		    backing->host_visible_epoch != backing->host_epoch)
			transaction->host_written = 1;
		for (uint32_t j = 0; !result && j < backing->access_count; ++j)
			if (switch_gpu_backing_same_timeline(backing->accesses[j].token,
							     *completion) &&
			    !switch_gpu_backing_later(*completion, backing->accesses[j].token))
				result = -22;
		if (!result &&
		    access_slot(backing, *completion, uses[i].access) == backing->access_count)
			result = reserve_access(backing);
		if (!result)
			result = ops->retain_token(completion);
		if (result)
			break;
		++transaction->prepared;
        if (uses[i].access & SWITCH_GPU_BACKING_WRITE)
            result = switch_gpu_backing_writer_prepare_locked(backing, *completion);
        if (result)
            break;
		if (backing->dirty & SWITCH_GPU_BACKING_CPU_DIRTY) {
			transaction->host_written = 1;
			switch_gpu_backing_cache(
				backing->pages->addresses, backing->pages->count,
				backing->clean_start,
				(uint64_t(backing->pages->count) << 12) - backing->clean_start, 0);
			backing->clean_start = 0;
			backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_CPU_DIRTY);
			backing->initial_clean = true;
		}
	}
	if (!result && matched_leases != lease_count)
		result = -22;
	if (result)
		switch_gpu_backing_abort(transaction);
	return result;
}

extern "C" int switch_gpu_backing_begin(switch_gpu_backing_use *uses, uint32_t count,
					const switch_gpu_backing_token *completion,
					switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, nullptr, 0, nullptr, 0, completion, transaction);
}

extern "C" int switch_gpu_backing_begin_conversion(switch_gpu_backing_use *uses, uint32_t count,
						   switch_gpu_cpu_lease *const *leases,
						   uint32_t lease_count,
						   const switch_gpu_backing_token *completion,
						   switch_gpu_backing_transaction *transaction)
{
	if (!lease_count)
		return -22;
	return begin(uses, count, leases, lease_count, nullptr, 0, completion, transaction);
}

extern "C" int switch_gpu_backing_begin_checked(switch_gpu_backing_use *uses, uint32_t count,
						const switch_gpu_backing_guard *guards,
						uint32_t guard_count,
						const switch_gpu_backing_token *completion,
						switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, nullptr, 0, guards, guard_count, completion, transaction);
}

extern "C" int switch_gpu_backing_begin_conversion_checked(
	switch_gpu_backing_use *uses, uint32_t count, switch_gpu_cpu_lease *const *leases,
	uint32_t lease_count, const switch_gpu_backing_guard *guards, uint32_t guard_count,
	const switch_gpu_backing_token *completion, switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, leases, lease_count, guards, guard_count, completion,
		     transaction);
}

extern "C" int switch_gpu_backing_dependencies(const switch_gpu_backing_transaction *transaction,
					       switch_gpu_backing_token *out, uint32_t capacity,
					       uint32_t *needed)
{
	if (!transaction || !needed || (capacity && !out) ||
	    transaction->locked != transaction->count)
		return -22;
	*needed = 0;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		const auto &use = transaction->uses[i];
		for (uint32_t j = 0; j < use.backing->access_count; ++j) {
			const auto &access = use.backing->accesses[j];
			if (!conflicts(access.access, use.access))
				continue;
			const bool external = (access.access & SWITCH_GPU_BACKING_WRITE) &&
				transaction->completion.engine != SWITCH_GPU_ENGINE_GM20B;
			const int status = external ? switch_gpu_backing_token_external_status(&access.token) :
				switch_gpu_backing_token_status(&access.token);
			if (status < 0 && (access.access & SWITCH_GPU_BACKING_WRITE))
				return status;
			if (status != 0 ||
			    switch_gpu_backing_same_timeline(access.token, transaction->completion))
				continue;
			uint32_t existing = 0;
			while (existing < *needed && existing < capacity &&
			       !switch_gpu_backing_same_timeline(out[existing], access.token))
				++existing;
			if (existing < *needed && existing < capacity) {
				if (switch_gpu_backing_later(access.token, out[existing]))
					out[existing] = access.token;
				continue;
			}
			// An exhausted output still reports an upper bound; retrying with needed slots cannot truncate.
			if (*needed < capacity)
				out[*needed] = access.token;
			++*needed;
		}
	}
	return *needed > capacity ? -28 : 0;
}

extern "C" void switch_gpu_backing_commit(switch_gpu_backing_transaction *transaction)
{
	if (!transaction || transaction->locked != transaction->count ||
	    transaction->prepared != transaction->count)
		return;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		auto *backing = transaction->uses[i].backing;
		const uint32_t access = access_kind(transaction->uses[i].access);
		if (transaction->completion.engine == SWITCH_GPU_ENGINE_GM20B &&
		    backing->host_visible_epoch != backing->host_epoch && !backing->host_acquire.engine) {
			if (!switch_gpu_backing_token_retain(&transaction->completion)) {
				backing->host_acquire = transaction->completion;
				backing->host_acquire_epoch = backing->host_epoch;
			}
		}
		const uint32_t slot = access_slot(backing, transaction->completion, access);
		if (slot < backing->access_count)
			switch_gpu_backing_drop_token(backing->accesses[slot].token);
		else
			++backing->access_count;
		backing->accesses[slot] = {transaction->completion,
			access & SWITCH_GPU_BACKING_WRITE ? backing->content + 1 : 0, access};
		if (access & SWITCH_GPU_BACKING_WRITE) {
            switch_gpu_backing_writer_commit_locked(backing, transaction->completion);
			++backing->content;
			if (transaction->completion.engine != SWITCH_GPU_ENGINE_GM20B)
				++backing->host_epoch;
			backing->dirty |= SWITCH_GPU_BACKING_GPU_DIRTY;
		}
	}
	unlock_transaction(transaction);
}

extern "C" void switch_gpu_backing_abort(switch_gpu_backing_transaction *transaction)
{
	if (!transaction)
		return;
	while (transaction->prepared) {
        switch_gpu_backing_writer_abort_locked(transaction->uses[transaction->prepared - 1].backing);
		switch_gpu_backing_drop_token(transaction->completion);
		--transaction->prepared;
	}
	unlock_transaction(transaction);
}
