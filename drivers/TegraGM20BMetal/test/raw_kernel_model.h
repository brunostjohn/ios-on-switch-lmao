// SPDX-License-Identifier: MIT
// The TegraGM20B user client as raw::Kernel calls it (drivers/TegraGM20B/TegraGM20BABI.h, TegraGM20BJobABI.h), for the
// raw path's channel recovery and resource set tests. Channels live in the slots of one fence page: a channel takes the
// lowest free slot, and a slot's seqnos go on across the channels it holds, so a channel made again can land in a slot
// whose fence is behind the old one. Jobs (SubmitAddress, SubmitJob) complete as they are submitted, or stay in flight
// until a kill, which writes the record and then moves the fence past every seqno given out, as the kernel does. Memory a
// killed job wrote holds undefined contents until a job writes it again; a job that only reads it is counted
// (staleReads). A SubmitJob naming a set uses every member as registered. Sets leave out what the kernel leaves out (a BO
// a CPU lease named, a BO of another set: access 0 in the caller's records) and end their generation, mirrored in the
// fence page, when a member is freed or leased by the CPU, a channel that used the set is destroyed killed, or the set is
// made again. Its IOKit calls replace the framework's:
// include it in one unit of a test.
#pragma once
#include "TegraGM20BABI.h"
#include "TegraGM20BJobABI.h"
#include <IOKit/IOKitLib.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace kernel_model {

constexpr uint32_t kSlots = 16, kRecordBytes = 64, kFenceToken = 0x7fff0000, kFencePageBytes = 4096;

struct BoState {
    bool import = false, cached = false, cpuReached = false;
    uint32_t set = 0;  // the set it is a member of, 0: none
};

struct SetState {
    std::vector<TegraGM20BAbiSurfaceUse> members;
    uint32_t generation = 0, used = 0;  // used: the slots whose channels ran its jobs
    bool live = false;
};

struct Job {
    uint32_t channel = 0, seqno = 0;
    uint64_t uid = 0;
    std::vector<TegraGM20BAbiSurfaceUse> uses;
};

struct Slot {
    uint32_t submitted = 0, error = 0;
    uint64_t uid = 0;      // the channel the slot holds, 0: free
    bool foreign = false;  // another driver's channel of the connection (NVK's) holds it
};

struct Model {
    alignas(64) uint8_t fences[kFencePageBytes] = {};
    Slot slots[kSlots];
    uint64_t nextHandle = 1, nextUid = 1;
    std::map<uint32_t, std::vector<uint8_t>> memory;
    std::map<uint64_t, BoState> bos;
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> binds;  // va: the BO bound there and its bytes
    SetState sets[TegraGM20BMaxResourceSets];
    unsigned submitJobs = 0, setJobs = 0, staleSets = 0, addressJobs = 0, staleViews = 0;
    std::map<uint32_t, std::pair<uint64_t, std::vector<uint64_t>>> admitted;  // channel: cookie, its uses' BOs
    std::vector<uint64_t> cpuLocked;  // an IOSurfaceLock in another process: a job naming one is refused CannotLock

    bool lockedByCpu(uint64_t bo) const
    {
        return std::find(cpuLocked.begin(), cpuLocked.end(), bo) != cpuLocked.end();
    }

    // Another process's IOSurfaceLock: it waits (kIOReturnBusy here) while a command buffer's admission holds the surface
    int cpuLock(uint64_t bo)
    {
        for (const auto &[channel, held] : admitted)
            if (std::find(held.second.begin(), held.second.end(), bo) != held.second.end())
                return kIOReturnBusy;
        cpuLocked.push_back(bo);
        return KERN_SUCCESS;
    }
    std::vector<Job> jobs;
    bool inFlight = false;  // jobs run until a kill
    unsigned refuse = 0;    // submissions refused from now, with `refusal`
    int refusal = kIOReturnNoResources;
    unsigned notResponding = 0;  // WaitFences answered kIOReturnNotResponding while GetStatus calls the channel healthy
    bool foreignTakesFreedSlot = false;
    unsigned creates = 0, destroys = 0, staleReads = 0;
    std::vector<uint64_t> freed;  // BoFree's handles, in order
    std::map<uint64_t, uint64_t> undefined;  // handle: the killed channel whose job wrote it last

    TegraGM20BAbiFence &fence(uint32_t slot)
    {
        return *reinterpret_cast<TegraGM20BAbiFence *>(fences + slot * kRecordBytes);
    }

    Slot *live(uint32_t channel)
    {
        if (!channel || channel > kSlots || !slots[channel - 1].uid || slots[channel - 1].foreign)
            return nullptr;
        return &slots[channel - 1];
    }

    unsigned liveChannels() const
    {
        unsigned n = 0;
        for (const Slot &s : slots)
            n += s.uid && !s.foreign;
        return n;
    }

    void kill(uint32_t channel, uint32_t error)
    {
        Slot &s = slots[channel - 1];
        TegraGM20BAbiFence &f = fence(channel - 1);
        for (const Job &job : jobs)
            if (job.uid == s.uid && int32_t(job.seqno - f.completed) > 0)
                for (const auto &use : job.uses)
                    if (use.access & TegraGM20BSurfaceWrite)
                        undefined[use.handle] = s.uid;
        f.killed_first = f.completed + 1;
        f.killed_last = s.submitted;
        f.error = error;
        f.completed = s.submitted;
        s.error = error;
    }

    int create(TegraGM20BAbiChannelCreateReply &reply)
    {
        uint32_t slot = 0;
        while (slot < kSlots && (slots[slot].uid || slots[slot].foreign))
            slot++;
        if (slot == kSlots)
            return kIOReturnNoResources;
        creates++;
        slots[slot].uid = nextUid++;
        slots[slot].error = 0;
        TegraGM20BAbiFence &f = fence(slot);
        f.killed_first = f.killed_last = f.error = 0;
        reply = {slot + 1, kFenceToken, slot * kRecordBytes, 0};
        return KERN_SUCCESS;
    }

    // A use's BO: by handle, or with TegraGM20BSurfaceGpuAddress the BO bound there; 0: none
    uint64_t resolve(const TegraGM20BAbiSurfaceUse &use) const
    {
        if (!(use.access & TegraGM20BSurfaceGpuAddress))
            return bos.count(use.handle) ? use.handle : 0;
        for (const auto &[va, bound] : binds)
            if (use.handle >= va && use.handle - va < bound.second)
                return bound.first;
        return 0;
    }

    int run(Slot &s, uint32_t channel, std::vector<TegraGM20BAbiSurfaceUse> uses, TegraGM20BAbiExecReply &reply)
    {
        for (auto &use : uses)
            use.handle = resolve(use) ? resolve(use) : use.handle;
        Job job{channel, ++s.submitted, s.uid, std::move(uses)};
        for (const auto &use : job.uses) {
            if (use.access & TegraGM20BSurfaceWrite)
                undefined.erase(use.handle);
            else if (undefined.count(use.handle))
                staleReads++;
        }
        jobs.push_back(job);
        if (!inFlight)
            fence(channel - 1).completed = job.seqno;
        reply = {job.seqno, 0};
        return KERN_SUCCESS;
    }

    int submit(const TegraGM20BAbiSubmitAddress &address, TegraGM20BAbiExecReply &reply)
    {
        const auto *head = reinterpret_cast<const TegraGM20BAbiSubmit *>(uintptr_t(address.address));
        Slot *s = live(head->channel);
        if (!s)
            return kIOReturnBadArgument;
        if (s->error)
            return kIOReturnNotResponding;
        if (refuse) {
            refuse--;
            return refusal;
        }
        const auto *uses = reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(
            reinterpret_cast<const uint8_t *>(head + 1) + head->push_count * sizeof(TegraGM20BAbiPush));
        addressJobs++;
        return run(*s, head->channel, {uses, uses + head->surface_count}, reply);
    }

    int submitJob(const TegraGM20BAbiSubmitAddress &address, TegraGM20BAbiExecReply &reply)
    {
        const auto *head = reinterpret_cast<const TegraGM20BAbiJob *>(uintptr_t(address.address));
        Slot *s = live(head->channel);
        if (!s || head->set > TegraGM20BMaxResourceSets || !head->set != !head->set_generation)
            return kIOReturnBadArgument;
        if (s->error)
            return kIOReturnNotResponding;
        if (refuse) {
            refuse--;
            return refusal;
        }
        SetState *set = head->set ? &sets[head->set - 1] : nullptr;
        if (set && !set->live)
            return kIOReturnBadArgument;
        if (set && set->generation != head->set_generation) {
            staleSets++;
            return kIOReturnNotReady;
        }
        const auto *shared = reinterpret_cast<const TegraGM20BAbiSurfaceUse *>(
            reinterpret_cast<const uint8_t *>(head + 1) + head->push_count * sizeof(TegraGM20BAbiPush));
        for (uint32_t i = 0; i < head->shared_count; ++i)
            if (const uint64_t bo = resolve(shared[i]); bo && bos.at(bo).set)
                return kIOReturnBadArgument;
        const uint32_t admission = head->flags & TegraGM20BJobAdmission;
        const uint64_t cookie =
            admission ? reinterpret_cast<const TegraGM20BAbiSubmitAdmission *>(shared + head->shared_count)->cookie : 0;
        auto held = admitted.find(head->channel);
        if (admission & TegraGM20BJobAdmissionCancel) {
            if (held != admitted.end() && held->second.first == cookie)
                admitted.erase(held);
            reply = {};
            return KERN_SUCCESS;
        }
        if (bool(admission & TegraGM20BJobAdmissionContinue) != (held != admitted.end()) ||
            (held != admitted.end() && held->second.first != cookie))
            return kIOReturnBadArgument;
        std::vector<uint64_t> named;
        for (uint32_t i = 0; i < head->shared_count; ++i)
            if (const uint64_t bo = resolve(shared[i]))
                named.push_back(bo);
        for (const uint64_t bo : named) {
            if (held != admitted.end() &&
                std::find(held->second.second.begin(), held->second.second.end(), bo) == held->second.second.end())
                return kIOReturnBadArgument;
            if (held == admitted.end() && lockedByCpu(bo))
                return kIOReturnCannotLock;
        }
        if (admission & TegraGM20BJobAdmissionBegin)
            admitted[head->channel] = {cookie, named};
        if (admission & TegraGM20BJobAdmissionEnd)
            admitted.erase(head->channel);
        if (staleViews) {
            staleViews--;
            return kIOReturnNotReady;
        }
        std::vector<TegraGM20BAbiSurfaceUse> uses(shared, shared + head->shared_count);
        if (set) {
            uses.insert(uses.end(), set->members.begin(), set->members.end());
            set->used |= 1u << (head->channel - 1);
            setJobs++;
        }
        submitJobs++;
        return run(*s, head->channel, std::move(uses), reply);
    }

    void endGeneration(uint32_t index)
    {
        reinterpret_cast<volatile uint32_t *>(fences)[TegraGM20BSetGenerationWord + index] = ++sets[index].generation;
    }

    int setCreate(const TegraGM20BAbiResourceSetCreate &in, TegraGM20BAbiResourceSetCreateReply &out)
    {
        if (!in.count || in.count > TegraGM20BMaxSetMembers || !in.uses_address || in.set > TegraGM20BMaxResourceSets)
            return kIOReturnBadArgument;
        uint32_t index = in.set ? in.set - 1 : 0;
        if (in.set && !sets[index].live)
            return kIOReturnBadArgument;
        while (!in.set && index < TegraGM20BMaxResourceSets && sets[index].live)
            ++index;
        if (index == TegraGM20BMaxResourceSets)
            return kIOReturnNoResources;
        auto *uses = reinterpret_cast<TegraGM20BAbiSurfaceUse *>(uintptr_t(in.uses_address));
        constexpr uint32_t kAccess = TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite;
        for (uint32_t i = 0; i < in.count; ++i)
            if (uses[i].reserved || !(uses[i].access & kAccess) || (uses[i].access & ~kAccess) ||
                !bos.count(uses[i].handle))
                return kIOReturnBadArgument;
        for (const auto &member : sets[index].members)
            if (const auto it = bos.find(member.handle); it != bos.end())
                it->second.set = 0;
        sets[index].members.clear();
        for (uint32_t i = 0; i < in.count; ++i) {
            const BoState &bo = bos.at(uses[i].handle);
            if (bo.cpuReached || (bo.set && bo.set != index + 1))
                uses[i].access = 0;
            else
                sets[index].members.push_back(uses[i]);
        }
        for (const auto &member : sets[index].members)
            bos[member.handle].set = index + 1;
        sets[index].live = true;
        endGeneration(index);
        out = {index + 1, sets[index].generation, kFenceToken, (TegraGM20BSetGenerationWord + index) * 4};
        return KERN_SUCCESS;
    }

    void freeBo(uint64_t handle)
    {
        const auto it = bos.find(handle);
        if (it == bos.end())
            return;
        if (const uint32_t set = it->second.set) {
            auto &members = sets[set - 1].members;
            for (auto at = members.begin(); at != members.end(); ++at)
                if (at->handle == handle) {
                    members.erase(at);
                    break;
                }
            endGeneration(set - 1);
        }
        bos.erase(it);
        for (auto bound = binds.begin(); bound != binds.end();)
            bound = bound->second.first == handle ? binds.erase(bound) : std::next(bound);
    }

    void cpuLease(uint64_t handle)
    {
        const auto it = bos.find(handle);
        if (it == bos.end())
            return;
        it->second.cpuReached = true;
        if (const uint32_t set = it->second.set) {
            auto &members = sets[set - 1].members;
            for (auto at = members.begin(); at != members.end();)
                at = at->handle == handle ? members.erase(at) : std::next(at);
            it->second.set = 0;
            endGeneration(set - 1);
        }
    }

    void destroyed(uint32_t channel, bool killed)
    {
        for (uint32_t i = 0; i < TegraGM20BMaxResourceSets; ++i)
            if (killed && sets[i].live && (sets[i].used & (1u << (channel - 1)))) {
                sets[i].used &= ~(1u << (channel - 1));
                endGeneration(i);
            }
    }

    int wait(const TegraGM20BAbiWaitFence &in)
    {
        Slot *s = live(in.channel);
        if (!s)
            return kIOReturnBadArgument;
        if (s->error)
            return kIOReturnNotResponding;
        if (notResponding) {
            notResponding--;
            return kIOReturnNotResponding;
        }
        return int32_t(fence(in.channel - 1).completed - in.seqno) >= 0 ? KERN_SUCCESS : kIOReturnTimeout;
    }

    int status(const TegraGM20BAbiChannel &in, TegraGM20BAbiStatus &out)
    {
        Slot *s = live(in.channel);
        if (!s)
            return kIOReturnBadArgument;
        out = {fence(in.channel - 1).completed, s->submitted, s->error, 0, 0};
        return KERN_SUCCESS;
    }
};

inline Model model;

}  // namespace kernel_model

extern "C" kern_return_t IOConnectCallStructMethod(mach_port_t, uint32_t method, const void *input, size_t,
                                                   void *output, size_t *)
{
    using kernel_model::model;
    switch (method) {
    case TegraGM20BMethodGetInfo: {
        auto &info = *static_cast<TegraGM20BAbiInfo *>(output);
        info = {};
        info.abi_version = TegraGM20BAbiVersion;
        info.flags = TegraGM20BInfoKernelMap | TegraGM20BInfoSubmitAddress | TegraGM20BInfoKillRecord |
                     TegraGM20BInfoCpuAccess;
        info.cls_copy = 0xb0b5;
        info.cls_eng3d = 0xb197;
        info.va_start = 0x1000000;
        info.va_end = uint64_t(1) << 40;
        info.page_size = 4096;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoAlloc: {
        const auto &in = *static_cast<const TegraGM20BAbiBoAlloc *>(input);
        const uint32_t token = uint32_t(model.nextHandle);
        model.memory[token].assign(in.bytes, 0);
        model.bos[model.nextHandle] = {false, !(in.flags & TegraGM20BBoCoherent), 0};
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, token, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoImport:
        model.bos[model.nextHandle] = {true, true, 0};
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, 0, 0};
        return KERN_SUCCESS;
    case TegraGM20BMethodBoFree:
        model.freed.push_back(static_cast<const TegraGM20BAbiBoFree *>(input)->handle);
        model.freeBo(static_cast<const TegraGM20BAbiBoFree *>(input)->handle);
        return KERN_SUCCESS;
    case TegraGM20BMethodVmBind: {
        const auto &op = static_cast<const TegraGM20BAbiVmBind *>(input)->ops[0];
        model.binds[op.va] = {op.handle, op.range};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCpuAcquire:
        model.cpuLease(static_cast<const TegraGM20BAbiCpuAccess *>(input)->handle);
        return KERN_SUCCESS;
    case TegraGM20BMethodCpuRelease:
        return KERN_SUCCESS;
    case TegraGM20BMethodResourceSetCreate:
        return model.setCreate(*static_cast<const TegraGM20BAbiResourceSetCreate *>(input),
                               *static_cast<TegraGM20BAbiResourceSetCreateReply *>(output));
    case TegraGM20BMethodSubmitJob:
        return model.submitJob(*static_cast<const TegraGM20BAbiSubmitAddress *>(input),
                               *static_cast<TegraGM20BAbiExecReply *>(output));
    case TegraGM20BMethodMap: {
        const auto &in = *static_cast<const TegraGM20BAbiMap *>(input);
        auto &reply = *static_cast<TegraGM20BAbiMapReply *>(output);
        if (in.token == kernel_model::kFenceToken)
            reply = {uint64_t(uintptr_t(model.fences)), sizeof model.fences};
        else
            reply = {uint64_t(uintptr_t(model.memory[in.token].data())), model.memory[in.token].size()};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodChannelCreate:
        return model.create(*static_cast<TegraGM20BAbiChannelCreateReply *>(output));
    case TegraGM20BMethodChannelDestroy: {
        const auto &in = *static_cast<const TegraGM20BAbiChannel *>(input);
        kernel_model::Slot *s = model.live(in.channel);
        if (!s)
            return kIOReturnBadArgument;
        model.destroys++;
        model.destroyed(in.channel, s->error != 0);
        s->uid = 0;
        s->foreign = model.foreignTakesFreedSlot;
        model.foreignTakesFreedSlot = false;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSubmitAddress:
        return model.submit(*static_cast<const TegraGM20BAbiSubmitAddress *>(input),
                            *static_cast<TegraGM20BAbiExecReply *>(output));
    case TegraGM20BMethodWaitFence:
        return model.wait(*static_cast<const TegraGM20BAbiWaitFence *>(input));
    case TegraGM20BMethodGetStatus:
        return model.status(*static_cast<const TegraGM20BAbiChannel *>(input),
                            *static_cast<TegraGM20BAbiStatus *>(output));
    case TegraGM20BMethodCpuComplete: {
        const auto &in = *static_cast<const TegraGM20BAbiCpuComplete *>(input);
        *static_cast<TegraGM20BAbiCpuCompleteReply *>(output) = {in.range_count, 0, 0, 0, 0, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodUnmap:
        return KERN_SUCCESS;
    default:
        return kIOReturnUnsupported;
    }
}

extern "C" io_service_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef matching)
{
    CFRelease(matching);
    return 111;
}

extern "C" kern_return_t IOServiceOpen(io_service_t, task_port_t, uint32_t, io_connect_t *connection)
{
    *connection = 42;
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOObjectRelease(io_object_t)
{
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOServiceClose(io_connect_t)
{
    return KERN_SUCCESS;
}
