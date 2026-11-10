// SPDX-License-Identifier: MIT
#include <string.h>
#include "SwitchGpuBackingInternal.h"
extern "C" void switch_gpu_surface_sweep(void) __attribute__((weak));

bool switch_gpu_backing_same_timeline(const switch_gpu_backing_token &a,
				     const switch_gpu_backing_token &b)
{
	return a.engine == b.engine && a.queue == b.queue && a.reset_generation == b.reset_generation;
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

namespace {
uint64_t logged_producer;

// What a failed write wrote is undefined, and its readers go on with it, as Metal's go on after a failed command buffer:
// the failure is logged, once per producer, and returned to nobody.
void note_failure(const switch_gpu_backing *backing, switch_gpu_backing_access &access, int status)
{
	if (!access.noted && access.access == SWITCH_GPU_BACKING_WRITE) {
		const auto &token = access.token;
		const uint64_t producer = (uint64_t(token.engine) << 56) ^ (token.reset_generation << 32) ^ token.queue;
		if (__atomic_exchange_n(&logged_producer, producer, __ATOMIC_RELAXED) != producer)
			IOLog("SwitchGpu: surface %u (allocation %llu) holds undefined contents from generation %llu (now "
			      "%llu): engine %u queue %u reset %llu job %llu failed (%d); its users go on\n",
			      backing->surface_id, backing->id, access.content, backing->content, token.engine, token.queue,
			      token.reset_generation, token.value, status);
	}
	access.noted = true;
}
} // namespace

void switch_gpu_backing_prune_locked(switch_gpu_backing *backing)
{
	bool pending = false;
	for (uint32_t i = 0; i < backing->access_count; ++i)
		pending |= switch_gpu_backing_token_status(&backing->accesses[i].token) == 0;
	for (uint32_t i = 0; i < backing->access_count;) {
		const int status = switch_gpu_backing_token_status(&backing->accesses[i].token);
		if (status < 0)
			note_failure(backing, backing->accesses[i], status);
		// Pending consumers may still have a hardware acquire referencing a completed producer's fence allocation.
		if (status != 0 && !pending) {
			switch_gpu_backing_drop_token(backing->accesses[i].token);
			backing->accesses[i] = backing->accesses[--backing->access_count];
		} else {
			++i;
		}
	}
}

namespace {
constexpr uint32_t kAccessMask = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kBesideCpu = SWITCH_GPU_BACKING_UNLISTED | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;
constexpr uint32_t kUseMask = kAccessMask | kBesideCpu;

uint32_t access_kind(uint32_t access)
{
	return (access & SWITCH_GPU_BACKING_WRITE ? SWITCH_GPU_BACKING_WRITE : SWITCH_GPU_BACKING_READ) |
		(access & SWITCH_GPU_BACKING_UNLISTED);
}

// A backing used both listed and unlisted by one job is ordered by everything it may do, as a listed use.
uint32_t merge(uint32_t a, uint32_t b)
{
	uint32_t merged = (a | b) & ~kBesideCpu;
	if (a & b & SWITCH_GPU_BACKING_UNLISTED)
		merged |= SWITCH_GPU_BACKING_UNLISTED;
	else if ((a & kBesideCpu) && (b & kBesideCpu))
		merged |= SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;
	return merged;
}

bool conflicts(uint32_t old_access, uint32_t access)
{
	return (old_access | access) & SWITCH_GPU_BACKING_WRITE;
}

uint32_t access_slot(switch_gpu_backing *backing, const switch_gpu_backing_token &token, uint32_t access)
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
} // namespace

static int begin(switch_gpu_backing_use *uses, uint32_t count,
		switch_gpu_cpu_lease *const *leases, uint32_t lease_count,
		const switch_gpu_backing_token *completion, switch_gpu_backing_transaction *transaction)
{
	if (!transaction || !completion || (count && !uses) || (lease_count && !leases))
		return -22;
	for (uint32_t i = 0; i < lease_count; ++i) {
		if (!leases[i] || !(leases[i]->access & SWITCH_GPU_BACKING_CPU_NO_CACHE))
			return -22;
		for (uint32_t j = 0; j < i; ++j)
			if (leases[i]->backing == leases[j]->backing)
				return -22;
		bool used = false;
		for (uint32_t j = 0; j < count; ++j)
			used |= uses[j].backing == leases[i]->backing;
		if (!used)
			return -22;
	}
	if (!lease_count && switch_gpu_surface_sweep)
		switch_gpu_surface_sweep();
	*transaction = {};
	const auto *ops = switch_gpu_backing_engine_ops(completion->engine);
	if (!ops)
		return -19;
	for (uint32_t i = 0; i < count; ++i) {
		if (!uses[i].backing || !(uses[i].access & kAccessMask) || (uses[i].access & ~kUseMask) ||
		    uses[i].reserved)
			return -22;
		for (uint32_t j = i; j && uses[j].backing->id < uses[j - 1].backing->id; --j) {
			const auto swap = uses[j];
			uses[j] = uses[j - 1];
			uses[j - 1] = swap;
		}
	}
	uint32_t unique = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (unique && uses[unique - 1].backing == uses[i].backing)
			uses[unique - 1].access = merge(uses[unique - 1].access, uses[i].access);
		else
			uses[unique++] = uses[i];
	}
	transaction->uses = uses;
	transaction->count = unique;
	transaction->completion = *completion;
	for (uint32_t i = 0; i < unique; ++i)
		switch_gpu_backing_retain(uses[i].backing);
	int result = 0;
	for (uint32_t i = 0; i < unique; ++i) {
		auto *backing = uses[i].backing;
		IOLockLock(backing->lock);
		++transaction->locked;
		uint32_t readers = backing->cpu_readers, writers = backing->cpu_writers;
		for (uint32_t j = 0; j < lease_count; ++j) {
			const auto *lease = leases[j];
			if (lease->backing != backing)
				continue;
			auto &owners = lease->access & SWITCH_GPU_BACKING_WRITE ? writers : readers;
			if (!owners || lease->offset || lease->bytes != (uint64_t(backing->pages->count) << 12))
				result = -22;
			else
				--owners;
		}
		if (!result && !(uses[i].access & kBesideCpu) &&
		    (writers || (readers && (uses[i].access & SWITCH_GPU_BACKING_WRITE))))
			result = -16;
		if (!result)
			switch_gpu_backing_prune_locked(backing);
		for (uint32_t j = 0; !result && j < backing->access_count; ++j)
			if (switch_gpu_backing_same_timeline(backing->accesses[j].token, *completion) &&
			    !switch_gpu_backing_later(*completion, backing->accesses[j].token))
				result = -22;
		if (!result && access_slot(backing, *completion, uses[i].access) == backing->access_count)
			result = reserve_access(backing);
		if (!result)
			result = ops->retain_token(completion);
		if (result)
			break;
		++transaction->prepared;
		if (backing->dirty & SWITCH_GPU_BACKING_CPU_DIRTY) {
			switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count, backing->clean_start,
				(uint64_t(backing->pages->count) << 12) - backing->clean_start, 0);
			backing->clean_start = 0;
			backing->dirty &= ~uint32_t(SWITCH_GPU_BACKING_CPU_DIRTY);
			backing->initial_clean = true;
		}
	}
	if (result)
		switch_gpu_backing_abort(transaction);
	return result;
}

extern "C" int switch_gpu_backing_begin(switch_gpu_backing_use *uses, uint32_t count,
		const switch_gpu_backing_token *completion, switch_gpu_backing_transaction *transaction)
{
	return begin(uses, count, nullptr, 0, completion, transaction);
}

extern "C" int switch_gpu_backing_begin_conversion(switch_gpu_backing_use *uses, uint32_t count,
		switch_gpu_cpu_lease *const *leases, uint32_t lease_count,
		const switch_gpu_backing_token *completion, switch_gpu_backing_transaction *transaction)
{
	if (!lease_count)
		return -22;
	return begin(uses, count, leases, lease_count, completion, transaction);
}

extern "C" int switch_gpu_backing_dependencies(const switch_gpu_backing_transaction *transaction,
		switch_gpu_backing_token *out, uint32_t capacity, uint32_t *needed)
{
	if (!transaction || !needed || (capacity && !out) || transaction->locked != transaction->count)
		return -22;
	*needed = 0;
	// The kernel's other engines follow what a GM20B job did not list; GM20B jobs follow only listed work, as
	// TegraGM20BFences.h orders them.
	const bool follows_unlisted = transaction->completion.engine != SWITCH_GPU_ENGINE_GM20B;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		const auto &use = transaction->uses[i];
		if (use.access & SWITCH_GPU_BACKING_UNLISTED)
			continue;
		for (uint32_t j = 0; j < use.backing->access_count; ++j) {
			auto &access = use.backing->accesses[j];
			if (((access.access & SWITCH_GPU_BACKING_UNLISTED) && !follows_unlisted) ||
			    !conflicts(access.access, use.access))
				continue;
			const int status = switch_gpu_backing_token_status(&access.token);
			if (status < 0)
				note_failure(use.backing, access, status);
			if (status || switch_gpu_backing_same_timeline(access.token, transaction->completion))
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
	if (!transaction || transaction->locked != transaction->count || transaction->prepared != transaction->count)
		return;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		auto *backing = transaction->uses[i].backing;
		const uint32_t access = access_kind(transaction->uses[i].access);
		const uint32_t slot = access_slot(backing, transaction->completion, access);
		if (slot < backing->access_count)
			switch_gpu_backing_drop_token(backing->accesses[slot].token);
		else
			++backing->access_count;
		if (access & SWITCH_GPU_BACKING_WRITE) {
			++backing->content;
			backing->dirty |= SWITCH_GPU_BACKING_GPU_DIRTY;
		}
		backing->accesses[slot] = {transaction->completion, backing->content, access, false};
	}
	unlock_transaction(transaction);
}

extern "C" void switch_gpu_backing_abort(switch_gpu_backing_transaction *transaction)
{
	if (!transaction)
		return;
	while (transaction->prepared) {
		switch_gpu_backing_drop_token(transaction->completion);
		--transaction->prepared;
	}
	unlock_transaction(transaction);
}
