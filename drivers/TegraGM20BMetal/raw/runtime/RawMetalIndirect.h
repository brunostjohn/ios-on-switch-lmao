// SPDX-License-Identifier: MIT
// Indirect command buffers (RawMetalIndirect.mm, raw_icb.h)
#pragma once
#import "RawMetalInternal.h"
#include <functional>
#include <unordered_map>

// A command as the CPU encoded it, beside its words: what m2v's replay draws and which index buffers a range names
struct RMICBCommand {
    enum Kind : uint8_t {
        None,
        Draw,
        Indexed
    } kind = None;

    uint8_t primitive = 0, indexType = 0;
    uint32_t first = 0, count = 0, instances = 0, baseInstance = 0;
    int32_t baseVertex = 0;
    uint64_t indexOffset = 0;
    RMBuffer *indices = nil;
};

@interface RMIndirectCommandBuffer : RMObject {
@public
    raw::Buffer _raw;  // the commands' words, raw::icb::kSlotWords each
    NSUInteger _count;
    raw::Mutex _lock;  // commands are encoded from any thread
    std::vector<RMICBCommand> _commands;
    std::unordered_map<void *, uint32_t> _indexBuffers;  // the index buffers its commands name, and how many name each
    uint32_t _kinds[2];                                  // its commands of triangles, and of lines or points
    // a blit encoder's reset or copy wrote its words on the GPU: an execute fetches them after the GPU's writes land
    std::atomic<bool> _gpuWritten;
}
@end

@interface RMIndirectRenderCommand : RMObject {
@public
    RMIndirectCommandBuffer *_buffer;
    NSUInteger _index;
}
@end

// The raw encode of OpExecuteCommands, and m2v's replay of it: each command drawn as the encoder's own draw
bool encodeCommands(raw::RenderEncoder *re, raw::CommandBuffer *rc, const RMPlanDraw &draw, const char **why);
void replayCommands(id<MTLRenderCommandEncoder> encoder, const RMPlanDraw &draw, const std::function<id(void *)> &use);
// OpResetCommands and OpCopyCommands: on the copy engine (`be`), or with none (m2v's replay) the commands as the CPU keeps
// them alone, which every encode brings to the commands' state in its command buffer's order
void encodeCommandsBlit(raw::BlitEncoder *be, const Cmd &c);
