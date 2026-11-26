// SPDX-License-Identifier: BSD-2-Clause-Patent
// host1x's general interrupt (T210 TRM 14.4.6.2; SPI 67, tegra210.dtsi host1x@50000000 "host"): each channel's command
// processor reports an illegal opcode in HINTSTATUS_EXT bits 13:0 (14.9.7). With CMDPROC_CTRL's DROP_ILLEGAL_OPCODES at
// its reset 0 (14.9.10) the channel stops there, and its bit may latch again for as long as it stays stopped, so the
// filter masks a channel's bit when it takes its error and the channel's restart unmasks it. The channel's jobs fail at
// once (SwitchHost1xChannels.h switch_host1x_channels_failed) instead of at their bound, 2 to 30 s later.
// Only the illegal opcodes reach the line: IP_BUSY_TIMEOUT is 0, so the IP read and write timeouts never fire, and
// configure sets HINTMASK to the EXT enable alone and clears INTC0MASK (the module interrupts), whatever the boot chain
// left there. A line that still fires with none of them pending is a source this driver does not know: the filter turns
// the master enable off rather than let a level line fire forever, and the action says so once.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
class IOWorkLoop;

namespace switch_host1x_errors {
// Sync-unit registers (TRM 14.9.2, 14.9.5-14.9.8, 14.9.11).
constexpr uint32_t kIntStatus = 0x000, kIntMask = 0x004, kIntC0Mask = 0x008, kHintStatus = 0x020, kHintMask = 0x024,
                   kHintStatusExt = 0x028, kHintMaskExt = 0x02c, kCmdprocStat = 0x0a8;
constexpr uint32_t kCpu0All = 1u << 0, kExtEnable = 1u << 31, kChannels = 14, kIllegalAll = (1u << kChannels) - 1;

// Registers: read(offset), write(offset, value), sync-unit relative. The caller serialises take() (the filter) and
// rearm() (a restart) against each other.
template <class Registers> struct Unit {
    uint32_t masked = 0;  // channels whose error was taken and whose restart has not come yet

    // After the sync unit came out of reset (boot, SC7), every channel freshly programmed: what latched before is
    // returned in *latched and cleared, the module interrupts' mask (*intc0 as it was) cleared, HINTMASK set to the EXT
    // enable alone, every channel's illegal opcode unmasked, then the master enable set (INTMASK's other bits kept:
    // bit 4 resets to 1). *hintmaskExt: HINTMASK_EXT as read back. False: CPU0's master enable did not read back, so
    // nothing reaches the CPU.
    bool configure(Registers &r, uint32_t *latched, uint32_t *intc0, uint32_t *hintmaskExt)
    {
        *latched = r.read(kHintStatusExt) & kIllegalAll;
        r.write(kHintStatusExt, *latched);
        masked = 0;
        *intc0 = r.read(kIntC0Mask);
        if (*intc0)
            r.write(kIntC0Mask, 0);
        r.write(kHintMaskExt, kIllegalAll);
        r.write(kHintMask, kExtEnable);
        r.write(kIntMask, r.read(kIntMask) | kCpu0All);
        *hintmaskExt = r.read(kHintMaskExt);
        return (r.read(kIntMask) & kCpu0All) != 0;
    }

    // The filter, interrupts off: the channels whose illegal opcode is pending and unmasked, masked and their status
    // cleared. 0: none of ours.
    uint32_t take(Registers &r)
    {
        const uint32_t pending = r.read(kHintStatusExt) & kIllegalAll & ~masked;
        if (!pending)
            return 0;
        masked |= pending;
        r.write(kHintMaskExt, kIllegalAll & ~masked);
        r.write(kHintStatusExt, pending);
        (void)r.read(kHintStatusExt);  // the clear reaches host1x before the filter returns and the line is re-enabled
        return pending;
    }

    // The line fired with none of ours pending: off, so a source this driver does not know cannot fire it forever.
    void masterOff(Registers &r)
    {
        r.write(kIntMask, r.read(kIntMask) & ~kCpu0All);
    }

    // The channel restarted on an empty stream: what its stopped command processor latched meanwhile is cleared, and
    // its illegal opcode unmasked again.
    void rearm(Registers &r, uint32_t channel)
    {
        const uint32_t bit = 1u << channel;
        if (channel >= kChannels || !(masked & bit))
            return;
        masked &= ~bit;
        r.write(kHintStatusExt, bit);
        r.write(kHintMaskExt, kIllegalAll & ~masked);
    }
};
}  // namespace switch_host1x_errors

// Thread context, once the channels are programmed: the interrupt attached on host1x's work loop and unmasked. False,
// with one line, when it could not be: a stopped channel's jobs then fail only at their bound.
bool switch_host1x_errors_start(IOWorkLoop *loop, volatile uint8_t *sync);
// SC7: off before the sync unit powers down, configured and on again once it is back and the channels programmed.
void switch_host1x_errors_stop();
void switch_host1x_errors_resume();
#endif

#ifdef __cplusplus
extern "C" {
#endif
// SwitchHost1xChannels.cpp's restart of `channel` (submit lock held).
void switch_host1x_errors_rearm(uint32_t channel);
#ifdef __cplusplus
}
#endif
