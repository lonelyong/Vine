#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief A depth attachment read back to the CPU, as the numbers the pass actually wrote.
 *
 * WHY THE DEPTH IS READ AND NOT INFERRED. A colour picture can only say what survived the depth test
 * ("this fragment is hidden"), never what the depth holds: the same picture comes out of a depth buffer that
 * was never written, one that was written too far away, and one whose load op did the wrong thing - and the
 * three need different fixes. Reading the depth turns each of those into a number: a clear that never happened
 * shows up as garbage instead of the clear value, a borrowed depth shows the LENDER's value at the lender's
 * geometry, and a depth written by the wrong pass shows a value no pass of this frame wrote.
 *
 * FORMAT HONESTY is the other half: the values here are normalised depth in [0, 1] for the formats that can be
 * read (D32 as its raw float, D16 divided by its full scale), and a format that cannot be read yields no probe
 * at all rather than a plausible-looking conversion (see the target's depth capture).
 */
VN_VSG_NS_BEGIN

namespace core
{

/** @brief Depth values of one attachment, in the row-major, tightly packed order the copy produced. */
class DepthProbe
{
  public:
    /** @brief Constructs an invalid probe (no pixels). */
    DepthProbe() = default;

    /** @brief Constructs a probe over @p values.
     *
     * @param width  Width in texels.
     * @param height Height in texels.
     * @param values Normalised depth values, width * height of them.
     */
    DepthProbe(int width, int height, std::vector<float> values);

    /** @brief Gets whether this probe holds a full image. */
    [[nodiscard]] bool valid() const noexcept;

    /** @brief Gets the width in texels (0 when invalid). */
    [[nodiscard]] int width() const noexcept;

    /** @brief Gets the height in texels (0 when invalid). */
    [[nodiscard]] int height() const noexcept;

    /** @brief Gets the depth at @p x, @p y, or 0 for a coordinate outside the image.
     *
     * @param x Column, 0-based from the left.
     * @param y Row, 0-based from the top.
     * @return The stored depth, or 0 when the coordinate is outside the image.
     */
    [[nodiscard]] float depthAt(int x, int y) const noexcept;

    /** @brief Counts the texels whose depth is within @p tolerance of @p value.
     *
     * The comparison is the one a pass makes: a clear is uniform, so the count says whether the whole
     * attachment holds it, and a tolerance is required because a clear value and a written depth arrive
     * through different paths (a clear value is converted by the driver, a written depth by the shader).
     *
     * @param value Expected depth.
     * @param tolerance Allowed absolute difference.
     * @return Number of matching texels.
     */
    [[nodiscard]] std::size_t countNear(float value, float tolerance) const noexcept;

    /** @brief Gets the normalised values the probe wraps (row-major, one per texel).
     *
     * For the caller that hands the depths ON rather than probes them - the SDK's readback copies the
     * values out of here.
     *
     * @return The values, exactly `width() * height()` of them when valid.
     */
    [[nodiscard]] const std::vector<float>& values() const noexcept;

  private:
    int                width_{0};
    int                height_{0};
    std::vector<float> values_;
};

}  // namespace core

VN_VSG_NS_END
