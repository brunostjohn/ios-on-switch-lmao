// SPDX-License-Identifier: MIT
#import "RawMetalReach.h"
#import "RawMetalPlanAdapters.h"

PlanDraws::PlanDraws(RMCommandBuffer *cb, size_t pass, size_t to, raw::CommandBuffer &commands)
    : cb_(cb), commands_(commands), from_(pass + 1), to_(from_)
{
    const RMCommandPlan &plan = cb->_plan;
    for (; to_ < to && plan.operation(to_).op != OpEnd; to_++) {
        if (!isPlanDraw(plan.operation(to_).op))
            continue;
        any_ = true;
        const RMPlanDraw &draw = plan.draw(to_);
        const raw::RasterState &raster = plan.raster[plan.renderStates[draw.state].raster];
        frame_ |= raw::reachedAtFrameResolution(draw.primitive, raster.set & raw::RasterState::Fill ? raster.fill : 0);
    }
}

bool PlanDraws::replay(raw::RenderEncoder &e, const char **why)
{
    raw::StateCursor cursor;
    for (size_t at = from_; at < to_; at++) {
        const Op op = cb_->_plan.operation(at).op;
        if (!isPlanDraw(op))
            continue;
        encodePlannedState(cb_, at, commands_, &e, nullptr, cursor);
        e.takeReachTwin();
        const RMPlanDraw &draw = cb_->_plan.draw(at);
        const uint32_t indexSize = draw.indexType == MTLIndexTypeUInt16 ? 2 : 4;
        RMBuffer *indices = (__bridge RMBuffer *)draw.indices, *arguments = (__bridge RMBuffer *)draw.arguments;
        bool drawn = false;
        switch (op) {
        case OpDraw:
            drawn = e.draw(draw.primitive, draw.first, draw.count, draw.instances, draw.baseInstance, why);
            break;
        case OpDrawIndexed:
            useBuffer(&commands_, indices, false);
            drawn = e.drawIndexed(draw.primitive, draw.count, indexSize, &indices->_raw, draw.indexOffset,
                                  draw.instances, draw.baseVertex, draw.baseInstance, why);
            break;
        case OpDrawIndirect:
            useBuffer(&commands_, arguments, false);
            drawn = e.drawIndirect(draw.primitive, &arguments->_raw, draw.argumentOffset, why);
            break;
        default:
            useBuffer(&commands_, arguments, false);
            useBuffer(&commands_, indices, false);
            drawn = e.drawIndexedIndirect(draw.primitive, indexSize, &indices->_raw, draw.indexOffset, &arguments->_raw,
                                          draw.argumentOffset, why);
            break;
        }
        if (!drawn)
            return false;
    }
    return true;
}
