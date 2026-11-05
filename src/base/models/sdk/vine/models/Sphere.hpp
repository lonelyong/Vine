#pragma once

#include <vine/models/models_global.hpp>

#include <optional>

#include <vine/geom3d/SphereSolid.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include <vine/models/Primitive.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief A sphere with identity: one radius in its own local space, centred on the origin.
 *
 * @details
 * - The sphere stores its radius; `solid()` builds the analytic sphere for the call (see `Box`).
 */
class VN_MODELS_API Sphere final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit sphere (radius 1) centred on the origin.
     */
    Sphere();

    /**
     * @brief Constructs a sphere of the given radius.
     *
     * @param radius The radius; a value at or below 0 leaves the shape invalid.
     */
    explicit Sphere(float radius);

  public:
    /**
     * @brief Returns the radius.
     * @return The radius.
     */
    [[nodiscard]] float radius() const noexcept
    {
        return radius_;
    }

    /**
     * @brief Sets the radius.
     * @param radius The new radius; a value at or below 0 leaves the shape invalid.
     */
    void setRadius(float radius) noexcept
    {
        radius_ = radius;
    }

    /**
     * @brief Returns the analytic solid this sphere describes.
     * @return The sphere solid with the stored radius.
     */
    [[nodiscard]] vn::geom3d::SphereSolidf solid() const noexcept
    {
        return vn::geom3d::SphereSolidf{ radius_ };
    }

    /**
     * @brief Returns whether the radius is positive.
     * @return `true` when the sphere can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the sphere encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the sphere encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the sphere in its local space.
     * @return The cube of side `2 * radius` around the origin.
     */
    [[nodiscard]] vn::geom3d::Aabbf bounds() const override;

  private:
    /// Radius; 1 for the unit sphere.
    float radius_{ 1.0f };
};

VN_MODELS_NS_END
