#pragma once

#include <vine/models/models_global.hpp>

#include <optional>

#include <vine/geom/spatial/BoxSolid.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include <vine/models/Primitive.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief A box with identity: three full extents in its own local space, centred on the origin.
 *
 * @details
 * - `width()` / `height()` / `depth()` are the full extents, the way a description file states them; `solid()` halves
 *   them for the analytic box it returns, and nothing stores the halves.
 * - The box stores its own extents, so a setter is visible to `bounds()` on the next call and no cache can go stale.
 * - A setter does not advance the revision: the holder reports the edit once it is complete (`Shape::bumpRevision`).
 *
 * @warning
 * - A dimension at or below 0 is accepted here and reported by `isValid()`, so a partially built shape never throws.
 */
class VN_MODELS_API Box final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit box (1 x 1 x 1) centred on the origin.
     */
    Box();

    /**
     * @brief Constructs a box from its full extents.
     *
     * @param width  Extent along X.
     * @param height Extent along Y.
     * @param depth  Extent along Z.
     */
    Box(float width, float height, float depth);

  public:
    /**
     * @brief Returns the extent along X.
     * @return The full extent along X.
     */
    [[nodiscard]] float width() const noexcept
    {
        return width_;
    }

    /**
     * @brief Returns the extent along Y.
     * @return The full extent along Y.
     */
    [[nodiscard]] float height() const noexcept
    {
        return height_;
    }

    /**
     * @brief Returns the extent along Z.
     * @return The full extent along Z.
     */
    [[nodiscard]] float depth() const noexcept
    {
        return depth_;
    }

    /**
     * @brief Sets the extent along X.
     * @param width The new full extent along X; a value at or below 0 leaves the shape invalid.
     */
    void setWidth(float width) noexcept
    {
        width_ = width;
    }

    /**
     * @brief Sets the extent along Y.
     * @param height The new full extent along Y; a value at or below 0 leaves the shape invalid.
     */
    void setHeight(float height) noexcept
    {
        height_ = height;
    }

    /**
     * @brief Sets the extent along Z.
     * @param depth The new full extent along Z; a value at or below 0 leaves the shape invalid.
     */
    void setDepth(float depth) noexcept
    {
        depth_ = depth;
    }

    /**
     * @brief Returns the analytic solid this box describes, built from the current extents.
     *
     * @details
     * - The solid is a value, not stored state: the extents above are the only copy, and the halves exist for the call
     *   only. A caller that needs a `vn::geom::spatial::Solidf&` keeps the returned value alive for that long.
     *
     * @return The box solid with half extents.
     */
    [[nodiscard]] vn::geom::spatial::BoxSolidf solid() const noexcept
    {
        return vn::geom::spatial::BoxSolidf{ width_ * 0.5f, height_ * 0.5f, depth_ * 0.5f };
    }

    /**
     * @brief Returns whether every extent is positive.
     * @return `true` when the box can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the box encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the box encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the box in its local space.
     * @return The box of half extents around the origin.
     */
    [[nodiscard]] vn::geom::spatial::Aabbf bounds() const override;

  private:
    /// Extent along X; 1 for the unit box.
    float width_{ 1.0f };

    /// Extent along Y; 1 for the unit box.
    float height_{ 1.0f };

    /// Extent along Z; 1 for the unit box.
    float depth_{ 1.0f };
};

VN_MODELS_NS_END
