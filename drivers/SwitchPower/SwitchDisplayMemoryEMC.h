// SPDX-License-Identifier: BSD-2-Clause-Patent

// The display's bandwidth demand: the reservation or its first head's plan, and the second head's, parked or not.
uint64_t SwitchEMC::floorBytes() const
{
    uint64_t bytes = __atomic_load_n(&gDisplayBytes, __ATOMIC_ACQUIRE);
    const switch_display::MemoryState *states[] = {&memoryState, &memoryParked};
    for (const auto *state : states) {
        if (state->owned[0])
            bytes = switch_display::memoryMax(bytes, state->heads[0].peak_bytes_per_second);
        if (state->owned[1])
            bytes = switch_display::add(bytes, state->heads[1].peak_bytes_per_second);
    }
    return bytes;
}

unsigned SwitchEMC::memoryFloor(unsigned first) const
{
    const unsigned bandwidth = switch_display::row(khz, count, floorBytes());
    if (first < bandwidth)
        first = bandwidth;
    return memoryState.floor(khz, count, first, khz[cur]);
}

bool SwitchEMC::programMemory(uint32_t before, uint32_t after, bool force)
{
    if ((memoryState.owned[0] || memoryState.owned[1]) && (force || memoryDirty || memoryAppliedKhz != after)) {
        if (!__atomic_load_n(&memoryQualified, __ATOMIC_ACQUIRE) || !memoryState.program(mcRegs, before, after))
            return false;
        memoryAppliedKhz = after;
        memoryDirty = false;
    }
    if (before == after)
        unparkDisplayMemory(after);
    return true;
}

IOReturn SwitchEMC::memoryAction(OSObject *owner, void *argument, void *result, void *, void *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    const auto &request = *static_cast<const switch_display_memory_head *>(argument);
    int error = __atomic_load_n(&self->memoryQualified, __ATOMIC_ACQUIRE) ? 0 : -95;
    if (request.head > 1)
        error = -22;
    if (!error && !self->memoryState.same(request)) {
        const auto old = self->memoryState;
        auto next = old;
        next.heads[request.head] = request;
        next.owned[request.head] = true;
        uint64_t bytes =
            next.owned[0] ? next.heads[0].peak_bytes_per_second : __atomic_load_n(&gDisplayBytes, __ATOMIC_ACQUIRE);
        if (next.owned[1])
            bytes = switch_display::add(bytes, next.heads[1].peak_bytes_per_second);
        unsigned floor = switch_display::row(self->khz, self->count, bytes);
        while (floor < self->count) {
            switch_display::MemoryPlan plans[2];
            if (next.calculate(self->khz[floor], plans))
                break;
            ++floor;
        }
        if (floor >= self->count || (floor > self->cur && !self->moveTo(floor, SWITCH_EMC_REASON_DISPLAY_MEMORY))) {
            error = -28;
        } else {
            const bool parked = self->memoryParked.owned[request.head];
            self->memoryParked.owned[request.head] = false;  // this request supersedes it
            self->memoryState = next;
            self->memoryDirty = true;
            if (!self->programMemory(self->khz[self->cur], self->khz[self->cur], true)) {
                self->memoryParked.owned[request.head] = parked;
                self->memoryState = old;
                self->memoryDirty = true;
                (void)self->programMemory(self->khz[self->cur], self->khz[self->cur], true);
                error = -28;
            } else {
                if (!request.head)
                    __atomic_store_n(&gDisplayBytes, request.peak_bytes_per_second, __ATOMIC_RELEASE);
                if (self->governing)
                    self->program();
            }
        }
    }
    *static_cast<int *>(result) = error;
    return kIOReturnSuccess;
}

int SwitchEMC::reserveMemory(const switch_display_memory_head &request)
{
    int error = 0;
    loop->runAction(memoryAction, this, const_cast<switch_display_memory_head *>(&request), &error);
    return error;
}

// A plan that no longer covers the rate running now (0: not known) is made again at that rate, keeping each head that
// still fits there. A head that does not is parked: the display is told admission is not ready (it shows the UI alone)
// until a later rate carries the head again (the governor climbs for its bandwidth) or the head's next request is
// planned at the rate then (-28 if it cannot fit). The display's envelope sends no request it already holds, so the
// head is kept, not dropped.
void SwitchEMC::replanDisplayMemory(uint32_t runningKhz, const char *why)
{
    static constexpr bool kKeep[][2] = {{true, true}, {true, false}, {false, true}};
    const auto held = memoryState;
    memoryState = {};
    for (const auto &keep : kKeep) {
        auto next = held;
        for (unsigned head = 0; head != 2; ++head)
            next.owned[head] = held.owned[head] && keep[head];
        if (runningKhz && next.program(mcRegs, runningKhz, runningKhz)) {
            memoryState = next;
            break;
        }
    }
    memoryAppliedKhz = runningKhz;
    memoryDirty = false;
    const bool lost[2] = {held.owned[0] && !memoryState.owned[0], held.owned[1] && !memoryState.owned[1]};
    for (unsigned head = 0; head != 2; ++head) {
        if (lost[head]) {
            memoryParked.heads[head] = held.heads[head];
            memoryParked.owned[head] = true;
        }
    }
    __atomic_store_n(&memoryUncovered, memoryParked.owned[0] || memoryParked.owned[1], __ATOMIC_RELEASE);
    if (lost[0] || lost[1]) {
        os_log_error(OS_LOG_DEFAULT, "SwitchEMC: display memory planned again at %u kHz, as %s: %s parked", runningKhz,
                     why,
                     lost[0] && lost[1] ? "heads 0 and 1"
                     : lost[0]          ? "head 0"
                                        : "head 1");
    }
}

// A settled rate: parked heads it carries are planned at it again, both if they fit, else the one that does.
void SwitchEMC::unparkDisplayMemory(uint32_t runningKhz)
{
    static constexpr bool kTake[][2] = {{true, true}, {true, false}, {false, true}};
    for (const auto &take : kTake) {
        auto next = memoryState;
        bool taken[2] = {};
        for (unsigned head = 0; head != 2; ++head) {
            taken[head] = take[head] && memoryParked.owned[head];
            if (taken[head]) {
                next.heads[head] = memoryParked.heads[head];
                next.owned[head] = true;
            }
        }
        if ((!taken[0] && !taken[1]) || !runningKhz || !next.program(mcRegs, runningKhz, runningKhz))
            continue;
        memoryState = next;
        memoryAppliedKhz = runningKhz;
        memoryDirty = false;
        for (unsigned head = 0; head != 2; ++head) {
            if (taken[head]) {
                memoryParked.owned[head] = false;
                os_log_error(OS_LOG_DEFAULT, "SwitchEMC: display memory head %u planned again at %u kHz", head,
                             runningKhz);
            }
        }
        break;
    }
    __atomic_store_n(&memoryUncovered, memoryParked.owned[0] || memoryParked.owned[1], __ATOMIC_RELEASE);
}

extern "C" int switch_emc_display_memory_reserve(const switch_display_memory_head *request)
{
    if (!request)
        return -22;
    SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE);
    return emc ? emc->reserveMemory(*request) : -19;
}

extern "C" int switch_emc_display_memory_ready(void)
{
    const SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE);
    return emc && emc->displayMemoryReady();
}
