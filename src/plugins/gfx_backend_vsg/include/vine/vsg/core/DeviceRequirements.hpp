#pragma once

#include <cstddef>
#include <cstdint>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The device a session may run on: the policy, checkable without a graphics API.
 *
 * WHY THE POLICY IS SEPARATE FROM THE PROBE. "Which device will we run on" has two halves with very
 * different testability: what the backend REQUIRES (a version floor and a handful of features, all of
 * them decisions this backend made) and what a machine OFFERS (a driver's answer, only obtainable on a
 * machine with a loader). Mixed together, the requirements can only be exercised on a machine that has
 * the device - which is exactly the machine where a mistake in them does not show up. Split, the
 * requirements are a table a device-free test pins (a device at 1.3 is refused, 1.4 is accepted, a
 * missing blend feature is named), and the probe is a thin adapter whose only job is to fill the facts
 * in.
 *
 * THE FLOOR IS A POLICY, NOT A HABIT. This backend delivers most of its pipeline state dynamically and
 * relies on core dynamic-state entry points; the floor is set at 1.4 so that anything below it can be
 * refused with a reason instead of being served on the subset of the contract that happens to work.
 * A refused device is reported, never quietly attempted.
 *
 * PATCH VERSIONS ARE IGNORED, and that is deliberate: drivers report their patch freely (and some
 * report nonsense), while the policy is about major/minor capability. Comparing the packed integer
 * would make a 1.4.0 device fail a 1.4.3 floor over a number nobody promised anything about.
 *
 * NO API HEADERS HERE, on purpose: the version is packed the way the API's own headers pack it (so the
 * values are comparable with what a driver reports) but the arithmetic is spelled out, and the feature
 * set is an enum of this layer's own. That is what keeps the whole file testable on a machine with no
 * Vulkan loader at all.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief Packs a version the way the API's own headers do: major << 22 | minor << 12 | patch.
 *
 * @param major Version major.
 * @param minor Version minor.
 * @param patch Version patch.
 * @return The packed version, comparable with a device's reported version.
 */
[[nodiscard]] constexpr std::uint32_t makeApiVersion(std::uint32_t major, std::uint32_t minor,
                                                     std::uint32_t patch = 0) noexcept
{
    return (major << 22U) | (minor << 12U) | patch;
}

/** @brief The oldest version a session may run on. */
inline constexpr std::uint32_t kRequiredApiVersion = makeApiVersion(1, 4);

/** @brief The device features a session requires (all of them, or the device is refused).
 *
 * What each one buys, so a reader can tell a real requirement from a preference:
 *   * FillModeNonSolid - wireframe rendering (a polygon mode of Line);
 *   * IndependentBlend - MRT pipelines whose attachments blend differently (a G-buffer);
 *   * SamplerAnisotropy - the samplers ask for it, so not requesting the feature is a validation error,
 *     not a degradation;
 *   * ExtendedDynamicState2 - the polygon mode state the pipelines assume is dynamic;
 *   * the three ExtendedDynamicState3 entries - per-attachment colour blend enable and equation, and the
 *     polygon mode delivered as dynamic state (the validator names the _3 feature for it).
 */
enum class DeviceFeature : std::uint8_t
{
    FillModeNonSolid,                       ///< Wireframe polygon mode.
    IndependentBlend,                       ///< Per-attachment blending (MRT).
    SamplerAnisotropy,                      ///< Anisotropic sampling.
    ExtendedDynamicState2,                  ///< The dynamic state the pipelines assume.
    ExtendedDynamicState3PolygonMode,       ///< Polygon mode as dynamic state.
    ExtendedDynamicState3ColorBlendEnable,  ///< Per-attachment blend enable.
    ExtendedDynamicState3ColorBlendEquation,///< Per-attachment blend equation.
    Count,                                  ///< Number of features (not a feature).
};

/** @brief How many features a device must have. */
inline constexpr std::size_t kDeviceFeatureCount = static_cast<std::size_t>(DeviceFeature::Count);

/** @brief The device EXTENSIONS a session requires (all of them, or the device is refused).
 *
 * An extension is a requirement of its own, separate from the features it carries, because the two fail
 * differently: a feature bit that is off means "this state cannot be delivered", while an extension that was
 * never ENABLED means the create-info was invalid and the calls expected through it have no defined effect at
 * all - a pipeline that declares a dynamic state nobody may set. Keeping the names in the policy (and not only
 * at the creation site) is what lets the probe refuse a device that cannot deliver them and a device-free test
 * pin the list.
 */
enum class DeviceExtension : std::uint8_t
{
    ExtendedDynamicState2,  ///< The dynamic state promoted into 1.3 as a core block.
    ExtendedDynamicState3,  ///< Polygon mode, per-attachment blend enable and equation (core on no version).
    Count,                  ///< Number of extensions (not an extension).
};

/** @brief How many extensions a device must offer. */
inline constexpr std::size_t kDeviceExtensionCount = static_cast<std::size_t>(DeviceExtension::Count);

/** @brief What a device offers, in the terms this policy checks. */
struct DeviceFacts
{
    std::uint32_t api_version{0};  ///< A physical device's reported version (NOT the instance's).
    std::uint32_t features{0};     ///< Bit i is set when DeviceFeature(i) is present.
    std::uint32_t extensions{0};   ///< Bit i is set when DeviceExtension(i) is available.

    /** @brief Gets whether @p feature is present.
     *
     * @param feature Feature to ask about.
     */
    [[nodiscard]] bool has(DeviceFeature feature) const noexcept;

    /** @brief Gets whether @p extension is available.
     *
     * @param extension Extension to ask about.
     */
    [[nodiscard]] bool has(DeviceExtension extension) const noexcept;

    /** @brief Records that @p feature is present.
     *
     * @param feature Feature the device offers.
     */
    void note(DeviceFeature feature) noexcept;

    /** @brief Records that @p extension is available.
     *
     * @param extension Extension the device offers.
     */
    void note(DeviceExtension extension) noexcept;
};

/** @brief The name of @p feature, for a diagnostic a human has to act on.
 *
 * @param feature Feature to name; an out-of-range value names nothing rather than reading memory.
 * @return A static string.
 */
[[nodiscard]] const char* featureName(DeviceFeature feature) noexcept;

/** @brief Gets whether a device reporting @p api_version may host a session (major/minor only).
 *
 * @param api_version Version a physical device reports.
 * @return true when it is at least kRequiredApiVersion.
 */
[[nodiscard]] bool supportsRequiredVersion(std::uint32_t api_version) noexcept;

/** @brief Counts the required features @p facts is missing.
 *
 * @param facts What the device offers.
 * @return Number of missing features; 0 means every one is present.
 */
[[nodiscard]] std::size_t missingFeatureCount(const DeviceFacts& facts) noexcept;

/** @brief Counts the required extensions @p facts is missing.
 *
 * @param facts What the device offers.
 * @return Number of missing extensions; 0 means every one is available.
 */
[[nodiscard]] std::size_t missingExtensionCount(const DeviceFacts& facts) noexcept;

/** @brief The name of @p extension, for a diagnostic a human has to act on.
 *
 * @param extension Extension to name; an out-of-range value names nothing rather than reading memory.
 * @return A static string, in the API's own spelling (VK_EXT_...).
 */
[[nodiscard]] const char* extensionName(DeviceExtension extension) noexcept;

/** @brief Gets whether a device described by @p facts may host a session.
 *
 * Both halves in one answer, so a caller cannot check the version and forget the features.
 *
 * @param facts What the device offers.
 * @return true when the version and every required feature are satisfied.
 */
[[nodiscard]] bool satisfiesRequirements(const DeviceFacts& facts) noexcept;

}  // namespace core

V_VSG_NS_END
