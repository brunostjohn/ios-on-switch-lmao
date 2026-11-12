// SPDX-License-Identifier: MIT
// stagecheck: a pack state's set 0 as the raw render encoder builds it, over the mock GPU. Every buffer slot of both stages
// takes a buffer, and every texture and sampler slot the record's handles name, at its stage, a texture and sampler of its
// own. Each handle word must name the header of what is bound at its entry's stage and slot; stagecheck.py then runs the
// state's programs over these words and over the words m2v's binding of the same slots makes.
// usage: stagecheck <pack.bin> <key hash, hex> <out.json>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "../runtime/raw_gpu.h"

namespace raw {
struct EncoderHarness {
    static const DescriptorWords &setWords(const RenderEncoder &e)
    {
        return e.descriptors_.last().words;
    }
};
}  // namespace raw

namespace {

using Slot = std::pair<uint32_t, uint32_t>;  // stage (1 vertex, 2 fragment), slot

bool handle(const raw::PackEntry &e)
{
    return e.kind == raw::EntryTexture || e.kind == raw::EntryTexture3d || e.kind == raw::EntrySamplerOnly;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: stagecheck <pack.bin> <key hash, hex> <out.json>\n");
        return 2;
    }
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        fprintf(stderr, "raw device: %s\n", err.c_str());
        return 1;
    }
    const uint64_t key = strtoull(argv[2], nullptr, 16);
    raw::Pipeline *pl = dev.pipeline(key);
    if (!pl) {
        fprintf(stderr, "%016llx: not in the pack\n", (unsigned long long)key);
        return 1;
    }
    const raw::PackPipeline &r = *pl->rec;
    raw::Buffer buffer;
    if (!dev.createBuffer(1 << 16, buffer))
        return 1;
    std::map<Slot, raw::Texture> textures;
    std::map<Slot, raw::Sampler> samplers;
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        if (!handle(e))
            continue;
        const Slot at{e.stage, e.slot};
        if (e.kind != raw::EntrySamplerOnly && !textures.count(at)) {
            raw::Texture &t = textures[at];
            const uint32_t size = 8 + 2 * e.slot + 64 * (e.stage - 1);
            const bool made = e.kind == raw::EntryTexture3d ? dev.createVolume(size, size, 4, 80, false, t)
                                                            : dev.createTexture(size, size, 80, t);
            if (!made) {
                fprintf(stderr, "texture %u:%u\n", e.stage, e.slot);
                return 1;
            }
        }
        if (e.samplerKind == raw::SamplerSlot && !samplers.count({e.stage, e.sampler})) {
            raw::SamplerDesc d;
            d.lodMin = float(16 * e.stage + e.sampler);
            if (!dev.createSampler(d, samplers[{e.stage, e.sampler}]))
                return 1;
        }
    }
    std::vector<raw::Texture> targets(9);
    raw::PassDesc pass;
    for (uint32_t j = 0; j < 8; j++) {
        if (!pl->colorFormats[j])
            continue;
        const bool made = pl->samples > 1
                              ? dev.createStoredMultisample(16, 16, pl->colorFormats[j], pl->samples, targets[j])
                              : dev.createTexture(16, 16, pl->colorFormats[j], targets[j], false, true);
        if (!made) {
            fprintf(stderr, "color %u: format %u at %u samples\n", j, pl->colorFormats[j], pl->samples);
            return 1;
        }
        pass.color[j].texture = &targets[j];
        pass.color[j].store = true;
        pass.count = j + 1;
    }
    if (pl->depthFormat) {
        if (!dev.createOwnedZeta(16, 16, pl->depthFormat, pl->samples, false, targets[8])) {
            fprintf(stderr, "depth: format %u at %u samples\n", pl->depthFormat, pl->samples);
            return 1;
        }
        pass.depthStencil.texture = &targets[8];
    }
    raw::CommandBuffer *cb = dev.begin();
    raw::RenderEncoder *enc = cb ? cb->render(pass) : nullptr;
    if (!enc) {
        fprintf(stderr, "render pass: %s\n", dev.channelError().c_str());
        return 1;
    }
    enc->setPipeline(pl);
    enc->setScissor(0, 0, 16, 16);
    for (int stage = 1; stage <= 2; stage++)
        for (uint32_t slot = 0; slot < 31; slot++)
            enc->setBuffer(stage, slot, &buffer, 0);
    for (auto &[at, t] : textures)
        enc->setTexture(int(at.first), at.second, &t);
    for (auto &[at, s] : samplers)
        enc->setSampler(int(at.first), at.second, &s);
    const char *why = nullptr;
    if (!enc->draw(3, 0, 3, 1, 0, &why)) {
        fprintf(stderr, "draw: %s\n", why ? why : "refused");
        return 1;
    }
    const raw::DescriptorWords &words = raw::EncoderHarness::setWords(*enc);
    unsigned wrong = 0, checked = 0;
    FILE *out = fopen(argv[3], "w");
    if (!out)
        return 1;
    fprintf(out, "{\"key\": \"%016llx\", \"entries\": [", (unsigned long long)key);
    for (uint32_t i = 0; i < r.entries; i++) {
        const raw::PackEntry &e = r.entry[i];
        uint32_t want = 0;
        bool sampled = false;
        if (handle(e)) {
            const auto t = textures.find({e.stage, e.slot});
            const uint32_t tic = t == textures.end() ? 0 : t->second.tic;
            const auto s = samplers.find({e.stage, e.sampler});
            want = tic | (e.samplerKind == raw::SamplerSlot && s != samplers.end() ? s->second.tsc << 20 : 0);
            sampled = true;
            const uint32_t got = words[e.offset / 4];
            const uint32_t mask = e.samplerKind == raw::SamplerConstexpr ? 0xFFFFFu : ~0u;
            ++checked;
            if ((got & mask) != want) {
                ++wrong;
                printf("WRONG %u: stage %u slot %u sampler %u at %u: %08x, bound %08x\n", i, e.stage, e.slot, e.sampler,
                       e.offset, got, want);
            }
        }
        fprintf(out, "%s[%u, %u, %u, %u, %u, %u, %u]", i ? ", " : "", e.offset, e.kind, e.stage, e.slot, e.samplerKind,
                e.sampler, sampled ? words[e.offset / 4] : 0);
    }
    fprintf(out, "], \"textures\": [");
    const char *sep = "";
    for (auto &[at, t] : textures) {
        fprintf(out, "%s[%u, %u, %u]", sep, at.first, at.second, t.tic);
        sep = ", ";
    }
    fprintf(out, "], \"samplers\": [");
    sep = "";
    for (auto &[at, smp] : samplers) {
        fprintf(out, "%s[%u, %u, %u]", sep, at.first, at.second, smp.tsc);
        sep = ", ";
    }
    fprintf(out, "], \"words\": [");
    for (uint32_t i = 0; i < words.size(); i++)
        fprintf(out, "%s%u", i ? ", " : "", words[i]);
    fprintf(out, "]}\n");
    fclose(out);
    enc->end();
    uint32_t sequence = 0;
    const bool committed = cb->commit(sequence) && dev.wait(sequence);
    printf("%016llx: %u handles, %u not what their stage and slot bind; %s\n", (unsigned long long)key, checked, wrong,
           committed ? "drawn" : dev.channelError().c_str());
    return wrong || !committed ? 1 : 0;
}
