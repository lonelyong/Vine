#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>

#include <volk.h>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief What a physical device OFFERS, in the terms this backend's floor is stated in.
 *
 * @details
 * - IT IS A VALUE, NOT A DEVICE. `Device::initialize` fills one per candidate from the Vulkan queries it already makes
 *   (properties, the 1.3 feature struct, the EDS3 feature struct and the extension list), and `candidateRefusal` then
 *   answers whether that device can serve a session - so the DECISION is testable without a driver, which two real
 *   GPUs are not (`VkDeviceChoiceTest` fabricates offers).
 * - WHY IT EXISTS AT ALL (`G4` / `G1`). "A device that can run this backend" is one question with one answer, and it
 *   used to be three checks performed AFTER the best-ranked device had been picked - so a machine whose fastest device
 *   missed a floor lost the session while a capable device sat in the list, and the refusal named the backend's choice
 *   rather than the machine. `vk-bootstrap`'s `PhysicalDeviceSelector` and Diligent's adapter enumeration answer it the
 *   same way: filter the candidates by what they offer, then rate only the survivors.
 * - IT NAMES NO DIAGNOSTIC. The codes a host matches (`vk.init.*`) belong to the device, so a refusal is a VALUE and
 *   the caller maps it to the sentence and the code it publishes - the same split `ContentRefusal` makes for draws.
 */
struct DeviceOffer {
    std::uint32_t api_version{ 0 };                 ///< `VkPhysicalDeviceProperties::apiVersion`.
    bool          synchronization2{ false };        ///< `VkPhysicalDeviceVulkan13Features::synchronization2`.
    bool          dynamic_rendering{ false };       ///< `VkPhysicalDeviceVulkan13Features::dynamicRendering`.
    bool          extended_dynamic_state3{ false }; ///< `VK_EXT_extended_dynamic_state3` is in the extension list.
    bool          polygon_mode{ false };            ///< `extendedDynamicState3PolygonMode`.
    bool          color_blend_enable{ false };      ///< `extendedDynamicState3ColorBlendEnable`.
    bool          color_blend_equation{ false };    ///< `extendedDynamicState3ColorBlendEquation`.
};

/// @brief Why a candidate cannot serve a session: the floor that stopped it (see @ref candidateRefusal).
enum class FloorRefusal : std::uint8_t
{
    None,          ///< The device meets every floor: it may serve a session.
    ApiVersion,    ///< Below Vulkan 1.3 (`vk.init.api_version_low`).
    CoreFeatures,  ///< 1.3 without `synchronization2` / `dynamicRendering` (`vk.init.device_features_missing`).
    DynamicState3, ///< No EDS3, or without the polygon-mode / colour-blend features (`vk.init.dynamic_state_missing`).
};

/**
 * @brief Answers which floor @p offer misses, in the order this backend states them.
 *
 * @details
 * - THE ORDER IS THE REASON, NOT A PREFERENCE: the 1.3 version comes first (an older device cannot even be asked for
 *   the 1.3 feature struct), then the two core features the recording path depends on, then the extension whose
 *   switches the raster state needs - so a device that misses two floors is described by the one that must change
 *   first.
 * - THE 1.3 FEATURES ARE FEATURES, NOT VERSIONS (`T11`, `ADR-2026-11-23` D4): a 1.3 device that does not offer
 *   `synchronization2` and `dynamicRendering` is refused exactly like a 1.2 device, because a version alone is no
 *   evidence that a feature is enabled.
 * - THE RASTER FLOOR (`T54`): cull, polygon mode, depth test/write/compare and blend are DYNAMIC state, so polygon mode
 *   and colour blend remain `VK_EXT_extended_dynamic_state3` - a device without them would have to bake silently, and
 *   the picture would be wrong in a way no test could name.
 *
 * @param offer What the device offers (see @ref DeviceOffer).
 * @return `FloorRefusal::None` when the device can serve a session.
 */
[[nodiscard]] inline FloorRefusal candidateRefusal(const DeviceOffer& offer) noexcept
{
    if (offer.api_version < VK_API_VERSION_1_3) {
        return FloorRefusal::ApiVersion;
    }
    if (!offer.synchronization2 || !offer.dynamic_rendering) {
        return FloorRefusal::CoreFeatures;
    }
    if (!offer.extended_dynamic_state3 || !offer.polygon_mode || !offer.color_blend_enable || !offer.color_blend_equation) {
        return FloorRefusal::DynamicState3;
    }
    return FloorRefusal::None;
}

VN_BACKEND_VK_NS_END
