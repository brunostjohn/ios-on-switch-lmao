// SPDX-License-Identifier: MIT
// See offline_metal.h. Each call answers as the frontend's (NativeBridgeMetalFrontend.mm NBLibrary, NBFunction,
// renderDescriptor, computeDescriptor) does for its own objects, with the translator's functions in place of the runtime's.
#import "offline_metal.h"

#include <dlfcn.h>
#include <string>
#include <vector>
#include "../runtime/RawPipelineKey.h"
#include "../runtime/RawRenderKey.h"
#include "MetallibSlice.h"
#include "bridge/NativeMetalDirect.h"

@interface RawOfflineLibrary : NSObject
@property (nonatomic) NmbOfflineLibrary *handle;
@property (nonatomic, strong) RawOfflineDevice *device;
@property (nonatomic, strong) id storage;
@property (nonatomic) const uint8_t *bytes;
@property (nonatomic) size_t length;
@end

@interface RawOfflineFunction : NSObject <RawLibraryFunction>
@property (nonatomic) NmbOfflineFunction *handle;
@property (nonatomic, strong) RawOfflineLibrary *library;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *libraryFunctionName;
@property (nonatomic) MTLFunctionType functionType;
+ (instancetype)of:(NmbOfflineFunction *)handle
           library:(RawOfflineLibrary *)library
              name:(NSString *)name
       libraryName:(NSString *)libraryName;
@end

// What Direct::make reads of a vertexAttributes entry
@interface RawOfflineAttribute : NSObject
@property (nonatomic) NSUInteger attributeIndex;
@property (nonatomic, getter=isActive) BOOL active;
@end

@implementation RawOfflineAttribute
@end

namespace {

NSError *refusal(NSString *why)
{
    return [NSError errorWithDomain:MTLLibraryErrorDomain
                               code:MTLLibraryErrorUnsupported
                           userInfo:@{NSLocalizedDescriptionKey : why}];
}

}  // namespace

@implementation RawOfflineFunction

- (void)dealloc
{
    nmb_offline_function_release(_handle);
}

+ (instancetype)of:(NmbOfflineFunction *)handle
           library:(RawOfflineLibrary *)library
              name:(NSString *)name
       libraryName:(NSString *)libraryName
{
    if (!handle)
        return nil;
    RawOfflineFunction *f = [RawOfflineFunction new];
    f.handle = handle;
    f.library = library;
    f.name = name;
    f.libraryFunctionName = libraryName;
    f.functionType = MTLFunctionType(nmb_offline_function_type(handle));
    return f;
}

- (const void *)libraryBytes:(size_t *)length
{
    *length = _library.length;
    return _library.bytes;
}

- (NSArray *)vertexAttributes
{
    const int64_t count = nmb_offline_function_vertex_attribute_count(_handle);
    if (count < 0)
        return nil;
    NSMutableArray *out = [NSMutableArray arrayWithCapacity:NSUInteger(count)];
    for (uint32_t i = 0; i < uint32_t(count); i++) {
        char name[256];
        uint64_t info[5] = {};
        if (!nmb_offline_function_vertex_attribute(_handle, i, name, sizeof name, info))
            continue;
        RawOfflineAttribute *a = [RawOfflineAttribute new];
        a.attributeIndex = info[0];
        a.active = info[2] != 0;
        [out addObject:a];
    }
    return out;
}

@end

@implementation RawOfflineLibrary

- (void)dealloc
{
    nmb_offline_library_release(_handle);
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    if (!name)
        return nil;
    return (id<MTLFunction>)[RawOfflineFunction of:nmb_offline_function_new(_handle, name.UTF8String)
                                           library:self
                                              name:name
                                       libraryName:name];
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    return [self newFunctionWithName:name specializedName:nil constantValues:values error:error];
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                       specializedName:(NSString *)specialized
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    if (!name || !values) {
        if (error)
            *error = refusal(@"unqualified function constant values");
        return nil;
    }
    MTLFunctionConstantValues *snapshot = [values copy];
    NSArray *indexed = [snapshot newIndexedConstantArray], *named = [snapshot newNamedConstantArray];
    std::vector<NmbConstant> constants;
    for (unsigned group = 0; group < 2; group++)
        for (id<RMConstantValue> v in (group ? named : indexed)) {
            const uint64_t bytes = raw::constantBytes(v.dataType);
            if (!bytes || !v.data) {
                if (error)
                    *error = refusal([NSString
                        stringWithFormat:@"constant type %lu is not qualified", (unsigned long)v.dataType]);
                return nil;
            }
            constants.push_back(
                {v.dataType, group ? 0 : v.index, group ? v.name.UTF8String : nullptr, v.data, bytes});
        }
    if (!specialized.length)
        specialized = nil;
    RawOfflineFunction *f =
        [RawOfflineFunction of:nmb_offline_function_new_specialized(_handle, name.UTF8String, specialized.UTF8String,
                                                                    constants.data(), uint32_t(constants.size()))
                       library:self
                          name:specialized ?: name
                   libraryName:name];
    if (!f && error)
        *error = refusal([NSString stringWithFormat:@"specialized function: %s", nmb_last_error()]);
    return (id<MTLFunction>)f;
}

- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error
{
    if (!d.name) {
        if (error)
            *error = refusal(@"a function name is required");
        return nil;
    }
    if (d.constantValues || d.specializedName.length)
        return [self newFunctionWithName:d.name
                         specializedName:d.specializedName
                          constantValues:d.constantValues ?: [MTLFunctionConstantValues new]
                                   error:error];
    id<MTLFunction> function = [self newFunctionWithName:d.name];
    if (!function && error)
        *error = refusal([NSString stringWithFormat:@"function %@: %s", d.name, nmb_last_error()]);
    return function;
}

@end

@implementation RawOfflineDevice

// The library of a metallib's AIR slice, as the frontend's metallibLibrary makes it; `storage` keeps the bytes
- (RawOfflineLibrary *)libraryOf:(id)storage bytes:(const uint8_t *)file length:(size_t)length error:(NSError **)error
{
    size_t offset = 0, size = 0;
    if (!switch_metal::airSlice(file, length, offset, size)) {
        if (error)
            *error = refusal(@"This library does not have a compatible AIR slice for this device");
        return nil;
    }
    NmbOfflineLibrary *handle = nmb_offline_library_new(file + offset, size);
    if (!handle) {
        if (error)
            *error = refusal(@(nmb_last_error()));
        return nil;
    }
    RawOfflineLibrary *library = [RawOfflineLibrary new];
    library.handle = handle;
    library.device = self;
    library.storage = storage;
    library.bytes = file + offset;
    library.length = size;
    return library;
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    NSData *bytes = url.isFileURL ? [NSData dataWithContentsOfFile:url.path
                                                           options:NSDataReadingMappedIfSafe
                                                             error:error]
                                  : nil;
    if (!bytes)
        return nil;
    return (id<MTLLibrary>)[self libraryOf:bytes
                                     bytes:static_cast<const uint8_t *>(bytes.bytes)
                                    length:bytes.length
                                     error:error];
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    const void *bytes = nullptr;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map(data, &bytes, &size);
    return (id<MTLLibrary>)[self libraryOf:mapped bytes:static_cast<const uint8_t *>(bytes) length:size error:error];
}

@end

namespace offline {
namespace {

void *gNvk = nullptr;

void *nvkSymbol(const char *name)
{
    return gNvk ? dlsym(gNvk, name) : nullptr;
}

bool handles(NSArray *functions, std::vector<NmbOfflineFunction *> &out)
{
    for (id f in functions) {
        if (![f isKindOfClass:[RawOfflineFunction class]])
            return false;
        out.push_back(((RawOfflineFunction *)f).handle);
    }
    return true;
}

// `linked` gets the stage's functions, then its privateFunctions: `visible` and `hidden` of them
bool stageLinked(MTLLinkedFunctions *functions, std::vector<NmbOfflineFunction *> &linked, size_t &visible,
                 size_t &hidden)
{
    if (functions.binaryFunctions.count || functions.groups.count)
        return false;
    const size_t before = linked.size();
    if (!handles(functions.functions, linked))
        return false;
    visible = linked.size() - before;
    if (!handles(functions.privateFunctions, linked))
        return false;
    hidden = linked.size() - before - visible;
    return true;
}

// `linked` holds the handles `p` points into
bool renderDescriptor(MTLRenderPipelineDescriptor *d, NmbOfflineRenderPipelineDescriptor &p,
                      std::vector<NmbOfflineFunction *> &linked)
{
    if (![(id)d.vertexFunction isKindOfClass:[RawOfflineFunction class]] ||
        (d.fragmentFunction && ![(id)d.fragmentFunction isKindOfClass:[RawOfflineFunction class]]))
        return false;
    size_t vertexVisible = 0, vertexHidden = 0, fragmentVisible = 0, fragmentHidden = 0;
    if (!stageLinked(d.vertexLinkedFunctions, linked, vertexVisible, vertexHidden) ||
        !stageLinked(d.fragmentLinkedFunctions, linked, fragmentVisible, fragmentHidden))
        return false;
    p.vertex_linked = linked.data();
    p.vertex_linked_count = vertexVisible;
    p.vertex_private_linked = linked.data() + vertexVisible;
    p.vertex_private_count = vertexHidden;
    p.fragment_linked = linked.data() + vertexVisible + vertexHidden;
    p.fragment_linked_count = fragmentVisible;
    p.fragment_private_linked = linked.data() + vertexVisible + vertexHidden + fragmentVisible;
    p.fragment_private_count = fragmentHidden;
    p.vertex = ((RawOfflineFunction *)d.vertexFunction).handle;
    p.fragment = d.fragmentFunction ? ((RawOfflineFunction *)d.fragmentFunction).handle : nullptr;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        // A two-plane code format renders into its texture's RGBA16Float stand-in (the frontend's rendersThroughStandIn)
        const NSUInteger format = a.pixelFormat == 542 ? MTLPixelFormatRGBA16Float : a.pixelFormat;
        p.colors[i] = {(uint64_t)format,
                       (uint64_t)a.writeMask,
                       (uint64_t)a.blendingEnabled,
                       (uint64_t)a.sourceRGBBlendFactor,
                       (uint64_t)a.destinationRGBBlendFactor,
                       (uint64_t)a.rgbBlendOperation,
                       (uint64_t)a.sourceAlphaBlendFactor,
                       (uint64_t)a.destinationAlphaBlendFactor,
                       (uint64_t)a.alphaBlendOperation};
    }
    p.depth_format = d.depthAttachmentPixelFormat;
    p.stencil_format = d.stencilAttachmentPixelFormat;
    p.samples = d.rasterSampleCount;
    p.topology = d.inputPrimitiveTopology;
    p.rasterization = d.rasterizationEnabled;
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid)
                continue;
            p.vertex_inputs = 1;
            p.attributes[i] = {(uint64_t)a.format, a.offset, a.bufferIndex};
            if (a.bufferIndex < 31) {
                MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex];
                p.layouts[a.bufferIndex] = {l.stride, (uint64_t)l.stepFunction, l.stepRate};
            }
        }
    return true;
}

// As the frontend's computeDescriptor: no stage inputs, binary functions, groups or libraries
bool computeDescriptor(MTLComputePipelineDescriptor *d, NmbOfflineComputePipelineDescriptor &p,
                       std::vector<NmbOfflineFunction *> &linked)
{
    if (!d || ![(id)d.computeFunction isKindOfClass:[RawOfflineFunction class]])
        return false;
    MTLStageInputOutputDescriptor *stage = d.stageInputDescriptor;
    bool empty = !stage || (stage.indexType == MTLIndexTypeUInt16 && stage.indexBufferIndex == 0);
    for (NSUInteger index = 0; stage && index < 31; ++index) {
        MTLAttributeDescriptor *a = stage.attributes[index];
        MTLBufferLayoutDescriptor *b = stage.layouts[index];
        empty = empty && a.format == MTLAttributeFormatInvalid && !a.offset && !a.bufferIndex && !b.stride &&
                b.stepFunction == MTLStepFunctionPerVertex && b.stepRate == 1;
    }
    MTLLinkedFunctions *functions = d.linkedFunctions;
    if (!empty || functions.binaryFunctions.count || functions.groups.count || d.preloadedLibraries.count ||
        d.insertLibraries.count)
        return false;
    if (!handles(functions.functions, linked))
        return false;
    const size_t visible = linked.size();
    if (!handles(functions.privateFunctions, linked))
        return false;
    p.linked = linked.data();
    p.linked_count = visible;
    p.private_linked = linked.data() + visible;
    p.private_count = linked.size() - visible;
    p.function = ((RawOfflineFunction *)d.computeFunction).handle;
    p.max_threads = d.maxTotalThreadsPerThreadgroup;
    p.multiple_of_execution_width = d.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    p.indirect = d.supportIndirectCommandBuffers;
    p.max_call_stack_depth = d.maxCallStackDepth;
    p.adding_binary_functions = d.supportAddingBinaryFunctions;
    for (NSUInteger index = 0; index < 31; ++index)
        p.mutability[index] = d.buffers[index].mutability;
    return true;
}

bool render(id descriptor, bool barrier, NSUInteger textureFormat, NSUInteger textureSamples,
            const NmbDirectBinds *binds, bool (*each)(const NmbDirectState *, void *), void *ctx, const char **why)
{
    NmbOfflineRenderPipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    if (!renderDescriptor((MTLRenderPipelineDescriptor *)descriptor, p, linked)) {
        *why = "a function of another device";
        return false;
    }
    if (nmb_render_pipeline_offline(&p, barrier, textureFormat, uint32_t(textureSamples), binds, each, ctx))
        return true;
    *why = nmb_last_error();
    return false;
}

bool compute(id descriptor, const NmbDirectDispatch *dispatch, bool (*each)(const NmbDirectKernel *, void *),
             void *ctx, const char **why)
{
    NmbOfflineComputePipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    if (!computeDescriptor((MTLComputePipelineDescriptor *)descriptor, p, linked)) {
        *why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    if (nmb_compute_pipeline_offline(&p, dispatch, each, ctx))
        return true;
    *why = nmb_last_error();
    return false;
}

}  // namespace

raw::DirectTranslator translator(const char *nvk)
{
    gNvk = dlopen(nvk, RTLD_NOW | RTLD_LOCAL);
    raw::DirectTranslator t;
    t.render = render;
    t.compute = compute;
    t.nvkSymbol = nvkSymbol;
    return t;
}

}  // namespace offline
