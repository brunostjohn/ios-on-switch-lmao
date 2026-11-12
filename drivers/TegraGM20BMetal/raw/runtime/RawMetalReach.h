// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "raw_reach.h"

// A render pass's draws in its command buffer's plan, from its OpRender at `pass`, as its reach pass draws them again
// (raw_reach.h): each with its own state, applied through a cursor of its own
class PlanDraws final : public raw::DrawReplay {
public:
    PlanDraws(RMCommandBuffer *cb, size_t pass, size_t to, raw::CommandBuffer &commands);

    bool any() const override
    {
        return any_;
    }

    bool atFrameResolution() const override
    {
        return frame_;
    }

    bool replay(raw::RenderEncoder &e, const char **why) override;

private:
    RMCommandBuffer *cb_;
    raw::CommandBuffer &commands_;
    size_t from_, to_;
    bool any_ = false, frame_ = false;
};
