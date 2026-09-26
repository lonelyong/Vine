#include <vine/vsg/api/DeviceProbe.hpp>

#include <utility>

#include <vsg/vk/Instance.h>
#include <vsg/vk/PhysicalDevice.h>

VN_VSG_NS_BEGIN

namespace api
{

namespace
{

using core::DeviceFacts;
using core::DeviceFeature;

/// @brief Translates the driver's feature answers into the facts the core's policy checks.
///
/// The extension-backed flags are collected through a chain (their own feature structs), because that is
/// where those answers live; a driver that does not know the structs leaves them as they were handed in
/// (zeroed here), which the policy then reads as "not present" - the honest reading.
void noteFeatures(const VkPhysicalDeviceFeatures& core_features,
                  const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT& eds2,
                  const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT& eds3, DeviceFacts& facts) noexcept
{
    if (core_features.fillModeNonSolid == VK_TRUE)
    {
        facts.note(DeviceFeature::FillModeNonSolid);
    }
    if (core_features.independentBlend == VK_TRUE)
    {
        facts.note(DeviceFeature::IndependentBlend);
    }
    if (core_features.samplerAnisotropy == VK_TRUE)
    {
        facts.note(DeviceFeature::SamplerAnisotropy);
    }
    if (eds2.extendedDynamicState2 == VK_TRUE)
    {
        facts.note(DeviceFeature::ExtendedDynamicState2);
    }
    if (eds3.extendedDynamicState3PolygonMode == VK_TRUE)
    {
        facts.note(DeviceFeature::ExtendedDynamicState3PolygonMode);
    }
    if (eds3.extendedDynamicState3ColorBlendEnable == VK_TRUE)
    {
        facts.note(DeviceFeature::ExtendedDynamicState3ColorBlendEnable);
    }
    if (eds3.extendedDynamicState3ColorBlendEquation == VK_TRUE)
    {
        facts.note(DeviceFeature::ExtendedDynamicState3ColorBlendEquation);
    }
}

}  // namespace

/// @brief Fills one device's facts (its version and the features the policy asks about).
ProbedDevice describePhysicalDevice(::vsg::PhysicalDevice& device)
{
    const VkPhysicalDeviceProperties& properties = device.getProperties();

    // The extension-backed flags come from their own feature structs (that is where those answers live); a
    // driver that does not know a struct leaves it zeroed, which the policy then reads as "not present" -
    // the honest reading.
    const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 =
        device.getFeatures<VkPhysicalDeviceExtendedDynamicState2FeaturesEXT,
                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT>();
    const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 =
        device.getFeatures<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT,
                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT>();

    ProbedDevice probed;
    probed.facts.api_version = properties.apiVersion;
    noteFeatures(device.getFeatures(), eds2, eds3, probed.facts);
    // Availability, not enablement: what the device OFFERS is what the probe can answer, and it is the half
    // that decides whether the device may host a session at all (see core::satisfiesRequirements).
    if (device.supportsDeviceExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME))
    {
        probed.facts.note(core::DeviceExtension::ExtendedDynamicState2);
    }
    if (device.supportsDeviceExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME))
    {
        probed.facts.note(core::DeviceExtension::ExtendedDynamicState3);
    }
    // A driver reports its device name as UTF-8 bytes in a char[], which is exactly the claim
    // String::fromUtf8() takes over: the bytes are copied as they are, up to the driver's NUL.
    probed.name   = vn::String::fromUtf8(properties.deviceName);
    probed.usable = core::satisfiesRequirements(probed.facts);
    return probed;
}

std::size_t ProbeResult::usableCount() const noexcept
{
    std::size_t count = 0;
    for (const ProbedDevice& device : devices)
    {
        if (device.usable)
        {
            ++count;
        }
    }
    return count;
}

ProbeResult probePhysicalDevices()
{
    ProbeResult result;

    // The instance is created with the versions the loader may grant, highest first: asking for a version a
    // loader does not have fails the instance outright, and the policy is checked per DEVICE below anyway.
    ::vsg::ref_ptr<::vsg::Instance> instance;
    for (const std::uint32_t version : { core::makeApiVersion(1, 4, 0), core::makeApiVersion(1, 3, 0),
                                         core::makeApiVersion(1, 2, 0), core::makeApiVersion(1, 1, 0),
                                         core::makeApiVersion(1, 0, 0) })
    {
        instance = ::vsg::Instance::create(::vsg::Names{}, ::vsg::Names{}, version);
        if (instance != nullptr)
        {
            result.loader_version = version;
            break;
        }
    }
    if (instance == nullptr)
    {
        result.error = vn::String(u8"no Vulkan loader (no instance could be created)");
        return result;
    }

    // Enumerating needs no extensions and no surface, which is what lets this run on a machine that cannot
    // present anything at all (a headless run, a CI container).
    for (const ::vsg::ref_ptr<::vsg::PhysicalDevice>& device : instance->getPhysicalDevices())
    {
        if (device != nullptr)
        {
            result.devices.push_back(describePhysicalDevice(*device));
        }
    }

    result.ok = true;
    return result;
}

}  // namespace api

VN_VSG_NS_END
