// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalMpsProbe.h"
#import "TegraGM20BMetalInternal.h"

#if SWITCH_METAL_DIAGNOSTICS
namespace {

std::atomic<uint32_t> gLines{0};
constexpr uint32_t kLinesMax = 400;

}  // namespace

void gm20bMpsProbeBind(const void *cb, const void *encoder, NSString *function)
{
    if ([function hasPrefix:@"canny_"] && gMpsProbed.exchange(cb) != cb)
        gLines.store(0);
    SWITCH_MPS_PROBE(cb, encoder, "setComputePipelineState", function);
}

void gm20bMpsProbeCall(const void *cb, const void *encoder, const char *call, NSString *detail)
{
    if (gLines.fetch_add(1) >= kLinesMax)
        return;
    os_log(gm20bLog(), "SwitchMetal: mps probe %{public}s cb %p encoder %p: %{public}s %{public}s", getprogname(), cb,
           encoder, call, detail.UTF8String ?: "");
}
#endif
