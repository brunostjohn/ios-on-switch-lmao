// SPDX-License-Identifier: MIT
#include "RawMetalPlan.h"
#include <unordered_set>

namespace {

template <typename T> uint32_t append(std::vector<T> &values, T value)
{
    const auto index = static_cast<uint32_t>(values.size());
    values.push_back(std::move(value));
    return index;
}

void appendBindings(std::vector<raw::ResourceUse> &uses, const raw::BufferBindings &buffers,
                    const raw::TextureBindings &textures, uint8_t access)
{
    for (const auto &binding : buffers)
        if (binding.source == raw::BufferBinding::Source::Buffer && binding.object)
            uses.push_back({binding.object, access});
    for (void *texture : textures.objects)
        if (texture)
            uses.push_back({texture, uint8_t(access | raw::ResourceUse::Sample)});
}

}  // namespace

uint32_t RMCommandPlan::saveUses(const std::vector<raw::ResourceUse> &uses)
{
    if (uses.empty())
        return 0;
    const uint32_t begin = static_cast<uint32_t>(uses_.size());
    for (const auto &use : uses) {
        if (!use.object)
            continue;
        auto found = std::find_if(uses_.begin() + begin, uses_.end(), [&](const auto &candidate) {
            return candidate.object == use.object;
        });
        if (found == uses_.end())
            uses_.push_back(use);
        else
            found->access |= use.access;
    }
    return append(useSpans_, {begin, static_cast<uint32_t>(uses_.size()) - begin});
}

uint32_t RMCommandPlan::captureRender()
{
    if (savedRender_ == renderGeneration_)
        return currentRender_;
    raw::RenderStateRef state;
    state.epoch = stateEpoch_;
    std::vector<raw::ResourceUse> uses;
    for (uint32_t stage = 0; stage < 2; ++stage) {
        state.buffers[stage] = buffers[stage].capture();
        state.textures[stage] = textures[stage].capture();
        state.samplers[stage] = samplers[stage].capture();
        appendBindings(uses, buffers[stage].current(), textures[stage].current(), raw::ResourceUse::Read);
    }
    state.raster = raster.capture();
    state.uses = saveUses(uses);
    currentRender_ = append(renderStates, state);
    savedRender_ = renderGeneration_;
    return currentRender_;
}

uint32_t RMCommandPlan::captureKernel()
{
    if (savedKernel_ == kernelGeneration_)
        return currentKernel_;
    std::vector<raw::ResourceUse> uses;
    appendBindings(uses, buffers[2].current(), textures[2].current(), raw::ResourceUse::Read | raw::ResourceUse::Write);
    currentKernel_ = append(kernelStates, {buffers[2].capture(), textures[2].capture(), samplers[2].capture(),
                                           kernel.capture(), saveUses(uses), stateEpoch_});
    savedKernel_ = kernelGeneration_;
    return currentKernel_;
}

bool RMCommandPlan::record(const Cmd &c, const std::vector<uint64_t> &extra)
{
    if (setter(c, extra))
        return false;
    RMPlanOperation operation;
    operation.op = c.op;
    switch (c.op) {
    case OpRender:
    case OpBlit:
    case OpCompute:
        encoder_ = c.op;
        encoderBegin_ = size();
        ++stateEpoch_;
        ++eligibilityGeneration_;
        for (auto &state : buffers)
            state.resetCurrent();
        for (auto &state : textures)
            state.resetCurrent();
        for (auto &state : samplers)
            state.resetCurrent();
        raster.resetCurrent();
        kernel.resetCurrent();
        ++renderGeneration_;
        ++kernelGeneration_;
        operation.payload = append(begins_, {c.obj, uint32_t(c.x), c.a, bool(c.index)});
        break;
    case OpDraw:
    case OpDrawIndexed:
    case OpDrawIndirect:
    case OpDrawIndexedIndirect: {
        RMPlanDraw draw;
        draw.state = captureRender();
        draw.primitive = c.stage;
        if (c.op == OpDraw) {
            draw.first = c.a;
            draw.count = c.b;
            draw.instances = uint32_t(c.c >> 32);
            draw.baseInstance = uint32_t(c.c);
        } else if (c.op == OpDrawIndexed) {
            draw.count = c.a;
            draw.indexType = uint8_t(c.b);
            draw.indices = c.obj;
            draw.indexOffset = c.c;
            draw.instances = uint32_t(extra[c.x]);
            draw.baseVertex = int32_t(extra[c.x + 1]);
            draw.baseInstance = uint32_t(extra[c.x + 2]);
        } else {
            draw.indices = c.obj;
            draw.indexType = uint8_t(c.a);
            draw.indexOffset = c.c;
            draw.arguments = c.obj2;
            draw.argumentOffset = c.x;
        }
        operation.uses = renderStates[draw.state].uses;
        operation.payload = append(draws_, draw);
        break;
    }
    case OpDispatch: {
        RMPlanDispatch dispatch;
        dispatch.state = captureKernel();
        dispatch.surfaceSlots = c.a;
        dispatch.kernel = c.obj2;
        dispatch.perThread = c.index;
        dispatch.threads = {uint32_t(c.c & 0xffff), uint32_t((c.c >> 16) & 0xffff), uint32_t(c.c >> 32)};
        std::copy_n(extra.begin() + c.x, 3, dispatch.grid.begin());
        std::copy_n(extra.begin() + c.x + 3, 3, dispatch.groups.begin());
        operation.uses = kernelStates[dispatch.state].uses;
        operation.payload = append(dispatches_, dispatch);
        break;
    }
    case OpSignalEvent:
    case OpWaitEvent:
        operation.payload = append(events_, {size(), c.obj, c.c, c.a, c.op == OpSignalEvent});
        break;
    case OpCopyTexture:
    case OpCopyBuffer:
    case OpCopyBufferToTexture:
    case OpCopyTextureToBuffer:
    case OpFillBuffer:
    case OpMipmaps:
        operation.payload = append(transfers_, {c.obj, c.obj2, c.c, 0, c.x, c.a, uint8_t(c.a)});
        operation.uses = saveUses({{c.obj, uint8_t(c.op == OpFillBuffer || c.op == OpMipmaps ? raw::ResourceUse::Write
                                                                                             : raw::ResourceUse::Read)},
                                   {c.obj2, raw::ResourceUse::Write}});
        break;
    case OpEnd:
        if (encoder_ != OpEnd)
            begins_[operations_[encoderBegin_].payload].end = size() + 1;
        encoder_ = OpEnd;
        break;
    default: {
        RMPlanControl control{c.obj, c.c, c.a, uint32_t(c.c), c.b, raw::kNoSnapshot, raw::kNoSnapshot, c.stage};
        if (c.op == OpInvocation) {
            if (encoder_ == OpRender)
                control.render = captureRender();
            if (encoder_ == OpCompute)
                control.compute = captureKernel();
        }
        operation.payload = append(controls_, control);
        if (c.op == OpInvocation) {
            const uint32_t before = control.render != raw::kNoSnapshot    ? renderStates[control.render].uses
                                    : control.compute != raw::kNoSnapshot ? kernelStates[control.compute].uses
                                                                          : 0;
            const UseSpan span = useSpans_[before];
            std::vector<raw::ResourceUse> possible(uses_.begin() + span.begin, uses_.begin() + span.begin + span.count);
            for (auto &use : possible)
                use.access |= raw::ResourceUse::Read | raw::ResourceUse::Write;
            operation.uses = saveUses(possible);
            for (auto &state : buffers)
                state.resetCurrent();
            for (auto &state : textures)
                state.resetCurrent();
            for (auto &state : samplers)
                state.resetCurrent();
            raster.resetCurrent();
            kernel.resetCurrent();
            ++renderGeneration_;
            ++kernelGeneration_;
            ++stateEpoch_;
            ++eligibilityGeneration_;
        }
        if (c.op == OpUse || c.op == OpKernelUse) {
            const uint32_t usage = c.op == OpUse ? c.a : uint32_t(c.c);
            operation.uses = saveUses(
                {{c.obj, uint8_t((usage & 2 ? raw::ResourceUse::Write : 0) | (usage & 5 ? raw::ResourceUse::Read : 0) |
                                 (usage & 4 ? raw::ResourceUse::Sample : 0) | raw::ResourceUse::Explicit)}});
        }
        break;
    }
    }
    operations_.push_back(operation);
    if (isPlanDraw(c.op)) {
        const auto &last = draw(size() - 1);
        operations_.back().uses = drawUses(operations_.back().uses, last.indices, last.arguments);
    }
    return true;
}

uint32_t RMCommandPlan::drawUses(uint32_t base, void *indices, void *arguments)
{
    if (!indices && !arguments)
        return base;
    const auto key =
        std::make_tuple(base, reinterpret_cast<uintptr_t>(indices), reinterpret_cast<uintptr_t>(arguments));
    const auto found = drawUseSpans_.find(key);
    if (found != drawUseSpans_.end())
        return found->second;
    const UseSpan span = useSpans_[base];
    std::vector<raw::ResourceUse> merged(uses_.begin() + span.begin, uses_.begin() + span.begin + span.count);
    merged.push_back({indices, raw::ResourceUse::Read});
    merged.push_back({arguments, raw::ResourceUse::Read});
    const uint32_t result = saveUses(merged);
    drawUseSpans_.emplace(key, result);
    return result;
}

void RMCommandPlan::addUses(size_t at, const std::vector<raw::ResourceUse> &uses)
{
    const UseSpan old = useSpans_[operations_[at].uses];
    std::vector<raw::ResourceUse> merged(uses_.begin() + old.begin, uses_.begin() + old.begin + old.count);
    merged.insert(merged.end(), uses.begin(), uses.end());
    operations_[at].uses = saveUses(merged);
}

void RMCommandPlan::setRenderUses(uint32_t state, const std::vector<raw::ResourceUse> &uses)
{
    renderStates[state].uses = saveUses(uses);
    renderStates[state].usesFinal = true;
    const auto &last = draw(size() - 1);
    operations_.back().uses = drawUses(renderStates[state].uses, last.indices, last.arguments);
}

void RMCommandPlan::prepareRange(size_t from, size_t to)
{
    if (rangeUses_.count({from, to}))
        return;
    std::unordered_set<uint32_t> spans;
    std::vector<raw::ResourceUse> merged;
    std::vector<void *> pipelines;
    auto pipeline = [&](void *object) {
        if (object && std::find(pipelines.begin(), pipelines.end(), object) == pipelines.end())
            pipelines.push_back(object);
    };
    auto span = [&](uint32_t index) {
        if (!spans.insert(index).second)
            return;
        const UseSpan use = useSpans_[index];
        merged.insert(merged.end(), uses_.begin() + use.begin, uses_.begin() + use.begin + use.count);
    };
    for (size_t at = from; at < std::min(to, size());) {
        const auto &operation = operations_[at];
        if (operation.op == OpRender || operation.op == OpCompute || operation.op == OpBlit) {
            const size_t end = encoderEnd(at);
            const auto cached = rangeUses_.find({at, end});
            if (end > at && end <= to && cached != rangeUses_.end()) {
                span(cached->second);
                for (void *object : rangePipelines_.at({at, end}))
                    pipeline(object);
                at = end;
                continue;
            }
        }
        span(operation.uses);
        if (isPlanDraw(operation.op))
            pipeline(raster[renderStates[draw(at).state].raster].pipeline);
        else if (operation.op == OpInvocation && control(at).render != raw::kNoSnapshot)
            pipeline(raster[renderStates[control(at).render].raster].pipeline);
        ++at;
    }
    rangeUses_[{from, to}] = saveUses(merged);
    rangePipelines_[{from, to}] = std::move(pipelines);
}

Cmd RMCommandPlan::legacy(size_t at) const
{
    const auto &operation = operations_[at];
    Cmd c = cmd(operation.op);
    switch (c.op) {
    case OpRender:
    case OpBlit:
    case OpCompute: {
        const auto &begin = begins_[operation.payload];
        c.obj = begin.descriptor;
        c.x = begin.pass;
        c.a = begin.dispatchType;
        c.index = begin.refused;
        break;
    }
    case OpDraw:
    case OpDrawIndexed:
    case OpDrawIndirect:
    case OpDrawIndexedIndirect: {
        const auto &draw = draws_[operation.payload];
        c.stage = draw.primitive;
        c.a = c.op == OpDraw ? draw.first : draw.count;
        c.b = c.op == OpDraw ? draw.count : draw.indexType;
        c.c = c.op == OpDraw ? uint64_t(draw.instances) << 32 | draw.baseInstance : draw.indexOffset;
        c.obj = draw.indices;
        c.x = operation.payload;
        break;
    }
    case OpDispatch: {
        const auto &dispatch = dispatches_[operation.payload];
        c.a = dispatch.surfaceSlots;
        c.index = dispatch.perThread;
        c.obj2 = const_cast<void *>(dispatch.kernel);
        c.x = operation.payload;
        break;
    }
    case OpSignalEvent:
    case OpWaitEvent: {
        const auto &event = events_[operation.payload];
        c.obj = event.event;
        c.c = event.value;
        c.a = event.timeout;
        break;
    }
    case OpCopyTexture:
    case OpCopyBuffer:
    case OpCopyBufferToTexture:
    case OpCopyTextureToBuffer:
    case OpFillBuffer:
    case OpMipmaps: {
        const auto &transfer = transfers_[operation.payload];
        c.obj = transfer.source;
        c.obj2 = transfer.destination;
        c.c = transfer.offset;
        c.x = transfer.parameters;
        c.a = transfer.aspect;
        break;
    }
    case OpEnd:
        break;
    default: {
        const auto &control = controls_[operation.payload];
        c.obj = control.object;
        c.c = control.value;
        c.a = control.scope;
        c.b = control.after;
        c.stage = control.stage;
        break;
    }
    }
    return c;
}

size_t RMCommandPlan::storageBytes() const
{
    size_t snapshots = raster.storageBytes() + kernel.storageBytes();
    for (const auto &state : buffers)
        snapshots += state.storageBytes();
    for (const auto &state : textures)
        snapshots += state.storageBytes();
    for (const auto &state : samplers)
        snapshots += state.storageBytes();
    snapshots +=
        renderStates.capacity() * sizeof(raw::RenderStateRef) + kernelStates.capacity() * sizeof(raw::KernelStateRef);
    return snapshots + operations_.capacity() * sizeof(RMPlanOperation) + draws_.capacity() * sizeof(RMPlanDraw) +
           dispatches_.capacity() * sizeof(RMPlanDispatch) + transfers_.capacity() * sizeof(RMPlanTransfer) +
           controls_.capacity() * sizeof(RMPlanControl) + begins_.capacity() * sizeof(RMPlanBegin) +
           events_.capacity() * sizeof(RMPlanEvent) + uses_.capacity() * sizeof(raw::ResourceUse);
}

void RMCommandPlan::clear()
{
    operations_.clear();
    begins_.clear();
    draws_.clear();
    dispatches_.clear();
    transfers_.clear();
    controls_.clear();
    events_.clear();
    uses_.clear();
    useSpans_.clear();
    useSpans_.push_back({});
    renderStates.clear();
    kernelStates.clear();
    for (auto &state : buffers)
        state.clear();
    for (auto &state : textures)
        state.clear();
    for (auto &state : samplers)
        state.clear();
    raster.clear();
    kernel.clear();
    renderGeneration_ = kernelGeneration_ = stateEpoch_ = eligibilityGeneration_ = 0;
    savedRender_ = savedKernel_ = UINT64_MAX;
    currentRender_ = currentKernel_ = raw::kNoSnapshot;
    encoder_ = OpEnd;
    encoderBegin_ = 0;
    rangeUses_.clear();
    rangePipelines_.clear();
    drawUseSpans_.clear();
}
