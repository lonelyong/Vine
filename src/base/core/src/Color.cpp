#include <vine/Color.hpp>

#include <optional>
#include <span>

#include "ColorTransfer.hpp"

#include <vine/Colorf.hpp>

VN_CORE_NS_BEGIN

namespace
{

/**
 * @brief Returns the value of one hex digit, or -1 when the character is not one.
 *
 * @details
 * - Written out instead of going through `String::toInt(&ok, 16)`, which accepts a sign, leading whitespace and an
 *   `0x` prefix: the text format accepts exactly the sixteen digits and nothing else.
 *
 * @param ch Character to read.
 * @return 0..15, or -1 when @p ch is not a hex digit.
 */
constexpr int hexDigit(char8_t ch) noexcept
{
    if (ch >= u8'0' && ch <= u8'9') {
        return static_cast<int>(ch - u8'0');
    }
    if (ch >= u8'a' && ch <= u8'f') {
        return static_cast<int>(ch - u8'a') + 10;
    }
    if (ch >= u8'A' && ch <= u8'F') {
        return static_cast<int>(ch - u8'A') + 10;
    }
    return -1;
}

} // namespace

std::uint32_t Color::toRgba32() const noexcept
{
    return (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) | (static_cast<std::uint32_t>(b) << 8) | static_cast<std::uint32_t>(a);
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
             detail::srgbToLinear(static_cast<float>(b) / 255.0f),
             static_cast<float>(a) / 255.0f };
}

String Color::toString() const
{
    // One text per value: the alpha pair is written only when it is not opaque, so a colour never has two
    // spellings and the alpha-less form always means 255 (the CSS hex contract a palette hands you).
    const std::uint8_t samples[] = { r, g, b, a };
    const std::size_t  count     = (a == 255) ? 3U : 4U;
    String             text{ u8"#" };
    text.append(String::hex(std::span<const std::uint8_t>(samples, count)));
    return text;
}

std::optional<Color> Color::parse(const String& text)
{
    // One grammar: '#' then 3, 4, 6 or 8 hex digits - the CSS hex family, where a short form duplicates each digit
    // (#CCC is #CCCCCC, #C0F8 is #CC00FF88). Refusing the neighbours (0xRRGGBB, bare digits, rgba(...)) is the point of
    // a format contract; accepting only 3 of the four lengths would be an arbitrary hole in it.
    if (text.empty() || text[0] != u8'#') {
        return std::nullopt;
    }

    const std::size_t digits = text.size() - 1;
    if (digits != 3 && digits != 4 && digits != 6 && digits != 8) {
        return std::nullopt;
    }

    const bool        shorthand  = digits < 6;
    const std::size_t count      = shorthand ? digits : digits / 2;
    std::uint8_t      channels[] = { 0, 0, 0, 255 };
    for (std::size_t i = 0; i < count; ++i) {
        const int high = hexDigit(text[1 + (shorthand ? i : 2 * i)]);
        const int low  = shorthand ? high : hexDigit(text[1 + 2 * i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        channels[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return Color{ channels[0], channels[1], channels[2], channels[3] };
}

const Color Color::kTransparent{ 0, 0, 0, 0 };
const Color Color::kWhite{ 255, 255, 255, 255 };
const Color Color::kBlack{ 0, 0, 0, 255 };
const Color Color::kRed{ 255, 0, 0, 255 };
const Color Color::kGreen{ 0, 255, 0, 255 };
const Color Color::kBlue{ 0, 0, 255, 255 };

VN_CORE_NS_END
