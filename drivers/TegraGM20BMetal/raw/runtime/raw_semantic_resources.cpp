// SPDX-License-Identifier: MIT
#include "RawMetalPlan.h"
#include <unordered_set>

void RMCommandPlan::registerResource(void *object, RMResourceFacts facts)
{
    if (object)
        resourceFacts_.try_emplace(object, facts);
}

void RMCommandPlan::classifyRange(Range &range)
{
    const UseSpan span = useSpans_[range.uses];
    range.resources.reserve(span.count);
    for (uint32_t i = 0; i < span.count; ++i) {
        const auto &use = uses_[span.begin + i];
        const auto found = resourceFacts_.find(use.object);
        const auto facts = found != resourceFacts_.end() ? found->second : RMResourceFacts{};
        range.resources.push_back({use.object, facts, use.access});
        const uint8_t feedback = raw::ResourceUse::Sample | raw::ResourceUse::Write;
        if (facts.kind == RMResourceKind::Texture &&
            (facts.roles & (RMResourceFacts::Converted | RMResourceFacts::PlanarTarget)) ==
                (RMResourceFacts::Converted | RMResourceFacts::PlanarTarget) &&
            (use.access & feedback) == feedback)
            range.planarFeedback = use.object;
    }
    std::stable_sort(range.resources.begin(), range.resources.end(), [](const auto &a, const auto &b) {
        return a.facts.kind < b.facts.kind;
    });
    uint32_t at = 0;
    for (size_t kind = 0; kind < size_t(RMResourceKind::Count); ++kind) {
        range.first[kind] = at;
        while (at < range.resources.size() && size_t(range.resources[at].facts.kind) == kind)
            ++at;
    }
    range.first[size_t(RMResourceKind::Count)] = at;
}

void *RMCommandPlan::nativeRangeRefusal(size_t from, size_t to) const
{
    const auto found = ranges_.find({from, to});
    return found != ranges_.end() && found->second.computes.empty() ? found->second.planarFeedback : nullptr;
}

size_t RMCommandPlan::rangeStorageBytes() const
{
    size_t bytes = resourceFacts_.size() * sizeof(std::pair<void *const, RMResourceFacts>) +
                   resourceFacts_.bucket_count() * sizeof(void *);
    for (const auto &entry : ranges_) {
        const auto &range = entry.second;
        bytes += sizeof(Range) + (range.pipelines.capacity() + range.variants.capacity()) * sizeof(void *) +
                 range.computes.capacity() * sizeof(size_t) + range.resources.capacity() * sizeof(RMRangeResource);
    }
    return bytes;
}

void RMCommandPlan::prepareRange(size_t from, size_t to)
{
    if (ranges_.count({from, to}))
        return;
    std::unordered_set<uint32_t> spans;
    std::vector<raw::ResourceUse> merged;
    std::vector<void *> pipelines;
    std::vector<void *> variants;
    std::vector<size_t> computes;
    auto variant = [&](void *object) {
        if (object && std::find(variants.begin(), variants.end(), object) == variants.end())
            variants.push_back(object);
    };
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
            if (end > at && end <= to && (at != from || end != to))
                prepareRange(at, end);
            const auto cached = ranges_.find({at, end});
            if (end > at && end <= to && cached != ranges_.end()) {
                span(cached->second.uses);
                for (void *object : cached->second.pipelines)
                    pipeline(object);
                for (void *object : cached->second.variants)
                    variant(object);
                const auto &pending = cached->second.computes;
                computes.insert(computes.end(), pending.begin(), pending.end());
                at = end;
                continue;
            }
        }
        span(operation.uses);
        if (isPlanDraw(operation.op)) {
            const auto &state = raster[renderStates[draw(at).state].raster];
            pipeline(state.pipeline);
            variant(state.nativeVariant);
        } else if (operation.op == OpInvocation && control(at).render != raw::kNoSnapshot)
            pipeline(raster[renderStates[control(at).render].raster].pipeline);
        else if (operation.op == OpDispatch && dispatch(at).pending)
            computes.push_back(at);
        ++at;
    }
    Range range;
    range.uses = saveUses(merged);
    range.pipelines = std::move(pipelines);
    range.variants = std::move(variants);
    range.computes = std::move(computes);
    classifyRange(range);
    ranges_.emplace(std::make_pair(from, to), std::move(range));
}
