#include <vine/vsg/api/DeviceFeatures.hpp>

#include <vine/vsg/core/DeviceRequirements.hpp>

VN_VSG_NS_BEGIN

void applyRequiredFeatures(const ::vsg::ref_ptr<::vsg::DeviceFeatures>& features)
{
    // The count is the contract: a feature added to core::DeviceRequirements without a request here (or the
    // other way round) fails the build, which is the only way the two cannot drift silently.
    static_assert(core::kDeviceFeatureCount == 7, "every required feature must be requested here");

    features->get().fillModeNonSolid  = VK_TRUE;
    features->get().independentBlend  = VK_TRUE;
    features->get().samplerAnisotropy = VK_TRUE;
    features
        ->get<VkPhysicalDeviceExtendedDynamicState2FeaturesEXT,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT>()
        .extendedDynamicState2 = VK_TRUE;
    features
        ->get<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT>()
        .extendedDynamicState3PolygonMode = VK_TRUE;
    features
        ->get<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT>()
        .extendedDynamicState3ColorBlendEnable = VK_TRUE;
    features
        ->get<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT>()
        .extendedDynamicState3ColorBlendEquation = VK_TRUE;
}

::vsg::Names requiredDeviceExtensions()
{
    static_assert(core::kDeviceExtensionCount == 2,
                  "every required extension must be named here, or a device that lacks it is served anyway");

    // The FEATURE BITS above are worth nothing without these names: a feature struct of an extension that was
    // never enabled is not just unqueried, the pipeline create-info that relies on it is invalid -
    // `vkCreateGraphicsPipelines` with VK_DYNAMIC_STATE_POLYGON_MODE_EXT and no EDS3 is a validation error,
    // and the commands that would set the state are called on a device that has no such entry point.
    //
    // Both are named, and EDS3 after EDS2, because EDS3 is built on EDS2's block: the pair is the unit the
    // extension dependency rules already require.
    return ::vsg::Names{ VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
                         VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME };
}

VN_VSG_NS_END
