// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "OriginalLegacyFramebufferABI.h"
#include "SwitchDPModes.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "../TegraGM20B/SwitchGpuReady.h"
#include "../TegraVIC/SwitchNativeScanout.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

class SwitchDisplayPort;
class TegraDisplayB;

class SwitchExternalScreen : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(SwitchExternalScreen);
public:
    bool begin(SwitchDisplayPort *transport, IOWorkLoop *workLoop);
    void publish(const struct switch_dp_mode_list &modes, uint64_t payload);
    void linkUp(const struct switch_dp_mode &mode, TegraDisplayB *controller);
    void linkDown();
    void detach();
    void frameEnd();
    void gpuComplete(int status);
    bool requested() const { return device == 2; }
    int selected() const { return selectedMode; }
    IOWorkLoop *getWorkLoop() const override { return loop; }
    bool isMainDisplay() override { return false; }
    bool startHardware() override { return true; }
    IOReturn getDotPitch(uint32_t *pitch) override;
    IOReturn setDisplayDevice(uint32_t requested) override;
    IOReturn setDigitalOutMode(uint32_t color, uint32_t timing) override;
    IOReturn getDigitalOutState(uint32_t *state) override;
    void kickSwapQueue() override;
    bool validateSwap(const OriginalLegacySwapRequest *request) override;
    bool areTimingsEnabled() const override { return dcb != nullptr; }
    IOReturn collectCompletionReports(uint32_t, void *, uint32_t *count, uint32_t) override;
    IOReturn supportedFrameInfo(uint32_t, uint32_t *count, char *, uint32_t *) override;
    IOReturn changeFrameInfo(uint32_t, void *, uint32_t) override { return kIOReturnUnsupported; }
    void platformFrameInfoNotify(void *, bool) override {}
    IOReturn sendFrameInfo(void *, const void *) override { return kIOReturnUnsupported; }
    IOReturn platformInternalPlug() override { return kIOReturnUnsupported; }
    IOReturn platformInternalUnplug() override { return kIOReturnUnsupported; }
    void platformRefreshRates() override {}
private:
    static constexpr unsigned kSlots = 3;
    static constexpr uint64_t kSlotBase = 0x10000000, kSlotSpan = 0x10000000;
    static constexpr uint32_t kTablePages = 1 + kSlots * (kSlotSpan >> 22);
    struct Slot {
        SwitchNativeScanout read;
        OSObject *surface;
        IOMemoryDescriptor *memory;
        switch_gpu_backing *backing;
        void *layout;
        uint64_t allocationId, generation, offset, used;
        uint32_t pages, stride, kind;
        bool counted, presented;
    } slots[kSlots] = {};
    SwitchDisplayPort *transport = nullptr;
    IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *completion = nullptr, *readySource = nullptr;
    switch_gpu_ready_watch *ready = nullptr;
    TegraDisplayB *dcb = nullptr;
    switch_dma_buffer tables = {};
    TegraSMMUDomain domain;
    struct switch_dp_mode_list modes = {};
    struct switch_dp_mode mode = {};
    uint64_t available = 0, sequence = 0, slotUses = 0, interval = 0, linkGeneration = 0, gpuGeneration = 0;
    int selectedMode = -1, front = -1, drawing = -1;
    uint32_t device = 0;
    bool connected = false, mapped = false, gpuPending = false, latched = false, kicking = false;
    const OriginalLegacySwapRequest *pending = nullptr;
    int gpuStatus = 0;
    static void completionFired(OSObject *owner, IOInterruptEventSource *, int);
    bool mapDomain();
    void cleanDomain();
    void releaseSlot(unsigned slot);
    void releaseRead(unsigned slot);
    int prepareSlot(OSObject *surface);
    void latchFrame();
    void retire(const OriginalLegacySwapRequest *request, bool shown);
    void requestFrame();
    void retryWhenReady();
    static int readyQuery(void *);
    static void readyDone(void *, int);
    static void readyFired(OSObject *, IOInterruptEventSource *, int);
    void publishModes();
    void publishScanoutLimits();
#include "../NativeMobileDisplay/NativeLegacySwapQueue.h"
};
