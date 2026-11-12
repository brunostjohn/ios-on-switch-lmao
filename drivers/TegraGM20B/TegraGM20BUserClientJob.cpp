// SPDX-License-Identifier: MIT
// The job interface (TegraGM20BJobABI.h): persistent resource sets and writer promises.
#include "TegraGM20BUserClientInternal.h"

IOReturn AGXDeviceUserClient::resourceSetCreate(const TegraGM20BAbiResourceSetCreate *in,
						TegraGM20BAbiResourceSetCreateReply *out)
{
	if (!in->count || in->count > TegraGM20BMaxSetMembers || !in->uses_address ||
	    in->set > TegraGM20BMaxResourceSets)
		return kIOReturnBadArgument;
	auto *uses = IONew(TegraGM20BAbiSurfaceUse, in->count);
	auto *members = IONew(switch_gpu_set_member, in->count);
	IOReturn result = !uses || !members ? kIOReturnNoMemory : kIOReturnSuccess;
	if (!result && copyin(in->uses_address, uses, in->count * sizeof(*uses)))
		result = kIOReturnBadArgument;
	if (!result) {
		IOLockLock(lock_);
		if (submitGate_.closed() || !submitGate_.beginMutation(lock_)) {
			result = kIOReturnNotOpen;
		} else {
			result = attachSet(in, uses, members, out);
			submitGate_.endMutation(lock_);
		}
		IOLockUnlock(lock_);
	}
	IODelete(uses, TegraGM20BAbiSurfaceUse, in->count);
	IODelete(members, switch_gpu_set_member, in->count);
	return result;
}

// lock_ held, every submission excluded
IOReturn AGXDeviceUserClient::attachSet(const TegraGM20BAbiResourceSetCreate *in,
					const TegraGM20BAbiSurfaceUse *uses,
					switch_gpu_set_member *members,
					TegraGM20BAbiResourceSetCreateReply *out)
{
	constexpr uint32_t kAccess = TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite;
	for (uint32_t i = 0; i < in->count; ++i) {
		const uint32_t access = uses[i].access;
		if (uses[i].reserved || !(access & kAccess) ||
		    (access & ~uint32_t(kAccess | TegraGM20BSurfaceGpuAddress)))
			return kIOReturnBadArgument;
		const Bo *bo = access & TegraGM20BSurfaceGpuAddress ? findAddress(uses[i].handle)
								     : findBo(uses[i].handle);
		if (!bo || !bo->backing || bo->wired || bo->surface || bo->coh || bo->nativeView ||
		    bo->representation || bo->readOnly)
			return kIOReturnBadArgument;
		static_assert(uint32_t(TegraGM20BSurfaceRead) == SWITCH_GPU_BACKING_READ &&
				      uint32_t(TegraGM20BSurfaceWrite) == SWITCH_GPU_BACKING_WRITE,
			      "use access");
		members[i] = {bo->backing, access & kAccess, 0};
	}
	uint32_t index = in->set ? in->set - 1 : 0;
	if (in->set && !sets_[index])
		return kIOReturnBadArgument;
	while (!in->set && index < TegraGM20BMaxResourceSets && sets_[index])
		++index;
	if (index == TegraGM20BMaxResourceSets)
		return kIOReturnNoResources;
	switch_gpu_resource_set *made = nullptr;
	if (!sets_[index]) {
		const int created =
			switch_gpu_set_create(fence_va_ + TegraGM20BSetGenerationWord + index, &made);
		if (created)
			return ioreturn(created);
	}
	uint32_t generation = 0;
	const int attached =
		switch_gpu_set_attach(made ? made : sets_[index], members, in->count, &generation);
	if (attached) {
		switch_gpu_set_destroy(made);
		return attached == -16 ? kIOReturnExclusiveAccess : ioreturn(attached);
	}
	if (made)
		sets_[index] = made;
	*out = {index + 1, generation, kFenceToken, (TegraGM20BSetGenerationWord + index) * 4};
	return kIOReturnSuccess;
}

// lock_ held, every submission excluded
IOReturn AGXDeviceUserClient::resourceSetRelease(const TegraGM20BAbiResourceSetRelease *in)
{
	if (in->reserved || !in->set || in->set > TegraGM20BMaxResourceSets || !sets_[in->set - 1])
		return kIOReturnBadArgument;
	switch_gpu_set_destroy(sets_[in->set - 1]);
	sets_[in->set - 1] = nullptr;
	return kIOReturnSuccess;
}

void AGXDeviceUserClient::destroySets()
{
	for (auto &set : sets_) {
		switch_gpu_set_destroy(set);
		set = nullptr;
	}
}

IOReturn AGXDeviceUserClient::writerPromise(const TegraGM20BAbiWriterPromise *in)
{
	if (!in->cookie || (in->flags != TegraGM20BPromiseBegin && in->flags != TegraGM20BPromiseEnd))
		return kIOReturnBadArgument;
	if (in->flags == TegraGM20BPromiseEnd) {
		if (in->count || in->uses_address)
			return kIOReturnBadArgument;
		IOReturn result = kIOReturnBadArgument;
		IOLockLock(lock_);
		for (uint32_t i = 0; i < TegraGM20BMaxWriterPromises; ++i) {
			if (promiseCookies_[i] != in->cookie)
				continue;
			promises_[i].end();
			promiseCookies_[i] = 0;
			result = kIOReturnSuccess;
			break;
		}
		IOLockUnlock(lock_);
		return result;
	}
	if (!in->count || in->count > TegraGM20BMaxPromisedWrites || !in->uses_address)
		return kIOReturnBadArgument;
	TegraGM20BAbiSurfaceUse uses[TegraGM20BMaxPromisedWrites];
	if (copyin(in->uses_address, uses, in->count * sizeof(*uses)))
		return kIOReturnBadArgument;
	IOLockLock(lock_);
	const IOReturn result = submitGate_.closed() ? kIOReturnNotOpen : promiseWrites(in, uses);
	IOLockUnlock(lock_);
	return result;
}

// lock_ held. Every handle must be an import of the connection's with its backing, the one the display snapshots: the
// registry keeps one backing per first page (switch_gpu_backing_register), and an import adopts its surface's (-95 for
// another, switch_gpu_surface_adopt).
IOReturn AGXDeviceUserClient::promiseWrites(const TegraGM20BAbiWriterPromise *in,
					    const TegraGM20BAbiSurfaceUse *uses)
{
	uint32_t slot = TegraGM20BMaxWriterPromises;
	for (uint32_t i = 0; i < TegraGM20BMaxWriterPromises; ++i) {
		if (promiseCookies_[i] == in->cookie)
			return kIOReturnBadArgument;
		if (!promiseCookies_[i] && slot == TegraGM20BMaxWriterPromises)
			slot = i;
	}
	if (slot == TegraGM20BMaxWriterPromises)
		return kIOReturnNoResources;
	switch_gpu_backing_use backings[TegraGM20BMaxPromisedWrites];
	for (uint32_t i = 0; i < in->count; ++i) {
		if (uses[i].reserved || uses[i].access != TegraGM20BSurfaceWrite)
			return kIOReturnBadArgument;
		const Bo *bo = findBo(uses[i].handle);
		if (!bo || !bo->backing)
			return kIOReturnNotFound;
		backings[i] = {bo->backing, SWITCH_GPU_BACKING_WRITE, 0};
	}
	const int promised = promises_[slot].promise(backings, in->count);
	if (promised)
		return ioreturn(promised);
	promiseCookies_[slot] = in->cookie;
	return kIOReturnSuccess;
}
