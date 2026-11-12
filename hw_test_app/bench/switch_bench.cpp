#include "hw_test_app/bench/switch_bench.h"
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_compute.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_gpu.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_probe_ops.h"
#include "hw_test_app/bench/bench_known.h"
#include "hw_test_app/bench/bench_records.h"

namespace {

constexpr const char *kPack = "/System/Library/Extensions/TegraGM20BMetal.raw/pack.bin";
constexpr const char kPlugin[] = "/TegraGM20BMetal.bundle/";
constexpr uint32_t kTriangleStrip = 4, kBgra8 = 80;
constexpr uint64_t kKeyBase = 0xbe7c000000000000ull;
// Placed programs stay in the device's shader heap for its life: a long session starts a fresh device now and then
constexpr uint64_t kRecordsPerDevice = 4096;

// placeCompute caches a placed kernel by its record's address for the device's life, so a record's address must never
// name another kernel: the records outlive every device the bench opens
std::deque<raw::ComputeRecord> &kernelRecords()
{
    static std::deque<raw::ComputeRecord> records;
    return records;
}

// The pack beside the Metal plugin this process loaded, where the plugin itself finds it (TegraGM20BMetalPlugin.mm
// rawPackRefusal); fetchform's path when no plugin is loaded
std::string packPath()
{
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        if (const char *at = name ? strstr(name, kPlugin) : nullptr)
            return std::string(name, at) + "/TegraGM20BMetal.raw/pack.bin";
    }
    return kPack;
}

uint64_t nanoseconds(uint64_t ticks)
{
    static mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t b;
        mach_timebase_info(&b);
        return b;
    }();
    return ticks * base.numer / base.denom;
}

void fail(switch_bench_result &r, uint32_t status, const std::string &why)
{
    r.status = status;
    snprintf(r.why, sizeof r.why, "%s", why.c_str());
}

}  // namespace

struct switch_bench {
    std::unique_ptr<raw::Device> device;
    const raw::PackPipeline *fragment = nullptr;
    const raw::PackKernel *kernel = nullptr;
    raw::Texture image;
    raw::Sampler sampler;
    std::string pack;
    bool imageMade = false, samplerMade = false;
    uint64_t records = 0;
    std::string error;

    bool open()
    {
        auto opened = std::make_unique<raw::Device>();
        pack = packPath();
        if (!opened->open(pack.c_str(), 0, error, raw::Device::Role::Client)) {
            error = pack + ": " + error;
            return false;
        }
        device = std::move(opened);
        fragment = bench::fragmentTemplate(device->pack());
        kernel = bench::kernelTemplate(device->pack());
        raw::SamplerDesc nearest;
        imageMade = device->createTexture(64, 64, kBgra8, image, true);
        samplerMade = imageMade && device->createSampler(nearest, sampler);
        if (!samplerMade) {
            error = "no memory for the template's texture and sampler";
            close();
            return false;
        }
        for (uint32_t i = 0; i < 64 * 64; i++)
            reinterpret_cast<uint32_t *>(image.cpu + (i / 64) * image.pitch)[i % 64] = 0xff000000 | i * 0x010307;
        return true;
    }

    // After a kill: a fresh connection, GPU VM and channel, so no state the killed work left behind reaches the next
    void reopen()
    {
        close();
        open();
    }

    void close()
    {
        if (device && samplerMade)
            device->destroySampler(sampler);
        if (device && imageMade)
            device->destroyTexture(image);
        imageMade = samplerMade = false;
        device.reset();
        fragment = nullptr;
        kernel = nullptr;
    }
};

namespace {

struct Target {
    raw::Texture color;
    std::vector<raw::Texture> others;  // the template's other colours, memoryless
};

std::vector<uint32_t> otherFormats(const char *key)
{
    std::vector<uint32_t> formats;
    for (unsigned c = 1; c < 8; c++) {
        char tag[8];
        unsigned format = 0;
        snprintf(tag, sizeof tag, " c%u=", c);
        const char *at = strstr(key, tag);
        if (!at || sscanf(at + strlen(tag), "%u", &format) != 1)
            break;
        formats.push_back(format);
    }
    return formats;
}

// The template's inputs as fetchform binds the pack's states (raw/test/fetchform.cpp): the rect's corners as float4
// clip-space positions through identity matrices, the other attributes and uniforms fixed, its textures one picture
void bind(raw::RenderEncoder *e, const raw::Pipeline &p, const switch_bench &b, const switch_bench_experiment &x)
{
    const raw::PackPipeline &r = *p.rec;
    std::vector<uint8_t> v(4 * r.vertexStride);
    const float x0 = float(x.rect[0]) / x.width * 2 - 1, x1 = float(x.rect[2]) / x.width * 2 - 1;
    const float y0 = 1 - float(x.rect[1]) / x.height * 2, y1 = 1 - float(x.rect[3]) / x.height * 2;
    for (uint32_t k = 0; k < 4; k++) {
        float *f = reinterpret_cast<float *>(v.data() + k * r.vertexStride);
        f[0] = k & 1 ? x1 : x0;
        f[1] = k & 2 ? y1 : y0;
        f[2] = 0.5f;
        f[3] = 1.f;
        for (uint32_t w = 4; w < r.vertexStride / 4 && w < 8; w++)
            f[w] = float((k >> (w & 1)) & 1);
    }
    e->setBytes(1, r.vertexSlot, v.data(), uint32_t(v.size()));
    float identity[64], uniforms[64];
    for (uint32_t k = 0; k < 64; k++) {
        identity[k] = (k % 16) % 5 == 0 ? 1.f : 0.f;
        uniforms[k] = float(k % 7) / 7.f;
    }
    for (uint32_t k = 0; k < r.entries; k++) {
        const raw::PackEntry &entry = r.entry[k];
        if (entry.kind == raw::EntryUbo)
            e->setBytes(entry.stage, entry.slot, entry.stage == 1 ? identity : uniforms, sizeof uniforms);
        else if (entry.kind == raw::EntryTexture)
            e->setTexture(entry.slot, &b.image);
        if (entry.kind != raw::EntryUbo && entry.samplerKind == raw::SamplerSlot)
            e->setSampler(entry.sampler, &b.sampler);
    }
}

void fill(raw::Texture &t, const switch_bench_experiment &x, uint32_t bpp)
{
    for (uint32_t y = 0; y < x.height; y++)
        memcpy(t.cpu + y * t.pitch, x.fill + size_t(y) * x.width * bpp, size_t(x.width) * bpp);
}

void readBack(const raw::Texture &t, const switch_bench_experiment &x, uint32_t bpp, switch_bench_result &r)
{
    if (!r.texels || r.texel_bytes < size_t(x.width) * x.height * bpp)
        return;
    for (uint32_t y = 0; y < x.height; y++)
        memcpy(r.texels + size_t(y) * x.width * bpp, t.cpu + y * t.pitch, size_t(x.width) * bpp);
}

// One command buffer: `draws` quads (or dispatches) and its times. False: `r` says why.
class Submission {
public:
    Submission(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r) : b_(b), x_(x), r_(r)
    {}

    bool render(Target &t, raw::Pipeline *pipeline, uint32_t draws, uint64_t &gpu, uint64_t &wall)
    {
        raw::Device &d = *b_.device;
        raw::CommandBuffer *cb = d.begin();
        if (!cb)
            return error("no channel: " + d.channelError());
        raw::PassDesc pass;
        pass.count = 1 + uint32_t(t.others.size());
        pass.color[0].texture = &t.color;
        pass.color[0].load = x_.fill ? raw::ColorAttachment::Load : raw::ColorAttachment::Clear;
        memcpy(pass.color[0].clear, x_.clear, sizeof x_.clear);
        for (size_t i = 0; i < t.others.size(); i++) {
            pass.color[1 + i].texture = &t.others[i];
            pass.color[1 + i].load = raw::ColorAttachment::Clear;
            pass.color[1 + i].store = false;
        }
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return refused("the raw path cannot render the target");
        }
        e->setViewport(0, 0, x_.width, x_.height, 0, 1);
        e->setScissor(0, 0, x_.width, x_.height);
        if (draws) {
            e->setPipeline(pipeline);
            bind(e, *pipeline, b_, x_);
        }
        for (uint32_t n = 0; n < draws; n++) {
            const char *why = nullptr;
            if (!e->draw(kTriangleStrip, 0, 4, 1, 0, &why)) {
                e->end();
                const raw::Refusal refusal = cb->refusal;
                cb->abandon();
                char line[160];
                snprintf(line, sizeof line, "draw refused: %s (predicate %u: %u %u %u)", why ? why : "?", refusal.id,
                         refusal.a, refusal.b, refusal.c);
                return refused(line);
            }
        }
        e->end();
        return finish(cb, gpu, wall);
    }

    bool twod(const raw::ProbeOp &op, const raw::Texture *read, const raw::Texture &written, uint64_t &gpu,
              uint64_t &wall)
    {
        raw::Device &d = *b_.device;
        raw::CommandBuffer *cb = d.begin();
        if (!cb)
            return error("no channel: " + d.channelError());
        raw::BlitEncoder *e = cb->blit();
        if (!e) {
            cb->abandon();
            return refused("the raw path opened no blit encoder");
        }
        e->probe(op, read, written);
        e->end();
        return finish(cb, gpu, wall);
    }

    bool dispatch(const raw::ComputeState *state, uint32_t dispatches, uint64_t &gpu, uint64_t &wall)
    {
        raw::Device &d = *b_.device;
        raw::CommandBuffer *cb = d.begin();
        if (!cb)
            return error("no channel: " + d.channelError());
        {
            raw::ComputeEncoder ce(*cb, d);
            ce.setState(state);
            for (uint32_t n = 0; n < dispatches; n++) {
                const char *why = nullptr;
                if (!ce.dispatch(x_.groups, x_.threads, &why)) {
                    ce.end();
                    cb->abandon();
                    return refused(std::string("dispatch refused: ") + (why ? why : "?"));
                }
            }
            ce.end();
        }
        return finish(cb, gpu, wall);
    }

private:
    switch_bench &b_;
    const switch_bench_experiment &x_;
    switch_bench_result &r_;

    bool finish(raw::CommandBuffer *cb, uint64_t &gpu, uint64_t &wall)
    {
        raw::Device &d = *b_.device;
        const uint32_t stamp = cb->stamp;
        uint32_t seq = 0;
        const uint64_t start = mach_absolute_time();
        const bool committed = cb->commit(seq);
        const bool done = committed && d.wait(seq);
        wall = nanoseconds(mach_absolute_time() - start);
        if (!done) {
            r_.lost_error = d.lostError();
            r_.fault_va = d.lostFaultVa();
            fail(r_, SWITCH_BENCH_KILLED,
                 committed ? "the kernel killed the channel" : "the kernel refused the submission");
            return false;
        }
        if (!d.gpuNanoseconds(stamp, gpu))
            gpu = 0;
        return true;
    }

    bool refused(const std::string &why)
    {
        fail(r_, SWITCH_BENCH_REFUSED, why);
        return false;
    }

    bool error(const std::string &why)
    {
        fail(r_, SWITCH_BENCH_ERROR, why);
        return false;
    }
};

// A trap in a lone command buffer can land after its fence (the channel is killed while the wait already returned), so
// one more empty pass on the channel: it never completes on a killed one
void checkAlive(switch_bench &b, const switch_bench_experiment &x, Target &t, switch_bench_result &r)
{
    uint64_t gpu = 0, wall = 0;
    if (Submission(b, x, r).render(t, nullptr, 0, gpu, wall) || r.status != SWITCH_BENCH_KILLED)
        return;
    snprintf(r.why, sizeof r.why, "the channel was dead after the experiment's last fence (a trap the fence missed)");
}

bool readsPositionW(const uint8_t *sph)
{
    uint32_t word5;
    memcpy(&word5, sph + 20, sizeof word5);
    return word5 & bench::kPositionW;
}

bool valid(const switch_bench_experiment &x, switch_bench_result &r)
{
    const raw::FormatInfo *f = raw::formatInfo(x.format);
    std::string why;
    if (!f || !f->rt)
        why = "not a format the raw path renders";
    else if (!x.width || !x.height || x.width > 2048 || x.height > 2048)
        why = "target size";
    else if (x.rect[0] >= x.rect[2] || x.rect[1] >= x.rect[3] || x.rect[2] > x.width || x.rect[3] > x.height)
        why = "rect outside the target";
    else if (!x.draw_counts || !x.repeats)
        why = "no command buffers";
    const switch_bench_program &p = x.program;
    const bool programmed =
        x.kind == SWITCH_BENCH_FRAGMENT || x.kind == SWITCH_BENCH_READBACK || x.kind == SWITCH_BENCH_COMPUTE;
    if (why.empty() && programmed &&
        (!p.code || !p.code_bytes || p.code_bytes % 32 || p.code_bytes > 0x10000 || p.data_bytes % 16 ||
         p.data_bytes > 0x10000 || !p.gprs || p.gprs > 255 || (x.kind != SWITCH_BENCH_COMPUTE && !p.sph)))
        why = "program: code in whole bundles, data in 16-byte steps, both at most 64 KiB, 1..255 registers, an SPH";
    else if (why.empty() && programmed && x.kind != SWITCH_BENCH_COMPUTE && !readsPositionW(p.sph))
        why = "program.sph: a pixel header must read position W (bit 191), or the GPU traps it";
    if (why.empty())
        return true;
    fail(r, SWITCH_BENCH_INVALID, why);
    return false;
}

bench::Program program(const switch_bench_experiment &x)
{
    bench::Program p;
    if (x.kind == SWITCH_BENCH_KNOWN) {
        const auto *sph = reinterpret_cast<const uint8_t *>(bench::kKnownSph);
        const auto *code = reinterpret_cast<const uint8_t *>(bench::kKnownCode);
        const auto *data = reinterpret_cast<const uint8_t *>(bench::kKnownData);
        p.sph.assign(sph, sph + sizeof bench::kKnownSph);
        p.code.assign(code, code + sizeof bench::kKnownCode);
        p.data.assign(data, data + sizeof bench::kKnownData);
        p.gprs = bench::kKnownGprs;
        return p;
    }
    const switch_bench_program &s = x.program;
    if (s.sph)
        p.sph.assign(s.sph, s.sph + 80);
    p.code.assign(s.code, s.code + s.code_bytes);
    if (s.data)
        p.data.assign(s.data, s.data + s.data_bytes);
    p.gprs = s.gprs;
    return p;
}

bench::Methods pairs(const uint32_t *words, uint32_t count)
{
    bench::Methods out;
    for (uint32_t i = 0; words && i < count; i++)
        out.emplace_back(words[2 * i], words[2 * i + 1]);
    return out;
}

bool timeDraws(switch_bench &b, const switch_bench_experiment &x, Target &t, raw::Pipeline *pipeline,
               switch_bench_result &r)
{
    const uint32_t bpp = raw::formatInfo(x.format)->bpp;
    Submission s(b, x, r);
    for (uint32_t c = 0; c < x.draw_counts; c++)
        for (uint32_t k = 0; k < x.warmup + x.repeats; k++) {
            uint64_t gpu = 0, wall = 0;
            if (x.fill)
                fill(t.color, x, bpp);
            if (!s.render(t, pipeline, pipeline ? x.draws[c] : 0, gpu, wall))
                return false;
            if (k >= x.warmup) {
                r.gpu_ns[c * x.repeats + k - x.warmup] = gpu;
                r.wall_ns[c * x.repeats + k - x.warmup] = wall;
            }
        }
    return true;
}

void runRender(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r)
{
    raw::Device &d = *b.device;
    raw::Pipeline *pipeline = nullptr;
    const raw::PackPipeline *tpl = x.template_key ? bench::fragmentTemplate(d.pack(), x.template_key) : b.fragment;
    if (x.kind != SWITCH_BENCH_NOOP) {
        if (!tpl && x.template_key)
            return fail(r, SWITCH_BENCH_INVALID, "no plain pack state has the template key");
        if (!tpl)
            return fail(r, SWITCH_BENCH_ERROR, "the pack has no state the bench can take as a template");
        raw::RuntimeRecord record;
        std::string why;
        const uint64_t key = kKeyBase | ++b.records;
        if (!bench::fragmentRecord(d.pack(), *tpl, program(x), x.format, key, pairs(x.methods, x.method_pairs),
                                   pairs(x.restore, x.restore_pairs), record, why, x.single_target != 0))
            return fail(r, SWITCH_BENCH_INVALID, why);
        if (!(pipeline = d.addRecord(record, false, why)))
            return fail(r, SWITCH_BENCH_REFUSED, why);
        r.template_key = tpl->keyHash;
    }
    const uint32_t bpp = raw::formatInfo(x.format)->bpp;
    Target t;
    std::vector<uint32_t> others =
        tpl && !x.single_target ? otherFormats(d.pack().string(tpl->keyString)) : std::vector<uint32_t>();
    t.others.resize(x.kind == SWITCH_BENCH_NOOP ? 0 : others.size());
    const bool colorMade = d.createTexture(x.width, x.height, x.format, t.color, true);
    size_t othersMade = 0;
    while (colorMade && othersMade < t.others.size() &&
           d.createMemoryless(x.width, x.height, others[othersMade], t.others[othersMade]))
        othersMade++;
    if (!colorMade || othersMade < t.others.size())
        fail(r, SWITCH_BENCH_ERROR, "no memory for the target");
    else if (timeDraws(b, x, t, pipeline, r)) {
        readBack(t.color, x, bpp, r);
        if (x.kind != SWITCH_BENCH_NOOP)
            checkAlive(b, x, t, r);
    }
    for (size_t i = 0; i < othersMade; i++)
        d.destroyTexture(t.others[i]);
    if (colorMade)
        d.destroyTexture(t.color);
}

raw::ProbeSurface twodSurface(const raw::Texture &t, uint32_t fmt2d)
{
    raw::ProbeSurface s;
    s.va = t.va;
    s.fmt2d = fmt2d ? fmt2d : raw::probeTwodFormat(t.format);
    s.pitch = t.pitch;
    s.width = t.width;
    s.height = t.height;
    s.blockLinear = t.blockLinear;
    s.blockHeight = t.blockHeight;
    return s;
}

// One 2D-engine operation a command buffer, so a rejected one names itself in the kernel's kill line
void runTwod(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r)
{
    raw::Device &d = *b.device;
    const switch_bench_twod &w = x.twod;
    raw::ProbeOp op;
    op.kind = w.op == SWITCH_BENCH_TWOD_FILL ? raw::ProbeOpKind::Fill : raw::ProbeOpKind::Scale;
    op.dx = x.rect[0];
    op.dy = x.rect[1];
    op.dw = x.rect[2] - x.rect[0];
    op.dh = x.rect[3] - x.rect[1];
    op.du = w.du;
    op.dv = w.dv;
    op.sx = w.sx;
    op.sy = w.sy;
    op.bilinear = w.bilinear != 0;
    op.corner = w.corner != 0;
    op.colorFormat = w.color_format;
    memcpy(op.color, w.color, sizeof op.color);
    const raw::Texture *read = op.kind == raw::ProbeOpKind::Scale ? &b.image : nullptr;
    if (read)
        op.src = twodSurface(*read, 0);
    raw::Texture t;
    if (!d.createTexture(x.width, x.height, x.format, t, true))
        return fail(r, SWITCH_BENCH_ERROR, "no memory for the target");
    op.dst = twodSurface(t, w.fmt2d);
    const uint32_t bpp = raw::formatInfo(x.format)->bpp;
    Submission s(b, x, r);
    bool ran = op.dst.fmt2d != 0;
    if (!ran)
        fail(r, SWITCH_BENCH_INVALID, "a format the 2D engine names none of");
    for (uint32_t c = 0; ran && c < x.draw_counts; c++)
        for (uint32_t k = 0; ran && k < x.warmup + x.repeats; k++) {
            uint64_t gpu = 0, wall = 0;
            if (x.fill)
                fill(t, x, bpp);
            else
                memset(t.cpu, 0, size_t(t.pitch) * x.height);
            ran = s.twod(op, read, t, gpu, wall);
            if (ran && k >= x.warmup) {
                r.gpu_ns[c * x.repeats + k - x.warmup] = gpu;
                r.wall_ns[c * x.repeats + k - x.warmup] = wall;
            }
        }
    if (ran) {
        readBack(t, x, bpp, r);
        Target alive;
        alive.color = t;
        checkAlive(b, x, alive, r);
    }
    d.destroyTexture(t);
}

void runCompute(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r)
{
    if (!b.kernel)
        return fail(r, SWITCH_BENCH_ERROR, "the pack has no kernel the bench can take as a template");
    raw::ComputeRecord &record = kernelRecords().emplace_back();
    std::string why;
    if (!bench::kernelRecord(*b.kernel, program(x), x.threads, record, why))
        return fail(r, SWITCH_BENCH_INVALID, why);
    const raw::ComputeState *state = raw::placeCompute(*b.device, record);
    if (!state)
        return fail(r, SWITCH_BENCH_REFUSED, "no room for the kernel in the shader heap");
    r.template_key = b.kernel->keyHash;
    Submission s(b, x, r);
    for (uint32_t c = 0; c < x.draw_counts; c++)
        for (uint32_t k = 0; k < x.warmup + x.repeats; k++) {
            uint64_t gpu = 0, wall = 0;
            if (!s.dispatch(state, x.draws[c], gpu, wall))
                return;
            if (k >= x.warmup) {
                r.gpu_ns[c * x.repeats + k - x.warmup] = gpu;
                r.wall_ns[c * x.repeats + k - x.warmup] = wall;
            }
        }
}

}  // namespace

extern "C" {

struct switch_bench *switch_bench_open(char *why, size_t why_size)
{
    auto *b = new switch_bench();
    if (b->open())
        return b;
    snprintf(why, why_size, "%s", b->error.c_str());
    b->close();
    delete b;
    return nullptr;
}

void switch_bench_close(struct switch_bench *bench)
{
    if (!bench)
        return;
    bench->close();
    delete bench;
}

uint32_t switch_bench_texel_bytes(uint32_t format)
{
    const raw::FormatInfo *f = raw::formatInfo(format);
    return f && f->rt ? f->bpp : 0;
}

size_t switch_bench_describe(struct switch_bench *b, char *out, size_t size)
{
    if (!b || !b->device)
        return size_t(snprintf(out, size, "no raw device: %s", b ? b->error.c_str() : "?"));
    const raw::Pack &pack = b->device->pack();
    return size_t(snprintf(out, size,
                           "%s v%u stack %016llx states %u; template %016llx (%s); kernel template %016llx; own GPU VM",
                           b->pack.c_str(), pack.header()->version, (unsigned long long)pack.header()->stack,
                           pack.header()->pipelines, (unsigned long long)(b->fragment ? b->fragment->keyHash : 0),
                           b->fragment ? pack.string(b->fragment->keyString) : "none",
                           (unsigned long long)(b->kernel ? b->kernel->keyHash : 0)));
}

void switch_bench_known_data(uint32_t out[4])
{
    memcpy(out, bench::kKnownData, sizeof bench::kKnownData);
}

size_t switch_bench_known_kernel(uint8_t *out, size_t size)
{
    if (size < sizeof bench::kKnownKernel)
        return 0;
    memcpy(out, bench::kKnownKernel, sizeof bench::kKnownKernel);
    return sizeof bench::kKnownKernel;
}

void switch_bench_run(struct switch_bench *b, const struct switch_bench_experiment *x, struct switch_bench_result *r)
{
    r->status = SWITCH_BENCH_OK;
    r->why[0] = 0;
    if (b->records >= kRecordsPerDevice) {
        b->records = 0;
        b->reopen();
    }
    if (!b->device && !b->open())
        return fail(*r, SWITCH_BENCH_ERROR, "no raw device: " + b->error);
    if (!valid(*x, *r))
        return;
    if (x->kind == SWITCH_BENCH_COMPUTE)
        runCompute(*b, *x, *r);
    else if (x->kind == SWITCH_BENCH_TWOD)
        runTwod(*b, *x, *r);
    else
        runRender(*b, *x, *r);
    if (r->status == SWITCH_BENCH_KILLED)
        b->reopen();
}

}  // extern "C"
