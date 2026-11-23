// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include "OriginalLegacyFramebufferABI.h"
#include "SwitchDPModes.h"
#include "SwitchExternalGeometry.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "../TegraGM20B/SwitchGpuReady.h"
#include "../TegraVIC/SwitchNativeScanout.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

class SwitchDisplayPort;
class TegraDisplayB;

class SwitchExternalScreen : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(SwitchExternalScreen);
public:
    void free() override;
    bool begin(SwitchDisplayPort *transport, IOWorkLoop *workLoop);
    bool publish(const struct switch_dp_mode_list &modes, const uint8_t *edid, unsigned bytes);
    void linkUp(const struct switch_dp_mode &mode, TegraDisplayB *controller);
    void linkDown();
    void modeFailed();
    void detach();
    bool frameEnd();
    void gpuComplete(int status);
    bool requested() const { return device == 2; }
    IOWorkLoop *getWorkLoop() const override { return loop; }
    bool isMainDisplay() override { return false; }
    bool startHardware() override { return true; }
    bool getRefreshAttributes(uint64_t *periods, uint32_t internal) override;
    IOReturn getDotPitch(uint32_t *pitch) override;
    IOReturn setDisplayDevice(uint32_t requested) override;
    IOReturn setDigitalOutMode(uint32_t color, uint32_t timing) override;
    IOReturn getDigitalOutState(uint32_t *state) override;
    IOReturn getMatrix(uint32_t location, void *matrix) override;
    IOReturn setMatrix(uint32_t location, const void *matrix) override;
    IOReturn swapSetColorMatrix(const void *matrix, uint32_t location, uint32_t flags) override;
    IOReturn setAccessibilityScale(uint32_t scale) override;
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
    static constexpr uint64_t kSlotBase = 0x10000000, kSlotSpan = 16ULL << 20;
    static constexpr uint32_t kTablePages = 1 + kSlots * (kSlotSpan >> 22);
    static_assert(kTablePages == 13);
    static_assert(kTablePages == 1 + TegraSMMUDomain::tablesFor(kSlotBase, kSlots * kSlotSpan));
    struct Slot {
        SwitchNativeScanout read;
        OSObject *surface;
        IOMemoryDescriptor *memory;
        switch_gpu_backing *backing;
        void *layout;
        uint64_t allocationId, generation, offset, used, allocationBytes, blockBytes;
        uint32_t pages, stride, kind, width, height, colorDepth;
        bool counted, presented;
    } slots[kSlots] = {};
    struct LayerRect {
        switch_external::Rect src, dst;
        bool valid;
    } layerRects[switch_external::kLayers] = {};
    SwitchDisplayPort *transport = nullptr;
    IOWorkLoop *loop = nullptr;
    IOLock *colorLock = nullptr;
    IOInterruptEventSource *completion = nullptr, *readySource = nullptr;
    switch_gpu_ready_watch *ready = nullptr;
    TegraDisplayB *dcb = nullptr;
    switch_dma_buffer tables = {};
    TegraSMMUDomain domain;
    struct switch_dp_mode_list modes = {};
    struct switch_dp_mode mode = {};
    switch_external::ExternalFrame pendingFrame = {};
    int64_t harmonyMatrix[9] = {};
    int64_t accessibilityMatrix[9] = {};
    uint32_t accessibilityScale = 0x10000u;
    uint32_t latchedAddr = 0;
    uint64_t sequence = 0, slotUses = 0, refreshPeriod = 0, interval = 0, linkGeneration = 0, gpuGeneration = 0;
    uint64_t activeGeometry = 0;
    int selectedMode = -1, front = -1, drawing = -1;
    uint32_t device = 0;
    bool connected = false, mapped = false, gpuPending = false, latched = false, kicking = false;
    const OriginalLegacySwapRequest *pending = nullptr;
    int gpuStatus = 0;
#if DEVELOPMENT || DEBUG
    uint32_t diagValidateRejects = 0;
    uint32_t diagScanoutFailures = 0;
    uint32_t diagFrameLogs = 0;
    uint32_t diagLastPos = ~0u, diagLastSize = ~0u, diagLastSurfW = 0, diagLastSurfH = 0, diagLastKind = ~0u;
#endif
    static void completionFired(OSObject *owner, IOInterruptEventSource *, int);
    void publishActiveGeometry();
    void drainSwapQueue();
    void clearLayerState();
    bool applyTiledLayout(Slot &slot, uint64_t firstPage, uint64_t bytes);
    bool mapDomain();
    void cleanDomain();
    void releaseSlot(unsigned slot);
    void releaseRead(unsigned slot);
    int prepareSlot(OSObject *surface);
    void latchFrame();
    bool retire(const OriginalLegacySwapRequest *request, bool shown);
    void dropFrame(const OriginalLegacySwapRequest *request);
    void requestFrame();
    void retryWhenReady();
    static int readyQuery(void *);
    static void readyFired(OSObject *, IOInterruptEventSource *, int);
    bool publishModes(OSString *uuid);
    void publishScanoutLimits();
    bool publishSourceLimits(uint32_t clockHz);
    void clearSinkProperties();
    IOReturn selectOutput(bool timing, uint32_t value);
#include "../NativeMobileDisplay/NativeLegacySwapQueue.h"
};
