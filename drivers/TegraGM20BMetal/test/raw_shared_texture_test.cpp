// SPDX-License-Identifier: MIT
// raw/runtime/raw_shared_texture.h and RawMetalSharedTexture.h, every kind: a shared texture's IOSurface creation
// properties from its descriptor and the descriptor read back from them. The keys are those an M4 Max's Metal writes
// for the same descriptor (its newSharedTextureWithDescriptor:, macOS 27), its swizzle key that Metal's own.
#include "../raw/runtime/RawMetalSharedTexture.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kRegistry = 0x10000065b;

NSArray<NSString *> *metalKeys(const raw::shared::Kind &k)
{
    NSMutableArray *keys = [@[
        @"kMetalRegistryID", @"kMetalTextureType", @"kMetalTexturePixelFormat", @"kMetalTextureWidth",
        @"kMetalTextureHeight", @"kMetalTextureDepth", @"kMetalTextureMipmapLevelCount", @"kMetalTextureSampleCount",
        @"kMetalTextureArrayLength", @"kMetalTextureResourceOptions", @"kMetalTextureUsage",
        @"kMetalTextureSparseValue", @"kMetalTextureGPUOptimization", @"kMetalTextureSwizzleKey",
        @"kMetalTextureRotation", @"kMetalTextureFootprint", @"kMetalTextureResourceIndex", @"kMetalTextureCSCMatrix"
    ] mutableCopy];
    if (k.writeAccessPattern)
        [keys addObject:@"kMetalTextureWriteAccessPattern"];
    return [keys sortedArrayUsingSelector:@selector(compare:)];
}

MTLTextureDescriptor *descriptor()
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:100
                                                                                height:70
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    d.allowGPUOptimizedContents = NO;
    d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed,
                                              MTLTextureSwizzleAlpha);
    return d;
}

NSMutableDictionary *properties(MTLTextureDescriptor *d, const raw::shared::Kind &k)
{
    NSMutableDictionary *p = rmSharedSurfaceProperties(d, 4);
    rmAddSharedTextureKeys(p, d, kRegistry, k);
    return p;
}

uint64_t number(NSDictionary *p, id key)
{
    return [p[key] unsignedLongLongValue];
}

TEST(RawSharedTexture, SurfaceRowsAndRoomForTheBlockLinearLayout)
{
    raw::shared::SurfaceShape s;
    ASSERT_TRUE(raw::shared::surfaceShape(100, 70, 4, s));
    EXPECT_EQ(s.bytesPerRow, 512u);
    EXPECT_EQ(s.allocBytes, 57344u);  // NIL's 448-byte rows, 128 of them in 64-row blocks, over 70 rows of 512
    ASSERT_TRUE(raw::shared::surfaceShape(64, 48, 4, s));
    EXPECT_EQ(s.bytesPerRow, 256u);
    EXPECT_EQ(s.allocBytes, 16384u);
    ASSERT_TRUE(raw::shared::surfaceShape(10, 10, 12, s));  // a texel size NIL does not tile: the rows alone
    EXPECT_EQ(s.bytesPerRow, 128u);
    EXPECT_EQ(s.allocBytes, 1280u);
    EXPECT_FALSE(raw::shared::surfaceShape(0, 8, 4, s));
    EXPECT_FALSE(raw::shared::surfaceShape(16385, 8, 4, s));
    EXPECT_EQ(rmSharedSurfaceProperties(descriptor(), 0), nil);
}

TEST(RawSharedTexture, CreationPropertiesHoldTheDescriptorUnderMetalsKeys)
{
    for (const raw::shared::Kind &k : raw::shared::kKinds) {
        NSDictionary *p = properties(descriptor(), k);
        NSArray *keys =
            [[p.allKeys filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH 'kMetal'"]]
                sortedArrayUsingSelector:@selector(compare:)];
        EXPECT_TRUE([keys isEqualToArray:metalKeys(k)]) << k.kind << ": " << keys.description.UTF8String;
        EXPECT_EQ(number(p, @"kMetalRegistryID"), kRegistry);
        EXPECT_EQ(number(p, @"kMetalTextureType"), MTLTextureType2D);
        EXPECT_EQ(number(p, @"kMetalTexturePixelFormat"), MTLPixelFormatRGBA8Unorm);
        EXPECT_EQ(number(p, @"kMetalTextureWidth"), 100u);
        EXPECT_EQ(number(p, @"kMetalTextureHeight"), 70u);
        EXPECT_EQ(number(p, @"kMetalTextureDepth"), 1u);
        EXPECT_EQ(number(p, @"kMetalTextureMipmapLevelCount"), 1u);
        EXPECT_EQ(number(p, @"kMetalTextureSampleCount"), 1u);
        EXPECT_EQ(number(p, @"kMetalTextureArrayLength"), 1u);
        EXPECT_EQ(number(p, @"kMetalTextureResourceOptions"), MTLResourceStorageModePrivate);
        EXPECT_EQ(number(p, @"kMetalTextureUsage"), MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        EXPECT_EQ(number(p, @"kMetalTextureSwizzleKey"), 0x05020304u);
        EXPECT_EQ(CFGetTypeID((__bridge CFTypeRef)p[@"kMetalTextureGPUOptimization"]), CFBooleanGetTypeID());
        EXPECT_EQ(number(p, @"kMetalTextureGPUOptimization"), 0u);
        EXPECT_EQ(number(p, @"IOSurfaceProtectionOptions"), 0u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceAllocSize), 57344u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceBytesPerRow), 512u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceBytesPerElement), 4u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceWidth), 100u);
        EXPECT_EQ(number(p, (__bridge NSString *)kIOSurfaceHeight), 70u);
    }
}

TEST(RawSharedTexture, TheDescriptorComesBackFromTheProperties)
{
    for (const raw::shared::Kind &k : raw::shared::kKinds) {
        MTLTextureDescriptor *d = descriptor();
        const bool sparse = [d respondsToSelector:rmSharedSetter("sparseSurfaceDefaultValue")];
        if (sparse)
            [d setValue:@1 forKey:@"sparseSurfaceDefaultValue"];
        MTLTextureDescriptor *back = rmSharedTextureDescriptor(properties(d, k), kRegistry, k);
        ASSERT_NE(back, nil) << k.kind;
        EXPECT_EQ(back.textureType, d.textureType);
        EXPECT_EQ(back.pixelFormat, d.pixelFormat);
        EXPECT_EQ(back.width, d.width);
        EXPECT_EQ(back.height, d.height);
        EXPECT_EQ(back.depth, d.depth);
        EXPECT_EQ(back.mipmapLevelCount, d.mipmapLevelCount);
        EXPECT_EQ(back.sampleCount, d.sampleCount);
        EXPECT_EQ(back.arrayLength, d.arrayLength);
        EXPECT_EQ(back.resourceOptions, d.resourceOptions);
        EXPECT_EQ(back.storageMode, MTLStorageModePrivate);
        EXPECT_EQ(back.cpuCacheMode, d.cpuCacheMode);
        EXPECT_EQ(back.hazardTrackingMode, d.hazardTrackingMode);
        EXPECT_EQ(back.usage, d.usage);
        EXPECT_EQ(back.allowGPUOptimizedContents, NO);
        EXPECT_EQ(rmSwizzleKey(back.swizzle), rmSwizzleKey(d.swizzle));
        if (sparse)
            EXPECT_EQ([[back valueForKey:@"sparseSurfaceDefaultValue"] unsignedIntegerValue], 1u);
    }
}

TEST(RawSharedTexture, AnotherDevicesOrAnIncompleteSurfaceOpensNothing)
{
    for (const raw::shared::Kind &k : raw::shared::kKinds) {
        NSDictionary *p = properties(descriptor(), k);
        EXPECT_EQ(rmSharedTextureDescriptor(p, kRegistry + 1, k), nil) << k.kind;
        EXPECT_EQ(rmSharedTextureDescriptor(p, 0, k), nil) << k.kind;
        for (NSString *key in metalKeys(k)) {
            NSMutableDictionary *missing = [p mutableCopy];
            [missing removeObjectForKey:key];
            EXPECT_EQ(rmSharedTextureDescriptor(missing, kRegistry, k), nil) << k.kind << " without " << key.UTF8String;
        }
        NSMutableDictionary *wrong = [p mutableCopy];
        wrong[@"kMetalTextureWidth"] = @"100";
        EXPECT_EQ(rmSharedTextureDescriptor(wrong, kRegistry, k), nil) << k.kind;
    }
    const raw::shared::Kind &k17 = raw::shared::kKinds[0], &k27 = raw::shared::kKinds[1];
    ASSERT_FALSE(k17.writeAccessPattern);
    ASSERT_TRUE(k27.writeAccessPattern);
    EXPECT_EQ(rmSharedTextureDescriptor(properties(descriptor(), k17), kRegistry, k27), nil);
    EXPECT_NE(rmSharedTextureDescriptor(properties(descriptor(), k27), kRegistry, k17), nil);
}

// _MTLDevice's own methods in each kind's ABI table (drivers/NativeMetal/abi/kinds)
TEST(RawSharedTexture, AResourceIndexOpensWhereTheKindsMetalHasIt)
{
    for (const raw::shared::Kind &k : raw::shared::kKinds) {
        NSString *path =
            [NSString stringWithFormat:@"drivers/NativeMetal/abi/kinds/%.*s.json", int(k.kind.size()), k.kind.data()];
        NSData *data = [NSData dataWithContentsOfFile:path];
        ASSERT_NE(data, nil) << path.UTF8String;
        NSDictionary *table = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        NSDictionary *methods = table[@"MTLDevice"][@"methods"];
        EXPECT_NE(methods[@"newSharedTextureWithHandle:"], nil) << k.kind;
        EXPECT_EQ(methods[@"newSharedTextureWithHandle:withResourceIndex:"] != nil, k.resourceIndexOpen) << k.kind;
    }
}

}  // namespace
