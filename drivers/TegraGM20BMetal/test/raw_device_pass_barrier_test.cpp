// SPDX-License-Identifier: MIT
// Rule barrier.pass_pixel on raw::Device (raw_pass_hazard.h): a pass whose recording says it samples a small
// block-linear target the pass before rendered opens with PIXEL_SHADER_BARRIER and no drain; one sampling a 1280x720
// target, and one the encode never described, open with today's.
#include "raw_device_harness.h"

namespace {

constexpr uint32_t kBgra8 = 80;
constexpr uint32_t kPixelShaderBarrier = 0x0de0, kWaitForIdle = 0x0110, kShaderCaches = 0x021c,
                   kTextureDataNoWfi = 0x1288;

// The 3D subchannel's methods of a job's words (IMMD, INC, NON_INC and ONE_INC), in order
std::vector<uint32_t> methods(const std::vector<uint32_t> &words)
{
    std::vector<uint32_t> out;
    for (size_t at = 0; at < words.size();) {
        const uint32_t h = words[at++], form = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       method = (h & 0xfff) << 2;
        if (!h)
            continue;
        if (form == 4) {
            if (subc == 0)
                out.push_back(method);
            continue;
        }
        for (uint32_t i = 0; i < count && at < words.size(); i++, at++)
            if (subc == 0)
                out.push_back(form == 3 ? method : form == 5 && i ? method + 4 : method + 4 * (form == 1 ? i : 0));
    }
    return out;
}

class DevicePassBarrier : public harness::DeviceTest {
protected:
    raw::Texture small, atlas, big, half;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kBgra8, small, false, true));
        ASSERT_TRUE(small.blockLinear);
        ASSERT_TRUE(dev.createTexture(64, 64, kBgra8, atlas));
        ASSERT_TRUE(dev.createTexture(1280, 720, kBgra8, big));
        ASSERT_TRUE(dev.createTexture(640, 360, kBgra8, half));
    }

    static raw::PassDesc cleared(raw::Texture &t)
    {
        raw::PassDesc pass;
        pass.color[0].texture = &t;
        pass.color[0].load = raw::ColorAttachment::Clear;
        pass.count = 1;
        return pass;
    }

    // Pass A clears `first`; pass B clears `second`, its recording sampling `sampled` where `described`: the PIXEL_SHADER_
    // BARRIERs and WAIT_FOR_IDLEs of the job
    std::pair<size_t, size_t> twoPasses(raw::Texture &first, raw::Texture &second, const raw::Texture *sampled,
                                        bool described)
    {
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return {};
        raw::RenderEncoder *a = cb->render(cleared(first));
        raw::PassUses uses;
        uses.known = true;
        a->plan(uses);
        a->end();
        raw::RenderEncoder *b = cb->render(cleared(second));
        if (sampled)
            uses.sample(raw::hazardImage(*sampled), false);
        if (described)
            b->plan(uses);
        b->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
        return {size_t(std::count(m.begin(), m.end(), kPixelShaderBarrier)),
                size_t(std::count(m.begin(), m.end(), kWaitForIdle))};
    }
};

TEST_F(DevicePassBarrier, ASmallTargetSampledByTheNextPassTakesThePixelBarrier)
{
    const auto drained = twoPasses(small, atlas, &small, false);
    const auto relaxed = twoPasses(small, atlas, &small, true);
    EXPECT_EQ(drained.first, 0u);
    EXPECT_EQ(relaxed.first, 1u);
    EXPECT_EQ(relaxed.second + 1, drained.second) << "the drain between the passes is gone";
}

// Rule barrier.entry_no_drain: a job opens with the planner's invalidates, its first pass with nothing, no drain at all
TEST_F(DevicePassBarrier, AJobOpensWithThePlannersInvalidatesAndNoDrain)
{
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->render(cleared(atlas))->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kWaitForIdle), 0);
    EXPECT_EQ(std::count(m.begin(), m.end(), kShaderCaches), 0) << "the waiting shader-cache flush";
    EXPECT_GE(std::count(m.begin(), m.end(), kTextureDataNoWfi), 1);
}

// A blit encoder that sends nothing changes no engine: the pass after it is ordered after the pass before it as if it were
// not there, never by the NO_WFI invalidates of a change from the copy engine that did not happen
TEST_F(DevicePassBarrier, AnEmptyBlitEncoderChangesNoEngine)
{
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    raw::PassUses uses;
    uses.known = true;
    raw::RenderEncoder *a = cb->render(cleared(small));
    a->plan(uses);
    a->end();
    cb->blit()->end();
    raw::RenderEncoder *b = cb->render(cleared(atlas));
    uses.sample(raw::hazardImage(small), false);
    b->plan(uses);
    b->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kPixelShaderBarrier), 1) << "pass B's opening, the rule's held cell";
}

// A storage form reads its target through the SMs' data cache, which the pixel barrier leaves: its pass keeps the drain,
// and so does the pass after it
TEST_F(DevicePassBarrier, APassOfStorageFormsKeepsTheDrain)
{
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    raw::PassUses uses;
    uses.known = true;
    raw::RenderEncoder *a = cb->render(cleared(small));
    a->plan(uses);
    a->end();
    raw::PassDesc storage = cleared(atlas);
    storage.storageMask = 1;
    raw::RenderEncoder *b = cb->render(storage);
    b->plan(uses);
    b->end();
    raw::RenderEncoder *c = cb->render(cleared(half));
    c->plan(uses);
    c->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<uint32_t> m = methods(kernel_model::model.jobs.back().words);
    EXPECT_EQ(std::count(m.begin(), m.end(), kPixelShaderBarrier), 0);
}

TEST_F(DevicePassBarrier, AFullSizeTargetKeepsTheDrain)
{
    const auto read = twoPasses(big, half, &big, true);
    EXPECT_EQ(read.first, 0u);
    const auto unrelated = twoPasses(big, half, nullptr, true);
    EXPECT_EQ(unrelated.first, 1u) << "nothing of the pass before is read or written";
}

}  // namespace
