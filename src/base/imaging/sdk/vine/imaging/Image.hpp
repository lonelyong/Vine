#pragma once

#include "imaging_global.hpp"

#include <cstddef>
#include <span>
#include <vector>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>

#include "PixelFormat.hpp"

VN_IMAGING_NS_BEGIN

/**
 * @brief One image's pixels in CPU memory, with an optional mip chain.
 *
 * This is the DATA side of an image, deliberately separate from the GPU object a renderer builds out of it:
 * nothing here knows about textures, samplers or a device, so the same type serves a decoded file, a
 * generated image, and the destination of a read-back — and it is usable in a process that has no renderer
 * at all.
 *
 * LAYOUT. One image is one packed 2D grid per mip level: rows are contiguous with no padding between them,
 * and the whole chain is one allocation, so the object is a single copy and a single free. The base level is
 * `width() x height()`; mip level N is `max(1, width >> N) x max(1, height >> N)`, the halving rule that
 * every backend and every file format already agrees on.
 *
 * WHAT IT IS NOT. A cube map is six images, not one image with six faces: a face IS an image, and adding a
 * face axis here would make every caller holding an ordinary 2D image carry a "faces = 1" special case.
 * A renderer that needs the six faces together groups six `Image`s; the grouping is the renderer's, not the
 * pixel data's.
 *
 * WHY AN OBJECT. Pixels are large and they are shared: many draws sample the same image, and a read-back
 * result is handed to whoever asked for it. `Image` is reference-counted like every other shared asset here,
 * so those hand-offs are ownership transfers rather than copies.
 *
 * ACCESS IS TOTAL. The mip accessors clamp an out-of-range index to the last level instead of failing,
 * because they are `noexcept` and a size query is not the place to discover a caller's bug. The constructor
 * is where an out-of-range request is rejected, and it throws rather than quietly allocating a smaller image
 * than the caller asked for.
 */
class VN_IMAGING_API Image : public Object, public RefCounted<Image> {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Allocates an image and its zero-initialised mip chain.
     *
     * The pixels start as zero bytes so that an image whose producer filled only part of it cannot leak
     * uninitialised memory into a render or a test.
     *
     * @param width     Width of the base level in pixels; must be positive.
     * @param height    Height of the base level in pixels; must be positive.
     * @param format    Byte layout of one pixel; must be a format with a non-zero pixel size, so
     *                  `PixelFormat::Unknown` and an out-of-range value are both rejected.
     * @param mip_count Number of mip levels including the base level; must lie in
     *                  `[1, mipCapacity(width, height)]`.
     * @throws std::invalid_argument if any of the above is violated.
     */
    Image(int width, int height, PixelFormat format, int mip_count = 1);

  public:
    /**
     * @brief Gets the largest legal mip count for a size.
     *
     * The chain stops where a level would be smaller than one pixel, so a 1x1 image has exactly one level.
     *
     * @param width  Width of the base level in pixels.
     * @param height Height of the base level in pixels.
     * @return The number of levels the size can hold, or 0 when either extent is not positive.
     */
    [[nodiscard]] static int mipCapacity(int width, int height) noexcept;

    /**
     * @brief Gets the width of the base level.
     *
     * @return The width in pixels.
     */
    [[nodiscard]] int width() const noexcept;

    /**
     * @brief Gets the height of the base level.
     *
     * @return The height in pixels.
     */
    [[nodiscard]] int height() const noexcept;

    /**
     * @brief Gets the byte layout of one pixel.
     *
     * All mip levels share it; a chain cannot change format partway down.
     *
     * @return The pixel format this image was allocated with.
     */
    [[nodiscard]] PixelFormat format() const noexcept;

    /**
     * @brief Gets how many mip levels this image owns.
     *
     * @return The mip count, always at least 1.
     */
    [[nodiscard]] int mipCount() const noexcept;

    /**
     * @brief Gets the width of one mip level.
     *
     * @param mip Mip level, 0 being the base; an out-of-range index clamps to the last level.
     * @return The level's width in pixels, never below 1.
     */
    [[nodiscard]] int mipWidth(int mip) const noexcept;

    /**
     * @brief Gets the height of one mip level.
     *
     * @param mip Mip level, 0 being the base; an out-of-range index clamps to the last level.
     * @return The level's height in pixels, never below 1.
     */
    [[nodiscard]] int mipHeight(int mip) const noexcept;

    /**
     * @brief Gets the size of one mip level in bytes.
     *
     * @param mip Mip level, 0 being the base; an out-of-range index clamps to the last level.
     * @return The level's size in bytes.
     */
    [[nodiscard]] std::size_t mipByteSize(int mip) const noexcept;

    /**
     * @brief Gets one mip level's pixels, mutably.
     *
     * @param mip Mip level, 0 being the base; an out-of-range index clamps to the last level.
     * @return A view of the level's `mipByteSize(mip)` bytes inside this image.
     */
    [[nodiscard]] std::span<std::byte> mipData(int mip) noexcept;

    /**
     * @brief Gets one mip level's pixels.
     *
     * @param mip Mip level, 0 being the base; an out-of-range index clamps to the last level.
     * @return A view of the level's `mipByteSize(mip)` bytes inside this image.
     */
    [[nodiscard]] std::span<const std::byte> mipData(int mip) const noexcept;

    /**
     * @brief Gets the size of the whole chain in bytes.
     *
     * This is the size of the single allocation behind the image, so it is also what one copy of the image
     * costs.
     *
     * @return The total size of every mip level in bytes.
     */
    [[nodiscard]] std::size_t totalByteSize() const noexcept;

  private:
    std::vector<std::size_t> mip_offsets_;
    std::vector<std::byte>   pixels_;
    int                      width_ = 0;
    int                      height_ = 0;
    PixelFormat              format_ = PixelFormat::Unknown;
    int                      mip_count_ = 1;
};

VN_IMAGING_NS_END
