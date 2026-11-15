// SPDX-License-Identifier: GPL-2.0-only
// SwitchDisplayPort: TV output over the USB-C port; see SwitchDisplayPortService.h. Started by the first DisplayPort
// Configure the BM92T36 engine reports, on a thread call so SwitchPD's work loop never waits on it. Every event is an
// interrupt (DPAUX1: AUX done, HPD plug, unplug, IRQ_HPD; display controller B: frame end) or a one-shot timer; an AUX
// transaction whose AUX_DONE never reaches the CPU panics, with the GIC's view of the line.
#include "SwitchDisplayPort.h"

#include <IOKit/IOLib.h>
#include <kern/thread_call.h>

#include "../ArmGIC/SwitchInterrupts.h"
#include "SwitchDisplayPortService.h"

OSDefineMetaClassAndStructors(SwitchDisplayPort, IOService);


namespace {
constexpr uint32_t kAuxLostMs = 50;  // DPAUX1 times out a silent sink itself within a millisecond
SwitchDisplayPort *gService;
thread_call_t gApply;
uint32_t gLanes, gFlipped;

void applyAltMode(thread_call_param_t, thread_call_param_t)
{
    if (gService == nullptr && switch_displayport_start())
        return;
    const unsigned lanes = __atomic_load_n(&gLanes, __ATOMIC_ACQUIRE);
    const bool flipped = __atomic_load_n(&gFlipped, __ATOMIC_ACQUIRE) != 0;
    gService->getWorkLoop()->runAction(
        [](OSObject *owner, void *lanes, void *flipped, void *, void *) -> IOReturn {
            static_cast<SwitchDisplayPort *>(owner)->setAltLanes((unsigned)(uintptr_t)lanes, flipped != nullptr);
            return kIOReturnSuccess;
        },
        gService, reinterpret_cast<void *>((uintptr_t)lanes), flipped ? gService : nullptr);
}
}  // namespace

extern "C" int switch_displayport_start(void)
{
    if (gService)
        return 0;
    auto *service = OSTypeAlloc(SwitchDisplayPort);
    if (!service || !service->init() || !service->attach(IOService::getResourceService()) || !service->begin()) {
        OSSafeReleaseNULL(service);
        return -1;
    }
    gService = service;
    return 0;
}

extern "C" void switch_displayport_alt_mode(unsigned lanes, bool flipped)
{
    __atomic_store_n(&gLanes, lanes, __ATOMIC_RELEASE);
    __atomic_store_n(&gFlipped, flipped ? 1u : 0u, __ATOMIC_RELEASE);
    if (gApply == nullptr) {
        thread_call_t fresh = thread_call_allocate(applyAltMode, nullptr), none = nullptr;
        if (!__atomic_compare_exchange_n(&gApply, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            thread_call_free(fresh);
        }
    }
    thread_call_enter(gApply);
}

bool SwitchDisplayPort::begin()
{
    loop = IOWorkLoop::workLoop();
    IOService *auxNub = switch_interrupt_nub(TEGRA_DPAUX1_INTID, false);
    IOService *dcNub = switch_interrupt_nub(TEGRA_DISPLAY_B_INTID, false);
    auxSource = loop && auxNub ? IOInterruptEventSource::interruptEventSource(this, &auxInterrupt, auxNub, 0) : nullptr;
    dcSource = loop && dcNub ? IOInterruptEventSource::interruptEventSource(this, &dcInterrupt, dcNub, 0) : nullptr;
    auxTimer = loop ? IOTimerEventSource::timerEventSource(this, &auxTimerFired) : nullptr;
    stepTimer = loop ? IOTimerEventSource::timerEventSource(this, &stepTimerFired) : nullptr;
    if (auxSource == nullptr || dcSource == nullptr || auxTimer == nullptr || stepTimer == nullptr ||
        loop->addEventSource(auxSource) != kIOReturnSuccess || loop->addEventSource(dcSource) != kIOReturnSuccess ||
        loop->addEventSource(auxTimer) != kIOReturnSuccess || loop->addEventSource(stepTimer) != kIOReturnSuccess) {
        panic("SwitchDisplayPort: cannot attach the DPAUX1 (INTID %u) or display controller B (INTID %u) interrupt",
              TEGRA_DPAUX1_INTID, TEGRA_DISPLAY_B_INTID);
    }
    screen = OSTypeAlloc(SwitchExternalScreen);
    if (!screen || !screen->init() || !screen->begin(this, loop))
        return false;
    setName("SwitchDisplayPort");
    registerService();
    return true;
}

void SwitchDisplayPort::setAltLanes(unsigned lanes, bool plugFlipped)
{
    if (lanes == altLanes && plugFlipped == flipped) {
        return;
    }
    if (lanes == 0) {
        IOLog("SwitchDisplayPort: DisplayPort alternate mode ended\n");
        powerOff();
        screen->detach();
        wantedMode = -1;
        altLanes = 0;
        return;
    }
    if (stage != Stage::Off)
        teardownLink();
    altLanes = lanes;
    flipped = plugFlipped;
    if (sleeping)
        return;
    stage = stage == Stage::Off ? Stage::Off : Stage::Waiting;
    char why[128] = "";
    if (stage == Stage::Off) {
        if (!aux.powerUp(why, sizeof(why))) {
            fail("DPAUX1", why);
            return;
        }
        auxSource->enable();
        dcSource->enable();
        stage = Stage::Waiting;
    }
    IOLog("SwitchDisplayPort: DisplayPort on %u lanes (pin assignment %c, plug %s), HPD %s\n", lanes,
          lanes == 2 ? 'D' : 'C', flipped ? "flipped" : "straight", aux.hpdHigh() ? "high" : "low");
    if (aux.hpdHigh()) {
        plugged();
    }
}

void SwitchDisplayPort::auxInterrupt(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    const uint32_t cause = self->aux.takeInterrupt();
    if ((cause & SWITCH_DPAUX_INTR_AUX_DONE) != 0 && self->auxPending) {
        self->auxTimer->cancelTimeout();
        self->auxPending = false;
        self->auxDone();
    }
    if ((cause & SWITCH_DPAUX_INTR_UNPLUG_EVENT) != 0) {
        self->unplugged();
    }
    if ((cause & SWITCH_DPAUX_INTR_PLUG_EVENT) != 0) {
        self->plugged();
    }
    if ((cause & SWITCH_DPAUX_INTR_IRQ_EVENT) != 0) {
        self->irqHpd();
    }
}

void SwitchDisplayPort::dcInterrupt(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    const uint32_t cause = self->dcb.takeInterrupt();
    if ((cause & (1u << 1)) != 0) {
        self->screen->frameEnd();
    }
}

void SwitchDisplayPort::auxStart(const struct switch_dp_aux_request &transaction, Next then)
{
    request = transaction;
    next = then;
    attempt = 0;
    auxIssue();
}

void SwitchDisplayPort::auxNative(enum switch_dp_aux_op op, uint32_t address, const uint8_t *bytes, uint8_t size,
                                  Next then)
{
    struct switch_dp_aux_request native = switch_dp_aux_make(op, address, size);
    for (uint8_t i = 0; bytes != nullptr && i < size; ++i) {
        native.data[i] = bytes[i];
    }
    auxStart(native, then);
}

void SwitchDisplayPort::auxIssue()
{
    auxRetrying = false;
    auxPending = true;
    aux.start(request);
    switch_check_after_ms(auxTimer, kAuxLostMs);
}

void SwitchDisplayPort::auxDone()
{
    const enum switch_dp_aux_result result = aux.finish(request);
    if (result == SWITCH_DP_AUX_ACK) {
        dispatch();
        return;
    }
    const uint32_t retryUs = switch_dp_aux_retry_us(result, attempt++);
    if (retryUs != 0) {
        auxRetrying = true;
        auxTimer->setTimeoutUS(retryUs);
        return;
    }
    static const char *const names[] = {"ack", "NACK", "defer", "I2C NACK", "I2C defer", "timeout", "error", "short"};
    char why[96];
    snprintf(why, sizeof(why), "AUX %s at %#x after %u tries (AUXSTAT %#x)", names[result], request.address, attempt,
             aux.auxstat());
    fail("the sink", why);
}

void SwitchDisplayPort::auxTimerFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    if (self->auxRetrying) {
        self->auxIssue();
        return;
    }
    if (!self->auxPending) {
        return;
    }
    char gic[192];
    switch_interrupt_describe(TEGRA_DPAUX1_INTID, gic, sizeof(gic));
    panic(
        "SwitchDisplayPort: DPAUX1 interrupt lost: no AUX_DONE %u ms after a transaction (AUXCTL %#x, AUXSTAT %#x); %s",
        kAuxLostMs, switch_dp_auxctl(&self->request), self->aux.auxstat(), gic);
}

void SwitchDisplayPort::stepTimerFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    if (self->stage == Stage::Training) {
        self->auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_LANE0_1_STATUS, nullptr, 6, Next::TrainingStatus);
    }
}

void SwitchDisplayPort::fail(const char *what, const char *why)
{
    IOLog("SwitchDisplayPort: %s: %s; DisplayPort off until the next plug\n", what, why);
    teardownLink();
    if (stage != Stage::Off) {
        stage = Stage::Waiting;
    }
}

void SwitchDisplayPort::teardownLink()
{
    auxTimer->cancelTimeout();
    stepTimer->cancelTimeout();
    auxPending = auxRetrying = false;
    if (sorUp) {
        sor.audioEnable(false);
        sor.audioPresence(nullptr, 0, false);
    }
    if (dcbUp) {
        dcb.windowOff();
    }
    sor.detachHead();
    if (dcbUp) {
        dcb.enableSor(false);
    }
    if (sorUp) {
        sor.powerDown();
        sorUp = false;
    }
    if (dcbUp) {
        dcb.powerDown();
        dcbUp = false;
    }
    screen->linkDown();
}

void SwitchDisplayPort::powerOff()
{
    teardownLink();
    if (stage != Stage::Off) {
        auxSource->disable();
        dcSource->disable();
        aux.powerDown();
    }
    stage = Stage::Off;
}

void SwitchDisplayPort::plugged()
{
    if (stage != Stage::Waiting) {
        return;
    }
    stage = Stage::Probing;
    auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_REV, nullptr, sizeof(caps), Next::Caps);
}

void SwitchDisplayPort::unplugged()
{
    if (stage == Stage::Off || stage == Stage::Waiting) {
        return;
    }
    IOLog("SwitchDisplayPort: the sink went away (HPD low)\n");
    teardownLink();
    screen->detach();
    wantedMode = -1;
    stage = Stage::Waiting;
}

void SwitchDisplayPort::irqHpd()
{
    if (stage == Stage::Running && !auxPending && !auxRetrying) {
        auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_SINK_COUNT, nullptr, 6, Next::LinkStatus);
    }
}

IOReturn SwitchDisplayPort::selectMode(int index, bool enable)
{
    if (!enable) {
        const bool configured = stage == Stage::Running || stage == Stage::Training || stage == Stage::Ready;
        teardownLink();
        if (configured)
            stage = Stage::Ready;
        return kIOReturnSuccess;
    }
    if (sleeping || index < 0 || static_cast<unsigned>(index) >= modes.count ||
        (stage != Stage::Ready && stage != Stage::Running && stage != Stage::Training))
        return kIOReturnNotReady;
    if (wantedMode == index && (stage == Stage::Running || stage == Stage::Training))
        return kIOReturnSuccess;
    teardownLink();
    wantedMode = index;
    chooseAndTrain();
    return stage == Stage::Training ? kIOReturnSuccess : kIOReturnUnsupported;
}

bool SwitchDisplayPort::gpuComplete(void *display, int status)
{
    if (display != screen)
        return false;
    screen->gpuComplete(status);
    return true;
}

extern "C" bool switch_displayport_gpu_complete(void *display, int status)
{
    return gService && gService->gpuComplete(display, status);
}

void SwitchDisplayPort::systemSleep(bool entering)
{
    if (entering) {
        sleeping = true;
        powerOff();
        return;
    }
    sleeping = false;
    const unsigned lanes = altLanes;
    const bool orientation = flipped;
    altLanes = 0;
    if (lanes)
        setAltLanes(lanes, orientation);
}

namespace {
void setSleeping(bool entering)
{
    if (!gService)
        return;
    gService->getWorkLoop()->runAction(
        [](OSObject *owner, void *entering, void *, void *, void *) -> IOReturn {
            static_cast<SwitchDisplayPort *>(owner)->systemSleep(entering != nullptr);
            return kIOReturnSuccess;
        }, gService, entering ? gService : nullptr);
}
}

extern "C" void switch_displayport_suspend(void) { setSleeping(true); }
extern "C" void switch_displayport_resume(void) { setSleeping(false); }

bool SwitchDisplayPort::audioFormat(uint32_t rate, uint32_t channels, uint32_t bits)
{
    return stage == Stage::Running && sor.audioFormat(rate, channels, bits);
}

void SwitchDisplayPort::audioEnable(bool enable)
{
    if (stage == Stage::Running)
        sor.audioEnable(enable);
}

void SwitchDisplayPort::audioPresence(const uint8_t *eld, uint32_t bytes, bool present)
{
    if (stage == Stage::Running)
        sor.audioPresence(eld, bytes, present);
}

extern "C" bool switch_displayport_audio_format(uint32_t rate, uint32_t channels, uint32_t bits)
{
    if (!gService)
        return false;
    const uint32_t format[] = {rate, channels, bits};
    return gService->getWorkLoop()->runAction(
        [](OSObject *owner, void *value, void *, void *, void *) -> IOReturn {
            const auto *format = static_cast<const uint32_t *>(value);
            return static_cast<SwitchDisplayPort *>(owner)->audioFormat(format[0], format[1], format[2])
                       ? kIOReturnSuccess : kIOReturnUnsupported;
        }, gService, const_cast<uint32_t *>(format)) == kIOReturnSuccess;
}

extern "C" void switch_displayport_audio_enable(bool enable)
{
    if (gService)
        gService->getWorkLoop()->runAction(
            [](OSObject *owner, void *enabled, void *, void *, void *) -> IOReturn {
                static_cast<SwitchDisplayPort *>(owner)->audioEnable(enabled != nullptr);
                return kIOReturnSuccess;
            }, gService, enable ? gService : nullptr);
}

extern "C" void switch_displayport_audio_presence(const uint8_t *eld, uint32_t bytes, bool present)
{
    if (gService)
        gService->getWorkLoop()->runAction(
            [](OSObject *owner, void *eld, void *bytes, void *present, void *) -> IOReturn {
                static_cast<SwitchDisplayPort *>(owner)->audioPresence(static_cast<const uint8_t *>(eld),
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bytes)), present != nullptr);
                return kIOReturnSuccess;
            }, gService, const_cast<uint8_t *>(eld), reinterpret_cast<void *>(static_cast<uintptr_t>(bytes)),
            present ? gService : nullptr);
}
