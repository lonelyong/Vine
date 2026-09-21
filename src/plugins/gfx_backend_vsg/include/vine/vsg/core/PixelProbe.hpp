#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/graphics/Viewport.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief A read-back image plus the questions a phase asks about it - the fixture half of every pixel
 * assertion.
 *
 * WHY A FIXTURE AND NOT "memcmp THE BUFFER". A backend's pixel phases do not ask "is this the exact
 * image?" (a GPU's rounding, a driver's dithering or a different sample count would fail that forever);
 * they ask the questions a picture has to answer: is anything drawn at all (the non-black fraction), is
 * the copy inside its rectangle while the target centre kept the clear colour (counts inside a
 * sub-rectangle), did the shadowed ground end up measurably darker (a comparison between two
 * rectangles). Those questions are the same in every phase, and writing them out per phase is how one
 * of them ends up subtly different (row stride vs tightly packed, inclusive vs exclusive bounds) and
 * the difference is read as a rendering regression.
 *
 * PACKING IS THE CONTRACT THE READBACK API PROMISES: tightly packed RGBA8, row-major, width * height *
 * 4 bytes, no stride padding. A probe built from anything else fails `valid()` instead of quietly
 * reading the wrong pixel - which matters because "the picture is wrong" and "the buffer is not what I
 * think it is" are different bugs.
 *
 * RECTANGLES ARE DEVICE PIXELS, top-left origin, in the SDK's `Viewport` shape, and they are CLAMPED to
 * the image: a phase that asks about a rectangle larger than the surface gets the overlap counted, not
 * an out-of-range read.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief One pixel, in the packing the readback API produces. */
struct Rgba8
{
    std::uint8_t r{0};    ///< Red.
    std::uint8_t g{0};    ///< Green.
    std::uint8_t b{0};    ///< Blue.
    std::uint8_t a{255};  ///< Alpha.

    /** @brief Compares all four channels. */
    [[nodiscard]] bool operator==(const Rgba8& other) const noexcept;
};

/**
 * @brief A read-back image and the assertions a phase makes about it (see the file note).
 */
class PixelProbe
{
  public:
    /** @brief Wraps @p pixels as a @p width x @p height RGBA8 image.
     *
     * @param width Image width in pixels.
     * @param height Image height in pixels.
     * @param pixels Tightly packed RGBA8 rows; a buffer whose size is not width * height * 4 makes the
     *               probe invalid rather than wrong.
     */
    PixelProbe(int width, int height, std::vector<std::uint8_t> pixels);

    /** @brief Gets whether the buffer matches width * height * 4 (see the file note). */
    [[nodiscard]] bool valid() const noexcept;

    /** @brief Gets the image width. */
    [[nodiscard]] int width() const noexcept;

    /** @brief Gets the image height. */
    [[nodiscard]] int height() const noexcept;

    /** @brief Gets the number of pixels. */
    [[nodiscard]] std::size_t pixelCount() const noexcept;

    /** @brief Reads one pixel; out-of-range coordinates read as opaque black.
     *
     * @param x Column, 0-based from the left.
     * @param y Row, 0-based from the top.
     * @return The pixel at (x, y).
     */
    [[nodiscard]] Rgba8 pixel(int x, int y) const noexcept;

    /** @brief Counts pixels whose colour channels are not all zero (alpha ignored).
     *
     * "Is anything on screen" as a number, which is the only form a phase can gate on: a
     * validation-clean run that drew nothing must fail, and this is how it fails.
     */
    [[nodiscard]] std::size_t nonBlackPixels() const noexcept;

    /** @brief Gets the fraction of pixels that are not black, in [0, 1]. */
    [[nodiscard]] double nonBlackFraction() const noexcept;

    /** @brief Counts pixels equal to @p color inside @p rect (clamped to the image).
     *
     * @param color Colour to look for.
     * @param rect Rectangle to look in, in device pixels with a top-left origin.
     * @return Number of matching pixels inside the (clamped) rectangle.
     */
    [[nodiscard]] std::size_t countMatching(const Rgba8& color, const vine::graphics::Viewport& rect) const noexcept;

    /** @brief Counts pixels NOT equal to @p color inside @p rect (clamped to the image).
     *
     * The complementary question, and the one a "this part must be untouched" assertion needs.
     *
     * @param color Colour the region must not show.
     * @param rect Rectangle to look in.
     * @return Number of non-matching pixels inside the (clamped) rectangle.
     */
    [[nodiscard]] std::size_t countDifferingFrom(const Rgba8& color, const vine::graphics::Viewport& rect) const noexcept;

    /** @brief Gets whether every pixel equals @p color.
     *
     * @param color Colour the whole image must show.
     */
    [[nodiscard]] bool wholeImageMatches(const Rgba8& color) const noexcept;


  private:
    /** @brief Clamps @p rect to the image, returning false when nothing of it is inside. */
    [[nodiscard]] bool clamped(const vine::graphics::Viewport& rect, int& x0, int& y0, int& x1, int& y1) const noexcept;

    int                      width_{0};
    int                      height_{0};
    std::vector<std::uint8_t> pixels_;
};

}  // namespace core

V_VSG_NS_END
