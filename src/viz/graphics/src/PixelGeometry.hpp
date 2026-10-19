#pragma once

#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief A size in the space a HOST reports: logical pixels (a Qt widget size, a pane's logical extent).
 *
 * @details
 * - A host reports logical sizes; a pass viewport and a render target are stated in DEVICE pixels. Keeping the two
 *   in named types is what makes a conversion visible at the boundary instead of hiding it in an `int`.
 */
struct LogicalSize
{
    int width{0};   ///< Width in logical pixels.
    int height{0};  ///< Height in logical pixels.

    /** @brief Returns whether the size describes a positive area. */
    [[nodiscard]] constexpr bool isValid() const noexcept { return width > 0 && height > 0; }
};

/**
 * @brief A size in the space the GRAPHICS API uses: device pixels (a swapchain, a viewport, a render target).
 *
 * @details
 * - The unit is the one `Viewport` and `RenderTarget::setSize` state; a host that reported a logical size converts
 *   through `toDeviceSize` before it reaches one of them.
 */
struct DeviceSize
{
    int width{0};   ///< Width in device pixels.
    int height{0};  ///< Height in device pixels.

    /** @brief Returns whether the size describes a positive area. */
    [[nodiscard]] constexpr bool isValid() const noexcept { return width > 0 && height > 0; }
};

/**
 * @brief Device pixels per logical pixel, as a host's `devicePixelRatio` states it.
 *
 * @details
 * - A non-positive value is clamped to 1.0 where it is constructed: "unset" and "1:1" are the same rendering
 *   decision, and letting a zero through would make every converted size zero.
 */
struct PixelRatio
{
    double value{1.0};   ///< Device pixels per logical pixel (> 0).

    /** @brief Constructs a ratio, clamping a non-positive value to 1.0.
     *
     * @param ratio Device pixels per logical pixel.
     */
    explicit constexpr PixelRatio(double ratio = 1.0) noexcept : value(ratio > 0.0 ? ratio : 1.0) {}
};

/**
 * @brief Converts a logical surface size into device pixels.
 *
 * @details
 * - ONE spelling of the conversion (the host's, `logical * ratio`), TRUNCATING like the host's own scaling: a
 *   fractional device pixel has no meaning to a viewport, and the engine's picture-shape check tolerates the
 *   one-pixel rounding this can produce (see EngineCompile.cpp's `kShapeTolerance`).
 * - The three consumers that used to spell this out separately (Pipeline::resize, AxisGizmo and FpsOverlay's
 *   surface-resize steps) now call this one function, so a change to the rounding rule has one place to land.
 *
 * @param logical Logical size the host reported.
 * @param ratio   Device pixels per logical pixel.
 * @return The size in device pixels (may be non-positive when the input was).
 */
[[nodiscard]] constexpr DeviceSize toDeviceSize(LogicalSize logical, PixelRatio ratio) noexcept
{
    return DeviceSize{ static_cast<int>(static_cast<double>(logical.width) * ratio.value),
                       static_cast<int>(static_cast<double>(logical.height) * ratio.value) };
}

}  // namespace detail

VN_GRAPHICS_NS_END
