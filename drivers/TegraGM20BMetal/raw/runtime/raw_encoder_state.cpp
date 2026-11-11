// SPDX-License-Identifier: MIT
// The render encoder's state that persists in the channel between draws: a state's registers by its segment or inline
// (raw_state_segment.h), and the raster state.
#include "raw_gpu.h"

namespace raw {

// hw_test_app's inline arm (CommandBuffer::inlineStates) binds every state as before segments: inline, none skipped
bool RenderEncoder::measuringInline() const
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    return cb_->inlineStates_;
#else
    return false;
#endif
}

bool RenderEncoder::bindsBySegment() const
{
    return !measuringInline() && cb_->segmentBinds_ < kSegmentBindsPerCommandBuffer;
}

void RenderEncoder::bindState(Push &p, const Pipeline &pl)
{
    HeldSegment &held = cb_->heldSegment_;
    if (held.holds(pl.segment) && !measuringInline()) {
        RAW_DIAGNOSTIC(dev_->stats.segmentSkips++);
        return;
    }
    if (!pl.segment || !bindsBySegment()) {
        p.raw(pl.methods.data(), uint32_t(pl.methods.size()));
        if (pl.segment && !measuringInline())
            held.bound(pl.segment);
        else
            held.lose();
        return;
    }
    bindSegment(bodies_, bodyStart_, p.at, pl.segment);
    held.bound(pl.segment);
    cb_->segmentBinds_++;
    RAW_DIAGNOSTIC(dev_->stats.segmentBinds++);
    RAW_DIAGNOSTIC(dev_->stats.segmentWords += pl.segment.words);
}

void RenderEncoder::writeRaster(Push &p, const Raster &want)
{
    if (want.bias != rasterHeld_.bias) {
        p.m(NVB197_SET_POLY_OFFSET_POINT, want.bias);
        p.m(NVB197_SET_POLY_OFFSET_LINE, want.bias);
        p.m(NVB197_SET_POLY_OFFSET_FILL, want.bias);
        rasterHeld_.bias = want.bias;
    }
    if (want.biasConstant != rasterHeld_.biasConstant || want.biasSlope != rasterHeld_.biasSlope ||
        want.biasClamp != rasterHeld_.biasClamp) {
        p.m(NVB197_SET_DEPTH_BIAS, want.biasConstant);
        p.m(NVB197_SET_SLOPE_SCALE_DEPTH_BIAS, want.biasSlope);
        p.m(NVB197_SET_DEPTH_BIAS_CLAMP, want.biasClamp);
        rasterHeld_.biasConstant = want.biasConstant;
        rasterHeld_.biasSlope = want.biasSlope;
        rasterHeld_.biasClamp = want.biasClamp;
    }
    if (want.cull != rasterHeld_.cull) {
        p.m(NVB197_OGL_SET_CULL, want.cull != 0);
        if (want.cull)
            p.m(NVB197_OGL_SET_CULL_FACE, want.cull == 1 ? 0x404 : 0x405);  // FRONT, BACK
        rasterHeld_.cull = want.cull;
    }
    if (want.winding != rasterHeld_.winding) {
        p.m(NVB197_OGL_SET_FRONT_FACE, want.winding ? 0x901 : 0x900);  // CCW, CW
        rasterHeld_.winding = want.winding;
    }
    if (want.fill != rasterHeld_.fill) {
        const uint32_t mode = want.fill ? 0x1b01 : 0x1b02;  // LINE, FILL
        p.m(NVB197_SET_FRONT_POLYGON_MODE, mode);
        p.m(NVB197_SET_BACK_POLYGON_MODE, mode);
        rasterHeld_.fill = want.fill;
    }
}

}  // namespace raw
