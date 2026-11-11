// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "raw_pass_fusion.h"
#include "raw_attachment_lifetime.h"

inline raw::FusionPass fusionPass(RMCommandBuffer *cb, size_t begin)
{
    RAW_ENCODE_SPAN(Fusion);
    const Cmd command = cb->_plan.legacy(begin);
    const RMPass &pass = cb->_passes[command.x];
    MTLRenderPassDescriptor *descriptor = (__bridge MTLRenderPassDescriptor *)command.obj;
    raw::FusionPass result;
    result.width = pass.width;
    result.height = pass.height;
    // a fused pass's draws take the first pass's sample layout
    result.bodySafe = !cb->_sampledShadows && !cb->_standIns && !pass.positions.count;
    std::array<RMTexture *, 10> targets{};
    auto attachment = [&](size_t slot, id object, NSUInteger level, MTLLoadAction load, MTLStoreAction store,
                          bool resolve) {
        if (!object)
            return;
        RMTexture *texture = (RMTexture *)object;
        const auto &t = texture->_raw;
        targets[slot] = texture;
        auto &a = result.attachments[slot];
        a.identity = uintptr_t((__bridge void *)texture);
        a.level = level;
        a.loads = load == MTLLoadActionLoad;
        a.stores = store == MTLStoreActionStore;
        a.resolves = resolve;
        a.native = raw::fusionTargetEligible(t, level);
    };
    for (size_t i = 0; i < 8; ++i) {
        const auto &a = pass.color[i];
        attachment(i, a.texture, a.level, a.load, a.store, descriptor.colorAttachments[i].resolveTexture != nil);
    }
    attachment(8, descriptor.depthAttachment.texture, descriptor.depthAttachment.level,
               descriptor.depthAttachment.loadAction, descriptor.depthAttachment.storeAction,
               descriptor.depthAttachment.resolveTexture != nil);
    attachment(9, descriptor.stencilAttachment.texture, descriptor.stencilAttachment.level,
               descriptor.stencilAttachment.loadAction, descriptor.stencilAttachment.storeAction,
               descriptor.stencilAttachment.resolveTexture != nil);
    const size_t end = cb->_plan.encoderEnd(begin);
    for (size_t i = begin + 1; i + 1 < end; ++i) {
        const Op op = cb->_plan.operation(i).op;
        if (!isPlanDraw(op) && op != OpBarrier)
            result.bodySafe = false;
        if (isPlanDraw(op) && !cb->_plan.renderStates[cb->_plan.draw(i).state].usesFinal)
            result.bodySafe = false;
    }
    if (!result.bodySafe)
        return result;
    cb->_plan.forEachUse(begin, end, [&](void *value, uint8_t access) {
        id object = (__bridge id)value;
        RMTexture *texture = [object isKindOfClass:[RMTexture class]] ? (RMTexture *)object : nil;
        RMBuffer *buffer = [object isKindOfClass:[RMBuffer class]] ? (RMBuffer *)object : nil;
        const bool target = texture && std::find(targets.begin(), targets.end(), texture) != targets.end();
        if ((access & raw::ResourceUse::Write) && !target)
            result.bodySafe = false;
        if (target && !(access & (raw::ResourceUse::Sample | raw::ResourceUse::Explicit)))
            return;
        if (!texture && !buffer)
            return;
        const uint64_t va = texture ? texture->_raw.va : buffer->_raw.va;
        const uint64_t cpu = uintptr_t(texture ? texture->_raw.cpu : buffer->_raw.cpu);
        const uint64_t bytes = texture ? raw::fusionImageBytes(texture->_raw)
                                      : buffer->_raw.length;
        const raw::Import *import = texture ? texture->_raw.import : buffer->_raw.import;
        for (RMTexture *attachment : targets) {
            if (!attachment)
                continue;
            const auto &t = attachment->_raw;
            const uint64_t size = raw::fusionImageBytes(t);
            if (texture == attachment || (import && import == t.import) ||
                raw::fusionRangesOverlap(va, bytes, t.va, size) ||
                raw::fusionRangesOverlap(cpu, bytes, uintptr_t(t.cpu), size) ||
                (import && t.import && raw::fusionRangesOverlap(import->base, import->bytes,
                                                                t.import->base, t.import->bytes)))
                result.bodySafe = false;
        }
    });
    return result;
}
