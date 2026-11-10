// SPDX-License-Identifier: BSD-2-Clause-Patent

unsigned SwitchEMC::reconcileRate(uint32_t observed, bool quiesced)
{
    unsigned row = switch_emc_row(khz, count, observed);
    if (row < count && !quiesced)
        return row;
    memoryDirty = true;
    if (!programMemory(quiesced ? khz[count - 1] : observed, khz[count - 1], true))
        panic("SwitchEMC: unrecognized rate %u cannot cover the running display", observed);
    unsigned restored = observed;
    if ((row != count - 1 && tegra_bpmp_emc_set(khz[count - 1], &restored)) ||
        (row = switch_emc_row(khz, count, restored)) >= count)
        panic("SwitchEMC: cannot restore a qualified rate after %u kHz", observed);
    if (!programMemory(restored, restored, true))
        panic("SwitchEMC: reconciled MC admission no longer covers the running display");
    return row;
}

bool SwitchEMC::moveTo(unsigned index)
{
    const unsigned floor = switch_display::row(khz, count, __atomic_load_n(&gDisplayBytes, __ATOMIC_ACQUIRE));
    if (floor >= count)
        index = count - 1;
    else if (index < floor)
        index = floor;
    index = memoryFloor(index);
    if (index >= count || !programMemory(khz[cur], khz[index]))
        return false;
    if (index == cur)
        return true;
    unsigned now = 0;
    const int err = tegra_bpmp_emc_set(khz[index], &now);
    if (err) {
        if (!refusalLogged) {
            os_log_error(OS_LOG_DEFAULT, "SwitchEMC: the BPMP refused %u kHz (%d); staying at %u kHz", khz[index], err,
                         khz[cur]);
        }
        refusalLogged = true;
        (void)programMemory(khz[cur], khz[cur], true);
        return false;
    }
    const unsigned actual = reconcileRate(now);
#if DEVELOPMENT || DEBUG
    const uint64_t t = mach_absolute_time();
    residency[cur] += t - since;
    since = t;
    moves++;
#endif
    cur = actual;
    if (!programMemory(khz[cur], khz[cur], true))
        panic("SwitchEMC: restored MC admission no longer covers the running display");
    return cur >= index;
}

IOReturn SwitchEMC::governAction(OSObject *owner, void *resume, void *, void *, void *)
{
    auto *self = static_cast<SwitchEMC *>(owner);
    unsigned now = 0;
    if (self->governing)
        return kIOReturnSuccess;
    if (tegra_bpmp_emc_get(&now)) {
        if (!resume)
            return kIOReturnError;
        now = 0;
    }
    self->cur = self->reconcileRate(now, resume != nullptr);
#if DEVELOPMENT || DEBUG
    self->since = mach_absolute_time();
#endif
    self->memoryDirty = true;
    if (!self->moveTo(self->cur))
        return kIOReturnError;
    self->governing = true;
    self->startActmon();
    return kIOReturnSuccess;
}
