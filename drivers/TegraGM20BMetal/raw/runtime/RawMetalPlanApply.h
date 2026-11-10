// SPDX-License-Identifier: MIT
#pragma once
#include "RawMetalPlan.h"

namespace raw {

template <typename Sink>
void applyRenderState(const RMCommandPlan &plan, uint32_t index, StateCursor &cursor, Sink &sink)
{
    if (cursor.render == index)
        return;
    const auto &next = plan.renderStates[index];
    const auto *previous = cursor.render == kNoSnapshot ? nullptr : &plan.renderStates[cursor.render];
    if (previous && previous->epoch != next.epoch)
        previous = nullptr;
    for (uint32_t stage = 0; stage < 2; ++stage) {
        if (!previous || previous->buffers[stage] != next.buffers[stage]) {
            const auto &bindings = plan.buffers[stage][next.buffers[stage]];
            const auto *old = previous ? &plan.buffers[stage][previous->buffers[stage]] : nullptr;
            for (uint32_t slot = 0; slot < bindings.size(); ++slot)
                if (bindings[slot].source != BufferBinding::Source::Unset && (!old || bindings[slot] != (*old)[slot]))
                    sink.buffer(stage + 1, slot, bindings[slot]);
        }
        if (!previous || previous->textures[stage] != next.textures[stage]) {
            const auto &bindings = plan.textures[stage][next.textures[stage]];
            const auto *old = previous ? &plan.textures[stage][previous->textures[stage]] : nullptr;
            for (uint32_t slot = 0; slot < bindings.objects.size(); ++slot)
                if ((bindings.set & (1u << slot)) &&
                    (!old || !(old->set & (1u << slot)) || bindings.objects[slot] != old->objects[slot]))
                    sink.texture(stage + 1, slot, bindings.objects[slot]);
        }
        if (!previous || previous->samplers[stage] != next.samplers[stage]) {
            const auto &bindings = plan.samplers[stage][next.samplers[stage]];
            const auto *old = previous ? &plan.samplers[stage][previous->samplers[stage]] : nullptr;
            for (uint32_t slot = 0; slot < bindings.objects.size(); ++slot)
                if ((bindings.set & (1u << slot)) &&
                    (!old || !(old->set & (1u << slot)) || bindings.objects[slot] != old->objects[slot]))
                    sink.sampler(stage + 1, slot, bindings.objects[slot]);
        }
    }
    if (!previous || previous->raster != next.raster)
        sink.raster(plan.raster[next.raster], previous ? &plan.raster[previous->raster] : nullptr);
    cursor.render = index;
}

template <typename Sink>
void applyKernelState(const RMCommandPlan &plan, uint32_t index, StateCursor &cursor, Sink &sink)
{
    if (cursor.compute == index)
        return;
    const auto &next = plan.kernelStates[index];
    const auto *previous = cursor.compute == kNoSnapshot ? nullptr : &plan.kernelStates[cursor.compute];
    if (previous && previous->epoch != next.epoch)
        previous = nullptr;
    if (!previous || previous->buffers != next.buffers) {
        const auto &bindings = plan.buffers[2][next.buffers];
        const auto *old = previous ? &plan.buffers[2][previous->buffers] : nullptr;
        for (uint32_t slot = 0; slot < bindings.size(); ++slot)
            if (bindings[slot].source != BufferBinding::Source::Unset && (!old || bindings[slot] != (*old)[slot]))
                sink.buffer(3, slot, bindings[slot]);
    }
    if (!previous || previous->textures != next.textures) {
        const auto &bindings = plan.textures[2][next.textures];
        const auto *old = previous ? &plan.textures[2][previous->textures] : nullptr;
        for (uint32_t slot = 0; slot < bindings.objects.size(); ++slot)
            if ((bindings.set & (1u << slot)) &&
                (!old || !(old->set & (1u << slot)) || bindings.objects[slot] != old->objects[slot]))
                sink.texture(3, slot, bindings.objects[slot]);
    }
    if (!previous || previous->samplers != next.samplers) {
        const auto &bindings = plan.samplers[2][next.samplers];
        const auto *old = previous ? &plan.samplers[2][previous->samplers] : nullptr;
        for (uint32_t slot = 0; slot < bindings.objects.size(); ++slot)
            if ((bindings.set & (1u << slot)) &&
                (!old || !(old->set & (1u << slot)) || bindings.objects[slot] != old->objects[slot]))
                sink.sampler(3, slot, bindings.objects[slot]);
    }
    if (!previous || previous->state != next.state)
        sink.kernel(plan.kernel[next.state], previous ? &plan.kernel[previous->state] : nullptr);
    cursor.compute = index;
}

template <typename T>
bool rasterChanged(const RasterState &next, const RasterState *previous, RasterState::Field field,
                   T RasterState::*member)
{
    return (next.set & field) && (!previous || !(previous->set & field) || next.*member != previous->*member);
}

inline float stateFloat(uint32_t value)
{
    float result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

inline double stateDouble(uint64_t value)
{
    double result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

}  // namespace raw
