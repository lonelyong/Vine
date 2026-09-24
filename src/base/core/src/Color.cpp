#include <vine/Color.hpp>

#include <vine/Colorf.hpp>

VN_CORE_NS_BEGIN

std::uint32_t Color::toRgba32() const noexcept
{
    return (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) | static_cast<std::uint32_t>(a);
}

Color Color::inverted() const noexcept
{
    return Color{ static_cast<std::uint8_t>(255 - r), static_cast<std::uint8_t>(255 - g),
                  static_cast<std::uint8_t>(255 - b), a };
}

Colorf Color::toColorf() const noexcept
{
    return { static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
             static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f };
}

const Color Color::Transparent{ 0, 0, 0, 0 };
const Color Color::White{ 255, 255, 255, 255 };
const Color Color::Black{ 0, 0, 0, 255 };
const Color Color::Red{ 255, 0, 0, 255 };
const Color Color::Green{ 0, 255, 0, 255 };
const Color Color::Blue{ 0, 0, 255, 255 };

VN_CORE_NS_END
