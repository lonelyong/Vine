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
using vn::String;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::FrameDescription;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderPass;
using vn::graphics::RenderTarget;
using vn::intrusive_ptr;

namespace
{

constexpr int kTargetSize = 32;

[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget(const char8_t* name)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(name));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(kTargetSize, kTargetSize);
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
        draw.clear        = ClearPolicy{};
        draw.clear->color = Colorf(0.25F, 0.5F, 0.75F, 1.0F);
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

    // THE ADVERTISED FEATURE AND THE REPORT MUST AGREE. This backend answers `GpuTimings` from the device, so a
    // session that answers it true has to fill `FrameTiming` - otherwise a host that branched on the feature shows a
    // clock that never moves (`Capabilities.hpp`: a feature says whether, the report part says whether this backend
    // measures). Skipping on the OBSERVATION (no timing) would quietly hide exactly that regression, so the skip is
    // tied to the feature instead, and the presence of the report part is asserted.
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
