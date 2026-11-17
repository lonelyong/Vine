#pragma once

#include <cstdint>

#include <vine/backend_vk/backend_vk_global.hpp>
#include <vine/backend_vk/internal/StateMapping.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The sample counts Vulkan offers, largest first (see largestSampleCount / ensureBuildableSamples).
 *
 * @details
 * - ONE LIST, TWO READERS: `Device::initialize` states `Capabilities::limits.max_samples` out of it, and `TargetTable::ensure`
 *   decides whether a request is a count at all - so the advertised limit and the accepted set cannot drift.
 */
inline constexpr std::uint32_t kSampleCounts[] = { 64U, 32U, 16U, 8U, 4U, 2U, 1U };

/**
 * @brief Whether @p samples is a count this device can build a target with.
 *
 * @details
 * - A COUNT IS A COUNT (see ADR-2026-11-14-sample-count-refused-not-lowered): the legal counts are the powers of two,
 *   so a number between two of them (three, say) is a count the device does not offer at all - mapping it through
 *   `StateMapping::sampleCount` would silently answer one sample, which is the same lie as lowering an over-large
 *   request. `Capabilities::limits.max_samples` plus "the counts are powers of two" names the whole set, and this is where
 *   that set is checked, once, before any object is made.
 *
 * @param samples Samples per pixel a target asks for (already normalised: 0 is not a count).
 * @param supported The device's `framebufferColorSampleCounts & framebufferDepthSampleCounts` flags.
 * @return true when @p samples is a power of two the @p supported flags contain.
 */
[[nodiscard]] inline bool ensureBuildableSamples(const std::uint32_t samples, const std::uint32_t supported) noexcept
{
    if (samples == 0U || (samples & (samples - 1U)) != 0U) {
        return false;
    }
    return (supported & static_cast<std::uint32_t>(StateMapping::sampleCount(samples))) != 0U;
}

/**
 * @brief Gets the largest sample count @p supported flags offer.
 *
 * @param supported The device's `framebufferColorSampleCounts & framebufferDepthSampleCounts` flags.
 * @return The largest supported count; 1 when the flags offer nothing usable.
 */
[[nodiscard]] inline std::uint32_t largestSampleCount(const std::uint32_t supported) noexcept
{
    for (const std::uint32_t count : kSampleCounts) {
        if ((supported & static_cast<std::uint32_t>(StateMapping::sampleCount(count))) != 0U) {
            return count;
        }
    }
    return 1U;
}

VN_BACKEND_VK_NS_END
