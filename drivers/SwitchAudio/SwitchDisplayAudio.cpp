// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchAudioDriver.h"
#include "../SwitchDisplayPort/SwitchDisplayPort.h"
#include <kern/thread.h>

using namespace switch_audio_hw;

bool SwitchAudio::displayInit()
{
    const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
                                 static_cast<IOOptionBits>(kIOMemoryKernelUserShared) |
                                 static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    memory[SwitchAudioMemoryDisplayControl] =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, PAGE_SIZE, kDmaMask);
    if (!memory[SwitchAudioMemoryDisplayControl])
        return false;
    bzero(memory[SwitchAudioMemoryDisplayControl]->getBytesNoCopy(), PAGE_SIZE);
    auto *page = static_cast<SwitchAudioControl *>(memory[SwitchAudioMemoryDisplayControl]->getBytesNoCopy());
    if (!displayAudio.initialize(gate, page, hz))
        return false;
    IOService *nub = switch_interrupt_nub(SwitchHDA::kIntid, false);
    displayInterrupt = IOInterruptEventSource::interruptEventSource(this, displayFired, nub, 0);
    if (!displayInterrupt || loop->addEventSource(displayInterrupt) != kIOReturnSuccess)
        panic("SwitchAudio: cannot attach HDA interrupt, INTID %u", SwitchHDA::kIntid);
    displayEvent = IOInterruptEventSource::interruptEventSource(this, displayChanged);
    if (!displayEvent || loop->addEventSource(displayEvent) != kIOReturnSuccess)
        return false;
    displayEvent->enable();
    displayCall =
        thread_call_allocate_with_options(displayProbe, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    return displayCall != nullptr;
}

void SwitchAudio::displayFree()
{
    if (displayEvent) {
        displayEvent->disable();
        if (loop)
            loop->removeEventSource(displayEvent);
        OSSafeReleaseNULL(displayEvent);
    }
    if (displayCall) {
        thread_call_cancel_wait(displayCall);
        thread_call_free(displayCall);
        displayCall = nullptr;
    }
    if (displayInterrupt) {
        displayInterrupt->disable();
        if (loop)
            loop->removeEventSource(displayInterrupt);
        OSSafeReleaseNULL(displayInterrupt);
    }
    displayAudio.release();
}

void SwitchAudio::displayFired(OSObject *owner, IOInterruptEventSource *, int)
{
    static_cast<SwitchAudio *>(owner)->displayAudio.interrupt();
}

void SwitchAudio::displayProbe(thread_call_param_t owner, thread_call_param_t)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    const bool prepared = self->gate->runAction(displayPrepareGated) == kIOReturnSuccess;
    const bool present = prepared && switch_displayport_audio_prepare_presence();
    self->gate->runAction(displayConnectGated, present ? self : nullptr);
}

IOReturn SwitchAudio::displayPrepareGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (!__atomic_load_n(&switch_audio_display_formats, __ATOMIC_ACQUIRE) || self->displayAsleep)
        return kIOReturnNotReady;
    auto *&output = self->memory[SwitchAudioMemoryDisplayOutput];
    if (!output) {
        const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
                                     static_cast<IOOptionBits>(kIOMemoryKernelUserShared) |
                                     static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
        output = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, kOutBytes, kDmaMask);
        if (!output)
            return kIOReturnNoMemory;
        bzero(output->getBytesNoCopy(), kOutBytes);
        switch_dcache_clean(output->getBytesNoCopy(), kOutBytes);
    }
    self->displayInterrupt->enable();
    self->displayPresencePending = self->displayAudio.prepare(output);
    return self->displayPresencePending ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn SwitchAudio::displayConnectGated(OSObject *owner, void *present, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    self->displayPresencePending = false;
    const uint32_t formats = __atomic_load_n(&switch_audio_display_formats, __ATOMIC_ACQUIRE);
    if (present && formats && !self->displayAsleep)
        (void)self->displayAudio.connect(formats, self->memory[SwitchAudioMemoryDisplayOutput]);
    else if (self->displayAsleep)
        self->displayAudio.sleep(true);
    else
        self->displayAudio.disconnect();
    self->gate->commandWakeup(&self->displayPresencePending, false);
    self->messageClients(kIOMessageServicePropertyChange);
    return kIOReturnSuccess;
}

void SwitchAudio::displayChanged(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    self->displayAudio.disconnect();
    if (self->displayInterrupt)
        self->displayInterrupt->disable();
    self->messageClients(kIOMessageServicePropertyChange);
    if (__atomic_load_n(&switch_audio_display_formats, __ATOMIC_ACQUIRE) && self->displayCall && !self->displayAsleep)
        self->displayStartProbe();
}

void SwitchAudio::displaySink()
{
    displayEvent->interruptOccurred(nullptr, nullptr, 0);
}

void SwitchAudio::displayStartProbe()
{
    thread_call_enter(displayCall);
}

IOReturn SwitchAudio::displayRequestGated(OSObject *owner, void *start, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    if (start)
        return self->displayAudio.start();
    self->displayAudio.stop();
    return kIOReturnSuccess;
}

IOReturn SwitchAudio::displayRequest(bool start)
{
    const auto *page =
        static_cast<const SwitchAudioControl *>(memory[SwitchAudioMemoryDisplayControl]->getBytesNoCopy());
    if (start && (!page->jack || !switch_displayport_audio_format(48000, 2, page->sample_bits)))
        return kIOReturnNotReady;
    const IOReturn result = gate->runAction(displayRequestGated, start ? this : nullptr);
    switch_displayport_audio_enable(start && result == kIOReturnSuccess);
    return result;
}

IOReturn SwitchAudio::displaySleepGated(OSObject *owner, void *entering, void *, void *, void *)
{
    auto *self = static_cast<SwitchAudio *>(owner);
    self->displayAsleep = entering != nullptr;
    if (self->displayAsleep) {
        self->displayAudio.disconnect();
        // SwitchSuspend and PM callers wait here; the worker finishes DP with the Audio gate open.
        while (self->displayPresencePending)
            self->gate->commandSleep(&self->displayPresencePending, THREAD_UNINT);
        self->displayAudio.sleep(true);
    } else {
        self->displayAudio.sleep(false);
    }
    if (self->displayInterrupt)
        self->displayInterrupt->disable();
    if (!self->displayAsleep && __atomic_load_n(&switch_audio_display_formats, __ATOMIC_ACQUIRE) && self->displayCall)
        self->displayStartProbe();
    self->messageClients(kIOMessageServicePropertyChange);
    return kIOReturnSuccess;
}
