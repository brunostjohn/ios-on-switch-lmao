// SPDX-License-Identifier: BSD-2-Clause-Patent
// The row-pitch probe (SwitchVICPitchProbe.h), debug kernels only. Its two surfaces are a bounce-pool slot, which is
// VIC-mapped kernel memory, and the CPU touches them only under the backing's CPU leases.
#include "SwitchVICPitchProbe.h"
#include "TegraVIC.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include "SwitchVICBouncePool.h"
#include "SwitchVICJobs.h"

namespace {
using namespace switch_vic_pitch_probe;

uint8_t *bytesOf(const SwitchVICBounceBuffer &buffer)
{
    auto *memory = OSDynamicCast(IOBufferMemoryDescriptor, buffer.memory);
    return memory ? static_cast<uint8_t *>(memory->getBytesNoCopy()) : nullptr;
}

TegraVICImage bgra(const SwitchVICBounceBuffer &buffer, uint32_t pixels, uint32_t pitch)
{
    TegraVICImage image = {};
    image.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    image.width = pixels;
    image.height = kRows;
    image.pitch[0] = pitch;
    image.rows[0] = kRows;
    image.base[0] = switch_vic_mapping_address(buffer.mapping);
    image.span[0] = kBufferBytes;
    return image;
}

int cpuWrite(const SwitchVICBounceBuffer &buffer, void (*write)(uint8_t *, uint32_t, uint32_t), uint32_t pitch,
             uint32_t pixels)
{
    constexpr uint32_t access = SWITCH_GPU_BACKING_WRITE;
    if (const int error = switch_gpu_backing_cpu_acquire(buffer.backing, 0, kBufferBytes, access))
        return error;
    write(bytesOf(buffer), pitch, pixels);
    return switch_gpu_backing_cpu_release(buffer.backing, 0, kBufferBytes, access);
}

void poison(uint8_t *bytes, uint32_t, uint32_t)
{
    memset(bytes, kPoison, kBufferBytes);
}

// 0, or the error that stops the probe (a job the engine failed: the next one would not tell anything more).
int runCase(const Case &c, const SwitchVICBounceSlot &slot, SwitchVICPitchResult *r)
{
    const SwitchVICBounceBuffer &source = slot.buffers[0], &destination = slot.buffers[1];
    const uint32_t pixels = width(c);
    *r = {c.sourcePitch, c.destinationPitch, pixels, 0, 0, 0, 0, 0, 0};
    int error = cpuWrite(source, fillSource, c.sourcePitch, pixels);
    if (!error)
        error = cpuWrite(destination, poison, 0, 0);
    TegraVICTraits traits = *tegra_vic_traits();
    traits.sourcePitchAlign = 64;
    const TegraVICImage in = bgra(source, pixels, c.sourcePitch), out = bgra(destination, pixels, c.destinationPitch);
    TegraVICOp op = {};
    op.srcW = uint64_t(pixels) << 16;
    op.srcH = uint64_t(kRows) << 16;
    op.dstW = pixels;
    op.dstH = kRows;
    op.nearest = 1;
    TegraVICJob job;
    if (!error && tegra_vic_build(&in, &out, &op, &traits, tegra_vic_config_address(), tegra_vic_filter_address(), &job)) {
        r->status = -95;
        return 0;
    }
    switch_gpu_backing_use uses[] = {{source.backing, SWITCH_GPU_BACKING_READ, 0},
                                     {destination.backing, SWITCH_GPU_BACKING_WRITE, 0}};
    const SwitchVICOwnership ownership = {uses, 2, nullptr, 0, nullptr, 0};
    uint32_t fence = 0;
    if (!error)
        error = switch_vic_submit_checked(&job, 1, nullptr, ownership, nullptr, nullptr, &fence);
    if (!error)
        error = switch_vic_wait(fence);
    if (!error)
        error = switch_gpu_backing_cpu_acquire(destination.backing, 0, kBufferBytes, SWITCH_GPU_BACKING_READ);
    if (error) {
        r->status = error;
        return error;
    }
    compare(bytesOf(destination), c.destinationPitch, pixels, r);
    return switch_gpu_backing_cpu_release(destination.backing, 0, kBufferBytes, SWITCH_GPU_BACKING_READ);
}
}  // namespace

IOReturn switch_vic_pitch_probe_call(const IOExternalMethodArguments *a)
{
    const auto *request = static_cast<const SwitchVICPitchProbeRequest *>(a->structureInput);
    if (a->scalarInputCount || a->scalarOutputCount || a->structureInputSize != sizeof(*request) || !request ||
        request->magic != SWITCH_VIC_PITCH_PROBE_MAGIC || request->reserved ||
        a->structureOutputSize != sizeof(SwitchVICPitchProbeReply) || !a->structureOutput)
        return kIOReturnBadArgument;
    if (!tegra_vic_ready() || !tegra_vic_traits())
        return kIOReturnNotReady;
    TegraVICSurfaceDesc desc = {};
    desc.fourcc = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
    desc.width = kMaxPitch / 4;
    desc.height = kRows + kSlackRows;
    desc.planes = 1;
    desc.planeBytesPerRow[0] = kMaxPitch;
    desc.planeWidth[0] = desc.width;
    desc.planeHeight[0] = desc.height;
    desc.length = kBufferBytes;
    SwitchVICBounceSlot *slot = nullptr;
    if (switch_vic_bounce_acquire(&desc, &desc, &slot))
        return kIOReturnNoMemory;
    auto *reply = IONewZero(SwitchVICPitchProbeReply, 1);
    int error = reply && bytesOf(slot->buffers[0]) && bytesOf(slot->buffers[1]) ? 0 : -12;
    for (uint32_t i = 0; !error && i < SWITCH_VIC_PITCH_PROBE_CASES; i++, reply->count++)
        error = runCase(kCases[i], *slot, &reply->results[i]);
    switch_vic_bounce_release(slot);
    if (!reply)
        return kIOReturnNoMemory;
    reply->magic = SWITCH_VIC_PITCH_PROBE_MAGIC;
    summarise(reply);
    IOLog("TegraVIC: pitch probe: %u cases, controls %s, clean cases %#x, error %d\n", reply->count,
          reply->controlsClean ? "clean" : "NOT clean", reply->cleanCases, error);
    memcpy(a->structureOutput, reply, sizeof(*reply));
    IODelete(reply, SwitchVICPitchProbeReply, 1);
    return error == -12 ? kIOReturnNoMemory : kIOReturnSuccess;
}
#endif
