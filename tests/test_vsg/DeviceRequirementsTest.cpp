/**
 * @brief What the backend requires of a device: the Vulkan floor, as a device-free rule.
 *
 * The backend is allowed to rely on core-1.4 behaviour (dynamic rendering's local read, maintenance5/6,
 * host image copy, on top of the 1.3 states it already delivers dynamically — see VsgDynamicState.hpp for
 * which of those are 1.3 core and which need an extension), so a device below it is refused with a reason
 * instead of being served on the subset of the contract that happens to work. Both the floor and the comparison are values, so the
 * policy is pinned here rather than only inside a session that needs a device to exist:
 *
 *  * the version that counts is the DEVICE's (`VkPhysicalDeviceProperties::apiVersion`) — the loader
 *    grants an instance version of its own choosing, and `WindowTraits::defaults()` already asks for the
 *    highest it supports, so an instance can be newer than the device behind it;
 *  * the comparison is a major/minor one. The packed integer form happens to sort the same way for real
 *    Vulkan versions (variant 0), and one case below pins that the two agree — so a reader who wonders
 *    whether the packing was assumed can see it was not.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <vine/vsg/VsgBackendUtility.hpp>

using vine::vsg::detail::kRequiredVulkanVersion;
using vine::vsg::detail::supportsRequiredVulkanVersion;

TEST(DeviceRequirementsTest, TheFloorIsVulkan14)
{
    EXPECT_EQ(VK_API_VERSION_MAJOR(kRequiredVulkanVersion), 1u);
    EXPECT_EQ(VK_API_VERSION_MINOR(kRequiredVulkanVersion), 4u);
}

TEST(DeviceRequirementsTest, DevicesBelowTheFloorAreRefused)
{
    EXPECT_FALSE(supportsRequiredVulkanVersion(VK_API_VERSION_1_0));
    EXPECT_FALSE(supportsRequiredVulkanVersion(VK_API_VERSION_1_1));
    EXPECT_FALSE(supportsRequiredVulkanVersion(VK_API_VERSION_1_2));
    EXPECT_FALSE(supportsRequiredVulkanVersion(VK_API_VERSION_1_3));
    // A driver that packs a patch level into 1.3 is still 1.3.
    EXPECT_FALSE(supportsRequiredVulkanVersion(VK_MAKE_API_VERSION(0, 1, 3, 999)));
}

TEST(DeviceRequirementsTest, TheFloorItselfAndEverythingNewerAreServed)
{
    // Exactly the floor is enough: what the backend relies on is part of 1.4, not of a patch release of it.
    EXPECT_TRUE(supportsRequiredVulkanVersion(VK_API_VERSION_1_4));
    // What a real device reports (the patch field carries the driver's own numbering).
    EXPECT_TRUE(supportsRequiredVulkanVersion(VK_MAKE_API_VERSION(0, 1, 4, 335)));
    EXPECT_TRUE(supportsRequiredVulkanVersion(VK_MAKE_API_VERSION(0, 1, 5, 0)));
}

TEST(DeviceRequirementsTest, TheMajorMinorRuleAgreesWithThePackedOrderForRealVersions)
{
    // Real Vulkan versions are `VK_MAKE_API_VERSION(0, major, minor, patch)`, which sorts exactly like
    // (major, minor, patch) — so the explicit comparison above and a raw `>=` on the packed value answer
    // the same thing. Pinned so a future edit that switches to the packed form (or the reverse) is a
    // provable no-op rather than a silent change of policy.
    for (std::uint32_t minor = 0; minor <= 4; ++minor) {
        for (std::uint32_t patch : { 0u, 1u, 280u }) {
            const std::uint32_t version = VK_MAKE_API_VERSION(0, 1, minor, patch);
            EXPECT_EQ(supportsRequiredVulkanVersion(version), version >= kRequiredVulkanVersion) << version;
        }
    }
}
