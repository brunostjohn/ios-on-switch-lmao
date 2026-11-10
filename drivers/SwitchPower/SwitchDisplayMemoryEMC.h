// SPDX-License-Identifier: BSD-2-Clause-Patent

unsigned SwitchEMC::memoryFloor(unsigned first) const
{
    uint64_t bytes = __atomic_load_n(&gDisplayBytes, __ATOMIC_ACQUIRE);
    if (memoryState.owned[0])
        bytes = switch_display::memoryMax(bytes, memoryState.heads[0].peak_bytes_per_second);
    if (memoryState.owned[1])
        bytes = switch_display::add(bytes, memoryState.heads[1].peak_bytes_per_second);
    const unsigned bandwidth = switch_display::row(khz, count, bytes);
    if (first < bandwidth)
        first = bandwidth;
    return memoryState.floor(khz, count, first, khz[cur]);
}

bool SwitchEMC::programMemory(uint32_t before, uint32_t after, bool force)
{
    if (!memoryState.owned[0] && !memoryState.owned[1])
        return true;
    if (!force && !memoryDirty && memoryAppliedKhz == after)
        return true;
    if (!memoryQualified || !memoryState.program(mcRegs, before, after))
        return false;
    memoryAppliedKhz = after;
    memoryDirty = false;
    return true;
}

IOReturn SwitchEMC::memoryAction(OSObject *owner, void *argument, void *result, void *, void *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    const auto &request = *static_cast<const switch_display_memory_head *>(argument);
    int error = self->memoryQualified ? 0 : -95;
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
        if (floor >= self->count || (floor > self->cur && !self->moveTo(floor))) {
            error = -28;
        } else {
            self->memoryState = next;
            self->memoryDirty = true;
            if (!self->programMemory(self->khz[self->cur], self->khz[self->cur], true)) {
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

extern "C" int switch_emc_display_memory_reserve(const switch_display_memory_head *request)
{
    if (!request)
        return -22;
    SwitchEMC *emc = __atomic_load_n(&gEMC, __ATOMIC_ACQUIRE);
    return emc ? emc->reserveMemory(*request) : -19;
}
