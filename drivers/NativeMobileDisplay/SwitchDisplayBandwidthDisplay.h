// SPDX-License-Identifier: BSD-2-Clause-Patent
switch_display::Reservation displayBandwidth;
switch_display::AdmissionEnvelope planeAdmission;
switch_display::MemoryEnvelope displayMemory;

switch_display::Demand planeDemand(const native_planes::Image &image)
{
    switch_display::Demand demand = {image.position & 0xffff, image.position >> 16, image.size & 0xffff,
                                     image.size >> 16, uint64_t(displayMemory.pixelClock()) * (image.yuv ? 2 : 4)};
    const uint32_t sourceWidth = (image.prescaled & 0xffff) / (image.yuv ? 1 : 4);
    demand.bytesPerSecond = switch_display::scaled(demand.bytesPerSecond, sourceWidth, demand.width);
    demand.bytesPerSecond = switch_display::scaled(demand.bytesPerSecond, image.prescaled >> 16, demand.height);
    return demand;
}

bool planeMemory(const native_planes::Image &image, switch_display_memory_window &out, bool conversion)
{
    out = {};
    if (!image.visible)
        return true;
    const auto demand = planeDemand(image);
    const uint32_t sourceWidth = (image.prescaled & 0xffff) / (image.yuv ? 1 : 4);
    if (!switch_display::memoryWindow(out, demand.bytesPerSecond, sourceWidth, image.prescaled >> 16, demand.width,
                                      demand.height, image.yuv, !image.surfaceKind, false))
        return false;
    if (conversion) {
        switch_display_memory_window rgb = {};
        const uint64_t bytes =
            image.yuv ? switch_display::add(demand.bytesPerSecond, demand.bytesPerSecond) : demand.bytesPerSecond;
        if (!switch_display::memoryWindow(rgb, bytes, demand.width, demand.height, demand.width, demand.height, false,
                                          true, false))
            return false;
        switch_display::mergeMemoryWindow(out, rgb);
    }
    return true;
}

static int bandwidthRecorded(uint64_t)
{
    return 0;
}

uint64_t planeBandwidth(const PlaneSet &p)
{
    uint32_t width = 0, height = 0;
    getDisplayDimensions(&width, &height);
    switch_display::Demand demand[3] = {{0, 0, width, height, uint64_t(displayMemory.pixelClock()) * 4}, {}, {}};
    for (unsigned window = 0; window != 2; ++window)
        if (p.window[window].on)
            demand[window + 1] = planeDemand(p.window[window].image);
    return switch_display::peak(demand, 3);
}

uint64_t planeAdmissionDemand(const native_planes::Image &image)
{
    const uint64_t bytes = planeDemand(image).bytesPerSecond;
    return image.yuv ? switch_display::add(bytes, bytes) : bytes;
}

bool validatePlaneBandwidth(const bool present[native_planes::kLayers], const native_planes::Layer *layers,
                            uint32_t width, uint32_t height)
{
    uint64_t updated[4] = {}, active[2] = {};
    switch_display_memory_window memoryUpdated[4] = {}, memoryActive[2] = {};
    for (unsigned layer = 0; layer != native_planes::kLayers; ++layer) {
        if (!present[layer])
            continue;
        native_planes::Image image;
        if (!native_planes::imageOf(layers[layer], width, height, image))
            return false;
        if (image.visible) {
            updated[layer] = planeAdmissionDemand(image);
            if (!planeMemory(image, memoryUpdated[layer], true))
                return false;
        }
    }
    const int states[] = {planeLatch.active, planeLatch.armed, planeLatch.superseded};
    for (int state : states) {
        if (state < 0)
            continue;
        const auto &set = planeSetOf(state);
        for (unsigned window = 0; window != 2; ++window)
            if (set.window[window].on) {
                switch_display_memory_window memory = {};
                if (!planeMemory(set.window[window].image, memory, true))
                    return false;
                switch_display::mergeMemoryWindow(memoryActive[window], memory);
                const uint64_t bytes = planeAdmissionDemand(set.window[window].image);
                if (bytes > active[window])
                    active[window] = bytes;
            }
    }
    // Updated masks omit inherited layers. Reserve any two accepted streams, including a later RGB conversion,
    // until the complete accepted queue retires; bank reassignment cannot introduce a new demand at arm time.
    return planeAdmission.admit(updated, active, uint64_t(displayMemory.pixelClock()) * 4, [&](uint64_t bytes) {
        return displayMemory.admit(memoryUpdated, memoryActive, bytes, switch_emc_display_memory_reserve) &&
               displayBandwidth.raise(bytes, bandwidthRecorded);
    });
}

bool planeReserveBandwidth(const PlaneSet &p)
{
    uint32_t width = 0, height = 0;
    getDisplayDimensions(&width, &height);
    const switch_display::Demand ui = {0, 0, width, height, uint64_t(displayMemory.pixelClock()) * 4};
    switch_display::Demand windows[2][4] = {};
    switch_display_memory_window memory[2] = {};
    unsigned count = 0;
    const PlaneSet *states[] = {&p, &planeSetOf(planeLatch.active), &planeSetOf(planeLatch.armed),
                                &planeSetOf(planeLatch.superseded)};
    for (const auto *state : states) {
        for (unsigned window = 0; window != 2; ++window)
            if (state->window[window].on) {
                windows[window][count] = planeDemand(state->window[window].image);
                switch_display_memory_window candidate = {};
                if (!planeMemory(state->window[window].image, candidate, false))
                    return false;
                switch_display::mergeMemoryWindow(memory[window], candidate);
            }
        ++count;
    }
    const uint64_t bytes = switch_display::transitionPeak(ui, windows[0], count, windows[1], count);
    return displayMemory.arm(memory, bytes, switch_emc_display_memory_reserve) &&
           displayBandwidth.raise(bytes, bandwidthRecorded);
}

void planeRetireBandwidth()
{
    if (planeRecovering || planeLatch.requested() || planeHeldSet || gpuScanout || firstUntaken())
        return;
    for (bool pending : entryPlaneSet)
        if (pending)
            return;
    const auto &active = planeSetOf(planeLatch.active);
    switch_display_memory_window memory[2] = {};
    for (unsigned window = 0; window != 2; ++window)
        if (active.window[window].on && !planeMemory(active.window[window].image, memory[window], false))
            return;
    const uint64_t bytes = planeBandwidth(active);
    if (displayMemory.retire(memory, bytes, switch_emc_display_memory_reserve) &&
        displayBandwidth.retire(bytes, bandwidthRecorded))
        planeAdmission.clear();
}
