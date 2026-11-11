// SPDX-License-Identifier: MIT
// The device's binary archives are m2v's (its twin states list them, its functions are added to them), opened by Metal's rules:
// m2v's own reader's, and an Apple GPU binary as an archive holding nothing for this GPU (raw_archive.h). What this GPU has
// compiled is the pack and the record cache, which every state's lookup reads whatever archive its descriptor lists.
// An archive file of Apple's carries its pack entry (raw_pack.h PackArchive): the states it declares for the identity's GPU,
// which answer a creation under MTLPipelineOptionFailOnBinaryArchiveMiss as Metal's archive would.
#import "RawMetalInternal.h"

#import <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>
#include <mutex>
#include "raw_archive.h"
#include "raw_device_answers.h"

static char kPackEntry;

static bool fileHead(NSURL *url, uint8_t (&head)[8], ssize_t &got)
{
    const int fd = url.isFileURL ? open(url.fileSystemRepresentation, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0)
        return false;
    got = read(fd, head, sizeof head);
    close(fd);
    return got > 0;
}

// Metal adds a device's own functions to its archives; the facade's descriptors name its functions, and m2v's archive takes
// only m2v's (NativeBridgeMetalFrontend.mm renderDescriptor), so the facade's archives add the twins its functions stand for
static Class gM2VArchive;

static BOOL addRenderTwins(id self, SEL cmd, MTLRenderPipelineDescriptor *d, NSError **error)
{
    const auto add =
        (BOOL (*)(id, SEL, MTLRenderPipelineDescriptor *, NSError **))class_getMethodImplementation(gM2VArchive, cmd);
    return add(self, cmd, twinDescriptor(d), error);
}

static BOOL addComputeTwins(id self, SEL cmd, MTLComputePipelineDescriptor *d, NSError **error)
{
    MTLComputePipelineDescriptor *twin = [d copy];
    twin.computeFunction = twinOf(d.computeFunction);
    const auto add =
        (BOOL (*)(id, SEL, MTLComputePipelineDescriptor *, NSError **))class_getMethodImplementation(gM2VArchive, cmd);
    return add(self, cmd, twin, error);
}

static Class facadeArchive(Class m2v)
{
    static Class made;
    static std::once_flag once;
    std::call_once(once, [m2v] {
        gM2VArchive = m2v;
        made = m2v;
        Class c = objc_allocateClassPair(m2v, "RMBinaryArchive", 0);
        if (!c)
            return;
        for (const auto &[selector, imp] :
             {std::pair{@selector(addRenderPipelineFunctionsWithDescriptor:error:), (IMP)addRenderTwins},
              std::pair{@selector(addComputePipelineFunctionsWithDescriptor:error:), (IMP)addComputeTwins}})
            class_addMethod(c, selector, imp, method_getTypeEncoding(class_getInstanceMethod(m2v, selector)));
        objc_registerClassPair(c);
        made = c;
    });
    return made;
}

@implementation RMDevice (Archives)

// RenderBox's load_library_archive logs a precondition failure where this is nil, and draws without the archive
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error
{
    uint8_t head[8];
    ssize_t got = 0;
    const bool read = d.url && fileHead(d.url, head, got);
    MTLBinaryArchiveDescriptor *opened = d;
    if (read && raw::appleGpuBinary(head, size_t(got))) {
        opened = [d copy];
        opened.url = nil;
    }
    id<MTLBinaryArchive> archive = [gRM->fallback newBinaryArchiveWithDescriptor:opened error:error];
    if (Class m2v = NSClassFromString(@"NBBinaryArchive"); archive && m2v && object_getClass(archive) == m2v)
        object_setClass(archive, facadeArchive(m2v));
    if (archive && read && !raw::m2vArchive(head, size_t(got))) {
        const std::string path = raw::inventoryPath(d.url.path.UTF8String ?: "");
        const raw::PackArchive *entry = gRM->raw->pack().archive(raw::Pack::fnv(path.data(), path.size()));
        objc_setAssociatedObject(archive, &kPackEntry, [NSValue valueWithPointer:entry], OBJC_ASSOCIATION_RETAIN);
    }
    return archive;
}

@end

RMArchiveAnswer rmArchiveAnswer(NSArray *archives, MTLPipelineOption options, uint64_t keyHash)
{
    if (!(options & MTLPipelineOptionFailOnBinaryArchiveMiss) || !archives.count)
        return RMArchiveAnswer::NotAsked;
    bool m2v = false;
    for (id archive in archives) {
        NSValue *entry = objc_getAssociatedObject(archive, &kPackEntry);
        if (!entry) {
            m2v = true;
            continue;
        }
        const auto *listed = static_cast<const raw::PackArchive *>(entry.pointerValue);
        if (listed && gRM->raw->pack().declares(*listed, keyHash))
            return RMArchiveAnswer::Declared;
    }
    return m2v ? RMArchiveAnswer::M2V : RMArchiveAnswer::Missed;
}

bool rmPackHoldsKernels(const std::string &stateKey)
{
    const raw::Pack &pack = gRM->raw->pack();
    const raw::PackArchive *held = pack.archive(raw::kPackHeldKernels);
    return held && pack.declares(*held, raw::Pack::fnv(stateKey.data(), stateKey.size()));
}

NSError *rmArchiveMissError(NSString *function, const char *pipeline)
{
    NSString *text = [NSString
        stringWithFormat:@"Unable to find function \"%@\" (%s pipeline) in binary archives.", function, pipeline];
    return [NSError errorWithDomain:@(raw::answers::kThisKind.driver)
                               code:3
                           userInfo:@{NSLocalizedDescriptionKey : text}];
}
