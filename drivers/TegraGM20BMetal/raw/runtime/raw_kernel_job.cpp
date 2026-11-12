// SPDX-License-Identifier: MIT
// raw::Kernel's job interface: SubmitJob and the resource set it names (drivers/TegraGM20B/TegraGM20BJobABI.h).
#include "raw_kernel.h"
#include "TegraGM20BJobABI.h"

#include <IOKit/IOReturn.h>
#include <mach/kern_return.h>
#include <cstdio>
#include <cstring>

namespace raw {
namespace {
std::string refused(const char *what, int kr)
{
    char line[64];
    snprintf(line, sizeof line, "%s failed (0x%x)", what, unsigned(kr));
    return line;
}

bool fits(const KernelJob &job, uint32_t maxFences)
{
    if (job.pushCount > TegraGM20BMaxJobPushes || job.useCount > TegraGM20BMaxJobUses ||
        (job.pushCount && !job.pushes) || (job.useCount && !job.uses) ||
        job.events.waitCount > TegraGM20BMaxJobEvents ||
        job.events.signalCount > TegraGM20BMaxJobEvents - job.events.waitCount ||
        (job.events.waitCount && !job.events.waits) || (job.events.signalCount && !job.events.signals) ||
        (!job.set) != (!job.generation))
        return false;
    for (uint32_t i = 0; i < job.pushCount; ++i)
        if ((job.pushes[i].flags & TegraGM20BPushFence) && !maxFences)
            return false;
    return true;
}
}  // namespace

bool Kernel::submitJob(const KernelJob &job, uint32_t &seqno, int *result)
{
    if (result)
        *result = KERN_SUCCESS;
    if (!fits(job, maxFences())) {
        if (result)
            *result = kIOReturnBadArgument;
        setError("The kernel cannot represent this job");
        return false;
    }
    const bool events = !job.events.empty();
    const size_t pushBytes = job.pushCount * sizeof(TegraGM20BAbiPush);
    const size_t useBytes = job.useCount * sizeof(TegraGM20BAbiSurfaceUse);
    const size_t eventBytes = events ? sizeof(TegraGM20BAbiSubmitEvents) +
                                           (job.events.waitCount + job.events.signalCount) * sizeof(TegraGM20BAbiEventUse)
                                     : 0;
    const size_t bytes = sizeof(TegraGM20BAbiJob) + pushBytes + useBytes + eventBytes;
    // Reused per submitting thread: a steady stream of jobs allocates nothing.
    thread_local std::vector<uint64_t> storage;
    storage.resize((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    auto *at = reinterpret_cast<uint8_t *>(storage.data());
    const uint32_t flags = (job.visibility != JobVisibility::Internal ? uint32_t(TegraGM20BJobExternal) : 0u) |
                           (events ? uint32_t(TegraGM20BJobHasEvents) : 0u) |
                           (job.hostWritten ? uint32_t(TegraGM20BJobHostWritten) : 0u);
    const TegraGM20BAbiJob header = {job.channel, job.pushCount, job.set, job.generation, job.useCount, flags};
    memcpy(at, &header, sizeof header);
    at += sizeof header;
    if (pushBytes)
        memcpy(at, job.pushes, pushBytes);
    at += pushBytes;
    if (useBytes)
        memcpy(at, job.uses, useBytes);
    at += useBytes;
    if (events) {
        const TegraGM20BAbiSubmitEvents tail = {job.events.waitCount, job.events.signalCount, 0};
        memcpy(at, &tail, sizeof tail);
        at += sizeof tail;
        if (job.events.waitCount)
            memcpy(at, job.events.waits, job.events.waitCount * sizeof(TegraGM20BAbiEventUse));
        at += job.events.waitCount * sizeof(TegraGM20BAbiEventUse);
        if (job.events.signalCount)
            memcpy(at, job.events.signals, job.events.signalCount * sizeof(TegraGM20BAbiEventUse));
    }
    const TegraGM20BAbiSubmitAddress address = {uint64_t(reinterpret_cast<uintptr_t>(storage.data())), uint32_t(bytes),
                                                0};
    TegraGM20BAbiExecReply reply = {};
    const uint32_t made = opened();
    const int kr = call(TegraGM20BMethodSubmitJob, &address, sizeof address, &reply, sizeof reply);
    if (result)
        *result = kr;
    if (kr == KERN_SUCCESS) {
        seqno = reply.seqno;
        return true;
    }
    if (kr == kIOReturnNotResponding)
        notResponding(job.channel, made);
    if (kr != kIOReturnNotReady)
        setError(refused("SubmitJob", kr));
    return false;
}

int Kernel::resourceSet(const std::vector<TegraGM20BAbiSurfaceUse> &members, uint32_t &set, uint32_t &generation,
                        const volatile uint32_t *&mirror)
{
    const TegraGM20BAbiResourceSetCreate in = {uint64_t(reinterpret_cast<uintptr_t>(members.data())),
                                               uint32_t(members.size()), set};
    TegraGM20BAbiResourceSetCreateReply reply = {};
    const int kr = call(TegraGM20BMethodResourceSetCreate, &in, sizeof in, &reply, sizeof reply);
    if (kr != KERN_SUCCESS) {
        setError(refused("ResourceSetCreate", kr));
        return kr;
    }
    std::lock_guard<std::mutex> l(lostLock_);
    if (!fenceMap_ && !map(reply.fence_token, true, fenceMap_, fenceMapBytes_)) {
        fenceMap_ = 0;
        fenceMapBytes_ = 0;
        setError("fence map failed");
        return kIOReturnNoMemory;
    }
    if ((reply.generation_offset & 3) || reply.generation_offset + sizeof(uint32_t) > fenceMapBytes_) {
        setError("set generation outside the mapped fence page");
        return kIOReturnBadArgument;
    }
    set = reply.set;
    generation = reply.generation;
    mirror = reinterpret_cast<const volatile uint32_t *>(uintptr_t(fenceMap_) + reply.generation_offset);
    return KERN_SUCCESS;
}

}  // namespace raw
