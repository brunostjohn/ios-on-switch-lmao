// SPDX-License-Identifier: MIT
// The TegraGM20B user client as raw::Kernel calls it (drivers/TegraGM20B/TegraGM20BABI.h), for the raw path's channel
// recovery tests. Channels live in the slots of one fence page: a channel takes the lowest free slot, and a slot's seqnos
// go on across the channels it holds, so a channel made again can land in a slot whose fence is behind the old one. Jobs
// (SubmitAddress) complete as they are submitted, or stay in flight until a kill, which writes the record and then moves
// the fence past every seqno given out, as the kernel does. Memory a killed job wrote holds undefined contents until a job
// writes it again; a job that only reads it is counted (staleReads). Its IOKit calls replace the framework's: include it
// in one unit of a test.
#pragma once
#include "TegraGM20BABI.h"
#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace kernel_model {

constexpr uint32_t kSlots = 16, kRecordBytes = 64, kFenceToken = 0x7fff0000;

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
    alignas(64) uint8_t fences[kSlots * kRecordBytes] = {};
    Slot slots[kSlots];
    uint64_t nextHandle = 1, nextUid = 1;
    std::map<uint32_t, std::vector<uint8_t>> memory;
    std::vector<Job> jobs;
    bool inFlight = false;  // jobs run until a kill
    unsigned refuse = 0;    // submissions refused from now, with `refusal`
    int refusal = kIOReturnNoResources;
    unsigned notResponding = 0;  // WaitFences answered kIOReturnNotResponding while GetStatus calls the channel healthy
    bool foreignTakesFreedSlot = false;
    unsigned creates = 0, destroys = 0, staleReads = 0;
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
        Job job{head->channel, ++s->submitted, s->uid, {uses, uses + head->surface_count}};
        for (const auto &use : job.uses) {
            if (use.access & TegraGM20BSurfaceWrite)
                undefined.erase(use.handle);
            else if (undefined.count(use.handle))
                staleReads++;
        }
        jobs.push_back(job);
        if (!inFlight)
            fence(head->channel - 1).completed = job.seqno;
        reply = {job.seqno, 0};
        return KERN_SUCCESS;
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
        info.flags = TegraGM20BInfoKernelMap | TegraGM20BInfoSubmitAddress | TegraGM20BInfoKillRecord;
        info.cls_copy = 0xb0b5;
        info.cls_eng3d = 0xb197;
        info.va_start = 0x1000000;
        info.va_end = 0x100000000;
        info.page_size = 4096;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoAlloc: {
        const auto &in = *static_cast<const TegraGM20BAbiBoAlloc *>(input);
        const uint32_t token = uint32_t(model.nextHandle);
        model.memory[token].assign(in.bytes, 0);
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, token, 0};
        return KERN_SUCCESS;
    }
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
    case TegraGM20BMethodVmBind:
    case TegraGM20BMethodBoFree:
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
