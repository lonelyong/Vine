#pragma once

#include "core_global.hpp"

VN_CORE_NS_BEGIN

class Color;

/**
 * @brief An RGBA color with floating-point channels, typically in the 0..1 range.
 */
class VN_CORE_API Colorf
{
  public:
    Colorf() noexcept = default;

    /**
     * @brief Constructs from per-channel values.
     *
     * @param r Red channel.
     * @param g Green channel.
     * @param b Blue channel.
     * @param a Alpha channel; defaults to opaque.
     */
    constexpr Colorf(float r, float g, float b, float a = 1.0f) noexcept
        : r(r)
        , g(g)
        , b(b)
        , a(a)
    {
    }

  public:
    /**
     * @brief Converts to an 8-bit color, clamping to 0..1 and rounding.
     *
     * @return The 8-bit color.
     */
    Color toColor() const noexcept;

    /**
     * @brief Creates a normalized color from an 8-bit color.
     *
     * @param c Source color.
     * @return The normalized color.
     */
    static Colorf fromColor(const Color& c) noexcept;

    bool operator==(const Colorf&) const = default;

  public:
    float r{ 1.0f };
    float g{ 1.0f };
    float b{ 1.0f };
    float a{ 1.0f };
};

VN_CORE_NS_END
