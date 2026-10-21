#include <vine/Color.hpp>

#include "ColorTransfer.hpp"

#include <vine/Colorf.hpp>

VN_CORE_NS_BEGIN

std::uint32_t Color::toRgba32() const noexcept
{
    return (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) | static_cast<std::uint32_t>(a);
}

void Color::invert() noexcept
{
    r = static_cast<std::uint8_t>(255 - r);
    g = static_cast<std::uint8_t>(255 - g);
    b = static_cast<std::uint8_t>(255 - b);
}

Color Color::inverted() const noexcept
{
    // Through invert(), so the mutating and the answering spelling cannot drift apart (the math types' convention).
    Color copy = *this;
    copy.invert();
    return copy;
}

Colorf Color::toLinear() const noexcept
{
    return { detail::srgbToLinear(static_cast<float>(r) / 255.0f),
             detail::srgbToLinear(static_cast<float>(g) / 255.0f),
             detail::srgbToLinear(static_cast<float>(b) / 255.0f), static_cast<float>(a) / 255.0f };
}

const Color Color::kTransparent{ 0, 0, 0, 0 };
const Color Color::kWhite{ 255, 255, 255, 255 };
const Color Color::kBlack{ 0, 0, 0, 255 };
const Color Color::kRed{ 255, 0, 0, 255 };
const Color Color::kGreen{ 0, 255, 0, 255 };
const Color Color::kBlue{ 0, 0, 255, 255 };

VN_CORE_NS_END
