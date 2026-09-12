#pragma once

#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/RefCounted.hpp>

V_GRAPHICS_NS_BEGIN

/**
 * @brief A texture the renderer samples: a description plus the CPU images that fill it.
 *
 * WHAT IT HOLDS. The sampling-facing facts — shape, size, pixel format, mip count — and the source images
 * that fill it, one per face.
 * The description exists on its own so a texture can be created once and filled face by face, and it is the
 * description that a source image must match: a mismatch is rejected where it is made, not discovered at
 * upload time.
 *
 * WHY IT IS NOT AN `imaging::Image`. An `Image` is pixels in CPU memory and knows nothing about sampling;
 * a `Texture` is what the renderer samples and need not have pixels at all yet. Neither expresses the other:
 * a read-back image has no sampling role, and a texture is what a source image has to agree with.
 *
 * WHY IT IS NOT THE GPU OBJECT EITHER. Like `RenderTarget`, this is a LOGICAL description: it owns no GPU
 * resource and no device handle.
 * The backend that can create textures owns them and materialises this description — the same division that
 * lets a pipeline be built, validated and tested with no device present.
 *
 * SHAPES. `D2` is one 2D image; `Cube` is six, each a 2D image of its own with its own mip chain.
 * Larger shapes (1D, 3D, arrays) are deliberately absent until something needs them, and adding one means
 * answering what `faceCount()` means for it.
 */
class V_GRAPHICS_API Texture : public Object, public RefCounted<Texture> {
    V_OBJECT_META_DECL;

  public:
    /** @brief How many images of which arrangement a texture is made of. */
    enum class Shape {
        D2,   ///< One 2D image.
        Cube, ///< Six 2D images, one per cube face.
    };

  public:
    /**
     * @brief Describes a texture, with every face left unfilled.
     *
     * The texture starts incomplete: a description is not content, and the faces are filled separately.
     *
     * @param shape     How many images the texture is made of.
     * @param width     Width in pixels of every face; must be positive.
     * @param height    Height in pixels of every face; must be positive.
     * @param format    Byte layout of one pixel; must be a format with a non-zero pixel size.
     * @param mip_count Number of mip levels including the base level; must lie in
     *                  `[1, Image::mipCapacity(width, height)]`.
     * @throws std::invalid_argument if any of the above is violated.
     */
    Texture(Shape shape, int width, int height, imaging::PixelFormat format, int mip_count = 1);

  public:
    /**
     * @brief Gets a stable, human-readable name for a shape, for diagnostics.
     *
     * @param shape Shape to name.
     * @return The shape's name, or `"Unknown"` for an out-of-range value.
     */
    [[nodiscard]] static const char* shapeName(Shape shape) noexcept;

    /**
     * @brief Gets how many images this texture is made of.
     *
     * @return 1 for `D2`, 6 for `Cube`.
     */
    [[nodiscard]] int faceCount() const noexcept;

    /**
     * @brief Gets the shape this texture was described with.
     *
     * @return The shape.
     */
    [[nodiscard]] Shape shape() const noexcept;

    /**
     * @brief Gets the width in pixels of every face.
     *
     * All faces share the size, so there is one width for the texture rather than one per face.
     *
     * @return The width in pixels.
     */
    [[nodiscard]] int width() const noexcept;

    /**
     * @brief Gets the height in pixels of every face.
     *
     * @return The height in pixels.
     */
    [[nodiscard]] int height() const noexcept;

    /**
     * @brief Gets the byte layout of one pixel.
     *
     * All faces and all mip levels share it.
     *
     * @return The pixel format this texture was described with.
     */
    [[nodiscard]] imaging::PixelFormat format() const noexcept;

    /**
     * @brief Gets how many mip levels every face has.
     *
     * @return The mip count, always at least 1.
     */
    [[nodiscard]] int mipCount() const noexcept;

    /**
     * @brief Fills one face with its source image, or clears it.
     *
     * A source image must agree with the description on format, size and mip count, because the texture
     * would otherwise describe something other than what it is filled with — a state the backend could only
     * discover by uploading the wrong amount of data.
     *
     * @param face  Face index in `[0, faceCount())`.
     * @param image The image filling this face, or null to clear the face.
     * @throws std::out_of_range if @p face is outside `[0, faceCount())`.
     * @throws std::invalid_argument if @p image does not match the description.
     */
    void setSource(int face, intrusive_ptr<const imaging::Image> image);

    /**
     * @brief Gets one face's source image.
     *
     * An index outside the shape is a query rather than a mistake, so it answers "no such face" instead of
     * failing; use `setSource()` to discover an out-of-range face.
     *
     * @param face Face index; an out-of-range index yields null.
     * @return The filling image, or null when the face is empty or does not exist.
     */
    [[nodiscard]] raw_ptr<const imaging::Image> source(int face) const noexcept;

    /**
     * @brief States whether one face holds a source image.
     *
     * @param face Face index; an out-of-range index yields false.
     * @return true when the face exists and is filled.
     */
    [[nodiscard]] bool hasSource(int face) const noexcept;

    /**
     * @brief States whether every face holds a source image.
     *
     * A complete texture is what the backend can upload; an incomplete one is a description still being
     * filled, which is why this is reported rather than prevented.
     *
     * @return true when no face is empty.
     */
    [[nodiscard]] bool complete() const noexcept;

    /**
     * @brief Gets the content revision.
     *
     * Bumped by every fill of a face, so retained backend state can tell "the same texture, still the same
     * pixels" from "the same texture, new pixels".
     * A pointer alone cannot: a backend caches the uploaded image per texture ADDRESS, so without a
     * revision a re-filled texture would keep sampling the old upload — the same failure the shader
     * program's revision exists to prevent.
     *
     * @return Monotonic revision counter (starts at 0).
     */
    [[nodiscard]] std::uint64_t revision() const noexcept;

  private:
    std::vector<intrusive_ptr<const imaging::Image>> sources_;
    int                       width_ = 0;
    int                       height_ = 0;
    imaging::PixelFormat      format_ = imaging::PixelFormat::Unknown;
    int                       mip_count_ = 1;
    Shape                     shape_ = Shape::D2;
    std::uint64_t             revision_ = 0;
};

V_GRAPHICS_NS_END
