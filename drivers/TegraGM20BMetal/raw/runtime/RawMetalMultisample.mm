// SPDX-License-Identifier: MIT
#import "RawMetalMultisample.h"

RMStoredSamples storedMultisampleKind(MTLTextureDescriptor *d)
{
    if (d.textureType != MTLTextureType2DMultisample)
        return RMStoredSamples::None;
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    if ((d.sampleCount != 2 && d.sampleCount != 4) || d.mipmapLevelCount > 1 || d.arrayLength > 1 || d.depth > 1 ||
        turned || d.storageMode != MTLStorageModePrivate)
        return RMStoredSamples::None;
    const uint32_t format = uint32_t(d.pixelFormat);
    if (raw::zetaFormatIndex(format) < raw::kZetaFormats)
        return (d.usage & ~MTLTextureUsageShaderRead) == MTLTextureUsageRenderTarget ? RMStoredSamples::Zeta
                                                                                     : RMStoredSamples::None;
    const raw::FormatInfo *f = raw::formatInfo(format);
    return f && f->rt && !raw::blockCompressed(f) && raw::xrForm(format) == raw::XrForm::None ? RMStoredSamples::Colour
                                                                                              : RMStoredSamples::None;
}

bool makeStoredMultisample(MTLTextureDescriptor *d, RMStoredSamples kind, raw::Texture &t)
{
    const uint32_t width = uint32_t(d.width), height = uint32_t(d.height), format = uint32_t(d.pixelFormat),
                   samples = uint32_t(d.sampleCount);
    const bool zeta = kind == RMStoredSamples::Zeta;
    const bool made =
        zeta ? gRM->raw->createOwnedZeta(width, height, format, samples, (d.usage & MTLTextureUsageShaderRead) != 0, t)
             : gRM->raw->createStoredMultisample(width, height, format, samples, t);
    static std::atomic<bool> said[2];
    if (made && !said[zeta].exchange(true))
        os_log(rawLog(), "raw: stored %ux multisampled %{public}s on (first %ux%u format %u: %llu bytes of %{public}s)",
               samples, zeta ? "depth/stencil" : "colour", width, height, format,
               (unsigned long long)(zeta ? t.storage.bytes : t.span.bytes),
               zeta ? "zeta memory, no m2v twin" : "block-linear memory, m2v's twin NVK's image over it");
    return made;
}

bool passSamplePositions(MTLRenderPassDescriptor *pass, NSUInteger count, uint32_t samples, raw::SamplePositions &out)
{
    const auto read = [pass](float *xy, uint32_t n) {
        MTLSamplePosition at[8];
        [pass getSamplePositions:at count:n];
        for (uint32_t i = 0; i < n; i++) {
            xy[2 * i] = at[i].x;
            xy[2 * i + 1] = at[i].y;
        }
    };
    if (!raw::readSamplePositions(uint32_t(std::min<NSUInteger>(count, 9)), samples, read, out))
        return false;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        os_log(rawLog(),
               "raw: a pass's own sample positions on (first: %u of %u samples, x | y << 4 %02x %02x %02x %02x)",
               unsigned(out.count), samples, out.xy[0], out.xy[1], out.xy[2], out.xy[3]);
    return true;
}

static bool ownedZeta(void *object)
{
    id o = (__bridge id)object;
    if (![o isKindOfClass:[RMTexture class]])
        return false;
    const RMTexture *t = (RMTexture *)o;
    return t->_rawOk && t->_raw.zeta && (t->_raw.samples > 1 || t->_raw.arena);
}

// Whether every pass of the range that attaches `t` neither loads nor stores it: m2v's own texture of its descriptor then
// stands in for it, as nothing in the raw path's memory is read or kept
static bool transientIn(const RMCommandPlan &plan, size_t from, size_t to, void *t)
{
    for (size_t at = from; at < to; at++) {
        const Cmd c = plan.legacy(at);
        if (c.op != OpRender)
            continue;
        MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)c.obj;
        for (MTLRenderPassAttachmentDescriptor *a : {(MTLRenderPassAttachmentDescriptor *)pass.depthAttachment,
                                                     (MTLRenderPassAttachmentDescriptor *)pass.stencilAttachment})
            if ((__bridge void *)a.texture == t &&
                (a.loadAction == MTLLoadActionLoad || a.storeAction != MTLStoreActionDontCare))
                return false;
    }
    return true;
}

RMTexture *ownedZetaIn(RMCommandBuffer *cb, size_t from, size_t to)
{
    void *found = nullptr;
    auto check = [&](void *object) {
        if (!found && object && ownedZeta(object))
            found = object;
    };
    const RMCommandPlan &plan = cb->_plan;
    plan.forEachUse(from, to, [&](void *object, uint8_t access) {
        if ((access & ~raw::ResourceUse::Write) || !transientIn(plan, from, to, object))
            check(object);
    });
    auto bound = [&](uint32_t stage, uint32_t index) {
        if (index < plan.textures[stage].size())
            for (void *object : plan.textures[stage][index].objects)
                check(object);
    };
    for (size_t at = from; at < to && !found; at++) {
        const Op op = plan.operation(at).op;
        if (isPlanDraw(op)) {
            const raw::RenderStateRef &state = plan.renderStates[plan.draw(at).state];
            bound(0, state.textures[0]);
            bound(1, state.textures[1]);
        } else if (op == OpDispatch) {
            bound(2, plan.kernelStates[plan.dispatch(at).state].textures);
        }
    }
    return (__bridge RMTexture *)found;
}

void failOwnedZeta(RMCommandBuffer *cb, RMTexture *t)
{
    SWITCH_REFUSE_DETAIL(
        "raw", "replay.zeta", t->_descriptor.pixelFormat,
        "a command buffer range failed before m2v encoded it: it uses a %ux%u depth/stencil texture of "
        "%u samples whose memory m2v has no texture of",
        t->_raw.width, t->_raw.height, unsigned(t->_raw.samples));
    NSString *text = [NSString
        stringWithFormat:@"The depth/stencil texture %s (%ux%u, pixel format %lu) has no compatibility texture",
                         [t label].UTF8String ?: "-", t->_raw.width, t->_raw.height,
                         (unsigned long)t->_descriptor.pixelFormat];
    NSError *error = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                         code:MTLCommandBufferErrorInternal
                                     userInfo:@{NSLocalizedDescriptionKey : text}];
    std::lock_guard<std::mutex> l(cb->_lock);
    if (!cb->_segError)
        cb->_segError = error;
}
