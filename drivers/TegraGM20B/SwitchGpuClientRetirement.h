// SPDX-License-Identifier: MIT
#pragma once

void AGXDeviceUserClient::accountRelease(Bo *bo)
{
	if (bo->handle && (bo->pool_va || bo->chunked))
		pool_bytes_ -= bo->bytes;
	if (bo->handle && bo->pool_va && (bo->flags & TegraGM20BBoControl))
		control_bytes_ -= bo->controlCharge;
}

void AGXDeviceUserClient::readyBo(Bo *bo)
{
	if (!bo->zombieLink || bo->bindings)
		return;
	*bo->zombieLink = bo->next;
	if (bo->next)
		bo->next->zombieLink = bo->zombieLink;
	bo->zombieLink = nullptr;
	accountRelease(bo);
	bo->next = readyBos_;
	readyBos_ = bo;
	__atomic_store_n(&retirementPending_, 1u, __ATOMIC_RELEASE);
}

void AGXDeviceUserClient::queueZombie(Bo *bo)
{
	bo->next = zombies_;
	bo->zombieLink = &zombies_;
	if (zombies_)
		zombies_->zombieLink = &bo->next;
	zombies_ = bo;
	readyBo(bo);
}

void AGXDeviceUserClient::scheduleRetirement()
{
	if (!__atomic_load_n(&retirementPending_, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE))
		return;
	__atomic_add_fetch(&retirementEpoch_, 1u, __ATOMIC_RELEASE);
	thread_call_enter(retireCall_);
}

void AGXDeviceUserClient::retirementFence(void *owner)
{
	static_cast<AGXDeviceUserClient *>(owner)->scheduleRetirement();
}

void AGXDeviceUserClient::retirementWork(thread_call_param_t owner, thread_call_param_t)
{
	static_cast<AGXDeviceUserClient *>(owner)->retireBatch();
}

bool AGXDeviceUserClient::bindingIdle(const RetiredBinding *binding) const
{
	for (uint32_t slot = 0; slot < kMaxChannels; ++slot) {
		const auto &snapshot = binding->snapshots[slot];
		if (!snapshot.engine || !chans_[slot])
			continue;
		switch_gpu_backing_token current = {};
		switch_gpu_chan_next_token(chans_[slot], &current);
		if (snapshot.queue == current.queue &&
		    snapshot.reset_generation == current.reset_generation &&
		    !tgm_chan_error(chans_[slot]) &&
		    int32_t(tgm_chan_completed(chans_[slot]) - uint32_t(snapshot.value)) < 0)
			return false;
	}
	return true;
}

void AGXDeviceUserClient::retireBatch()
{
	constexpr uint32_t batch = 64;
	IOLockLock(lock_);
	if (__atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE)) {
		IOLockUnlock(lock_);
		return;
	}
	if (!retireRemaining_) {
		retireRemaining_ = retiredCount_;
		retireScanEpoch_ = __atomic_load_n(&retirementEpoch_, __ATOMIC_ACQUIRE);
	}
	for (uint32_t checked = 0; retired_ && retireRemaining_ && checked < batch; ++checked) {
		auto *record = retired_;
		retired_ = record->next;
		if (!retired_)
			retiredTail_ = &retired_;
		--retireRemaining_;
		if (bindingIdle(record)) {
			--retiredCount_;
			record->next = unmapping_;
			unmapping_ = record;
		} else {
			record->next = nullptr;
			*retiredTail_ = record;
			retiredTail_ = &record->next;
		}
	}
	auto *ready = unmapping_;
	IOLockUnlock(lock_);
	for (auto *record = ready; record; record = record->next)
		switch_gpu_vm_unmap(vm_, &record->mapping);
	IOLockLock(lock_);
	unmapping_ = nullptr;
	while (ready) {
		auto *next = ready->next;
		--ready->bo->bindings;
		readyBo(ready->bo);
		IODelete(ready, RetiredBinding, 1);
		ready = next;
	}
	Bo *release = nullptr;
	for (uint32_t count = 0; readyBos_ && count < batch; ++count) {
		auto *bo = readyBos_;
		readyBos_ = bo->next;
		bo->next = release;
		release = bo;
	}
	__atomic_store_n(&retirementPending_, unsigned(retired_ || readyBos_), __ATOMIC_RELEASE);
	const bool again = readyBos_ || retireRemaining_ ||
			   retireScanEpoch_ != __atomic_load_n(&retirementEpoch_, __ATOMIC_ACQUIRE);
	if (again && !__atomic_load_n(&retirementStopped_, __ATOMIC_ACQUIRE))
		thread_call_enter(retireCall_);
	IOLockWakeup(lock_, &retired_, false);
	IOLockUnlock(lock_);
	while (release) {
		auto *next = release->next;
		releaseBo(release);
		release = next;
	}
}

IOReturn AGXDeviceUserClient::waitRetiredOverlaps(uint64_t va, uint64_t bytes)
{
	const auto overlaps = [=](const RetiredBinding *record) {
		return record->va < va + bytes && va < record->va + record->range;
	};
	for (;;) {
		if (submitGate_.closed())
			return kIOReturnNotOpen;
		const RetiredBinding *pending = retired_;
		while (pending && !overlaps(pending))
			pending = pending->next;
		const RetiredBinding *moving = unmapping_;
		while (moving && !overlaps(moving))
			moving = moving->next;
		if (!pending && !moving)
			return kIOReturnSuccess;
		TegraGM20BAbiWaitFence wait = {};
		if (pending) {
			for (uint32_t slot = 0; slot < kMaxChannels; ++slot) {
				if (!chans_[slot] || tgm_chan_error(chans_[slot]))
					continue;
				switch_gpu_backing_token current = {};
				switch_gpu_chan_next_token(chans_[slot], &current);
				const auto &snapshot = pending->snapshots[slot];
				if (snapshot.engine && snapshot.queue == current.queue &&
				    snapshot.reset_generation == current.reset_generation &&
				    int32_t(tgm_chan_completed(chans_[slot]) -
					    uint32_t(snapshot.value)) < 0) {
					wait = {slot + 1, uint32_t(snapshot.value), UINT64_MAX};
					break;
				}
			}
		}
		if (wait.channel) {
			IOLockUnlock(lock_);
			waitFence(&wait);
			IOLockLock(lock_);
			scheduleRetirement();
		} else {
			scheduleRetirement();
			IOLockSleep(lock_, &retired_, THREAD_UNINT);
		}
	}
}

void AGXDeviceUserClient::reap(bool force)
{
	if (!force)
		return;
	while (retired_) {
		auto *record = retired_;
		retired_ = record->next;
		switch_gpu_vm_unmap(vm_, &record->mapping);
		--record->bo->bindings;
		readyBo(record->bo);
		IODelete(record, RetiredBinding, 1);
	}
	retiredTail_ = &retired_;
	retiredCount_ = retireRemaining_ = 0;
	while (readyBos_) {
		auto *bo = readyBos_;
		readyBos_ = bo->next;
		releaseBo(bo);
	}
	__atomic_store_n(&retirementPending_, 0u, __ATOMIC_RELEASE);
}

void AGXDeviceUserClient::drainResources()
{
	IOLockLock(lock_);
	submitGate_.close(lock_);
	__atomic_store_n(&retirementStopped_, 1u, __ATOMIC_RELEASE);
	IOLockUnlock(lock_);
	events_.stop();
	resources_.stop();
	releaseMappings();
#if TGM_STATS
	tgm_readback_closed(this);
#endif
	IOLockLock(lock_);
	releaseCpuLeases();
	hangHolder_ = current_thread();
	for (auto &chan : chans_)
		tgm_chan_del(&chan);
	for (auto &channel : eventChannels_)
		channel.reset();
	for (auto &admission : admissions_)
		admission.end();
	bzero(admissionCookies_, sizeof(admissionCookies_));
	hangHolder_ = nullptr;
	IOLockUnlock(lock_);
	// Channel destruction drains the callbacks before their preallocated work is cancelled.
	if (retireCall_)
		thread_call_cancel_wait(retireCall_);
	IOLockLock(lock_);
	while (nbinds_)
		unbindOverlaps(binds_[0].va, binds_[0].range, nullptr);
	surfaces_.clear();
	while (bos_) {
		auto *bo = bos_;
		bos_ = bo->next;
		queueZombie(bo);
	}
	reap(true);
	IOLockUnlock(lock_);
	scheduleOrphanRelease();
}

IOReturn AGXDeviceUserClient::boFree(const TegraGM20BAbiBoFree *in)
{
	const uint64_t start = nowNs();
	Bo **p = &bos_;
	while (*p && (*p)->handle != in->handle)
		p = &(*p)->next;
	if (!*p)
		return kIOReturnBadArgument;
	Bo *bo = *p;
	*p = bo->next;
	surfaces_.remove(bo->handle);
	for (uint32_t i = 0; i < nbinds_;) {
		if (binds_[i].bo == bo) {
			unbindOverlaps(binds_[i].va, binds_[i].range, bo);
		} else {
			i++;
		}
	}
	queueZombie(bo);
	scheduleRetirement();
	tgm_stats_bo_free(nowNs() - start);
	return kIOReturnSuccess;
}

// Binds are tracked whole. A Map or Unmap that overlaps a recorded bind removes that whole bind first (NVK binds and
// unbinds whole VA ranges); this keeps nvkm's raw get/put page-table references balanced.
void AGXDeviceUserClient::unbindOverlaps(uint64_t va, uint64_t range, Bo *only)
{
	for (uint32_t i = 0; i < nbinds_;) {
		Binding &b = binds_[i];
		if (b.va < va + range && va < b.va + b.range && (!only || b.bo == only)) {
			auto *retired = b.retirement;
			for (uint32_t slot = 0; slot < kMaxChannels; ++slot) {
				retired->snapshots[slot] = {};
				if (chans_[slot]) {
					switch_gpu_chan_next_token(chans_[slot],
								   &retired->snapshots[slot]);
					retired->snapshots[slot].value =
						uint32_t(retired->snapshots[slot].value - 1);
				}
			}
			retired->next = nullptr;
			*retiredTail_ = retired;
			retiredTail_ = &retired->next;
			++retiredCount_;
			__atomic_store_n(&retirementPending_, 1u, __ATOMIC_RELEASE);
			--nbinds_;
			memmove(&binds_[i], &binds_[i + 1], sizeof(Binding) * (nbinds_ - i));
		} else {
			i++;
		}
	}
	scheduleRetirement();
}
