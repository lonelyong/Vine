#pragma once

#include "core_global.hpp"

#include <cstdint>
#include <optional>

#include "String.hpp"

VN_CORE_NS_BEGIN

class Colorf;

/**
 * @brief An RGBA color whose channels are 8-bit samples in the sRGB encoding.
 *
 * @details
 * - The 8-bit half of a pair: the samples a palette, a PNG, a `QColor` or a theme table holds - the values a display
 *   shows, rather than the light they stand for.
 * - The float half is @ref Colorf; @ref toLinear decodes (sRGB curve) and @ref Colorf::toColor encodes, which is the
 *   only bridge between the two.
 * - Alpha is not encoded: it is a coverage fraction and crosses as `channel / 255`.
 *
 * @note
 * - There is deliberately NO implicit conversion to @ref Colorf: an implicit one would silently divide by 255, and
 *   that division is the mistake this pair exists to make visible (51 encodes 0.0331, not 0.2).
 * - There is no `lerp` here on purpose: interpolating ENCODED samples is a display-space operation (a UI gradient),
 *   and the graphics module's fades blend light instead - that is @ref Colorf::lerp. When something genuinely needs
 *   the encoded blend, it lands here with a name that says so rather than being reached for by mistake.
 */
class VN_CORE_API Color {
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
    {}

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
    {}

    /**
     * @brief Constructs from a packed 0xAARRGGBB integer - the order Qt's `ARGB32` and Win32's `ARGB` hand you.
     *
     * @details
     * - The byte order IS the name: a call says which order it means, instead of leaving the reader to count shifts.
     * - It is deliberately NOT the same integer as @ref Color(std::uint32_t), which is 0xRRGGBBAA: `0x80FF0000` is
     *   half-transparent red here and opaque-green-with-red-alpha there. That is exactly the mistake a named entry
     *   point prevents, and why this one is not spelled as another shift.
     *
     * @param argb Packed colour; the most significant byte is alpha, the least significant is blue.
     * @return The colour.
     */
    [[nodiscard]] static constexpr Color fromArgb(std::uint32_t argb) noexcept
    {
        return Color{ static_cast<std::uint8_t>((argb >> 16) & 0xFFU),
                      static_cast<std::uint8_t>((argb >> 8) & 0xFFU),
                      static_cast<std::uint8_t>(argb & 0xFFU),
                      static_cast<std::uint8_t>((argb >> 24) & 0xFFU) };
    }

    /**
     * @brief Constructs from a packed 0x00BBGGRR integer - Win32's `COLORREF` order, opaque.
     *
     * @details
     * - Red sits in the low byte and blue in bits 16..23; the high byte is not part of the value and is IGNORED, so a
     *   `COLORREF` whose top byte carries a flag reads the same as the plain one.
     * - `COLORREF` has no coverage, so the result is opaque. A caller that holds an alpha wants @ref fromArgb.
     *
     * @param bgr Packed colour; red in the low byte, blue in bits 16..23; the high byte is ignored.
     * @return The opaque colour.
     */
    [[nodiscard]] static constexpr Color fromBgr(std::uint32_t bgr) noexcept
    {
        return Color{ static_cast<std::uint8_t>(bgr & 0xFFU),
                      static_cast<std::uint8_t>((bgr >> 8) & 0xFFU),
                      static_cast<std::uint8_t>((bgr >> 16) & 0xFFU),
                      255 };
    }

  public:
    /**
     * @brief Returns the packed 0xRRGGBBAA representation.
     *
     * @return The packed color value.
     */
    std::uint32_t toRgba32() const noexcept;

    /**
     * @brief Complements the colour IN PLACE (255 - channel, alpha unchanged).
     *
     * @details
     * - The same pair the math types use (`Matrix4x4::invert` mutates, `inverted` answers a copy), so "invert this"
     *   and "give me the inverse" read the same way for a colour as for a matrix.
     * - A DISPLAY-SPACE complement: the bytes are complemented as encoded samples, which is what a UI palette means by
     *   "inverse". It is not the optical inverse of the light the colour stands for - @ref Colorf::inverted does that
     *   one, on the linear value.
     *
     * @note
     * - Alpha is untouched: it is coverage, not a channel of the colour.
     */
    void invert() noexcept;

    /**
     * @brief Returns the inverted color (255 - channel, alpha unchanged).
     *
     * @details
     * - The copy of @ref invert, with the same display-space meaning: this is the inverse of the SAMPLES, not of the
     *   light.
     *
     * @return The inverted color.
     */
    [[nodiscard]] Color inverted() const noexcept;

    /**
     * @brief Converts to linear light: decodes r, g and b from sRGB and scales alpha.
     *
     * @details
     * - The transfer function is the sRGB curve (a linear segment near black, a 2.4 power above it), so decoding is
     *   not a multiply by a constant: 51 encodes 0.0331, not 0.2.
     * - Alpha only loses its 8-bit scale (`a / 255`); encoding it would make every blend and every alpha readback
     *   wrong.
     *
     * @return The same colour as a linear-light value, 0..1 per channel.
     */
    [[nodiscard]] Colorf toLinear() const noexcept;

    /**
     * @brief Returns the canonical `#rrggbb` / `#rrggbbaa` text of the colour.
     *
     * @details
     * - Lowercase hex, one leading `#`, and the alpha pair written ONLY when the colour is not opaque - so one value
     *   has exactly one text, and an opaque colour never carries a redundant trailing pair.
     * - The short forms @ref parse also accepts (`#CCC`, `#C0F8`) are never written: they are an input convenience, and
     *   a writer that chose between the two spellings would give one value two texts.
     * - The samples go out as they are: this is the sRGB-encoded half, so the text is what a palette, a style sheet or
     *   a colour picker expects. There is no linear-light text - that would have to name the space in the string;
     *   @ref Colorf answers it by writing `toColor()` first.
     *
     * @return The text, 7 or 9 characters long.
     */
    [[nodiscard]] String toString() const;

    /**
     * @brief Parses the `#rrggbb` / `#rrggbbaa` text and its short forms; anything else is a refusal, not a guess.
     *
     * @details
     * - ONE grammar, the CSS hex family: a `#` followed by 3, 4, 6 or 8 hex digits in either case (a palette may well
     *   hand you `#FF0000`). A short form duplicates each digit, so `#CCC` is `#CCCCCC` and `#C0F8` is `#CC00FF88`;
     *   a missing alpha means opaque.
     * - Refused on purpose: `0xRRGGBB`, bare digits, `rgba(255,0,0,1)`, 2 or 5 or 7 digits, and surrounding
     *   whitespace. Each of those is a second way to spell one value, and a format contract exists so that there is
     *   one - the four lengths above are one grammar's productions, not four grammars.
     *
     * @param text The text to parse.
     * @return The colour, or an empty optional when @p text is not that grammar.
     */
    [[nodiscard]] static std::optional<Color> parse(const String& text);

    constexpr bool operator==(const Color& other) const
    {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }

    constexpr bool operator!=(const Color& other) const
    {
        return !(*this == other);
    }

    constexpr bool operator<(const Color& other) const
    {
        if (r != other.r) {
            return r < other.r;
        }
        if (g != other.g) {
            return g < other.g;
        }
        if (b != other.b) {
            return b < other.b;
        }
        return a < other.a;
    }

  public:
    std::uint8_t r{ 255 };
    std::uint8_t g{ 255 };
    std::uint8_t b{ 255 };
    std::uint8_t a{ 255 };

    /// The colour that covers nothing: no samples, no coverage - what "nothing was drawn here" means.
    static const Color kTransparent;

    /// Full white, and one of the few colours whose NUMBERS are the same in both spellings (0 and 255 are the sRGB
    /// curve's fixed points), which is why this vocabulary is shared with @ref Colorf rather than duplicated.
    static const Color kWhite;

    /// Black at full coverage: no light, but still drawn - distinct from @ref kTransparent.
    static const Color kBlack;

    /// The saturated primaries, at full coverage and in the same encoding-independent numbers as @ref kWhite.
    static const Color kRed;
    static const Color kGreen;
    static const Color kBlue;
};

VN_CORE_NS_END

template <>
struct std::hash<vn::Color> {
    std::size_t operator()(const vn::Color& color) const noexcept
    {
        return color.toRgba32();
    }
};
