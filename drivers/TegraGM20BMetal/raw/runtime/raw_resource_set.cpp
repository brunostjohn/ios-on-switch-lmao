// SPDX-License-Identifier: MIT
#include "raw_resource_set.h"

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

const SetMember *SetView::find(uint64_t va, uint64_t bytes) const
{
    const auto after = std::upper_bound(members.begin(), members.end(), va, [](uint64_t at, const SetMember &m) {
        return at < m.va;
    });
    if (after == members.begin())
        return nullptr;
    const SetMember &m = *std::prev(after);
    return va - m.va < m.bytes && bytes <= m.bytes - (va - m.va) ? &m : nullptr;
}

bool SetView::named(uint64_t handle) const
{
    for (const SetMember &m : members)
        if (m.handle == handle)
            return true;
    return false;
}

void ResourceSet::insertLocked(const Bo &bo, uint32_t access, bool always)
{
    const uint64_t va = bo.vaBase ? bo.vaBase : bo.va;
    for (SetMember &m : members_)
        if (m.handle == bo.handle && m.va == va) {
            m.access |= access;
            return;
        }
    const SetMember made = {bo.handle, va, bo.bytes, access, always};
    members_.insert(std::upper_bound(members_.begin(), members_.end(), made,
                                     [](const SetMember &a, const SetMember &b) {
                                         return a.va < b.va;
                                     }),
                    made);
}

bool ResourceSet::eraseLocked(uint64_t handle)
{
    const auto gone = [&](const SetMember &m) {
        return m.handle == handle;
    };
    const size_t before = members_.size() + outside_.size();
    members_.erase(std::remove_if(members_.begin(), members_.end(), gone), members_.end());
    outside_.erase(std::remove_if(outside_.begin(), outside_.end(), gone), outside_.end());
    return members_.size() + outside_.size() != before;
}

// What the kernel refused comes back with access 0 and leaves members_ for good (always ones for outside_).
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
    size_t kept = 0;
    for (size_t i = 0; i < members_.size(); ++i) {
        if (uses[i].access)
            members_[kept++] = members_[i];
        else if (members_[i].always)
            outside_.push_back(members_[i]);
    }
    removals_ += uint32_t(members_.size() - kept);
    members_.resize(kept);
    set_ = set;
    generation_ = generation;
    mirror_ = mirror;
    version_.fetch_add(1, std::memory_order_release);
    return true;
}

bool ResourceSet::add(Kernel &k, std::initializer_list<Candidate> candidates, bool always)
{
    std::lock_guard<std::mutex> l(lock_);
    for (const Candidate &c : candidates)
        insertLocked(*c.bo, c.access, always);
    if (!registerLocked(k)) {
        for (const Candidate &c : candidates)
            eraseLocked(c.bo->handle);
        os_log_error(setLog(), "raw: the resource set was not made (%{public}s)", k.error().c_str());
        return false;
    }
    for (const Candidate &c : candidates)
        if (std::none_of(members_.begin(), members_.end(), [&](const SetMember &m) {
                return m.handle == c.bo->handle;
            }))
            return false;
    return true;
}

void ResourceSet::free(Kernel &k, Bo &bo)
{
    std::lock_guard<std::mutex> l(lock_);
    const bool listed = eraseLocked(bo.handle);
    if (listed) {
        ++removals_;
        version_.fetch_add(1, std::memory_order_release);
    }
    k.free(bo);
    if (listed && !registerLocked(k))
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
    std::lock_guard<std::mutex> l(lock_);
    v.set = members_.empty() ? 0 : set_;
    v.generation = generation_;
    v.removals = removals_;
    v.members = members_;
    v.outside = outside_;
    v.version = version_.load(std::memory_order_relaxed);
}

bool submitInSet(Kernel &k, ResourceSet &set, SetView &view, bool named, KernelJob job, uint32_t &seqno, int *result,
                 SurfaceList *uses, const std::vector<SetUse> *recorded, uint32_t removals)
{
    set.view(view);
    for (;;) {
        if (uses) {
            for (const SetMember &m : view.outside)
                uses->addAccess(m.handle, m.access);
            if (recorded && view.removals != removals)
                for (const SetUse &u : *recorded)
                    if (!view.named(u.handle))
                        uses->addAccess(u.handle, u.access);
            if (!uses->valid()) {
                if (result)
                    *result = kIOReturnBadArgument;
                return false;
            }
            job.uses = uses->uses().data();
            job.useCount = uint32_t(uses->uses().size());
        }
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
        set.view(view);
    }
}

}  // namespace raw
