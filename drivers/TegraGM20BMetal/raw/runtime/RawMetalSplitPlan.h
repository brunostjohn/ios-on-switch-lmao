// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "raw_raster_area.h"

inline void planSplitPrograms(RMCommandBuffer *cb, size_t begin, raw::RenderEncoder &encoder)
{
    encoder.beginSplitPlan();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (cb->_forceWholePrograms)
        return;
#endif
    cb->_plan.forEachRasterGroup(begin, [&](const raw::RasterState &state, uint32_t draws) {
        RMPipeline *pipeline = (__bridge RMPipeline *)state.pipeline;
        auto *variant = static_cast<raw::Variant<raw::Pipeline> *>(state.nativeVariant);
        raw::Pipeline *native = variant              ? variant->value()
                                : state.nativePipeline ? static_cast<raw::Pipeline *>(state.nativePipeline)
                                : pipeline             ? pipeline->_raw
                                                       : nullptr;
        encoder.addSplitCandidates(native, draws, raw::rasterPixelLimit(state, encoder.areaWidth(),
                                                                       encoder.areaHeight()));
    });
    encoder.finishSplitPlan();
}
