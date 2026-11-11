// SPDX-License-Identifier: GPL-2.0-or-later OR LGPL-3.0-or-later
// MTLArgumentEncoder over a layout of argument descriptors (raw_argument_layout.h). Each argument is the 8 bytes a program
// could write itself: a buffer's GPU address plus its offset, any other object's gpuResourceID (raw_resource_id.h); a
// constant is the program's own bytes (constantDataAtIndex:). What Metal's validation stops writes nothing and is one
// refusal line, as metal2vulkan's encoder refuses it (metal2vulkan-runtime argument_encoder.rs, measured on an M3 Max and
// an M4): a setter before an argument buffer, an argument buffer the CPU cannot write or whose encoded length passes its end,
// a buffer offset past the buffer, an argument of another type at the index. Plain Metal's answers stand where validation
// would assert: a setter at an index no argument has writes at the arguments' offset 0 while 8 bytes fit there, and
// constantDataAtIndex: there answers the arguments' start.
#import "RawMetalInternal.h"
#include "raw_argument_layout.h"
#include "raw_resource_id.h"

// MTLDataTypeDepthStencilState (iOS 26): spelled as its value, so the iOS 17 build names no constant its SDK floor lacks
static constexpr NSUInteger kDataTypeDepthStencilState = 139;

// metal2vulkan's type_size_align: every object 8 bytes, a constant's MSL scalar, rows and columns
static bool argumentType(NSUInteger dataType, raw::ArgumentType &out)
{
    using K = raw::ArgumentKind;
    const auto handle = [&](K kind) {
        out = {kind, 0, 1, 1};
        return true;
    };
    const auto constant = [&](uint8_t scalar, uint8_t rows, uint8_t columns = 1) {
        out = {K::Constant, scalar, columns, rows};
        return true;
    };
    if (dataType == kDataTypeDepthStencilState)
        return handle(K::DepthStencilState);
    switch (MTLDataType(dataType)) {
    case MTLDataTypePointer:
        return handle(K::Buffer);
    case MTLDataTypeTexture:
        return handle(K::Texture);
    case MTLDataTypeSampler:
        return handle(K::Sampler);
    case MTLDataTypeIndirectCommandBuffer:
        return handle(K::IndirectCommandBuffer);
    case MTLDataTypeComputePipeline:
        return handle(K::ComputePipeline);
    case MTLDataTypeRenderPipeline:
        return handle(K::RenderPipeline);
    case MTLDataTypeVisibleFunctionTable:
        return handle(K::VisibleFunctionTable);
    case MTLDataTypeIntersectionFunctionTable:
        return handle(K::IntersectionFunctionTable);
    case MTLDataTypePrimitiveAccelerationStructure:
    case MTLDataTypeInstanceAccelerationStructure:
        return handle(K::AccelerationStructure);
    case MTLDataTypeFloat:
        return constant(4, 1);
    case MTLDataTypeFloat2:
        return constant(4, 2);
    case MTLDataTypeFloat3:
        return constant(4, 3);
    case MTLDataTypeFloat4:
        return constant(4, 4);
    case MTLDataTypeFloat2x2:
        return constant(4, 2, 2);
    case MTLDataTypeFloat2x3:
        return constant(4, 3, 2);
    case MTLDataTypeFloat2x4:
        return constant(4, 4, 2);
    case MTLDataTypeFloat3x2:
        return constant(4, 2, 3);
    case MTLDataTypeFloat3x3:
        return constant(4, 3, 3);
    case MTLDataTypeFloat3x4:
        return constant(4, 4, 3);
    case MTLDataTypeFloat4x2:
        return constant(4, 2, 4);
    case MTLDataTypeFloat4x3:
        return constant(4, 3, 4);
    case MTLDataTypeFloat4x4:
        return constant(4, 4, 4);
    case MTLDataTypeHalf:
        return constant(2, 1);
    case MTLDataTypeHalf2:
        return constant(2, 2);
    case MTLDataTypeHalf3:
        return constant(2, 3);
    case MTLDataTypeHalf4:
        return constant(2, 4);
    case MTLDataTypeHalf2x2:
        return constant(2, 2, 2);
    case MTLDataTypeHalf2x3:
        return constant(2, 3, 2);
    case MTLDataTypeHalf2x4:
        return constant(2, 4, 2);
    case MTLDataTypeHalf3x2:
        return constant(2, 2, 3);
    case MTLDataTypeHalf3x3:
        return constant(2, 3, 3);
    case MTLDataTypeHalf3x4:
        return constant(2, 4, 3);
    case MTLDataTypeHalf4x2:
        return constant(2, 2, 4);
    case MTLDataTypeHalf4x3:
        return constant(2, 3, 4);
    case MTLDataTypeHalf4x4:
        return constant(2, 4, 4);
    case MTLDataTypeBFloat:
        return constant(2, 1);
    case MTLDataTypeBFloat2:
        return constant(2, 2);
    case MTLDataTypeBFloat3:
        return constant(2, 3);
    case MTLDataTypeBFloat4:
        return constant(2, 4);
    case MTLDataTypeInt:
    case MTLDataTypeUInt:
        return constant(4, 1);
    case MTLDataTypeInt2:
    case MTLDataTypeUInt2:
        return constant(4, 2);
    case MTLDataTypeInt3:
    case MTLDataTypeUInt3:
        return constant(4, 3);
    case MTLDataTypeInt4:
    case MTLDataTypeUInt4:
        return constant(4, 4);
    case MTLDataTypeShort:
    case MTLDataTypeUShort:
        return constant(2, 1);
    case MTLDataTypeShort2:
    case MTLDataTypeUShort2:
        return constant(2, 2);
    case MTLDataTypeShort3:
    case MTLDataTypeUShort3:
        return constant(2, 3);
    case MTLDataTypeShort4:
    case MTLDataTypeUShort4:
        return constant(2, 4);
    case MTLDataTypeChar:
    case MTLDataTypeUChar:
    case MTLDataTypeBool:
        return constant(1, 1);
    case MTLDataTypeChar2:
    case MTLDataTypeUChar2:
    case MTLDataTypeBool2:
        return constant(1, 2);
    case MTLDataTypeChar3:
    case MTLDataTypeUChar3:
    case MTLDataTypeBool3:
        return constant(1, 3);
    case MTLDataTypeChar4:
    case MTLDataTypeUChar4:
    case MTLDataTypeBool4:
        return constant(1, 4);
    case MTLDataTypeLong:
    case MTLDataTypeULong:
        return constant(8, 1);
    case MTLDataTypeLong2:
    case MTLDataTypeULong2:
        return constant(8, 2);
    case MTLDataTypeLong3:
    case MTLDataTypeULong3:
        return constant(8, 3);
    case MTLDataTypeLong4:
    case MTLDataTypeULong4:
        return constant(8, 4);
    default:
        return false;
    }
}

static uint64_t resourceWord(id object)
{
    if (!object || ![object respondsToSelector:@selector(gpuResourceID)])
        return 0;
    const MTLResourceID resource = [(id<MTLTexture>)object gpuResourceID];
    uint64_t word;
    static_assert(sizeof resource == sizeof word);
    memcpy(&word, &resource, sizeof word);
    return word;
}

static void refuse(const char *call, NSString *why)
{
    once([NSString stringWithFormat:@"argument encoder %s refused: %@", call, why]);
}

@interface RMArgumentEncoder : RMObject <MTLArgumentEncoder> {
@public
    raw::ArgumentLayout _layout;
    id<MTLBuffer> _buffer;
    uint64_t _start;  // the arguments' offset in _buffer
}
@end

@implementation RMArgumentEncoder
@dynamic device;  // RMObject's

+ (Class)twinClass
{
    return NSClassFromString(@"NBArgumentEncoder");
}

- (NSUInteger)encodedLength
{
    return NSUInteger(_layout.encodedLength);
}

- (NSUInteger)alignment
{
    return NSUInteger(_layout.alignment);
}

- (void)setArgumentBuffer:(id<MTLBuffer>)buffer startOffset:(NSUInteger)start arrayElement:(NSUInteger)element
{
    _buffer = nil;
    if (!buffer)
        return;
    if (![(id)buffer isKindOfClass:[RMBuffer class]])
        return refuse("setArgumentBuffer", @"an argument buffer of another device");
    if (buffer.storageMode == MTLStorageModePrivate || !buffer.contents)
        return refuse("setArgumentBuffer", @"an argument buffer the CPU cannot write");
    const uint64_t at = uint64_t(start) + uint64_t(element) * _layout.encodedLength;
    if (at > buffer.length || buffer.length - at < _layout.encodedLength)
        return refuse("setArgumentBuffer", @"the encoded length passes the argument buffer's end");
    _buffer = buffer;
    _start = at;
}

- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset
{
    [self setArgumentBuffer:buffer startOffset:offset arrayElement:0];
}

// Where argument `index` of `kind` goes in the argument buffer's bytes; null where Metal's validation stops the call
- (uint8_t *)at:(NSUInteger)index kind:(raw::ArgumentKind)kind call:(const char *)call
{
    if (!_buffer) {
        refuse(call, @"no argument buffer is set");
        return nullptr;
    }
    raw::ArgumentPlace place;
    uint64_t offset = 0;
    if (index <= UINT32_MAX && _layout.place(uint32_t(index), place)) {
        if (place.kind != kind) {
            refuse(call,
                   [NSString stringWithFormat:@"index %lu holds an argument of another type", (unsigned long)index]);
            return nullptr;
        }
        offset = place.offset;
    } else if (_buffer.length - _start < raw::kArgumentHandleBytes) {
        refuse(call, [NSString stringWithFormat:@"no argument has index %lu", (unsigned long)index]);
        return nullptr;
    }
    return static_cast<uint8_t *>(_buffer.contents) + _start + offset;
}

- (bool)write:(uint64_t)word index:(NSUInteger)index kind:(raw::ArgumentKind)kind call:(const char *)call
{
    uint8_t *at = [self at:index kind:kind call:call];
    if (at)
        memcpy(at, &word, sizeof word);
    return at != nullptr;
}

- (bool)fits:(NSRange)range kind:(raw::ArgumentKind)kind call:(const char *)call
{
    for (NSUInteger i = 0; i < range.length; i++)
        if (![self at:range.location + i kind:kind call:call])
            return false;
    return true;
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (buffer && offset >= buffer.length)
        return refuse("setBuffer", @"an offset at or past the buffer's end");
    [self write:buffer ? buffer.gpuAddress + offset : 0 index:index kind:raw::ArgumentKind::Buffer call:"setBuffer"];
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])buffers
           offsets:(const NSUInteger[])offsets
         withRange:(NSRange)range
{
    if (![self fits:range kind:raw::ArgumentKind::Buffer call:"setBuffers"])
        return;
    for (NSUInteger i = 0; i < range.length; i++)
        [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}

- (void)object:(id)object index:(NSUInteger)index kind:(raw::ArgumentKind)kind call:(const char *)call
{
    [self write:resourceWord(object) index:index kind:kind call:call];
}

- (void)objects:(const id __unsafe_unretained[])objects
          range:(NSRange)range
           kind:(raw::ArgumentKind)kind
           call:(const char *)call
{
    if (![self fits:range kind:kind call:call])
        return;
    for (NSUInteger i = 0; i < range.length; i++)
        [self object:objects[i] index:range.location + i kind:kind call:call];
}

- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    [self object:texture index:index kind:raw::ArgumentKind::Texture call:"setTexture"];
}

- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])textures withRange:(NSRange)range
{
    [self objects:textures range:range kind:raw::ArgumentKind::Texture call:"setTextures"];
}

- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    [self object:sampler index:index kind:raw::ArgumentKind::Sampler call:"setSamplerState"];
}

- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])samplers withRange:(NSRange)range
{
    [self objects:samplers range:range kind:raw::ArgumentKind::Sampler call:"setSamplerStates"];
}

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline atIndex:(NSUInteger)index
{
    [self object:pipeline index:index kind:raw::ArgumentKind::RenderPipeline call:"setRenderPipelineState"];
}

- (void)setRenderPipelineStates:(const id<MTLRenderPipelineState> __unsafe_unretained[])pipelines
                      withRange:(NSRange)range
{
    [self objects:pipelines range:range kind:raw::ArgumentKind::RenderPipeline call:"setRenderPipelineStates"];
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline atIndex:(NSUInteger)index
{
    [self object:pipeline index:index kind:raw::ArgumentKind::ComputePipeline call:"setComputePipelineState"];
}

- (void)setComputePipelineStates:(const id<MTLComputePipelineState> __unsafe_unretained[])pipelines
                       withRange:(NSRange)range
{
    [self objects:pipelines range:range kind:raw::ArgumentKind::ComputePipeline call:"setComputePipelineStates"];
}

- (void)setIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)buffer atIndex:(NSUInteger)index
{
    [self object:buffer index:index kind:raw::ArgumentKind::IndirectCommandBuffer call:"setIndirectCommandBuffer"];
}

- (void)setIndirectCommandBuffers:(const id<MTLIndirectCommandBuffer> __unsafe_unretained[])buffers
                        withRange:(NSRange)range
{
    [self objects:buffers range:range kind:raw::ArgumentKind::IndirectCommandBuffer call:"setIndirectCommandBuffers"];
}

- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)structure atIndex:(NSUInteger)index
{
    [self object:structure index:index kind:raw::ArgumentKind::AccelerationStructure call:"setAccelerationStructure"];
}

- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)table atIndex:(NSUInteger)index
{
    [self object:table index:index kind:raw::ArgumentKind::VisibleFunctionTable call:"setVisibleFunctionTable"];
}

- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained[])tables
                       withRange:(NSRange)range
{
    [self objects:tables range:range kind:raw::ArgumentKind::VisibleFunctionTable call:"setVisibleFunctionTables"];
}

- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)table atIndex:(NSUInteger)index
{
    [self object:table
           index:index
            kind:raw::ArgumentKind::IntersectionFunctionTable
            call:"setIntersectionFunctionTable"];
}

- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained[])tables
                            withRange:(NSRange)range
{
    [self objects:tables
            range:range
             kind:raw::ArgumentKind::IntersectionFunctionTable
             call:"setIntersectionFunctionTables"];
}

- (void)setDepthStencilState:(id<MTLDepthStencilState>)state atIndex:(NSUInteger)index
{
    [self object:state index:index kind:raw::ArgumentKind::DepthStencilState call:"setDepthStencilState"];
}

- (void)setDepthStencilStates:(const id<MTLDepthStencilState> __unsafe_unretained[])states withRange:(NSRange)range
{
    [self objects:states range:range kind:raw::ArgumentKind::DepthStencilState call:"setDepthStencilStates"];
}

- (void *)constantDataAtIndex:(NSUInteger)index
{
    if (!_buffer) {
        refuse("constantDataAtIndex", @"no argument buffer is set");
        return nullptr;
    }
    raw::ArgumentPlace place;
    const uint64_t offset = index <= UINT32_MAX && _layout.place(uint32_t(index), place) ? place.offset : 0;
    return static_cast<uint8_t *>(_buffer.contents) + _start + offset;
}

// A layout of descriptors holds no nested argument buffer: Metal answers nil ("No argument buffer at that index")
- (id<MTLArgumentEncoder>)newArgumentEncoderForBufferAtIndex:(NSUInteger)index
{
    (void)index;
    return nil;
}

@end

@implementation RMTexture (ResourceID)

- (MTLResourceID)gpuResourceID
{
    rmTexturePlain(self);
    MTLResourceID resource;
    const uint64_t word = raw::textureResourceId(_raw.tic);
    memcpy(&resource, &word, sizeof word);
    return resource;
}

@end

@implementation RMSampler (ResourceID)

- (MTLResourceID)gpuResourceID
{
    MTLResourceID resource;
    const uint64_t word = raw::samplerResourceId(_raw.tsc);
    memcpy(&resource, &word, sizeof word);
    return resource;
}

@end

@implementation RMDevice (ArgumentEncoders)

- (id<MTLArgumentEncoder>)newArgumentEncoderWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments
{
    std::vector<raw::ArgumentDescription> descriptions;
    descriptions.reserve(arguments.count);
    for (MTLArgumentDescriptor *a in arguments) {
        raw::ArgumentDescription d;
        if (!argumentType(NSUInteger(a.dataType), d.type)) {
            refuse("creation", [NSString stringWithFormat:@"no argument has data type %lu", (unsigned long)a.dataType]);
            return nil;
        }
        d.index = uint32_t(a.index);
        d.arrayLength = uint32_t(a.arrayLength);
        d.constantBlockAlignment = uint32_t(a.constantBlockAlignment);
        descriptions.push_back(d);
    }
    RMArgumentEncoder *encoder = [RMArgumentEncoder new];
    size_t position = 0;
    if (raw::ArgumentLayout::fromDescriptions(descriptions, encoder->_layout, position) !=
        raw::ArgumentLayoutError::None) {
        refuse("creation", [NSString stringWithFormat:@"descriptor %zu is one Metal refuses", position]);
        return nil;
    }
    encoder->_device = self;
    return encoder;
}

@end
