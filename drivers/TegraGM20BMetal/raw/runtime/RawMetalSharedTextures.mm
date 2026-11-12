// SPDX-License-Identifier: MIT
// Shared texture handles, in Apple's drivers' object model: a shared texture is an IOSurface texture of the device
// whose surface's creation properties hold its descriptor and the device's registry ID (raw_shared_texture.h), made by
// newSharedTextureWithDescriptor: as _MTLDevice makes it over copyIOSurfaceSharedTextureProperties: (AGX's override);
// any texture over such a surface is shareable (IOGPUMetalTexture), and its handle is Metal's own
// MTLSharedTextureHandle over the surface, which finds the receiving process's device by that ID and crosses processes
// by XPC or mach port. The device's registry ID is the IORegistry entry ID of the one accelerator service with a Metal
// plugin (TegraGM20BMetalPlugin.mm), the same kernel object in every process.
#import "RawMetalInternal.h"
#import "RawMetalSharedTexture.h"

@interface MTLSharedTextureHandle (Surface)
- (instancetype)initWithIOSurface:(IOSurfaceRef)surface label:(NSString *)label;
- (IOSurfaceRef)ioSurface;
@end

namespace {

constexpr const raw::shared::Kind &kKind = raw::shared::kThisKind;

// kIOSurfaceCreationProperties, which no header declares
NSDictionary *creationProperties(IOSurfaceRef s)
{
    id p = CFBridgingRelease(IOSurfaceCopyValue(s, CFSTR("CreationProperties")));
    return [p isKindOfClass:[NSDictionary class]] ? p : nil;
}

MTLSharedTextureHandle *handleOver(IOSurfaceRef s, NSString *label)
{
    static const bool made = [MTLSharedTextureHandle instancesRespondToSelector:@selector(initWithIOSurface:label:)];
    if (!made) {
        SWITCH_REFUSE("raw", "texture.shared.handle", "Metal's MTLSharedTextureHandle has no initWithIOSurface:label:");
        return nil;
    }
    MTLSharedTextureHandle *h = [[MTLSharedTextureHandle alloc] initWithIOSurface:s label:label];
    if (!h)
        SWITCH_REFUSE_DETAIL("raw", "texture.shared.device", IOSurfaceGetID(s),
                             "IOSurface %u: Metal found no device of its registry ID for its handle",
                             IOSurfaceGetID(s));
    return h;
}

const char *unsharedShape(MTLTextureDescriptor *d, const raw::FormatInfo *f)
{
    if (d.storageMode == MTLStorageModeMemoryless)
        return "memoryless";
    if (!rawShape(d))
        return "the raw path takes IOSurface textures 2D, of one level, sample and layer, unrotated, of a format it "
               "keeps";
    if (raw::blockCompressed(f))
        return "block-compressed, which the raw path lays out block-linear in memory of its own alone";
    if (raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::Planes)
        return "an extended-range format, which a surface holds in two planes";
    return nullptr;
}

id<MTLTexture> openShared(id<MTLDevice> device, MTLSharedTextureHandle *handle, const NSUInteger *resourceIndex)
{
    IOSurfaceRef s = [handle respondsToSelector:@selector(ioSurface)] ? [handle ioSurface] : nullptr;
    if (!s) {
        SWITCH_REFUSE("raw", "texture.shared.open", "newSharedTextureWithHandle: a handle with no IOSurface");
        return nil;
    }
    NSDictionary *p = creationProperties(s);
    MTLTextureDescriptor *d = p ? rmSharedTextureDescriptor(p, device.registryID, kKind) : nil;
    if (!d) {
        SWITCH_REFUSE_DETAIL("raw", "texture.shared.open", IOSurfaceGetID(s),
                             "newSharedTextureWithHandle: IOSurface %u holds no shared texture of registry ID 0x%llx",
                             IOSurfaceGetID(s), (unsigned long long)device.registryID);
        return nil;
    }
    if (resourceIndex && [d respondsToSelector:rmSharedSetter("resourceIndex")])
        [d setValue:@(*resourceIndex) forKey:@"resourceIndex"];
    id<MTLTexture> t = [device newTextureWithDescriptor:d iosurface:s plane:0];
    if ([(id)t isKindOfClass:[RMTexture class]])
        ((RMTexture *)t)->_shareable = RMShareable::Yes;
    t.label = handle.label;
    return t;
}

}  // namespace

// Its methods go onto the device class of a kind whose _MTLDevice has them (installSharedTextureMethods)
@interface RMSharedTextureAtIndex : NSObject
@end
@implementation RMSharedTextureAtIndex

- (id<MTLTexture>)newSharedTextureWithHandle:(MTLSharedTextureHandle *)handle withResourceIndex:(NSUInteger)index
{
    return openShared((id<MTLDevice>)self, handle, &index);
}

@end

void installSharedTextureMethods(Class cls)
{
    if (!kKind.resourceIndexOpen)
        return;
    const SEL sel = @selector(newSharedTextureWithHandle:withResourceIndex:);
    Method m = class_getInstanceMethod([RMSharedTextureAtIndex class], sel);
    class_addMethod(cls, sel, method_getImplementation(m), method_getTypeEncoding(m));
}

@interface RMDevice (SharedTextures)
@end
@implementation RMDevice (SharedTextures)

- (NSMutableDictionary *)copyIOSurfaceSharedTextureProperties:(MTLTextureDescriptor *)d
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    return unsharedShape(d, f) ? nil : rmSharedSurfaceProperties(d, f->bpp);
}

- (id<MTLTexture>)newSharedTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    NSMutableDictionary *p = [self copyIOSurfaceSharedTextureProperties:d];
    if (!p) {
        const char *why = unsharedShape(d, raw::formatInfo(uint32_t(d.pixelFormat)));
        SWITCH_REFUSE_DETAIL("raw", "texture.shared", d.pixelFormat << 8 | rawShapeTerm(d),
                             "newSharedTextureWithDescriptor: %lux%lu format %lu type %lu levels %lu samples %lu: %s",
                             (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                             (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount,
                             (unsigned long)d.sampleCount, why ? why : "an extent past the GPU's");
        return nil;
    }
    rmAddSharedTextureKeys(p, d, [(id<MTLDevice>)self registryID], kKind);
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
    if (!s) {
        SWITCH_REFUSE("raw", "texture.shared.surface", "IOSurfaceCreate refused a %lux%lu shared texture's surface",
                      (unsigned long)d.width, (unsigned long)d.height);
        return nil;
    }
    MTLSharedTextureHandle *h = handleOver(s, nil);
    CFRelease(s);
    return h ? [self newSharedTextureWithHandle:h] : nil;
}

- (id<MTLTexture>)newSharedTextureWithHandle:(MTLSharedTextureHandle *)handle
{
    return openShared((id<MTLDevice>)self, handle, nullptr);
}

@end

@interface RMTexture (SharedTextures)
@end
@implementation RMTexture (SharedTextures)

- (BOOL)isShareable
{
    RMShareable known = _shareable.load(std::memory_order_relaxed);
    if (known == RMShareable::Unasked) {
        NSDictionary *p = _kind == RMTextureSurface && _surface ? creationProperties(_surface) : nil;
        known = p[rmSharedString(raw::shared::kRegistryKey)] ? RMShareable::Yes : RMShareable::No;
        _shareable.store(known, std::memory_order_relaxed);
    }
    return known == RMShareable::Yes;
}

- (MTLSharedTextureHandle *)newSharedTextureHandle
{
    return [self isShareable] ? handleOver(_surface, self.label) : nil;
}

@end
