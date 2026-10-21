#pragma once

#include "core_global.hpp"

VN_CORE_NS_BEGIN

class Color;

/**
 * @brief An RGBA color with floating-point channels.
 *
 * @details
 * - Channels are nominally 0..1 and may exceed 1 for an HDR colour; a quantizing conversion clamps, it does not fold.
 * - It is the FLOAT half of a pair whose other half is @ref Color (8-bit samples in the sRGB encoding). The bridge
 *   is @ref Color::toLinear and @ref toColor, both of which apply the sRGB curve - dividing by 255 is not decoding.
 * - WHICH SPACE a value is in belongs to whoever consumes it, not to the type: this is a value type, and the modules
 *   that give a colour a meaning declare it. The graphics module declares that every colour it is handed is LINEAR
 *   light (see its ClearPolicy), so a palette / PNG / `QColor` value is decoded once on the way in.
 *
 * @note
 * - Alpha is a coverage fraction in both halves of the pair and is never encoded.
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
     * @brief Converts to 8-bit sRGB: encodes r, g and b and quantizes alpha.
     *
     * @details
     * - The inverse of @ref Color::toLinear: values outside 0..1 clamp, and the transfer function is the sRGB curve,
     *   so linear 0.5 becomes the byte 188 - not 128.
     * - Alpha is only quantized (coverage, not light).
     *
     * @return The same colour as the display's sRGB samples.
     */
    [[nodiscard]] Color toColor() const noexcept;

    /**
     * @brief Complements the colour IN PLACE (1 - channel on r, g and b), alpha unchanged.
     *
     * @details
     * - The same pair the math types use (`Matrix4x4::invert` mutates, `inverted` answers a copy), so "invert this"
     *   and "give me the inverse" read the same way for a colour as for a matrix.
     * - The complement is taken in LINEAR light, the space this type holds: it is the additive inverse of the light,
     *   not the inverse of the samples a display shows. The display complement is @ref Color::inverted, on the encoded
     *   samples.
     *
     * @note
     * - Two things it does not do: alpha is untouched (coverage, not light), and a channel above 1 (an HDR colour)
     *   complements to a negative value rather than clamping - the operation is defined on the 0..1 range a displayed
     *   colour occupies, and a caller whose result is meant for a display converts with @ref toColor, which clamps.
     */
    void invert() noexcept;

    /**
     * @brief Returns the inverted colour: the copy of @ref invert.
     *
     * @details
     * - Same linear-light meaning as @ref invert; it exists so a value can be complemented without mutating the one
     *   the caller holds, which is the spelling most call sites want.
     *
     * @return The complemented colour.
     */
    [[nodiscard]] Colorf inverted() const noexcept;

    /**
     * @brief Interpolates between two colours channel by channel: `from + (to - from) * t`.
     *
     * @details
     * - STATIC AND SYMMETRIC, like @ref Quaternion::slerp: both colours are inputs, so neither of them reads as the
     *   value being changed and the call has no hidden subject.
     * - The channels are interpolated AS THEY ARE, which is what makes the space a property of the caller's values:
     *   for the graphics module those are linear, so this is the same blend a shader's `mix(a, b, t)` performs.
     *   Between #000000 and #ffffff, `t = 0.5` gives **0.5 light** - the byte 188 on screen, not 128 - because
     *   blending light is not blending samples.
     * - Alpha is interpolated too, and these are STRAIGHT-alpha colours (not premultiplied): two OPAQUE colours
     *   cross-fade correctly with this call alone, while two translucent ones do not - the midpoint of a straight-alpha
     *   blend averages every channel, which is a different picture from a real cross-dissolve over a background.
     *   Reach for this when the coverage is opaque or the fade is single-sided; compositing stays the caller's job.
     *
     * @note
     * - @p t is NOT clamped, and values outside 0..1 therefore extrapolate (`t = 2` lands one step beyond @p to).
     *   Clamping would silently change a caller's arithmetic, and the type already allows channels outside 0..1.
     *
     * @param from Colour at `t = 0`.
     * @param to Colour at `t = 1`.
     * @param t Position between them, nominally 0..1.
     * @return The interpolated colour.
     */
    [[nodiscard]] static Colorf lerp(const Colorf& from, const Colorf& to, float t) noexcept;

    bool operator==(const Colorf&) const = default;

  public:
    float r{ 1.0f };
    float g{ 1.0f };
    float b{ 1.0f };
    float a{ 1.0f };

    /// The colour that adds nothing: no light and no coverage - what "nothing was drawn here" means.
    static const Colorf kTransparent;

    /// Full white, and one of the few colours whose NUMBERS are the same in both spellings: 0 and 1 are the sRGB
    /// curve's fixed points, which is why this vocabulary is shared with @ref Color rather than duplicated.
    static const Colorf kWhite;

    /// Black at full coverage: no light, but still drawn - distinct from @ref kTransparent.
    static const Colorf kBlack;

    /// The saturated primaries, at full coverage. Like @ref kWhite their numbers are encoding-independent.
    static const Colorf kRed;
    static const Colorf kGreen;
    static const Colorf kBlue;
};

VN_CORE_NS_END
