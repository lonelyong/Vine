/**
 * @brief The one escape door: `vkHandles()` answers a `vk` device's real Vulkan objects and nothing else.
 *
 * What this pins (see `.ai/research/graphics-2026-11-13-vk-exposure-boundary.md` §4.5, §5.2 and §8): the door is a FREE
 * function named after the backend - not a verb on `RenderDevice` - and its answer follows the device's lifecycle: the
 * empty struct before `initialize()`, real handles while the device runs, the empty struct again after `shutdown()`.
 *
 * THE "OTHER BACKEND" ANSWER IS NOT TESTED HERE, AND DOES NOT NEED TO BE. `vkHandles()` is one `dynamic_cast` to this
 * plugin's `Device` and answers `{}` when it fails, so "a device of another backend answers the empty struct" is true
 * by construction rather than by a case: a stub device written here would only prove that the stub is not a `Device`,
 * which is the same fact `dynamic_cast` already states. What a test can pin is that a REAL `vk` device answers real
 * handles, and that is what the case below does.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <volk.h>

#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/backend_vk/internal/Device.hpp>
#include <vine/backend_vk/support/Handles.hpp>

using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::RenderBackendRegistry;
using vn::backend_vk::Device;
using vn::backend_vk::Handles;
using vn::backend_vk::vkHandles;

TEST(VkHandlesTest, TheDoorFollowsTheDeviceLifecycle)
{
    // A device asked for BY NAME, exactly as a host or a tool would: the door takes the neutral `RenderDevice`, so
    // nothing here names the plugin's device type to reach the handles.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);

    // BEFORE: nothing exists, so the door answers the empty struct - the same answer a device of another backend gets.
    const Handles before = vkHandles(*backend);
    EXPECT_EQ(before.instance, VK_NULL_HANDLE);
    EXPECT_EQ(before.physical, VK_NULL_HANDLE);
    EXPECT_EQ(before.device, VK_NULL_HANDLE);
    EXPECT_EQ(before.graphics, VK_NULL_HANDLE);
    EXPECT_EQ(before.graphics_family, 0U);

    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // UP: every handle is real, and the family is a number the DEVICE reported - asked of the physical device itself,
    // so a sentinel that merely looks plausible would fail here.
    const Handles up = vkHandles(*backend);
    EXPECT_NE(up.instance, VK_NULL_HANDLE);
    EXPECT_NE(up.physical, VK_NULL_HANDLE);
    EXPECT_NE(up.device, VK_NULL_HANDLE);
    EXPECT_NE(up.graphics, VK_NULL_HANDLE);
    EXPECT_NE(up.graphics_family, VK_QUEUE_FAMILY_IGNORED);
    std::uint32_t family_count = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties(up.physical, &family_count, nullptr);
    ASSERT_GT(family_count, 0U) << "a physical device always reports at least one queue family";
    EXPECT_LT(up.graphics_family, family_count);

    // AND IT IS THE SAME ANSWER THE PLUGIN SEES: the door is a cast onto the internal accessor, not a second copy of
    // the truth that could drift from it.
    auto* device = dynamic_cast<Device*>(backend.get());
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->handles().instance, up.instance);
    EXPECT_EQ(device->handles().device, up.device);
    EXPECT_EQ(device->handles().graphics, up.graphics);
    EXPECT_EQ(device->handles().graphics_family, up.graphics_family);

    // DOWN: `shutdown()` destroys them, so the door must not answer dangling handles - a tool that kept the previous
    // value is the caller's own use-after-free, and this door cannot pretend otherwise.
    backend->shutdown();
    const Handles after = vkHandles(*backend);
    EXPECT_EQ(after.instance, VK_NULL_HANDLE);
    EXPECT_EQ(after.physical, VK_NULL_HANDLE);
    EXPECT_EQ(after.device, VK_NULL_HANDLE);
    EXPECT_EQ(after.graphics, VK_NULL_HANDLE);
    EXPECT_EQ(after.graphics_family, 0U);

    // UP AGAIN: the door reads the device, it does not remember a session - a backend whose surface is recreated must
    // answer the new handles, not the ones it answered before.
    ASSERT_TRUE(backend->initialize()) << "a shut-down device must be initializable again";
    const Handles again = vkHandles(*backend);
    EXPECT_NE(again.instance, VK_NULL_HANDLE);
    EXPECT_NE(again.device, VK_NULL_HANDLE);
    EXPECT_NE(again.graphics, VK_NULL_HANDLE);
    backend->shutdown();
}
