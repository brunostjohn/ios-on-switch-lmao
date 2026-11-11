// SPDX-License-Identifier: MIT
#include "raw_resource_set.h"
#include "raw_encode_profile.h"

#include <IOKit/IOReturn.h>
#include <mach/kern_return.h>
#include <os/log.h>
#include <algorithm>

namespace raw {
namespace {
os_log_t setLog()
{
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw");
    return log;
}
}  // namespace

bool SetView::holds(uint64_t va, uint64_t bytes) const
{
    const auto after = std::upper_bound(members.begin(), members.end(), va, [](uint64_t at, const SetMember &m) {
        return at < m.va;
    });
    if (after == members.begin())
        return false;
    const SetMember &m = *std::prev(after);
    return va - m.va < m.bytes && bytes <= m.bytes - (va - m.va);
}

void ResourceSet::insertLocked(const Bo &bo, uint32_t access)
{
    for (SetMember &m : members_)
        if (m.handle == bo.handle) {
            m.access |= access;
            return;
        }
    const SetMember made = {bo.handle, bo.vaBase ? bo.vaBase : bo.va, bo.bytes, access};
    members_.insert(std::upper_bound(members_.begin(), members_.end(), made,
                                     [](const SetMember &a, const SetMember &b) {
                                         return a.va < b.va;
                                     }),
                    made);
}

void ResourceSet::eraseLocked(uint64_t handle)
{
    members_.erase(std::remove_if(members_.begin(), members_.end(),
                                  [&](const SetMember &m) {
                                      return m.handle == handle;
                                  }),
                   members_.end());
}

bool ResourceSet::registerLocked(Kernel &k)
{
    if (members_.empty())
        return true;
    std::vector<TegraGM20BAbiSurfaceUse> uses;
    uses.reserve(members_.size());
    for (const SetMember &m : members_)
        uses.push_back({m.handle, m.access, 0});
    uint32_t set = set_, generation = 0;
    const volatile uint32_t *mirror = nullptr;
    if (k.resourceSet(uses, set, generation, mirror) != KERN_SUCCESS)
        return false;
    set_ = set;
    generation_ = generation;
    mirror_ = mirror;
    version_.fetch_add(1, std::memory_order_release);
    return true;
}

bool ResourceSet::add(Kernel &k, std::initializer_list<Candidate> candidates)
{
    std::lock_guard<std::mutex> l(lock_);
    for (const Candidate &c : candidates)
        insertLocked(*c.bo, c.access);
    if (registerLocked(k))
        return true;
    for (const Candidate &c : candidates)
        eraseLocked(c.bo->handle);
    bool all = true;
    for (const Candidate &c : candidates) {
        insertLocked(*c.bo, c.access);
        if (registerLocked(k))
            continue;
        eraseLocked(c.bo->handle);
        all = false;
        os_log_error(setLog(), "raw: the kernel kept BO %llu out of the resource set (%{public}s): its jobs name it as a use",
                     (unsigned long long)c.bo->handle, k.error().c_str());
    }
    return all;
}

void ResourceSet::free(Kernel &k, Bo &bo)
{
    std::lock_guard<std::mutex> l(lock_);
    const size_t before = members_.size();
    eraseLocked(bo.handle);
    const bool member = members_.size() != before;
    if (member)
        version_.fetch_add(1, std::memory_order_release);
    k.free(bo);
    if (member && !registerLocked(k))
        os_log_error(setLog(), "raw: the resource set was not made again after a member's free (%{public}s)",
                     k.error().c_str());
}

bool ResourceSet::renew(Kernel &k, uint32_t generation)
{
    std::lock_guard<std::mutex> l(lock_);
    if (generation != generation_)
        return true;
    if (!mirror_ || *mirror_ == generation_)
        return false;
    if (registerLocked(k))
        return true;
    os_log_error(setLog(), "raw: the resource set's generation ended and it was not made again (%{public}s)",
                 k.error().c_str());
    return false;
}

void ResourceSet::view(SetView &v) const
{
    if (v.version == version_.load(std::memory_order_acquire))
        return;
    RAW_ENCODE_LOCK(l, lock_);
    v.set = members_.empty() ? 0 : set_;
    v.generation = generation_;
    v.members = members_;
    v.version = version_.load(std::memory_order_relaxed);
}

bool submitInSet(Kernel &k, ResourceSet &set, SetView &view, bool named, KernelJob job, uint32_t &seqno, int *result)
{
    for (;;) {
        set.view(view);
        job.set = named ? view.set : 0;
        job.generation = job.set ? view.generation : 0;
        int kr = KERN_SUCCESS;
        const bool submitted = k.submitJob(job, seqno, &kr);
        if (result)
            *result = kr;
        if (submitted)
            return true;
        if (kr != kIOReturnNotReady || !job.set || !set.renew(k, job.generation))
            return false;
    }
}

}  // namespace raw
