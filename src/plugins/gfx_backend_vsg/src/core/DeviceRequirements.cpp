#include <vine/vsg/core/DeviceRequirements.hpp>

VN_VSG_NS_BEGIN

namespace core
{

bool DeviceFacts::has(DeviceFeature feature) const noexcept
{
    const auto index = static_cast<std::uint32_t>(feature);
    if (index >= kDeviceFeatureCount)
    {
        return false;
    }
    return (features & (1U << index)) != 0U;
}

void DeviceFacts::note(DeviceFeature feature) noexcept
{
    const auto index = static_cast<std::uint32_t>(feature);
    if (index < kDeviceFeatureCount)
    {
        features |= (1U << index);
    }
}

bool DeviceFacts::has(DeviceExtension extension) const noexcept
{
    const auto index = static_cast<std::uint32_t>(extension);
    if (index >= kDeviceExtensionCount)
    {
        return false;
    }
    return (extensions & (1U << index)) != 0U;
}

void DeviceFacts::note(DeviceExtension extension) noexcept
{
    const auto index = static_cast<std::uint32_t>(extension);
    if (index < kDeviceExtensionCount)
    {
        extensions |= (1U << index);
    }
}

const char* featureName(DeviceFeature feature) noexcept
{
    switch (feature)
    {
    case DeviceFeature::FillModeNonSolid:
        return "fillModeNonSolid";
    case DeviceFeature::IndependentBlend:
        return "independentBlend";
    case DeviceFeature::SamplerAnisotropy:
        return "samplerAnisotropy";
    case DeviceFeature::ExtendedDynamicState2:
        return "extendedDynamicState2";
    case DeviceFeature::ExtendedDynamicState3PolygonMode:
        return "extendedDynamicState3PolygonMode";
    case DeviceFeature::ExtendedDynamicState3ColorBlendEnable:
        return "extendedDynamicState3ColorBlendEnable";
    case DeviceFeature::ExtendedDynamicState3ColorBlendEquation:
        return "extendedDynamicState3ColorBlendEquation";
    case DeviceFeature::Count:
        break;
    }
    return "";
}

bool supportsRequiredVersion(std::uint32_t api_version) noexcept
{
    // Major/minor only: shifting the patch away IS the comparison the policy describes, and it keeps a
    // driver's freely-reported patch from deciding whether a session runs.
    return (api_version >> 12U) >= (kRequiredApiVersion >> 12U);
}

std::size_t missingFeatureCount(const DeviceFacts& facts) noexcept
{
    std::size_t missing = 0;
    for (std::size_t i = 0; i < kDeviceFeatureCount; ++i)
    {
        if (!facts.has(static_cast<DeviceFeature>(i)))
        {
            ++missing;
        }
    }
    return missing;
}

std::size_t missingExtensionCount(const DeviceFacts& facts) noexcept
{
    std::size_t missing = 0;
    for (std::size_t i = 0; i < kDeviceExtensionCount; ++i)
    {
        if (!facts.has(static_cast<DeviceExtension>(i)))
        {
            ++missing;
        }
    }
    return missing;
}

const char* extensionName(DeviceExtension extension) noexcept
{
    switch (extension)
    {
    case DeviceExtension::ExtendedDynamicState2:
        return "VK_EXT_extended_dynamic_state2";
    case DeviceExtension::ExtendedDynamicState3:
        return "VK_EXT_extended_dynamic_state3";
    case DeviceExtension::Count:
        break;
    }
    return "";
}

bool satisfiesRequirements(const DeviceFacts& facts) noexcept
{
    return supportsRequiredVersion(facts.api_version) && missingFeatureCount(facts) == 0 &&
           missingExtensionCount(facts) == 0;
}

}  // namespace core

VN_VSG_NS_END
