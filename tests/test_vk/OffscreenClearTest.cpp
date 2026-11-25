/**
 * @brief M1's acceptance: a pass that clears its off-screen target leaves exactly that colour, and the readback says so.
 *
 * What this pins, and why each half matters:
 *   - the frame value is enough to run a pass: `PassDraw::target` + `PassDraw::attachment_policy` are the whole input, so a backend
 *     needs nothing from a live `RenderPass` to clear a target (see FrameDescription's note);
 *   - the clear colour travels as LINEAR values (`AttachmentPolicy::color`) and comes back as packed RGBA8, which is the
 *     readback contract - a colour that drifted through a wrong conversion would show here;
 *   - a target the device never built answers `NotReady`, NOT a picture: "no pixels yet" and "black pixels" are
 *     different answers, and only one of them is evidence.
 *
 * The device case skips on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::intrusive_ptr;
using vn::graphics::AttachmentPolicy;
using vn::graphics::ColorReadback;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::FrameDescription;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::ReadbackStatus;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderTarget;

namespace
{

/// @brief A 64x64 RGBA8 + D24 target, the shape M1 serves.
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(vn::String(u8"vk_m1_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(64, 64);
    return target;
}

/// @brief A frame with ONE pass that names @p target and clears it to @p colour (and draws nothing).
[[nodiscard]] FrameDescription clearingFrame(RenderTarget* target, const Colorf& colour)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest(); // an entry that names no pass is nothing to run (see RenderFrameExecutor)
    entry.order = 0;

    PassDraw draw;
    draw.target = target;
    draw.attachment_policy        = AttachmentPolicy{};
    draw.attachment_policy->color = colour;
    entry.draw = draw;

    frame.passes.push_back(std::move(entry));
    return frame;
}

} // namespace

TEST(VkOffscreenTest, ADeclaredTargetIsBuiltWithoutAFrame)
{
    // CAP-86 / BC-09. `RenderDevice::setRenderTarget` says the device holds the target and builds its attachments,
    // and the backend does exactly that. A target
    // announced before any frame answers about its images at once, and the first frame that names it builds nothing.
    //
    // The witness is the difference between two refusals: a target this device built can be asked about an attachment
    // it does not have (Invalid), while one it never built answers NotReady. No pixel is compared.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> target = makeTarget(); // RGBA8 + D24, one colour attachment

    backend->setRenderTarget(target.get());
    const ColorReadback out_of_range = backend->readColorBuffer(target.get(), 1);
    EXPECT_EQ(out_of_range.status, ReadbackStatus::Invalid) << "the announcement built the one colour attachment the target states";
    EXPECT_TRUE(out_of_range.values.empty());
    // BUILT IS NOT DRAWN: the image exists, but no pass has written it, so the picture is still NotReady.
    EXPECT_EQ(backend->readColorBuffer(target.get(), 0).status, ReadbackStatus::NotReady) << "nothing has drawn into the announced target yet";

    // A FRAME THAT NAMES IT finds it built: the same question still answers Invalid, and the clear it recorded is
    // readable - the frame built no attachment of its own.
    backend->executeFrame(clearingFrame(target.get(), Colorf(0.0F, 0.0F, 0.0F, 1.0F)));
    const ColorReadback still_out_of_range = backend->readColorBuffer(target.get(), 1);
    EXPECT_EQ(still_out_of_range.status, ReadbackStatus::Invalid) << "the frame did not have to build what the announcement built";
    EXPECT_TRUE(still_out_of_range.values.empty());
    EXPECT_TRUE(backend->readColorBuffer(target.get(), 0).ok()) << "the frame's clear is readable";

    backend->releaseRenderTarget(target.get());
    backend->shutdown();
}

TEST(VkOffscreenTest, AClearedTargetReadsBackAsThatColour)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // M1 IS THE OFF-SCREEN HALF, so the capability says so (a host plans frames against this value).
    EXPECT_TRUE(backend->capabilities().render_targets);
    EXPECT_FALSE(backend->capabilities().presentation) << "the window half is M5";

    const intrusive_ptr<RenderTarget> target = makeTarget();
    const Colorf                      colour{ 0.25F, 0.5F, 0.75F, 1.0F };

    const PresentOutcome outcome = backend->executeFrame(clearingFrame(target.get(), colour));
    EXPECT_EQ(outcome, PresentOutcome::Skipped) << "the device presents nothing yet; drawing into a target is not presenting";

    const ColorReadback read = backend->readColorBuffer(target.get(), 0);
    ASSERT_TRUE(read.ok()) << "readback refused: " << static_cast<int>(read.status);
    const std::vector<std::uint8_t>& pixels = read.values;
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(target->width() * target->height()) * 4U);

    // One UNORM conversion is allowed to round: compare every pixel against the clear colour within one step.
    const auto expected = [](const float value) { return static_cast<int>(value * 255.0F + 0.5F); };
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); index += 4U) {
        const int red   = pixels[index + 0U];
        const int green = pixels[index + 1U];
        const int blue  = pixels[index + 2U];
        const int alpha = pixels[index + 3U];
        const bool ok   = std::abs(red - expected(colour.r)) <= 1 && std::abs(green - expected(colour.g)) <= 1 &&
                        std::abs(blue - expected(colour.b)) <= 1 && std::abs(alpha - expected(colour.a)) <= 1;
        if (!ok) {
            if (mismatches == 0U) {
                std::printf("[vk] first mismatch at pixel %zu: %d,%d,%d,%d (expected %d,%d,%d,%d)\n",
                            index / 4U,
                            red,
                            green,
                            blue,
                            alpha,
                            expected(colour.r),
                            expected(colour.g),
                            expected(colour.b),
                            expected(colour.a));
            }
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0U) << "the picture the pass cleared is not the picture the readback returned";

    // VALIDATION'S VERDICT ON THE WHOLE PATH (create, clear, transition, copy): every driver message this device can
    // produce is reported through its route, so a non-zero count here is a layer complaint about the frame or the
    // readback - not about the picture, which the assertions above already judged.
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "the validation layer reported something while the target was cleared and read back";

    backend->shutdown();
}

TEST(VkOffscreenTest, ATargetThatWasNeverDrawnHasNoPixelsRatherThanBlackOnes)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> target = makeTarget();
    const ColorReadback               unknown_read = backend->readColorBuffer(target.get(), 0);
    EXPECT_EQ(unknown_read.status, ReadbackStatus::NotReady) << "an unknown target is 'nothing to read', not an invalid request";
    EXPECT_TRUE(unknown_read.values.empty());

    // An attachment index this device does not have IS an invalid request - the two refusals stay distinguishable.
    backend->executeFrame(clearingFrame(target.get(), Colorf(0.0F, 0.0F, 0.0F, 1.0F)));
    EXPECT_EQ(backend->readColorBuffer(target.get(), 3).status, ReadbackStatus::Invalid);

    backend->shutdown();
}
