/**
 * @brief M0 of the direct-Vulkan backend: the seam works before anything is drawn.
 *
 * What M0 claims (`.ai/design/vk-backend.md` 2.1 for the two halves, 2.10 for the test contract): the factory registers
 * under its own name, the device comes up and goes down cleanly and idempotently, and its answers describe what it can
 * really serve rather than what the GPU could do. Two of those are device-free (registration, the closed escape door);
 * the lifecycle case skips on a machine with no Vulkan device, which is how every device case in this repository
 * behaves.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/backend_vk/internal/Device.hpp>
#include <vine/backend_vk/internal/TargetObjects.hpp>
#include <vine/backend_vk/shell/BackendFactory.hpp>

using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::RenderBackendRegistry;
using vn::backend_vk::Device;
using vn::backend_vk::Handles;

// This test compiles the backend's sources into its own module rather than loading the plugin, so it states its build
// identity exactly as the plugin does (see VN_RENDER_BACKEND_ABI_VERSION): the factory's static registration reports
// the revision of THIS binary, and the registry checks it like any other module's.
VN_DECLARE_RENDER_BACKEND_ABI()

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
    // The handles are the one place Vulkan may leave this plugin (`.ai/design/vk-backend.md` 2.1): they answer the
    // truth - nothing exists before the device is brought up - instead of dangling.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    auto* device = dynamic_cast<Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const Handles handles = device->handles();
    EXPECT_EQ(handles.instance, VK_NULL_HANDLE);
    EXPECT_EQ(handles.physical, VK_NULL_HANDLE);
    EXPECT_EQ(handles.device, VK_NULL_HANDLE);
    EXPECT_EQ(handles.graphics, VK_NULL_HANDLE);
    EXPECT_EQ(handles.graphics_family, 0U);
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
    const Handles handles = device->handles();
    EXPECT_NE(handles.instance, VK_NULL_HANDLE);
    EXPECT_NE(handles.physical, VK_NULL_HANDLE);
    EXPECT_NE(handles.device, VK_NULL_HANDLE);
    EXPECT_NE(handles.graphics, VK_NULL_HANDLE);
    std::printf("[vk] device=%p queue_family=%u validation=%s\n",
                static_cast<const void*>(handles.device),
                handles.graphics_family,
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
    // THE SAMPLE LIMIT IS THE DEVICE'S ANSWER (see Capabilities::limits.max_samples): M11 builds multisampled targets, so the
    // number is what the colour and depth attachments both support, and a device that offers none answers 1.
    EXPECT_GE(capabilities.limits.max_samples, 1U);

    // THE FEATURE BITS ARE ANSWERS ABOUT THIS SESSION, and each is checked against the limit that has to travel beside
    // it: "anisotropy is available" next to a factor of 1 would advertise a quality setting that cannot do anything,
    // and a line-width range below 1 would be no range at all.
    EXPECT_EQ(capabilities.features.supports(vn::graphics::Features::Feature::AnisotropicFiltering), capabilities.limits.max_anisotropy > 1.0F);
    EXPECT_GE(capabilities.limits.max_anisotropy, 1.0F);
    EXPECT_GE(capabilities.limits.max_line_width, 1.0F);
    // THE POOL TELLS ITS OWN STORY (see Feature::MemoryUsage): this backend allocates the device memory itself, so the
    // figure a host reads is a measurement rather than an estimate.
    EXPECT_TRUE(capabilities.features.supports(vn::graphics::Features::Feature::MemoryUsage))
        << "the pool's bookkeeping is this backend's own";
    // THE DEVICE'S OWN COLOUR SLOT COUNT: Vulkan guarantees at least 4, and an MRT pass may use every one.
    EXPECT_GE(capabilities.limits.max_color_attachments, 4U);
    // THE EXTENT CEILING IS THE SMALLEST OF THE DEVICE'S FIVE DIMENSION ANSWERS (see
    // Capabilities::limits.max_target_extent): Vulkan guarantees 4096 for each, so no conformant device answers less.
    EXPECT_GE(capabilities.limits.max_target_extent, 4096U);

    // IDEMPOTENT, and back to a fresh object: the SDK requires a device whose surface is recreated to take this path.
    backend->shutdown();
    EXPECT_FALSE(backend->initialized());
    backend->shutdown();
    EXPECT_FALSE(backend->initialized());
    EXPECT_EQ(device->handles().device, VK_NULL_HANDLE) << "shutdown releases the handles, not only the flag";

    EXPECT_TRUE(backend->initialize()) << "a shut-down device must be initializable again";
    EXPECT_TRUE(backend->initialized());
    backend->shutdown();
    EXPECT_FALSE(backend->initialized());
}

TEST(VkBackendTest, AColourFormatIsATargetOnlyWhenTheDeviceBacksEveryUse)
{
    // THE THREE USES `buildTarget` GIVES A COLOUR IMAGE - drawn into, sampled by a later pass, copied by a readback -
    // are the three bits a format must offer, so the answer is all-or-nothing: extra support is never a refusal, one
    // missing bit always is.
    const std::uint32_t every_use = vn::backend_vk::colorTargetFeatures();
    EXPECT_TRUE(vn::backend_vk::supportsColorTarget(every_use));
    EXPECT_TRUE(vn::backend_vk::supportsColorTarget(every_use | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT));
    EXPECT_FALSE(vn::backend_vk::supportsColorTarget(every_use & ~VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    EXPECT_FALSE(vn::backend_vk::supportsColorTarget(every_use & ~VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
    EXPECT_FALSE(vn::backend_vk::supportsColorTarget(every_use & ~VK_FORMAT_FEATURE_TRANSFER_SRC_BIT));
    EXPECT_FALSE(vn::backend_vk::supportsColorTarget(0U));

    // NO DEVICE, NOTHING BUILDABLE (the mask answers 0 rather than throwing or guessing).
    EXPECT_EQ(vn::backend_vk::buildableColorFormats(VK_NULL_HANDLE), 0U);

    // THE MAPPING IS THE VOCABULARY'S RULE, value by value: the channel count and the family are the Vulkan format's.
    using ColorFormat = vn::graphics::RenderTarget::ColorFormat;
    EXPECT_EQ(vn::backend_vk::colorFormatOf(ColorFormat::R8), VK_FORMAT_R8_UNORM);
    EXPECT_EQ(vn::backend_vk::colorFormatOf(ColorFormat::RG8), VK_FORMAT_R8G8_UNORM);
    EXPECT_EQ(vn::backend_vk::colorFormatOf(ColorFormat::R16F), VK_FORMAT_R16_SFLOAT);
    EXPECT_EQ(vn::backend_vk::colorFormatOf(ColorFormat::RG16F), VK_FORMAT_R16G16_SFLOAT);
    EXPECT_EQ(vn::backend_vk::colorFormatOf(ColorFormat::R32F), VK_FORMAT_R32_SFLOAT);
    EXPECT_EQ(vn::backend_vk::colorFormatOf(ColorFormat::RG32F), VK_FORMAT_R32G32_SFLOAT);
    // ... and EVERY value has a defined format, which is what lets a cleared bit in `buildableColorFormats` mean "this
    // device does not back it" rather than hidden behind an unmapped value.
    for (const ColorFormat format : vn::backend_vk::kColorFormats) {
        EXPECT_NE(vn::backend_vk::colorFormatOf(format), VK_FORMAT_UNDEFINED) << vn::backend_vk::colorFormatName(format);
    }

    // THE NAMES ARE THE HEADER'S SPELLINGS: a diagnostic a host reads names the value it writes.
    EXPECT_EQ(vn::backend_vk::colorFormatName(ColorFormat::RGBA8), "RGBA8");
    EXPECT_EQ(vn::backend_vk::colorFormatName(ColorFormat::R8), "R8");
    EXPECT_EQ(vn::backend_vk::colorFormatName(ColorFormat::RG16F), "RG16F");
    EXPECT_EQ(vn::backend_vk::colorFormatName(ColorFormat::RG32F), "RG32F");
}
