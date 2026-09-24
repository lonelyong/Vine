#include <vine/Colorf.hpp>

#include <vine/Color.hpp>

VN_CORE_NS_BEGIN

namespace
{

std::uint8_t toChannel(float v) noexcept
{
    if (v <= 0.0f)
    {
        return 0;
    }
    if (v >= 1.0f)
    {
        return 255;
    }
    return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}

} // namespace

Color Colorf::toColor() const noexcept
{
    return Color{ toChannel(r), toChannel(g), toChannel(b), toChannel(a) };
}

Colorf Colorf::fromColor(const Color& c) noexcept
{
    return { static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
             static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f };
}

VN_CORE_NS_END
