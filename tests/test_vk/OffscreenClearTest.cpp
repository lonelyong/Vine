/**
 * @brief M1's acceptance: a pass that clears its off-screen target leaves exactly that colour, and the readback says so.
 *
 * What this pins, and why each half matters:
 *   - the frame value is enough to run a pass: `PassDraw::target` + `PassDraw::clear` are the whole input, so a backend
 *     needs nothing from a live `RenderPass` to clear a target (see FrameDescription's note);
 *   - the clear colour travels as LINEAR values (`ClearPolicy::color`) and comes back as packed RGBA8, which is the
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
#include <vine/graphics/RenderTarget.hpp>

using vn::Colorf;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::FrameDescription;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::ReadbackResult;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderTarget;
using vn::intrusive_ptr;

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
    entry.pass = nullptr; // M1 keys nothing by the pass identity; M2 will.
    entry.order = 0;

    PassDraw draw;
    draw.target = target;
    draw.clear  = ClearPolicy{};
    draw.clear->color = colour;
    entry.draw = draw;

    frame.passes.push_back(std::move(entry));
    return frame;
}

} // namespace

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

    std::vector<std::uint8_t> pixels;
    ReadbackResult            why = ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels, &why)) << "readback refused: " << static_cast<int>(why);
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
    std::vector<std::uint8_t>         pixels;
    ReadbackResult                    why = ReadbackResult::Ok;
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 0, pixels, &why));
    EXPECT_EQ(why, ReadbackResult::NotReady) << "an unknown target is 'nothing to read', not an invalid request";
    EXPECT_TRUE(pixels.empty());

    // An attachment index this device does not have IS an invalid request - the two refusals stay distinguishable.
    backend->executeFrame(clearingFrame(target.get(), Colorf(0.0F, 0.0F, 0.0F, 1.0F)));
    ReadbackResult wrong = ReadbackResult::Ok;
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 3, pixels, &wrong));
    EXPECT_EQ(wrong, ReadbackResult::Invalid);

    backend->shutdown();
}
