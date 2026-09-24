#pragma once

#include "core_global.hpp"

#include <cstdint>

VN_CORE_NS_BEGIN

class Colorf;

/**
 * @brief An RGBA color; each channel is an unsigned byte.
 */
class VN_CORE_API Color
{
  public:
    Color() noexcept = default;

    /**
     * @brief Constructs from per-channel values.
     *
     * @param r Red channel.
     * @param g Green channel.
     * @param b Blue channel.
     * @param a Alpha channel; defaults to opaque.
     */
    constexpr Color(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) noexcept
        : r(r)
        , g(g)
        , b(b)
        , a(a)
    {
    }

    /**
     * @brief Constructs from a packed 0xRRGGBBAA integer.
     *
     * @param rgba Packed color; the most significant byte is red.
     */
    explicit constexpr Color(std::uint32_t rgba) noexcept
        : r(static_cast<std::uint8_t>(rgba >> 24))
        , g(static_cast<std::uint8_t>(rgba >> 16))
        , b(static_cast<std::uint8_t>(rgba >> 8))
        , a(static_cast<std::uint8_t>(rgba))
    {
    }

  public:
    /**
     * @brief Returns the packed 0xRRGGBBAA representation.
     *
     * @return The packed color value.
     */
    std::uint32_t toRgba32() const noexcept;

    /**
     * @brief Returns the inverted color (255 - channel, alpha unchanged).
     *
     * @return The inverted color.
     */
    Color inverted() const noexcept;

    /**
     * @brief Converts to a floating-point color with normalized channels.
     *
     * @return The normalized color.
     */
    Colorf toColorf() const noexcept;

    bool operator==(const Color&) const = default;

  public:
    std::uint8_t r{ 255 };
    std::uint8_t g{ 255 };
    std::uint8_t b{ 255 };
    std::uint8_t a{ 255 };

    static const Color Transparent;
    static const Color White;
    static const Color Black;
    static const Color Red;
    static const Color Green;
    static const Color Blue;
};

VN_CORE_NS_END
