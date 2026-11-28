/**
 * @brief Per-pass GPU attribution: the report says WHICH pass spent the frame's time (see PassTiming).
 *
 * WHY THIS FILE EXISTS. `FrameTiming::gpu_micros` answered "the frame cost 8 ms" and nothing more, so "the shadow pass
 * or the lighting pass is the one to fix" could not be asked of the device at all - and an earlier review read the
 * whole-frame number as host work precisely because there was no attribution to compare it with. The clock now writes
 * one boundary per pass, and this case pins that the report carries them: two passes, two entries, the passes' own
 * names, and spans that add up to no more than the frame.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/String.hpp>
#include <vine/graphics/Capabilities.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameReport.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>

#include <vine/backend_vk/internal/Device.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AttachmentPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::FrameDescription;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderPass;
using vn::graphics::RenderTarget;

namespace
{

constexpr int kTargetSize = 32;

[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget(const char8_t* name, const int size = kTargetSize)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(name));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(size, size);
    return target;
}

[[nodiscard]] intrusive_ptr<RenderPass> makePass(const char8_t* name)
{
    vn::graphics::PassDesc desc;
    desc.name = String(name);
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    vn::graphics::detail::PassNode::adopt(*pass, std::move(desc));
    return pass;
}

/// @brief One frame with TWO passes, each clearing its own target: two spans the report must tell apart.
[[nodiscard]] FrameDescription twoPassFrame(const intrusive_ptr<RenderPass>& first,
                                            RenderTarget*                     first_target,
                                            const intrusive_ptr<RenderPass>&  second,
                                            RenderTarget*                     second_target)
{
    FrameDescription frame;
    const auto      add = [&frame](const intrusive_ptr<RenderPass>& pass, RenderTarget* target) {
        FrameDescription::Pass entry;
        entry.pass = pass.get();

        vn::graphics::PassDraw draw;
        draw.target       = target;
        draw.attachment_policy        = AttachmentPolicy{};
        draw.attachment_policy->color = Colorf(0.25F, 0.5F, 0.75F, 1.0F);
        entry.draw        = std::move(draw);
        frame.passes.push_back(std::move(entry));
    };
    add(first, first_target);
    add(second, second_target);
    return frame;
}

} // namespace

TEST(VkPassTimingTest, TheReportAttributesTheFrameToThePassesThatSpentIt)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> first_target  = makeTarget(u8"vk_timing_first_target");
    const intrusive_ptr<RenderTarget> second_target = makeTarget(u8"vk_timing_second_target");
    const intrusive_ptr<RenderPass>   first_pass    = makePass(u8"vk_timing_first_pass");
    const intrusive_ptr<RenderPass>   second_pass   = makePass(u8"vk_timing_second_pass");

    // THE FIRST FRAME OF A SESSION IS A SAMPLED ONE (see kTimestampSampleFrames: frames 1, 31, 61...), which is what
    // lets this case read the clock without driving thirty frames first.
    (void)backend->executeFrame(twoPassFrame(first_pass, first_target.get(), second_pass, second_target.get()), nullptr);

    // THE ADVERTISED FEATURE AND THE REPORT MUST AGREE. This backend answers `GpuTimings` from the CHOSEN queue
    // family's timestamp bits and the device's period (review 30), so a session that answers it true has to fill
    // `FrameTiming` - otherwise a host that branched on the feature shows a clock that never moves (`Capabilities.hpp`:
    // a feature says whether, the report part says whether this backend measures). Skipping on the OBSERVATION (no
    // timing) would quietly hide exactly that regression, so the skip is tied to the feature instead, and the presence
    // of the report part is asserted.
    const bool timed = backend->capabilities().features.supports(vn::graphics::Features::Feature::GpuTimings);
    const vn::graphics::FrameReport report = backend->executor().frameReport();
    if (!timed) {
        GTEST_SKIP() << "this device cannot write timestamps, so its report has no clock to attribute";
    }
    ASSERT_TRUE(report.timing.has_value())
        << "the backend advertises GpuTimings but the frame report carries no FrameTiming";

    ASSERT_EQ(report.timing->passes.size(), 2U) << "two passes ran and the report attributed neither";
    EXPECT_TRUE(report.timing->passes[0].pass.std_u8str_view() == u8"vk_timing_first_pass")
        << "the first entry does not name the first pass (the report must be matchable against the plan)";
    EXPECT_TRUE(report.timing->passes[1].pass.std_u8str_view() == u8"vk_timing_second_pass")
        << "the second entry does not name the second pass";

    // THE SPANS ARE SPANS, NOT THE WHOLE FRAME TWICE: attributing the frame's time to every pass would make the sum
    // a multiple of the frame, which is exactly the mistake a per-pass report exists to make impossible.
    std::uint64_t sum = 0U;
    for (const vn::graphics::PassTiming& entry : report.timing->passes) {
        sum += entry.gpu_micros;
    }
    EXPECT_LE(sum, report.timing->gpu_micros + 1000U) << "the per-pass spans add up to more than the frame they split";

    backend->shutdown();
}

/**
 * @brief THE BOUNDARY WAITS: a pass that writes a large attachment reports more time than one that writes a few bytes.
 *
 * WHY THIS CASE EXISTS. The first version of this feature wrote both ends of a pass' span at `TOP_OF_PIPE`, a stage that
 * imposes no wait - so a timestamp measured when the command processor REACHED the boundary, not when the pass had
 * finished. Two passes with the same number of commands therefore reported the same (near-zero) time whatever each one
 * drew, and `TheReportAttributesTheFrameToThePassesThatSpentIt` could not tell: its only quantitative assertion is
 * "the segments do not exceed the frame", which a report of zeros satisfies. The witness that a boundary MEANS
 * something is a difference in what the passes actually cost: one clears a large target, the other a tiny one, and the
 * large one must come out slower.
 */
TEST(VkPassTimingTest, AHeavyPassReportsMoreTimeThanACheapOne)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    if (!backend->capabilities().features.supports(vn::graphics::Features::Feature::GpuTimings)) {
        GTEST_SKIP() << "this device cannot write timestamps, so there is no attribution to compare";
    }

    // 512x512 RGBA8 is 1 MiB of clear: enough that a device which really waits sees it, small enough to stay a test.
    const intrusive_ptr<RenderTarget> heavy_target = makeTarget(u8"vk_timing_heavy_target", 512);
    const intrusive_ptr<RenderTarget> light_target = makeTarget(u8"vk_timing_light_target", 4);
    const intrusive_ptr<RenderPass>   heavy_pass   = makePass(u8"vk_timing_heavy_pass");
    const intrusive_ptr<RenderPass>   light_pass   = makePass(u8"vk_timing_light_pass");

    // THE FIRST FRAME OF A SESSION IS A SAMPLED ONE (see kTimestampSampleFrames).
    (void)backend->executeFrame(twoPassFrame(heavy_pass, heavy_target.get(), light_pass, light_target.get()), nullptr);

    const vn::graphics::FrameReport report = backend->executor().frameReport();
    ASSERT_TRUE(report.timing.has_value());
    ASSERT_EQ(report.timing->passes.size(), 2U);

    // A FRAME TOO SHORT TO DIVIDE is not evidence either way: report 0 microseconds for both and the case would be
    // asserting on quantisation noise. The skip is on the FRAME's total, not on the observation the case makes.
    if (report.timing->gpu_micros < 50U) {
        GTEST_SKIP() << "this device finished the frame too fast to attribute (" << report.timing->gpu_micros << " us)";
    }

    EXPECT_GT(report.timing->passes[0].gpu_micros, report.timing->passes[1].gpu_micros)
        << "the pass that cleared 512x512 reported no more time than the one that cleared 4x4 (" << report.timing->passes[0].gpu_micros
        << " vs " << report.timing->passes[1].gpu_micros
        << " us) - a boundary that does not wait measures arrival, not work";

    backend->shutdown();
}
