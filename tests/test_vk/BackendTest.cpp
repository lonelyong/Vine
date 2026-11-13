/**
 * @brief M0 of the direct-Vulkan backend: the seam works before anything is drawn.
 *
 * What M0 claims (see `.ai/research/graphics-2026-11-13-gpu-resource-abstraction.md` §8.3): the factory registers
 * under its own name, the device comes up and goes down cleanly and idempotently, and its answers describe what it can
 * really serve rather than what the GPU could do. Two of those are device-free (registration, the closed escape door);
 * the lifecycle case skips on a machine with no Vulkan device, which is how every device case in this repository
 * behaves.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/backend_vk/internal/Device.hpp>
#include <vine/backend_vk/shell/BackendFactory.hpp>

using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::RenderBackendRegistry;
using vn::backend_vk::Device;

TEST(VkBackendTest, TheFactoryRegistersItselfByName)
{
    // WHAT THIS PINS. "A backend is created by NAME" is the whole reason the seam exists: a host (and the plugin
    // manager) must be able to ask for "vk" without a compile-time dependency on this module.
    const RenderBackendRegistry& registry = RenderBackendRegistry::instance();
    EXPECT_TRUE(registry.has(u8"vk"));

    auto backend = registry.create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr) << "the registered factory must answer a device";

    const std::vector<RenderBackendRegistry::Entry> entries = registry.entries();
    const auto factory = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.info.name == u8"vk";
    });
    ASSERT_NE(factory, entries.end());
    EXPECT_EQ(factory->info.api_flags, vn::graphics::RenderApi::Vulkan);
    EXPECT_NE(factory->info.display_name.find(u8"vk"), vn::String::npos) << "a host shows this name in a menu";
}

TEST(VkBackendTest, TheEscapeDoorIsClosedBeforeInitialize)
{
    // The handles are the one place Vulkan may leave this plugin (see
    // .ai/research/graphics-2026-11-13-vk-exposure-boundary.md): they answer the truth - nothing exists before the
    // device is brought up - instead of dangling.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    auto* device = dynamic_cast<Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const Device::Handles handles = device->handles();
    EXPECT_EQ(handles.instance, 0U);
    EXPECT_EQ(handles.physical_device, 0U);
    EXPECT_EQ(handles.device, 0U);
    EXPECT_EQ(handles.queue, 0U);
    EXPECT_FALSE(handles.validation);
    EXPECT_FALSE(backend->initialized());
}

TEST(VkBackendTest, TheDeviceComesUpAndGoesDownCleanly)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);

    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    EXPECT_TRUE(backend->initialized());

    auto* device = dynamic_cast<Device*>(backend.get());
    ASSERT_NE(device, nullptr);
    const Device::Handles handles = device->handles();
    EXPECT_NE(handles.instance, 0U);
    EXPECT_NE(handles.physical_device, 0U);
    EXPECT_NE(handles.device, 0U);
    EXPECT_NE(handles.queue, 0U);
    std::printf("[vk] device=%llu validation=%s\n",
                static_cast<unsigned long long>(handles.device),
                handles.validation ? "on" : "off (layer not installed)");
    std::fflush(stdout);

    // M0'S EVIDENCE LINE: coming up is not enough, it has to come up CLEAN. Every message the validation layer emits is
    // reported through this device's route (category InitFailed at M0, see Device::reportDriverMessage), so a non-zero
    // count here is the layer saying something about the instance or the device we just built.
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "the validation layer reported something while the instance and device were built";

    // WHAT IT ADVERTISES IS WHAT IT SERVES. M1 builds off-screen colour targets and still presents nothing, so a host
    // must not plan a window against it - the capability value, not the device's abilities, is the contract.
    const vn::graphics::Capabilities capabilities = backend->capabilities();
    EXPECT_TRUE(capabilities.render_targets) << "M1 builds the off-screen half";
    EXPECT_FALSE(capabilities.presentation) << "M5 brings up the window half";
    // THE SAMPLE LIMIT IS THE DEVICE'S ANSWER (see Capabilities::max_samples): M11 builds multisampled targets, so the
    // number is what the colour and depth attachments both support, and a device that offers none answers 1.
    EXPECT_GE(capabilities.max_samples, 1U);

    // IDEMPOTENT, and back to a fresh object: the SDK requires a device whose surface is recreated to take this path.
    backend->shutdown();
    EXPECT_FALSE(backend->initialized());
    backend->shutdown();
    EXPECT_FALSE(backend->initialized());
    EXPECT_EQ(device->handles().device, 0U) << "shutdown releases the handles, not only the flag";

    EXPECT_TRUE(backend->initialize()) << "a shut-down device must be initializable again";
    EXPECT_TRUE(backend->initialized());
    backend->shutdown();
    EXPECT_FALSE(backend->initialized());
}
