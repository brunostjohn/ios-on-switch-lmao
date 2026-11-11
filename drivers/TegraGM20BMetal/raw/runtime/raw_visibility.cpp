// SPDX-License-Identifier: MIT
#include "raw_visibility.h"
#include <algorithm>
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitReport.h"
#include "raw_kit.h"

namespace raw {

const char *visibilityRefusal(const VisibilityAsk &a)
{
    if (a.mode == kVisibilityDisabled)
        return nullptr;
    if (a.mode != kVisibilityCounting && a.mode != kVisibilityBoolean)
        return "a visibility result mode Metal does not have";
    if (!a.rawBuffer)
        return "a visibility result buffer the raw path does not hold";
    if ((a.offset & ~uint64_t(7)) + 8 > a.bufferLength)
        return "a visibility offset past its buffer";
    if (a.accumulate)
        return "visibility results accumulated across passes";
    if (a.mode == kVisibilityBoolean && !copyEnginePredicates() && !a.probed)
        return "a Boolean visibility result (rule vis.boolean unproven)";
    return nullptr;
}

void VisibilityQueries::close(Push &p)
{
    if (!open_)
        return;
    kit(p, switch_gm20b_samples_report, queries_.back().slotVa);
    open_ = false;
}

VisibilityQueries::Set VisibilityQueries::set(Push &p, uint32_t mode, uint64_t resultVa, uint64_t slotVa)
{
    if (mode != kVisibilityDisabled) {
        if (std::any_of(queries_.begin(), queries_.end(), [&](const Query &q) {
                return q.resultVa == resultVa;
            }))
            return Set::Retaken;
        if (queries_.size() >= kVisibilityQueriesPerEncoder)
            return Set::Full;
    }
    close(p);
    if (mode == kVisibilityDisabled) {
        if (counting_)
            kit(p, switch_gm20b_samples_end);
        counting_ = false;
        return Set::Done;
    }
    queries_.push_back({resultVa, slotVa, mode});
    kit(p, switch_gm20b_samples_begin);
    open_ = counting_ = true;
    return Set::Done;
}

void VisibilityQueries::finish(Push &p)
{
    close(p);
    if (counting_)
        kit(p, switch_gm20b_samples_end);
    counting_ = false;
    if (!queries_.empty())
        kit(p, switch_gm20b_wait_for_idle);
}

void VisibilityQueries::finishResult(Push &p, size_t i) const
{
    const Query &q = queries_[i];
    kit(p, switch_gm20b_copy_bytes, q.slotVa, q.resultVa, 8u, uint8_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED));
    if (q.mode != kVisibilityBoolean)
        return;
    kit(p, switch_gm20b_copy_predicate, q.resultVa, uint32_t(NVB0B5_SET_RENDER_ENABLE_C_MODE_CONDITIONAL));
    kit(p, switch_gm20b_copy_u64, q.resultVa, uint64_t(1), uint8_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED));
    kit(p, switch_gm20b_copy_predicate, uint64_t(0), uint32_t(NVB0B5_SET_RENDER_ENABLE_C_MODE_TRUE));
}

}  // namespace raw
