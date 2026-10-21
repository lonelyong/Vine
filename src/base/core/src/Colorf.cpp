#include <vine/Colorf.hpp>

#include "ColorTransfer.hpp"

#include <vine/Color.hpp>

VN_CORE_NS_BEGIN

namespace
{

/// @brief Quantizes one channel, clamping outside 0..1 and rounding.
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
    // r, g and b are ENCODED - this is where linear light becomes the display's sRGB samples (the inverse of
    // Color::toLinear). Alpha is only quantized: it is coverage, not light.
    return Color{ toChannel(detail::linearToSrgb(r)), toChannel(detail::linearToSrgb(g)),
                  toChannel(detail::linearToSrgb(b)), toChannel(a) };
}

Colorf Colorf::inverted() const noexcept
{
    // Through invert(), so the mutating and the answering spelling cannot drift apart (the math types' convention).
    Colorf copy = *this;
    copy.invert();
    return copy;
}

void Colorf::invert() noexcept
{
    // The complement in LINEAR light: 1 - channel, alpha untouched. The display complement is a different number
    // (Color::invert complements the encoded samples), and the difference between the two is the sRGB curve -
    // keeping the two spellings apart is what this pair is for.
    r = 1.0f - r;
    g = 1.0f - g;
    b = 1.0f - b;
}

Colorf Colorf::lerp(const Colorf& from, const Colorf& to, float t) noexcept
{
    // No clamping and no special cases: this is the blend a shader's mix() performs, applied to the values the caller
    // holds. Alpha takes part because coverage fades linearly alongside the colour.
    return { from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t,
             from.a + (to.a - from.a) * t };
}

const Colorf Colorf::kTransparent{ 0.0f, 0.0f, 0.0f, 0.0f };
const Colorf Colorf::kWhite{ 1.0f, 1.0f, 1.0f, 1.0f };
const Colorf Colorf::kBlack{ 0.0f, 0.0f, 0.0f, 1.0f };
const Colorf Colorf::kRed{ 1.0f, 0.0f, 0.0f, 1.0f };
const Colorf Colorf::kGreen{ 0.0f, 1.0f, 0.0f, 1.0f };
const Colorf Colorf::kBlue{ 0.0f, 0.0f, 1.0f, 1.0f };

VN_CORE_NS_END
