// SPDX-License-Identifier: MIT
// What raw::Device submits, decoded by nvdec as raw/pack's mock kernel (mock_iokit.c, cap.cpp) decodes NVK's
// submissions for nvkframe: the kernel model (raw_kernel_model.h, whose IOKit calls replace the framework's) answers the
// device, every job it takes is decoded against the model's memory as the device left it, and completes. A binary
// links this unit to draw raw's side of eq (test/eq): RAWCAP_LOG, RAWCAP_PROGRAMS and NVDEC_REGS as cap.cpp reads them.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include "../../raw/pack/nvdec.h"
#include "../raw_kernel_model.h"

namespace {

class Capture {
public:
    Capture()
    {
        using kernel_model::model;
        model.vaEnd = uint64_t(1) << 40;  // the program window's quarter of the client VA
        if (const char *path = getenv("RAWCAP_LOG"))
            log_ = fopen(path, "w");
        model.bound = [this](const TegraGM20BAbiBindOp &op) {
            if (op.kind)
                kinds_[op.va] = {op.range, op.kind};
            else
                kinds_.erase(op.va);
        };
        model.jobAdded = [this] {
            const kernel_model::Job &job = model.jobs.back();
            decode(job);
            if (!model.inFlight)
                model.fence(job.channel - 1).completed = job.seqno;
        };
    }

    uint32_t kind(uint64_t va) const
    {
        auto at = kinds_.upper_bound(va);
        if (at == kinds_.begin())
            return 0;
        --at;
        return va - at->first < at->second.first ? at->second.second : 0;
    }

private:
    nvdec::Decoder &decoder(uint32_t channel)
    {
        auto &d = decoders_[channel];
        if (!d) {
            d = std::make_unique<nvdec::Decoder>(
                [](uint64_t va, uint64_t) -> const uint8_t * {
                    return kernel_model::model.host(va);
                },
                log_);
            d->vaKind = [this](uint64_t va) {
                return kind(va);
            };
            if (const char *path = getenv("RAWCAP_PROGRAMS"); path && !d->loadPrograms(path))
                fprintf(stderr, "eq_capture: cannot read programs %s\n", path);
        }
        return *d;
    }

    void decode(const kernel_model::Job &job)
    {
        if (!log_)
            return;
        nvdec::Decoder &d = decoder(job.channel);
        d.submitBegin();
        for (const TegraGM20BAbiPush &push : job.pushes)
            if (const uint8_t *words = kernel_model::model.host(push.va))
                d.push(reinterpret_cast<const uint32_t *>(words), push.dwords);
        d.submitEnd();
        fflush(log_);
    }

    FILE *log_ = nullptr;
    std::map<uint32_t, std::unique_ptr<nvdec::Decoder>> decoders_;
    std::map<uint64_t, std::pair<uint64_t, uint32_t>> kinds_;  // va: the bind's range and PTE kind
};

Capture capture;

}  // namespace

extern "C" uint32_t mock_iokit_va_kind(uint64_t va)
{
    return capture.kind(va);
}

extern "C" void pushcount_report(const char *)
{}

extern "C" void mock_iokit_comp_stats(unsigned long out[5])
{
    for (int i = 0; i < 5; i++)
        out[i] = 0;
}
