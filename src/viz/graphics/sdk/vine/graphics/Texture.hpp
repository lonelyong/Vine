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

VN_GRAPHICS_NS_BEGIN

/**
 * @brief A texture the renderer samples: a description plus the CPU images that fill it.
 *
 * WHAT IT HOLDS. The sampling-facing facts — kind, size, pixel format, mip count — and the source images
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
 * KINDS. `D2` is one 2D image; `Cube` is six, each a 2D image of its own with its own mip chain.
 * Larger kinds (1D, 3D, arrays) are deliberately absent until something needs them, and adding one means
 * answering what `faceCount()` means for it.
 */
class VN_GRAPHICS_API Texture : public Object, public RefCounted<Texture> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief How many images of which arrangement a texture is made of. */
    enum class Kind {
        D2,   ///< One 2D image.
        Cube, ///< Six 2D images, one per cube face.
    };

  protected:
    /**
     * @brief Describes a texture, with every face left unfilled.
     *
     * The texture starts incomplete: a description is not content, and the faces are filled separately.
     *
     * PROTECTED, because a kind is a TYPE here rather than a constructor argument. A public constructor
     * taking a kind would let a caller describe a cube through a path that skips whatever invariants the
     * named type carries — that its faces are square, that they are addressed by name rather than by an
     * index whose meaning only Vulkan's layer order defines.
     *
     * @param kind      How many images the texture is made of.
     * @param width     Width in pixels of every face; must be positive.
     * @param height    Height in pixels of every face; must be positive.
     * @param format    Byte layout of one pixel; must be a format with a non-zero pixel size.
     * @param mip_count Number of mip levels including the base level; must lie in
     *                  `[1, Image::mipCapacity(width, height)]`.
     * @throws std::invalid_argument if any of the above is violated.
     */
    Texture(Kind kind, int width, int height, imaging::PixelFormat format, int mip_count = 1);

  public:
    /**
     * @brief Gets a stable, human-readable name for a kind, for diagnostics.
     *
     * @param kind Kind to name.
     * @return The kind's name, or `"Unknown"` for an out-of-range value.
     */
    [[nodiscard]] static const char* kindName(Kind kind) noexcept;

    /**
     * @brief Gets how many images this texture is made of.
     *
     * @return 1 for `D2`, 6 for `Cube`.
     */
    [[nodiscard]] int faceCount() const noexcept;

    /**
     * @brief Gets the kind this texture was described with.
     *
     * A NAMING answer, not the one a consumer branches on: a backend that needs to know how much to upload
     * asks `layerCount()`, so that adding a kind means adding a type rather than editing every switch.
     *
     * @return The kind.
     */
    [[nodiscard]] virtual Kind kind() const noexcept;

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
     * An index outside the kind is a query rather than a mistake, so it answers "no such face" instead of
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
    [[nodiscard]] bool isComplete() const noexcept;

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

    /**
     * @brief Gets how many images this texture is filled from.
     *
     * The one question a consumer that does not care WHICH kind it holds has to ask: an uploader walks
     * `0..layerCount()-1` through `layer()` and is done, so `D2` (1 layer), `Cube` (6) and a future array
     * (N) need no branch anywhere — which is what keeps a kind hierarchy from costing a switch per kind.
     *
     * @return The layer count, at least 1.
     */
    [[nodiscard]] int layerCount() const noexcept;

    /**
     * @brief Gets one layer's source image.
     *
     * The kind-agnostic spelling of `source()`: an index outside the kind is a query rather than a
     * mistake, so it answers null instead of failing.
     *
     * @param index Layer index; an out-of-range index yields null.
     * @return The filling image, or null when the layer is empty or does not exist.
     */
    [[nodiscard]] raw_ptr<const imaging::Image> layer(int index) const noexcept;

  private:
    std::vector<intrusive_ptr<const imaging::Image>> sources_;
    int                       width_ = 0;
    int                       height_ = 0;
    imaging::PixelFormat      format_ = imaging::PixelFormat::Unknown;
    int                       mip_count_ = 1;
    Kind                     kind_ = Kind::D2;
    std::uint64_t             revision_ = 0;
};

/**
 * @brief A texture of one 2D image.
 *
 * The named form of `Kind::D2`. It takes no layer index at all, because a 2D texture has exactly one image
 * and an index that can only ever be 0 is a mistake waiting to be written.
 */
class VN_GRAPHICS_API Texture2D : public Texture {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Describes a 2D texture, with its image left unfilled.
     *
     * @param width     Width in pixels; must be positive.
     * @param height    Height in pixels; must be positive.
     * @param format    Byte layout of one pixel; must be a format with a non-zero pixel size.
     * @param mip_count Number of mip levels including the base level; must lie in
     *                  `[1, Image::mipCapacity(width, height)]`.
     * @throws std::invalid_argument if any of the above is violated.
     */
    Texture2D(int width, int height, imaging::PixelFormat format, int mip_count = 1);

  public:
    /**
     * @brief Gets the kind, for diagnostics.
     *
     * @return Always `Kind::D2`.
     */
    [[nodiscard]] Kind kind() const noexcept override;

    /**
     * @brief Fills the texture with its image, or clears it.
     *
     * @param image The image filling the texture, which must match the description, or null to clear it.
     * @throws std::invalid_argument if @p image does not match the description.
     */
    void setImage(intrusive_ptr<const imaging::Image> image);

    /**
     * @brief Gets the image filling the texture.
     *
     * @return The image, or null while the texture is still empty.
     */
    [[nodiscard]] raw_ptr<const imaging::Image> image() const noexcept;
};

/**
 * @brief A texture of six 2D images forming a cube, one per face.
 *
 * The faces are NAMED rather than numbered, because the numbering is not a free choice: Vulkan fixes the
 * layer order to +X, -X, +Y, -Y, +Z, -Z, so a caller passing "4" would have to know that order to fill the
 * face it meant.
 *
 * The faces are square by construction: a cube map with non-square faces is a description no sampling
 * hardware accepts, so the size is stated once rather than width and height being two chances to disagree.
 */
class VN_GRAPHICS_API CubeMap : public Texture {
    VN_OBJECT_META_DECL;

  public:
    /** @brief One face of the cube, in Vulkan's layer order. */
    enum class Face {
        PosX, ///< Layer 0, +X.
        NegX, ///< Layer 1, -X.
        PosY, ///< Layer 2, +Y.
        NegY, ///< Layer 3, -Y.
        PosZ, ///< Layer 4, +Z.
        NegZ, ///< Layer 5, -Z.
    };

  public:
    /**
     * @brief Describes a cube map, with every face left unfilled.
     *
     * @param size      Width AND height in pixels of every face; must be positive.
     * @param format    Byte layout of one pixel; must be a format with a non-zero pixel size.
     * @param mip_count Number of mip levels including the base level; must lie in
     *                  `[1, Image::mipCapacity(size, size)]`.
     * @throws std::invalid_argument if any of the above is violated.
     */
    CubeMap(int size, imaging::PixelFormat format, int mip_count = 1);

  public:
    /**
     * @brief Gets the kind, for diagnostics.
     *
     * @return Always `Kind::Cube`.
     */
    [[nodiscard]] Kind kind() const noexcept override;

    /**
     * @brief Gets a stable, human-readable name for a face, for diagnostics.
     *
     * @param face Face to name.
     * @return The face's name, or `"Unknown"` for an out-of-range value.
     */
    [[nodiscard]] static const char* faceName(Face face) noexcept;

    /**
     * @brief Fills one face, or clears it.
     *
     * @param face  Face to fill.
     * @param image The image filling the face, which must match the description, or null to clear it.
     * @throws std::invalid_argument if @p image does not match the description.
     */
    void setFaceImage(Face face, intrusive_ptr<const imaging::Image> image);

    /**
     * @brief Gets one face's image.
     *
     * @param face Face to query.
     * @return The image filling that face, or null while it is still empty.
     */
    [[nodiscard]] raw_ptr<const imaging::Image> faceImage(Face face) const noexcept;
};

VN_GRAPHICS_NS_END
