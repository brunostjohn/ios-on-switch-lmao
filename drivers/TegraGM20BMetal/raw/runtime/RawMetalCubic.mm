// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_fragment_tests.h"

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
    SWITCH_REFUSE_DETAIL("raw", "state.variant_unpacked", raw::Pack::fnv(key.data(), key.size()),
                         "a packed state's variant the pack lacks: its draws are refused: %s", key.c_str());
    return nullptr;
}

static raw::Variant<raw::Pipeline> *renderVariant(RMPipeline *p, const raw::CubicBinds &binds, uint8_t tests)
{
    const std::string suffix = binds.empty() ? std::string() : binds.key();
    const std::string name = suffix + (tests ? " | tests=" + std::to_string(tests) : std::string());
    std::shared_ptr<raw::Variant<raw::Pipeline>> variant;
    {
        std::lock_guard<std::mutex> lock(p->_variantLock);
        if (const auto found = p->_variants.find(name); found != p->_variants.end())
            return found->second.get();
        variant = std::make_shared<raw::Variant<raw::Pipeline>>();
        p->_variants.emplace(name, variant);
    }
    if (p->_packed) {
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
    request.resolveDescriptor = true;
    raw::Pipeline *base = p->_raw;
    raw::Completion *completion = gRM->completion.get();
    if (!request.descriptor || !completion) {
        variant->publish(binds.empty() ? base : nullptr);
        return variant.get();
    }
    raw::Completion::Ask fallback = request;
    fallback.key = p->_key + suffix;
    fallback.fragmentTests = 0;
    completion->ask(std::move(request), [variant, base, fallback, tests, completion](raw::Pipeline *made) {
        if (!made && tests && !fallback.cubic.empty()) {
            completion->ask(fallback, [variant](raw::Pipeline *cubic) {
                variant->publish(cubic);
            });
        } else {
            variant->publish(made ? made : fallback.cubic.empty() ? base : nullptr);
        }
    });
    return variant.get();
}

@implementation RMRenderEncoder (Cubic)

- (const raw::Pipeline *)drawnRecord
{
    raw::Pipeline *base = _pipe->_raw;
    const raw::CubicBinds binds = _cubicMask ? cubicBinds(*base, _samp, _tex) : raw::CubicBinds();
    const uint8_t tests =
        base->fetchBits && base->depthFormat && !base->storageBits ? uint8_t(raw::fragmentTests(_ds, _zeta)) : 0;
    auto *variant = (!binds.empty() || tests) ? renderVariant(_pipe, binds, tests) : nullptr;
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
