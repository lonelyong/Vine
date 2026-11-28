/**
 * @brief What a surface allows a window's swapchain to BE, resolved from its answers into one value (see
 *        `surfaceSwapchain`).
 *
 * WHY THIS FILE EXISTS. The window path needs a real surface, so `WindowPresentTest` can only exercise the answers THIS
 * machine's surfaces give - every one of them supports colour attachments, prefers an 8-bit sRGB pair and states a
 * current extent. The resolution is a pure function of the surface's answers (review 29), so the synthetic table below
 * pins the branches no desktop produces: the `VK_FORMAT_UNDEFINED` sentinel, a free surface's clamp, the image-count cap,
 * the composite-alpha fallback and the one refusal that means "this surface cannot be drawn into".
 *
 * THE PREMISE OF EVERY CASE IS A SURFACE THAT ALLOWS A WINDOW TARGET: `allowedCapabilities()` states one, and each case
 * changes only the field it is about - so a case that goes red because the defaults stopped being allowed is telling the
 * truth about the fixture, not about its own subject.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <span>

#include <volk.h>

#include <vine/backend_vk/internal/WindowTarget.hpp>

namespace
{

using vn::backend_vk::surfaceSwapchain;
using vn::backend_vk::SurfaceRefusal;
using vn::backend_vk::SurfaceSwapchain;

/// @brief A capability block that allows a window target (`VkSurfaceCapabilitiesKHR` as every desktop states it).
[[nodiscard]] VkSurfaceCapabilitiesKHR allowedCapabilities() noexcept
{
    VkSurfaceCapabilitiesKHR capabilities{};
    capabilities.minImageCount           = 2U;
    capabilities.maxImageCount           = 0U; // no cap
    capabilities.currentExtent           = VkExtent2D{ 1280U, 720U };
    capabilities.minImageExtent          = VkExtent2D{ 1U, 1U };
    capabilities.maxImageExtent          = VkExtent2D{ 16384U, 16384U };
    capabilities.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    capabilities.supportedUsageFlags     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    capabilities.supportedTransforms     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities.currentTransform        = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    return capabilities;
}

/// @brief The 8-bit sRGB pair every desktop surface lists first.
[[nodiscard]] VkSurfaceFormatKHR sRGBPair() noexcept
{
    return VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
}

} // namespace

TEST(VkWindowSurfaceTest, AWindowedSurfaceSuppliesItsOwnExtentAndAnImageCountInsideItsRange)
{
    const VkSurfaceCapabilitiesKHR capabilities = allowedCapabilities();
    const VkSurfaceFormatKHR       formats[]    = { sRGBPair() };

    const SurfaceSwapchain shape = surfaceSwapchain(capabilities, formats, 640U, 480U);

    ASSERT_TRUE(shape.ok()) << "a desktop surface allows a window target";
    // THE SURFACE'S EXTENT WINS while it states one: the presentation engine owns the window's size.
    EXPECT_EQ(shape.extent.width, 1280U);
    EXPECT_EQ(shape.extent.height, 720U);
    // ONE IMAGE PRESENTING WHILE THE NEXT IS DRAWN.
    EXPECT_EQ(shape.image_count, capabilities.minImageCount + 1U);
    EXPECT_EQ(shape.composite_alpha, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    EXPECT_EQ(shape.format.format, VK_FORMAT_B8G8R8A8_SRGB);
}

TEST(VkWindowSurfaceTest, ThePreferredSrgbPairWinsOverTheFirstEntryAndAFirstEntryStillWins)
{
    const VkSurfaceCapabilitiesKHR capabilities = allowedCapabilities();

    // THE SURFACE'S ORDER IS ITS PREFERENCE, narrowed to what the engine wants to decode.
    const VkSurfaceFormatKHR preferred[] = { VkSurfaceFormatKHR{ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
                                             sRGBPair() };
    const SurfaceSwapchain  chosen      = surfaceSwapchain(capabilities, preferred, 0U, 0U);
    ASSERT_TRUE(chosen.ok());
    EXPECT_EQ(chosen.format.format, VK_FORMAT_B8G8R8A8_SRGB);

    // A SURFACE THAT OFFERS ONLY ITS FIRST PAIR STILL PRESENTS, in that pair.
    const VkSurfaceFormatKHR only[] = { VkSurfaceFormatKHR{ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } };
    const SurfaceSwapchain   front  = surfaceSwapchain(capabilities, only, 0U, 0U);
    ASSERT_TRUE(front.ok());
    EXPECT_EQ(front.format.format, VK_FORMAT_R8G8B8A8_UNORM);
}

TEST(VkWindowSurfaceTest, AnUndefinedFormatAnswerBecomesTheClassicPair)
{
    // THE SENTINEL IS "NO PREFERRED FORMAT", NOT A FORMAT (VUID-VkSwapchainCreateInfoKHR-imageFormat-01273): the
    // surface accepts any pair, and the request must name one.
    VkSurfaceCapabilitiesKHR capabilities = allowedCapabilities();
    capabilities.currentExtent            = VkExtent2D{ 64U, 64U };
    const VkSurfaceFormatKHR formats[]    = { VkSurfaceFormatKHR{ VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } };

    const SurfaceSwapchain shape = surfaceSwapchain(capabilities, formats, 0U, 0U);

    ASSERT_TRUE(shape.ok());
    EXPECT_EQ(shape.format.format, VK_FORMAT_B8G8R8A8_UNORM) << "the classic pair, never the sentinel";
    EXPECT_EQ(shape.format.colorSpace, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
}

TEST(VkWindowSurfaceTest, AFreeSurfaceClampsTheAnnouncedSizeIntoTheRangeItStates)
{
    // A FREE SURFACE (`currentExtent` = UINT32_MAX) lets the application choose; the choice must land inside the range.
    VkSurfaceCapabilitiesKHR capabilities = allowedCapabilities();
    capabilities.currentExtent            = VkExtent2D{ std::numeric_limits<std::uint32_t>::max(),
                                                         std::numeric_limits<std::uint32_t>::max() };
    capabilities.minImageExtent           = VkExtent2D{ 320U, 200U };
    capabilities.maxImageExtent           = VkExtent2D{ 4096U, 2048U };
    const VkSurfaceFormatKHR formats[]    = { sRGBPair() };

    const SurfaceSwapchain large = surfaceSwapchain(capabilities, formats, 5000U, 100U);
    ASSERT_TRUE(large.ok());
    EXPECT_EQ(large.extent.width, 4096U) << "clamped down to the surface's maximum";
    EXPECT_EQ(large.extent.height, 200U) << "clamped up to the surface's minimum";

    // AN UNANNOUNCED SIZE IS THE FALLBACK (64x64), clamped the same way.
    const SurfaceSwapchain unannounced = surfaceSwapchain(capabilities, formats, 0U, 0U);
    ASSERT_TRUE(unannounced.ok());
    EXPECT_EQ(unannounced.extent.width, 320U);
    EXPECT_EQ(unannounced.extent.height, 200U);
}

TEST(VkWindowSurfaceTest, TheImageCountStaysInsideTheRangeTheSurfaceStates)
{
    const VkSurfaceFormatKHR formats[] = { sRGBPair() };

    VkSurfaceCapabilitiesKHR capped  = allowedCapabilities();
    capped.minImageCount             = 2U;
    capped.maxImageCount             = 2U;
    const SurfaceSwapchain   atMost  = surfaceSwapchain(capped, formats, 0U, 0U);
    ASSERT_TRUE(atMost.ok());
    EXPECT_EQ(atMost.image_count, 2U) << "the maximum wins over minImageCount + 1";

    VkSurfaceCapabilitiesKHR uncapped = allowedCapabilities();
    uncapped.minImageCount            = 3U;
    uncapped.maxImageCount            = 0U; // no cap, so the minimum plus one is the answer
    const SurfaceSwapchain   oneMore  = surfaceSwapchain(uncapped, formats, 0U, 0U);
    ASSERT_TRUE(oneMore.ok());
    EXPECT_EQ(oneMore.image_count, 4U);
}

TEST(VkWindowSurfaceTest, ACompositeModeTheSurfaceStatesBeatsOpaqueWhenOpaqueIsMissing)
{
    VkSurfaceCapabilitiesKHR capabilities = allowedCapabilities();
    capabilities.supportedCompositeAlpha =
        static_cast<VkCompositeAlphaFlagsKHR>(VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR);
    const VkSurfaceFormatKHR formats[] = { sRGBPair() };

    const SurfaceSwapchain shape = surfaceSwapchain(capabilities, formats, 0U, 0U);

    ASSERT_TRUE(shape.ok()) << "the picture is the same, so a stated mode beats refusing";
    EXPECT_EQ(shape.composite_alpha, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR);
}

TEST(VkWindowSurfaceTest, ASurfaceWhoseImagesCannotBeDrawnIntoIsRefusedWithItsCause)
{
    // EVERY WINDOW PASS DRAWS INTO THE IMAGE: a surface whose images are not colour attachments has no window target,
    // and the refusal names that, instead of a `vkCreateSwapchainKHR` failure that names nothing.
    VkSurfaceCapabilitiesKHR capabilities = allowedCapabilities();
    capabilities.supportedUsageFlags     = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkSurfaceFormatKHR formats[]   = { sRGBPair() };

    const SurfaceSwapchain shape = surfaceSwapchain(capabilities, formats, 0U, 0U);

    EXPECT_FALSE(shape.ok());
    EXPECT_EQ(shape.refusal, SurfaceRefusal::NoColorUsage);

    // THE OPPOSITE PREMISE, for contrast: the same block with the bit set allows one.
    VkSurfaceCapabilitiesKHR allowed  = allowedCapabilities();
    const SurfaceSwapchain   accepted = surfaceSwapchain(allowed, formats, 0U, 0U);
    EXPECT_TRUE(accepted.ok());
}

TEST(VkWindowSurfaceTest, AZeroExtentAndAnEmptyFormatListAreTheOtherTwoRefusals)
{
    const VkSurfaceFormatKHR formats[] = { sRGBPair() };

    VkSurfaceCapabilitiesKHR minimised = allowedCapabilities();
    minimised.currentExtent            = VkExtent2D{ 0U, 0U };
    const SurfaceSwapchain zero        = surfaceSwapchain(minimised, formats, 0U, 0U);
    EXPECT_FALSE(zero.ok());
    EXPECT_EQ(zero.refusal, SurfaceRefusal::ZeroExtent);

    const SurfaceSwapchain none = surfaceSwapchain(allowedCapabilities(), std::span<const VkSurfaceFormatKHR>{}, 0U, 0U);
    EXPECT_FALSE(none.ok());
    EXPECT_EQ(none.refusal, SurfaceRefusal::NoFormats);
}
