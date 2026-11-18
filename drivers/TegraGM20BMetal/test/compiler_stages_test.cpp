// SPDX-License-Identifier: MIT
// The shader compiler's kept translations and stages (compiler_stages.h, metal2vulkan's kept stages): states that
// differ in one descriptor field each, made once cold (nothing kept) and once after the base state with everything
// kept. A state made from kept stages is byte for byte the state made cold; a stage whose compile reads the same inputs
// is compiled once; and the table of which inputs each field reaches, per stage, is the one stated here.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "compiler_requests.h"
#include "../compiler/compiler_facts.h"
#include "../compiler/compiler_libraries.h"
#include "../compiler/compiler_stages.h"
#include "../TegraGM20BMetalOffline.h"
#include "../compute_key.h"
#include "../pack.h"

#include <gtest/gtest.h>
#include <fstream>
#include <iterator>

namespace {

constexpr const char *kLibrary = "/test/compiler_stages.metallib";
constexpr size_t kKept = 64u << 20;

std::string dir()
{
    const char *tmp = getenv("TEST_TMPDIR");
    return tmp ? tmp : "/tmp";
}

std::string root()
{
    return compiler_test::systemRoot(getenv("SWITCH_QUARTZCORE"));
}

std::string library()
{
    const char *path = getenv("SWITCH_STAGES_METALLIB");
    return path ? path : "";
}

struct Case {
    const char *name;
    std::string line;
    // false: the field does not reach the vertex stage's, the fragment stage's compile, or m2v's translation (each
    // measured; true leaves it unjudged, so a translator that keys less stays green)
    bool vertex, fragment, translated;
};

// The base states: the render state's and the fetching state's; each case after one changes one field of it
constexpr size_t kRenderBase = 0, kFetchBase = 17;

size_t baseOf(size_t i)
{
    return i < kFetchBase ? kRenderBase : kFetchBase;
}

const std::string kVertex = " | v stages_vertex lib=" + std::string(kLibrary);
const std::string kFragment = " | f stages_fragment lib=" + std::string(kLibrary);
const std::string kFetch = " | f stages_fetch_fragment lib=" + std::string(kLibrary);
const std::string kAttributes = " a0=31:0:0:24:1:1 a1=29:16:0:24:1:1";
const std::string kBlended = "c0=80:15:1:4:5:0:4:5:0";

std::string state(const std::string &head, const std::string &colours, const std::string &attributes = kAttributes,
                  const std::string &fragment = kFragment)
{
    return "R ok=1 " + head + " " + colours + attributes + kVertex + fragment;
}

// The base state first, then one field changed each, as compiler_stages_test's table measured them: the vertex stage's
// compile reads the blend switch (its push words and layout), and the fragment stage's the blend switch, a 16-bit
// float target, the sample count and, fetching its attachment, the write mask; the colour format within a class, the
// blend factors and operations, the topology class, a depth attachment and every attribute layout reach neither
std::vector<Case> renderCases()
{
    const std::string plain = "samples=1 topo=0 ds=0/0";
    return {
        {"base", state(plain, kBlended), true, true, true},
        {"colour format BGRA8 -> RGBA8", state(plain, "c0=70:15:1:4:5:0:4:5:0"), false, false, false},
        {"colour format -> RGBA16Float", state(plain, "c0=115:15:1:4:5:0:4:5:0"), false, true, false},
        {"colour format -> R8Unorm", state(plain, "c0=10:15:1:4:5:0:4:5:0"), false, false, true},
        {"write mask", state(plain, "c0=80:7:1:4:5:0:4:5:0"), false, false, false},
        {"blending off", state(plain, "c0=80:15:0:4:5:0:4:5:0"), true, true, true},
        {"source RGB factor", state(plain, "c0=80:15:1:1:5:0:4:5:0"), false, false, false},
        {"destination RGB factor", state(plain, "c0=80:15:1:4:1:0:4:5:0"), false, false, false},
        {"RGB operation", state(plain, "c0=80:15:1:4:5:1:4:5:0"), false, false, false},
        {"alpha factors and operation", state(plain, "c0=80:15:1:1:1:1:4:5:0"), false, false, false},
        {"a second colour attachment", state(plain, kBlended + " c1=80:15:0:1:0:0:1:0:0"), false, false, true},
        {"four samples", state("samples=4 topo=0 ds=0/0", kBlended), false, true, true},
        {"topology class", state("samples=1 topo=3 ds=0/0", kBlended), false, false, false},
        {"depth attachment", state("samples=1 topo=0 ds=252/0", kBlended), false, false, false},
        {"buffer stride", state(plain, kBlended, " a0=31:0:0:48:1:1 a1=29:16:0:48:1:1"), false, false, false},
        {"attribute offset", state(plain, kBlended, " a0=31:0:0:32:1:1 a1=29:20:0:32:1:1"), false, false, false},
        {"attribute format", state(plain, kBlended, " a0=31:0:0:24:1:1 a1=25:16:0:24:1:1"), false, false, false},
        {"fetch base", state(plain, "c0=80:15:0:1:0:0:1:0:0", kAttributes, kFetch), true, true, true},
        {"fetch format BGRA8 -> RGBA8", state(plain, "c0=70:15:0:1:0:0:1:0:0", kAttributes, kFetch), false, false,
         true},
        {"fetch blending on", state(plain, kBlended, kAttributes, kFetch), true, true, true},
        {"fetch write mask", state(plain, "c0=80:7:0:1:0:0:1:0:0", kAttributes, kFetch), true, true, true},
    };
}

struct Made {
    int32_t result = -1;
    std::string entry, why;
    std::vector<switch_compiler::StageInputs> stages;  // each stage compile asked, in order
    uint64_t stagesMade = 0, translationsMade = 0;
};

// The engine's compile of a prepared request (compiler_engine.mm): its libraries, its record by the direct compile
class Compiler {
public:
    explicit Compiler(bool keep) : libraries_(std::make_shared<switch_compiler::LibraryCache>(root()))
    {
        std::string why;
        EXPECT_TRUE(pack_.load(compiler_test::emptyPack(dir()).c_str(), 0, why)) << why;
        direct_ = std::make_unique<gm20b::Direct>(gm20b::PackRules::of(pack_), false, offline::translator());
        if (keep)
            direct_->keepStages(&recording_);
    }

    Made render(const std::string &line)
    {
        compiler_test::Requests made(root(), {{kLibrary, library()}});
        gm20b::CompileRequest r;
        std::string why;
        EXPECT_TRUE(made.render(line, r, why)) << why;
        return compile(r);
    }

    Made compile(const gm20b::CompileRequest &r)
    {
        Made out;
        switch_compiler::SystemLibraries system(root());
        switch_compiler::Prepared p;
        if (!switch_compiler::prepare(gm20b::encodeRequest(r), 0, system, p, out.why)) {
            ADD_FAILURE() << "not prepared: " << out.why;
            return out;
        }
        recording_.asked.clear();
        const uint64_t stages = kept_.kept().entries;
        uint64_t before[2] = {}, after[2] = {};
        nmb_kept_translations(before);
        if (p.request.kind == gm20b::RequestKind::Facts) {
            out.result = switch_compiler::makeFacts(p, root(), out.entry, out.why);
        } else {
            switch_compiler::LibraryCache &cache = *libraries_;
            const switch_compiler::Libraries libraries = [&cache](const gm20b::RequestLibrary &l, std::string &why) {
                return cache.library(l, why);
            };
            out.result = switch_compiler::makeEntry(p, libraries, *direct_, 0, out.entry, out.why);
        }
        nmb_kept_translations(after);
        out.stages = recording_.asked;
        out.stagesMade = kept_.kept().entries - stages;
        out.translationsMade = after[0] - before[0];
        return out;
    }

private:
    // The kept stages, each compile's inputs recorded as it is asked
    struct Recording final : public gm20b::StageMemo {
        switch_compiler::CompiledStages &kept;
        std::vector<switch_compiler::StageInputs> asked;

        explicit Recording(switch_compiler::CompiledStages &k) : kept(k)
        {}

        int32_t compile(const gm20b::StageCompile &c, const gm20b::NvkStageInfo &info, const gm20b::NakLayout &layout,
                        gm20b::NvkStage &out, std::shared_ptr<const void> &held) override
        {
            asked.push_back(switch_compiler::stageInputs(c, info, layout));
            return kept.compile(c, info, layout, out, held);
        }
    };

    gm20b::Pack pack_;
    std::shared_ptr<switch_compiler::LibraryCache> libraries_;
    switch_compiler::CompiledStages kept_{kKept};
    Recording recording_{kept_};
    std::unique_ptr<gm20b::Direct> direct_;
};

// The parts of each stage's inputs (vertex 0, fragment 1 of the primary form) that differ from `base`'s
std::string differing(const Made &base, const Made &made, size_t stage)
{
    if (stage >= base.stages.size() || stage >= made.stages.size())
        return "(no stage)";
    std::string out;
    for (int part = 0; part < switch_compiler::StageInputs::kParts; part++)
        if (base.stages[stage].parts[part] != made.stages[stage].parts[part])
            out += std::string(out.empty() ? "" : ",") + switch_compiler::StageInputs::name(part);
    return out.empty() ? "-" : out;
}

gm20b::CompileRequest kernelRequest(const std::string &bytes, gm20b::RequestKind kind, const uint32_t threads[3],
                                    uint64_t format)
{
    std::map<std::string, gm20b::AirHash> hashes;
    gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), hashes);
    gm20b::CompileRequest r;
    r.kind = kind;
    gm20b::RequestLibrary l;
    l.path = kLibrary;
    l.bytes = bytes;
    r.libraries.push_back(l);
    gm20b::RequestStage s;
    s.tag = "k";
    s.function = s.name = "stages_kernel";
    r.stages.push_back(s);
    const gm20b::AirHash &air = hashes["stages_kernel"];
    const std::string state = gm20b::computeStateKey(air, "", 0, false);
    if (kind == gm20b::RequestKind::Facts) {
        r.key = gm20b::factsCacheKey(0, state).key;
        return r;
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = MTLPixelFormat(format);
    d.textureType = MTLTextureType2D;
    d.usage = MTLTextureUsageShaderRead;
    gm20b::KernelDispatch dispatch(threads, {});
    dispatch.texture(0, d);
    const NmbDirectDispatch &nd = dispatch.dispatch();
    memcpy(r.kernel.threads, nd.threads, sizeof r.kernel.threads);
    r.kernel.textures.assign(nd.textures, nd.textures + nd.textureCount);
    r.key = "kernel | " +
            gm20b::computeKey(air, "", 0, false, threads, "", "0:" + std::to_string(format) + ":2:1:1:02030405", "");
    return r;
}

std::string fileBytes(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

class CompilerStages : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (root().empty() || library().empty())
            GTEST_SKIP() << "no crawl or no test library";
    }

    void TearDown() override
    {
        nmb_keep_translations(0);
    }
};

}  // namespace

TEST_F(CompilerStages, AStateOfKeptStagesIsTheStateMadeColdAndEachFieldReachesTheStagesStated)
{
    const std::vector<Case> cases = renderCases();
    std::vector<Made> cold;
    {
        nmb_keep_translations(0);
        for (const Case &c : cases) {
            Compiler compiler(false);
            cold.push_back(compiler.render(c.line));
        }
    }
    nmb_keep_translations(kKept);
    Compiler warm(true);
    std::vector<Made> made;
    for (const Case &c : cases)
        made.push_back(warm.render(c.line));
    std::string table = "\nfield | vertex inputs | fragment inputs | stages made | translations made\n";
    for (size_t i = 0; i < cases.size(); i++) {
        const Made &base = made[baseOf(i)];
        table += std::string(cases[i].name) + " | " + differing(base, made[i], 0) + " | " +
                 differing(base, made[i], 1) + " | " + std::to_string(made[i].stagesMade) + " | " +
                 std::to_string(made[i].translationsMade) + "\n";
    }
    printf("%s", table.c_str());
    for (size_t i = 0; i < cases.size(); i++) {
        const Case &c = cases[i];
        ASSERT_EQ(made[i].result, TegraGM20BCompileMade) << c.name << ": " << made[i].why;
        EXPECT_EQ(made[i].entry, cold[i].entry)
            << c.name << ": a state of kept stages differs from the state made cold";
        if (i == baseOf(i))
            continue;
        const Made &base = made[baseOf(i)];
        if (!c.vertex)
            EXPECT_EQ(differing(base, made[i], 0), "-") << c.name << ": the vertex stage's inputs" << table;
        if (!c.fragment)
            EXPECT_EQ(differing(base, made[i], 1), "-") << c.name << ": the fragment stage's inputs" << table;
        if (!c.vertex && !c.fragment)
            EXPECT_EQ(made[i].stagesMade, 0u) << c.name << ": its stages were compiled again";
        if (!c.translated)
            EXPECT_EQ(made[i].translationsMade, 0u) << c.name << ": its functions were translated again";
    }
}

TEST_F(CompilerStages, AKernelIsTranslatedOnceForItsFactsAndEveryDispatch)
{
    const std::string bytes = fileBytes(library());
    const uint32_t shapes[][3] = {{8, 8, 1}, {8, 8, 1}, {2, 8, 1}, {16, 16, 1}};
    const uint64_t formats[] = {80, 70, 70, 115};
    std::vector<Made> cold;
    nmb_keep_translations(0);
    for (size_t i = 0; i < 4; i++) {
        Compiler compiler(false);
        cold.push_back(compiler.compile(kernelRequest(bytes, gm20b::RequestKind::Kernel, shapes[i], formats[i])));
    }
    nmb_keep_translations(kKept);
    Compiler warm(true);
    const Made facts = warm.compile(kernelRequest(bytes, gm20b::RequestKind::Facts, shapes[0], 0));
    ASSERT_EQ(facts.result, TegraGM20BCompileMade) << facts.why;
    EXPECT_EQ(facts.translationsMade, 1u);
    for (size_t i = 0; i < 4; i++) {
        const Made k = warm.compile(kernelRequest(bytes, gm20b::RequestKind::Kernel, shapes[i], formats[i]));
        ASSERT_EQ(k.result, TegraGM20BCompileMade) << k.why;
        EXPECT_EQ(k.entry, cold[i].entry) << "dispatch " << i << ": a kernel of a kept translation differs";
        EXPECT_EQ(k.translationsMade, 0u) << "dispatch " << i << " translated its kernel again";
    }
    // a kept translation is sized by what its compile left charged (the native program and its typed module), so the
    // bound holds: never by its bookkeeping alone
    uint64_t kept[2] = {};
    nmb_kept_translations(kept);
    printf("kept translations: %llu, %llu bytes\n", (unsigned long long)kept[0], (unsigned long long)kept[1]);
    EXPECT_GT(kept[1], kept[0] * 4096);
}
