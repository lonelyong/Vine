/**
 * @brief The device floor is ONE predicate over what a device OFFERS, so the choice is testable without a driver.
 *
 * WHY THIS FILE EXISTS. `K3` of review 9 found device selection ranking BEFORE it filtered: the best-ranked physical
 * device was picked, and only then were the API version, the 1.3 features and the EDS3 switches checked - each
 * failure reporting and shutting the session down, so a machine whose fastest device missed a floor lost the session
 * while a capable device sat in the list. The fix is a filter (`candidateRefusal`) applied to every candidate before
 * the ranking, and this file pins the predicate itself: two real GPUs are not available in CI, and a fabricated
 * `DeviceOffer` is what makes "the capable second device survives" a case instead of a hope on the test machine.
 *
 * THE SECOND DECISION PINNED HERE IS THE QUEUE CLOCK (review 30): `queueWritesTimestamps` answers from the CHOSEN
 * queue family's `timestampValidBits` and the device's period, and no CI machine has the shape that separates it from
 * the device-wide `timestampComputeAndGraphics` floor - so the rule is pinned over fabricated answers, like the floor.
 */

#include <gtest/gtest.h>

#include <vine/backend_vk/internal/DeviceChoice.hpp>

namespace
{

using vn::backend_vk::candidateRefusal;
using vn::backend_vk::DeviceOffer;
using vn::backend_vk::FloorRefusal;
using vn::backend_vk::queueWritesTimestamps;

/// @brief The offer of a device that meets every floor (each case below changes exactly one fact).
[[nodiscard]] DeviceOffer capableOffer()
{
    DeviceOffer offer;
    offer.api_version             = VK_API_VERSION_1_3;
    offer.synchronization2        = true;
    offer.dynamic_rendering       = true;
    offer.extended_dynamic_state3 = true;
    offer.polygon_mode            = true;
    offer.color_blend_enable      = true;
    offer.color_blend_equation    = true;
    return offer;
}

} // namespace

TEST(VkDeviceChoiceTest, ACapableDeviceIsRefusedByNoFloor)
{
    EXPECT_EQ(candidateRefusal(capableOffer()), FloorRefusal::None);
}

TEST(VkDeviceChoiceTest, TheVersionFloorIsNamedFirst)
{
    // A 1.2 device is described by its VERSION even when the feature structs would fail too: an older device cannot be
    // asked for the 1.3 features at all, so the sentence a host reads names the fact that has to change first.
    DeviceOffer offer = capableOffer();
    offer.api_version      = VK_API_VERSION_1_2;
    offer.synchronization2 = false;
    EXPECT_EQ(candidateRefusal(offer), FloorRefusal::ApiVersion);
}

TEST(VkDeviceChoiceTest, A13DeviceWithoutTheCoreFeaturesIsRefusedForTheFeature)
{
    // A VERSION IS NOT A FEATURE (`T11`): a 1.3 device that does not offer `synchronization2` or `dynamicRendering`
    // is refused with its own refusal, and either of the two alone is enough.
    DeviceOffer no_sync2 = capableOffer();
    no_sync2.synchronization2 = false;
    EXPECT_EQ(candidateRefusal(no_sync2), FloorRefusal::CoreFeatures);

    DeviceOffer no_dynamic_rendering = capableOffer();
    no_dynamic_rendering.dynamic_rendering = false;
    EXPECT_EQ(candidateRefusal(no_dynamic_rendering), FloorRefusal::CoreFeatures);
}

TEST(VkDeviceChoiceTest, TheRasterFloorNeedsTheExtensionAndEverySwitch)
{
    // THE RASTER FLOOR (`T54`): the extension without a switch is a device that would have to bake polygon mode or the
    // blend equation silently, so all three switches are part of the same floor.
    DeviceOffer no_extension = capableOffer();
    no_extension.extended_dynamic_state3 = false;
    EXPECT_EQ(candidateRefusal(no_extension), FloorRefusal::DynamicState3);

    DeviceOffer no_polygon_mode = capableOffer();
    no_polygon_mode.polygon_mode = false;
    EXPECT_EQ(candidateRefusal(no_polygon_mode), FloorRefusal::DynamicState3);

    DeviceOffer no_blend_enable = capableOffer();
    no_blend_enable.color_blend_enable = false;
    EXPECT_EQ(candidateRefusal(no_blend_enable), FloorRefusal::DynamicState3);

    DeviceOffer no_blend_equation = capableOffer();
    no_blend_equation.color_blend_equation = false;
    EXPECT_EQ(candidateRefusal(no_blend_equation), FloorRefusal::DynamicState3);
}

TEST(VkDeviceChoiceTest, ACapableSecondDeviceSurvivesTheFilterTheRankedFirstOneDoesNot)
{
    // THE TWO-GPU MACHINE (`K3`, review 9), stated as the filter sees it: the device the ranking would prefer misses a
    // floor and the one below it does not. The selection loop ranks only the survivors, so the second offer's `None`
    // is what makes it the chosen device - before the fix it was never looked at.
    DeviceOffer fastest;
    fastest.api_version             = VK_API_VERSION_1_3;
    fastest.extended_dynamic_state3 = true;
    fastest.synchronization2        = false; // one driver revision behind: no synchronization2
    fastest.dynamic_rendering       = false;
    EXPECT_EQ(candidateRefusal(fastest), FloorRefusal::CoreFeatures) << "the top-ranked device must be filtered out";

    const DeviceOffer second = capableOffer();
    EXPECT_EQ(candidateRefusal(second), FloorRefusal::None) << "the capable second device must survive the filter";
}

TEST(VkDeviceChoiceTest, TheFamilysZeroBitsAnswerNoClockWhateverTheDeviceReports)
{
    // THE DEVICE-WIDE BOOL IS NOT THIS ANSWER: `timestampComputeAndGraphics` may be true with this family's count at 0
    // (it only promises the families that carry GRAPHICS or COMPUTE - and 0 is the family's "no timestamps" answer).
    EXPECT_FALSE(queueWritesTimestamps(0U, 1.0F));
}

TEST(VkDeviceChoiceTest, TheFamilysLowestLegalAnswerIsAUsableClock)
{
    // 36 IS THE SMALLEST COUNT VULKAN DEFINES ("36 to 64, or 0"): a family that answers it can time a frame, which is
    // the case the old device-wide gate refused whenever `timestampComputeAndGraphics` was false.
    EXPECT_TRUE(queueWritesTimestamps(36U, 1.0F));
    EXPECT_TRUE(queueWritesTimestamps(64U, 1.0F));
}

TEST(VkDeviceChoiceTest, ABitCountVulkanDoesNotDefineIsNotAClock)
{
    // THE RULE IS THE SPEC'S FLOOR, NOT "> 0": a count between 1 and 35 is an answer Vulkan does not define, and a
    // clock that wrapped there would make the frame's delta arithmetic read as a negative or a tiny time.
    EXPECT_FALSE(queueWritesTimestamps(20U, 1.0F));
}

TEST(VkDeviceChoiceTest, ATickWithoutAPeriodIsNotAClock)
{
    // Ticks become nanoseconds through `timestampPeriod`: valid bits alone are not a clock.
    EXPECT_FALSE(queueWritesTimestamps(36U, 0.0F));
}
