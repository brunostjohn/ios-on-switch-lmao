// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_fragment_tests.h"
#include "raw_written_textures.h"
#include "raw_xr_pass.h"

raw::CubicBinds cubicBinds(const raw::Pipeline &pl, RMSampler *const samplers[16], RMTexture *const textures[16])
{
    raw::CubicBinds b;
    const raw::PackPipeline &r = *pl.rec;
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        if ((e.kind != raw::EntryTexture && e.kind != raw::EntrySamplerOnly) || e.samplerKind != raw::SamplerSlot ||
            e.sampler >= 16)
            continue;
        RMSampler *s = samplers[e.sampler];
        if (!s || !s->_raw.desc.cubic)
            continue;
        const raw::SamplerDesc &d = s->_raw.desc;
        if (std::none_of(b.samplers.begin(), b.samplers.end(), [&](const auto &c) {
                return c.slot == e.sampler;
            }))
            b.samplers.push_back(
                {e.sampler, d.sAddress, d.tAddress, d.rAddress, uint8_t(s->_descriptor.borderColor & 3), !d.pixel});
        RMTexture *t = e.kind == raw::EntryTexture && e.slot < 16 ? textures[e.slot] : nil;
        if (t && std::none_of(b.textures.begin(), b.textures.end(), [&](const auto &c) {
                return c.slot == e.slot;
            }))
            b.textures.push_back({e.slot, uint32_t(t->_descriptor.pixelFormat)});
    }
    std::sort(b.samplers.begin(), b.samplers.end(), [](const auto &x, const auto &y) {
        return x.slot < y.slot;
    });
    std::sort(b.textures.begin(), b.textures.end(), [](const auto &x, const auto &y) {
        return x.slot < y.slot;
    });
    return b;
}

void packedVariants(RMPipeline *p, uint64_t keyHash)
{
    std::lock_guard<std::mutex> lock(p->_variantLock);
    for (const auto &[name, pl] : gRM->raw->variantPipelines(keyHash, p->_caTransform)) {
        auto variant = std::make_shared<raw::Variant<raw::Pipeline>>();
        variant->publish(pl);
        p->_variants.emplace(name, std::move(variant));
    }
}

// A packed state's variant the pack does not hold: a draw that needs one of its cubic samplers' binds is refused (neither its
// own program nor another binds' filters them as Metal does), and one of a test class the pack builder could not make draws
// the record without it (the state's own, or its cubic variant's), as a refused device compile left it, and says so
static raw::Pipeline *unpackedVariant(RMPipeline *p, const std::string &suffix, const std::string &name)
{
    const std::string key = p->_key + name;
    raw::Pipeline *untested = p->_raw;
    if (!suffix.empty()) {
        std::lock_guard<std::mutex> lock(p->_variantLock);
        const auto found = p->_variants.find(suffix);
        untested = suffix != name && found != p->_variants.end() ? found->second->value() : nullptr;
    }
    if (untested) {
        SWITCH_REFUSE_DETAIL("raw", "state.tests_unpacked", raw::Pack::fnv(key.data(), key.size()),
                             "a packed state's test class the pack builder could not make: drawn without it: %s",
                             key.c_str());
        return untested;
    }
    // The variant's name and its state's hash come first, as the line is cut at SWITCH_REFUSAL_TEXT:
    // tools/gpu/logharvest.py finds the state by the hash and harvests the variant
    SWITCH_REFUSE_DETAIL("raw", "state.variant_unpacked", raw::Pack::fnv(key.data(), key.size()),
                         "a packed state's variant the pack lacks: its draws are refused:%s of %016llx %s",
                         name.c_str(), (unsigned long long)raw::Pack::fnv(p->_key.data(), p->_key.size()),
                         p->_key.c_str());
    return nullptr;
}

// The state's variant for a draw's cubic binds, test class, pass of two-plane colours, two-plane textures sampled at
// `xrSlots` (raw_xr_pass.h) and textures written in other formats than its record's (raw_written_textures.h), named in that
// order. A packed state's sampled or written variant the pack lacks is made here as an unpacked state's are: until it is, its
// draws sample those textures' stand-ins, and its draws that write textures wait for it (no other record stores their
// formats).
static raw::Variant<raw::Pipeline> *renderVariant(RMPipeline *p, const raw::CubicBinds &binds, uint8_t tests,
                                                  bool planes, uint16_t xrSlots,
                                                  const std::vector<raw::WrittenTexture> &written)
{
    const std::string cubic = binds.empty() ? std::string() : binds.key();
    const std::string forms = (planes ? std::string(raw::kXrPlanesVariant) : std::string()) +
                              (xrSlots ? raw::xrSampledVariant(xrSlots) : std::string()) + raw::writtenVariant(written);
    const std::string suffix = cubic + forms;
    const std::string name = cubic + (tests ? " | tests=" + std::to_string(tests) : std::string()) + forms;
    std::shared_ptr<raw::Variant<raw::Pipeline>> variant;
    {
        std::lock_guard<std::mutex> lock(p->_variantLock);
        if (const auto found = p->_variants.find(name); found != p->_variants.end())
            return found->second.get();
        variant = std::make_shared<raw::Variant<raw::Pipeline>>();
        p->_variants.emplace(name, variant);
    }
    if (p->_packed && !xrSlots && written.empty()) {
        variant->publish(unpackedVariant(p, suffix, name));
        return variant.get();
    }
    raw::Completion::Ask request;
    request.descriptor = p->_madeFrom;
    request.key = p->_key + name;
    request.caTransform = p->_caTransform;
    request.own = !p->_packable;
    request.cubic = binds;
    request.fragmentTests = tests;
    request.twoPlane = planes;
    request.xrSlots = xrSlots;
    request.written = written;
    request.resolveDescriptor = true;
    // the state's own record draws a test class's draws it could not be made for, never a cubic, two-plane or written
    // variant's
    raw::Pipeline *base = binds.empty() && !planes && !xrSlots && written.empty() ? p->_raw : nullptr;
    raw::Completion *completion = gRM->completion.get();
    if (!request.descriptor || !completion) {
        variant->publish(base);
        return variant.get();
    }
    raw::Completion::Ask fallback = request;
    fallback.key = p->_key + suffix;
    fallback.fragmentTests = 0;
    completion->ask(std::move(request), [variant, base, fallback, tests, completion](raw::Pipeline *made) {
        if (!made && tests && !base) {
            completion->ask(fallback, [variant](raw::Pipeline *untested) {
                variant->publish(untested);
            });
        } else {
            variant->publish(made ? made : base);
        }
    });
    return variant.get();
}

// The textures a draw of `pl` writes in other formats than its record's programs store (raw_written_textures.h)
static std::vector<raw::WrittenTexture> reboundTextures(const raw::Pipeline &pl, RMTexture *const textures[16])
{
    if (!pl.writtenSlots)
        return {};
    return raw::reboundWritten(*pl.rec, [&](uint32_t slot) {
        return textures[slot] ? uint32_t(textures[slot]->_descriptor.pixelFormat) : 0u;
    });
}

// The fragment slots a draw of `pl` samples a two-plane RGB10A8_2P_XR10 texture at (raw_xr_pass.h)
static uint16_t xrSampledSlots(const raw::Pipeline &pl, RMTexture *const textures[16])
{
    uint16_t slots = 0;
    const raw::PackPipeline &r = *pl.rec;
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        RMTexture *t = e.kind == raw::EntryTexture && e.slot < 16 ? textures[e.slot] : nil;
        if (t && t->_raw.xrPlanes && !t->_raw.memoryless)
            slots |= uint16_t(1u << e.slot);
    }
    return slots;
}

@implementation RMRenderEncoder (Cubic)

- (const raw::Pipeline *)drawnRecord
{
    raw::Pipeline *base = _pipe->_raw;
    const raw::CubicBinds binds = _cubicMask ? cubicBinds(*base, _samp, _tex) : raw::CubicBinds();
    const uint8_t tests =
        base->fetchBits && base->depthFormat && !base->storageBits ? uint8_t(raw::fragmentTests(_ds, _zeta)) : 0;
    const std::vector<raw::WrittenTexture> written = reboundTextures(*base, _tex);
    auto *variant = (!binds.empty() || tests || _xrPlanes || !written.empty())
                        ? renderVariant(_pipe, binds, tests, _xrPlanes, 0, written)
                        : nullptr;
    if (const uint16_t xrSlots = xrSampledSlots(*base, _tex)) {
        auto *sampled = renderVariant(_pipe, binds, tests, _xrPlanes, xrSlots, written);
        if (sampled->state() == raw::Variant<raw::Pipeline>::State::Ready)
            variant = sampled;
        else
            for (uint32_t slot = 0; slot < 16; slot++)
                if (xrSlots >> slot & 1)
                    _cb->_xrStandIns.insert(_tex[slot]);
    }
    if (base != _drawn || variant != _drawnVariant) {
        Cmd state = cmd(OpPipeline);
        state.obj = (__bridge void *)_pipe;
        state.obj2 = variant;
        [_cb record:state];
        _drawn = base;
        _drawnVariant = variant;
    }
    return base;
}

@end
