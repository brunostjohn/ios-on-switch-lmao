// SPDX-License-Identifier: MIT
// A process that draws only states the pack holds never opens its m2v device (RawM2V); each thing only m2v does opens it
// once, at its first use, naming it. Each scenario is a process of its own: one raw device and one m2v device a process.
#include "raw_facade_harness.h"
#include "MetalABI.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <set>

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm, kUnpacked = MTLPixelFormatRGBA8Unorm;

void note(const char *scenario, const std::string &line)
{
    std::ofstream(dir() + "/" + scenario + ".txt", std::ios::app) << line << "\n";
}

void show(const char *scenario)
{
    std::ifstream in(dir() + "/" + scenario + ".txt");
    for (std::string line; std::getline(in, line);)
        printf("%s: %s\n", scenario, line.c_str());
}

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Opened {
    id<MTLDevice> device;
    id<MTLLibrary> library;
};

Opened openWithLibrary(const std::string &pack, Class methods = [HarnessM2VMethods class])
{
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(pack, methods);
    if (!device)
        return {};
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    return {device, library};
}

void expectOpenedFor(id<MTLDevice> device, const char *need)
{
    NSDictionary *m2v = m2vRecord(device);
    EXPECT_EQ(gOpens, 1);
    EXPECT_TRUE([m2v[@"opened"] boolValue]);
    EXPECT_EQ([m2v[@"opens"] intValue], 1);
    EXPECT_STREQ([m2v[@"opened_for"] UTF8String], need) << text(m2v);
}

void packedDraw()
{
    const uint64_t atStart = footprint();
    const std::string pack = writePack(packKey(kPacked));
    const auto [device, library] = openWithLibrary(pack);
    ASSERT_TRUE(device != nil && library != nil);
    const uint64_t atOpen = footprint();
    MTLRenderPipelineDescriptor *d = descriptor(library, kPacked);
    EXPECT_EQ(std::string(raw_metal_pipeline_key(d).UTF8String), packKey(kPacked));
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    id<MTLCommandBuffer> cb = draw(device, state, kPacked);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    const uint64_t atDraw = footprint();

    NSDictionary *drivers = counters(device);
    EXPECT_EQ([drivers[@"raw"] intValue], 1) << text(drivers);
    EXPECT_EQ([drivers[@"m2v"] intValue], 0) << text(drivers);
    // every m2v object is made on the m2v device, and every way to it is a need: none asked, none opened, no twin
    NSDictionary *m2v = m2vRecord(device);
    EXPECT_EQ(gOpens, 0);
    EXPECT_FALSE([m2v[@"opened"] boolValue]);
    EXPECT_EQ([m2v[@"needs"] count], 0u) << text(m2v);

    // both programs run in place: bound imports of the pack file's own pages, holding what the file holds there
    NSDictionary *held = memory(device);
    EXPECT_EQ([held[@"heap"] unsignedLongLongValue], 0u) << text(held);
    EXPECT_GE([held[@"heap_in_place"] unsignedLongLongValue], 2 * kSlot) << text(held);
    const std::vector<FileImport> imports = boundFileImports(pack);
    ASSERT_FALSE(imports.empty());
    for (uint32_t k = 0; k < 2; k++) {
        const uint64_t at = kHeap + k * kSlot;
        const FileImport *in = nullptr;
        for (const FileImport &i : imports)
            if (at >= i.offset && at + kSlot <= i.offset + i.bytes)
                in = &i;
        ASSERT_NE(in, nullptr) << "program slot " << k << " is in no bound import of the pack";
        const std::vector<uint8_t> file = fileBytes(pack, at, kSlot);
        EXPECT_EQ(memcmp(reinterpret_cast<const void *>(in->address + (at - in->offset)), file.data(), kSlot), 0);
        EXPECT_EQ(file, slot(k));
    }

    char line[256];
    snprintf(line, sizeof line,
             "phys_footprint start %llu KB, raw device open %+lld KB, packed draw %+lld KB; currentAllocatedSize %llu KB; "
             "%s",
             (unsigned long long)(atStart >> 10), (long long)(int64_t(atOpen) - int64_t(atStart)) / 1024,
             (long long)(int64_t(atDraw) - int64_t(atOpen)) / 1024,
             (unsigned long long)(device.currentAllocatedSize >> 10),
             [[held.description stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String]);
    note("packed_draw", line);
}

void unpackedState()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil && library != nil);
    NSError *error = nil;
    (void)[device newRenderPipelineStateWithDescriptor:descriptor(library, kUnpacked) error:&error];
    expectOpenedFor(device, "a render state's twin");
}

void reflection()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil && library != nil);
    MTLAutoreleasedRenderPipelineReflection reflected = nil;
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked)
                                                                             options:MTLPipelineOptionArgumentInfo
                                                                          reflection:&reflected
                                                                               error:&error];
    EXPECT_TRUE(state != nil) << text(error);
    expectOpenedFor(device, "a render state's twin with its reflection");
}

void foreignSampler()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
    s.compareFunction = MTLCompareFunctionLess;
    (void)[device newSamplerStateWithDescriptor:s];
    expectOpenedFor(device, "a sampler's twin");
}

// A method of m2v's device class the raw device has none of answers respondsToSelector: at once, opens the m2v device at
// its first call and runs m2v's method on the raw device, as the subclass of m2v's class did
void inheritedMethod()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    EXPECT_TRUE([(id)device respondsToSelector:@selector(harnessSelfRegistry)]);
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([(HarnessM2VMethods *)device harnessSelfRegistry], kRegistryID);
    expectOpenedFor(device, "m2v's -[MTLDevice harnessSelfRegistry]");
    EXPECT_EQ([(HarnessM2VMethods *)device harnessSelfRegistry], kRegistryID);
    EXPECT_EQ(gOpens, 1);
}

// The device is an _MTLDevice of its own; its identity and every limits field m2v's device answered are its own answers
void ownDevice()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    Class cls = object_getClass(device), base = class_getSuperclass(cls);
    EXPECT_EQ(base, NSClassFromString(@"_MTLDevice") ?: [NSObject class]);
    EXPECT_TRUE([(id)device conformsToProtocol:@protocol(MTLDevice)]);
    EXPECT_EQ(device.registryID, kRegistryID);
    // fields no driver answers (m2v's device class had no method for them either): the base class's own value
    const std::set<std::string> unanswered = {
        "linearTextureArrayAlignmentBytes", "linearTextureArrayAlignmentSlice", "maxTileInlineDataSize",
        "minTilePixels", "maxLineWidth", "maxPointSize", "padmaxBufferLength", "maxTessellationFactor",
        "maxIOCommandsInFlight", "maxAccelerationStructureLevels"};
    for (const LimitField &field : kLimitFields) {
        const SEL sel = sel_registerName(field.selector);
        Method own = class_getInstanceMethod(cls, sel), inherited = class_getInstanceMethod(base, sel);
        const bool answered = own && (!inherited || method_getImplementation(own) != method_getImplementation(inherited));
        EXPECT_EQ(answered, !unanswered.count(field.selector)) << field.selector;
    }
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
}

}  // namespace

TEST(RawFacadeLazy, PackedDrawOpensNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(packedDraw);
    show("packed_draw");
}

TEST(RawFacadeLazy, UnpackedStateOpensM2VForItsTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(unpackedState);
}

TEST(RawFacadeLazy, ReflectionOpensM2VForItsTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(reflection);
}

TEST(RawFacadeLazy, ForeignSamplerOpensM2VForItsTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(foreignSampler);
}

TEST(RawFacadeLazy, InheritedMethodOpensM2VAtItsFirstCall)
{
    switch_ios::test::ExpectPassesInFreshProcess(inheritedMethod);
}

TEST(RawFacadeLazy, DeviceIsItsOwn)
{
    switch_ios::test::ExpectPassesInFreshProcess(ownDevice);
}
