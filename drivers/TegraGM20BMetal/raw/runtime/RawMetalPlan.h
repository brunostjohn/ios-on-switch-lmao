// SPDX-License-Identifier: MIT
#pragma once
#include "RawMetalArguments.h"
#include "raw_semantic_state.h"
#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>
#include <tuple>

struct RMPlanEvent {
    size_t position = 0;
    void *event = nullptr;
    uint64_t value = 0;
    uint32_t timeout = 0;
    bool signal = false;
};

struct RMPlanBegin {
    void *descriptor = nullptr;
    uint32_t pass = 0;
    uint32_t dispatchType = 0;
    bool refused = false;
    size_t end = 0;
};

struct RMPlanDraw {
    uint32_t state = 0;
    uint32_t first = 0, count = 0, instances = 1, baseInstance = 0;
    int32_t baseVertex = 0;
    void *indices = nullptr;
    uint64_t indexOffset = 0;
    void *arguments = nullptr;
    uint64_t argumentOffset = 0;
    uint8_t primitive = 0, indexType = 0;
};

struct RMPlanDispatch {
    uint32_t state = 0;
    uint32_t surfaceSlots = 0;
    const void *kernel = nullptr;
    std::array<uint64_t, 3> grid{}, groups{};
    std::array<uint32_t, 3> threads{};
    bool perThread = false;
};

struct RMPlanTransfer {
    void *source = nullptr, *destination = nullptr;
    uint64_t offset = 0, length = 0, parameters = 0;
    uint32_t aspect = 0;
    uint8_t value = 0;
};

struct RMPlanControl {
    void *object = nullptr;
    uint64_t value = 0;
    uint32_t scope = 0, before = 0, after = 0;
    uint32_t render = raw::kNoSnapshot, compute = raw::kNoSnapshot;
    uint8_t stage = 0;
};

struct RMPlanOperation {
    Op op = OpEnd;
    uint32_t payload = 0;
    uint32_t uses = 0;
};

class RMCommandPlan {
    struct UseSpan {
        uint32_t begin = 0, count = 0;
    };

    std::vector<RMPlanOperation> operations_;
    std::vector<RMPlanBegin> begins_;
    std::vector<RMPlanDraw> draws_;
    std::vector<RMPlanDispatch> dispatches_;
    std::vector<RMPlanTransfer> transfers_;
    std::vector<RMPlanControl> controls_;
    std::vector<RMPlanEvent> events_;
    std::vector<raw::ResourceUse> uses_;
    std::vector<UseSpan> useSpans_{{}};
    uint64_t renderGeneration_ = 0, kernelGeneration_ = 0, stateEpoch_ = 0;
    uint64_t eligibilityGeneration_ = 0;
    uint64_t savedRender_ = UINT64_MAX, savedKernel_ = UINT64_MAX;
    uint32_t currentRender_ = raw::kNoSnapshot, currentKernel_ = raw::kNoSnapshot;
    Op encoder_ = OpEnd;
    size_t encoderBegin_ = 0;
    std::map<std::pair<size_t, size_t>, uint32_t> rangeUses_;
    std::map<std::pair<size_t, size_t>, std::vector<void *>> rangePipelines_;
    std::map<std::tuple<uint32_t, uintptr_t, uintptr_t>, uint32_t> drawUseSpans_;
    uint32_t drawUses(uint32_t base, void *indices, void *arguments);
    uint32_t captureRender();
    uint32_t captureKernel();
    uint32_t saveUses(const std::vector<raw::ResourceUse> &uses);
    bool setter(const Cmd &c, const std::vector<uint64_t> &extra);

public:
    raw::StateSnapshots<raw::BufferBindings> buffers[3];
    raw::StateSnapshots<raw::TextureBindings> textures[3];
    raw::StateSnapshots<raw::SamplerBindings> samplers[3];
    raw::StateSnapshots<raw::RasterState> raster;
    raw::StateSnapshots<raw::KernelState> kernel;
    std::vector<raw::RenderStateRef> renderStates;
    std::vector<raw::KernelStateRef> kernelStates;

    bool record(const Cmd &c, const std::vector<uint64_t> &extra);

    size_t size() const
    {
        return operations_.size();
    }

    bool empty() const
    {
        return operations_.empty();
    }

    const RMPlanOperation &operation(size_t at) const
    {
        return operations_[at];
    }

    const RMPlanDraw &draw(size_t at) const
    {
        return draws_[operations_[at].payload];
    }

    const RMPlanDispatch &dispatch(size_t at) const
    {
        return dispatches_[operations_[at].payload];
    }

    const RMPlanControl &control(size_t at) const
    {
        return controls_[operations_[at].payload];
    }

    const std::vector<RMPlanEvent> &events() const
    {
        return events_;
    }

    Cmd legacy(size_t at) const;

    void setRefused(size_t at, bool refused)
    {
        begins_[operations_[at].payload].refused = refused;
    }

    void addUses(size_t at, const std::vector<raw::ResourceUse> &uses);
    void setRenderUses(uint32_t state, const std::vector<raw::ResourceUse> &uses);
    void prepareRange(size_t from, size_t to);

    size_t encoderEnd(size_t begin) const
    {
        return begins_[operations_[begin].payload].end;
    }

    uint64_t renderGeneration() const
    {
        return renderGeneration_;
    }

    uint64_t eligibilityGeneration() const
    {
        return eligibilityGeneration_;
    }

    const std::vector<void *> &renderPipelines(size_t from, size_t to) const
    {
        return rangePipelines_.at({from, to});
    }

    size_t storageBytes() const;
    void clear();

    template <typename F> void forEachUse(size_t from, size_t to, F visit) const
    {
        if (const auto found = rangeUses_.find({from, to}); found != rangeUses_.end()) {
            const UseSpan span = useSpans_[found->second];
            for (uint32_t i = 0; i < span.count; ++i) {
                const auto &use = uses_[span.begin + i];
                visit(use.object, use.access);
            }
            return;
        }
        std::unordered_map<void *, uint8_t> unioned;
        for (size_t at = from; at < std::min(to, size()); ++at) {
            const UseSpan span = useSpans_[operations_[at].uses];
            for (uint32_t i = 0; i < span.count; ++i) {
                const auto &use = uses_[span.begin + i];
                if (use.object)
                    unioned[use.object] |= use.access;
            }
        }
        for (const auto &use : unioned)
            visit(use.first, use.second);
    }

    template <typename F> void forEachWritten(size_t from, size_t to, F visit) const
    {
        forEachUse(from, to, [&](void *object, uint8_t access) {
            if (access & raw::ResourceUse::Write)
                visit(object);
        });
    }
};
