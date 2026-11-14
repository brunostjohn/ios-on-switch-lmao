// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GICv2 sequences ArmCpu and ArmGIC run on every interrupt and every IPI, with their barriers in one place:
// ArmCpu's acknowledge loop (GICC_IAR, the handler, GICC_EOIR), an SGI sent through GICD_SGIR, and ArmGIC's run of an
// SPI's IOKit vector with the line masked or unmasked at the distributor. test/switch_gic_barrier_test.cpp runs these
// same templates against a model that counts the barriers.
#ifndef SWITCH_GIC_V2_H
#define SWITCH_GIC_V2_H

#include <stdint.h>

#ifdef SWITCH_GIC_HOST_MODEL
namespace switchgic {
void hostBarrier(const char *instruction);
}

#define SWITCH_GIC_BARRIER(instruction) ::switchgic::hostBarrier(instruction)
#else
#define SWITCH_GIC_BARRIER(instruction) __asm__ volatile(instruction ::: "memory")
#endif

namespace switchgic {

constexpr uint32_t kIpiSgi = 0, kTimerPpi = 27, kSpurious = 1020;

// Every interrupt pending at this core's CPU interface, until GICC_IAR reads spurious. `cpu` provides acknowledge()
// (GICC_IAR), end(acknowledge) (GICC_EOIR, with the source CPU bits of an SGI), timer() for PPI 27, ipi(acknowledge)
// for SGI 0 and other(intid) for everything else (the SPIs).
template <class Cpu> void acknowledgeLoop(Cpu &cpu)
{
    for (;;) {
        const uint32_t acknowledge = cpu.acknowledge();
        const uint32_t intid = acknowledge & 0x3ffU;
        SWITCH_GIC_BARRIER("dmb ish");
        if (intid >= kSpurious)
            return;
        if (intid == kTimerPpi) {
            cpu.timer();
        } else if (intid == kIpiSgi) {
            cpu.ipi(acknowledge);
        } else {
            cpu.other(intid);
        }
        SWITCH_GIC_BARRIER("dsb sy");
        cpu.end(acknowledge);
    }
}

// One SGI: `write` stores GICD_SGIR.
template <class Write> void sendSgi(Write write)
{
    SWITCH_GIC_BARRIER("dsb ishst");
    write();
    SWITCH_GIC_BARRIER("dsb sy");
    SWITCH_GIC_BARRIER("isb");
}

// An SPI's IOKit vector (IOInterruptVector), in ArmCpu's acknowledge loop: interruptActive brackets the handler for
// IOInterruptController::disableInterrupt on another core, which stores interruptDisabledSoft, issues a DMB and waits
// for interruptActive to clear. `run` calls the handler; `mask` writes the line's ICENABLER bit (and the Tegra LIC's).
template <class Vector, class Run, class Mask> void runVector(Vector &vector, Run run, Mask mask)
{
    vector.interruptActive = 1;
    SWITCH_GIC_BARRIER("dmb ish");
    if (!vector.interruptDisabledSoft)
        run();
    if (vector.interruptDisabledSoft && !vector.interruptDisabledHard) {
        vector.interruptDisabledHard = 1;
        mask();
        SWITCH_GIC_BARRIER("dsb sy");
    }
    SWITCH_GIC_BARRIER("dmb ish");
    vector.interruptActive = 0;
}

// A line masked from a thread (XNU's registerInterrupt and unregisterInterrupt): masked at the distributor on return.
template <class Mask> void maskFromThread(Mask mask)
{
    mask();
    SWITCH_GIC_BARRIER("dsb sy");
}

// A line unmasked (XNU's enableInterrupt).
template <class Unmask> void unmask(Unmask write)
{
    write();
    SWITCH_GIC_BARRIER("dsb sy");
}

}  // namespace switchgic

#endif
