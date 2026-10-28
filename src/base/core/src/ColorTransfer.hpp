#pragma once

#include <cmath>

#include <vine/core_global.hpp>

VN_CORE_NS_BEGIN

namespace detail
{

/**
 * @brief Decodes one sRGB-encoded channel to linear light.
 *
 * @details
 * - This is the IEC 61966-2-1 curve, not a `pow(x, 2.2)` approximation: a short LINEAR segment near black keeps the
 *   slope at 1 (so the dark 8-bit values stay distinguishable) and everything above it is the 2.4 power.
 * - The two directions share one breakpoint - 0.04045 encoded is 0.0031308 linear - which is what makes them exact
 *   inverses; a pair that picks its own thresholds per direction does not round-trip.
 *
 * @note
 * - Private to the core colour types: the graphics module's contract is that it never converts an encoding itself
 *   (see video/graphics' colour-space note), so this header is not installed.
 *
 * @param encoded Channel value in the sRGB encoding, 0 at black and 1 at white.
 * @return The channel's linear value, 0..1.
 */
inline float srgbToLinear(float encoded) noexcept
{
    if (encoded <= 0.0F) {
        return 0.0F;
    }
    if (encoded <= 0.04045F) {
        return encoded / 12.92F;
    }
    return std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

/**
 * @brief Encodes one linear channel as sRGB.
 *
 * @details
 * - The inverse of @ref srgbToLinear, over the whole real line: a value above 1 (an HDR colour) maps above 1 and is
 *   clamped by whoever quantizes it, rather than being silently folded here.
 *
 * @param linear Channel value in linear light.
 * @return The channel's sRGB-encoded value, 0..1 for a linear value in 0..1.
 */
inline float linearToSrgb(float linear) noexcept
{
    if (linear <= 0.0F) {
        return 0.0F;
    }
    if (linear <= 0.0031308F) {
        return linear * 12.92F;
    }
    return 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

} // namespace detail

VN_CORE_NS_END
