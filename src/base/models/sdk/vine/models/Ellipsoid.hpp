#pragma once

#include <vine/models/models_global.hpp>

#include <optional>

#include <vine/geom3d/EllipsoidSolid.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include <vine/models/Primitive.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief An axis-aligned ellipsoid with identity: three semi-axes, centred on the origin.
 *
 * @details
 * - The ellipsoid stores semi-axes, which is what its analytic kind states too, so `solid()` passes them through.
 */
class VN_MODELS_API Ellipsoid final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit sphere-shaped ellipsoid (all three radii 1).
     */
    Ellipsoid();

    /**
     * @brief Constructs an ellipsoid from its three semi-axes.
     *
     * @param radius_x Semi-axis along X; a value at or below 0 leaves the shape invalid.
     * @param radius_y Semi-axis along Y; a value at or below 0 leaves the shape invalid.
     * @param radius_z Semi-axis along Z; a value at or below 0 leaves the shape invalid.
     */
    Ellipsoid(float radius_x, float radius_y, float radius_z);

  public:
    /**
     * @brief Returns the semi-axis along X.
     * @return The semi-axis along X.
     */
    [[nodiscard]] float radiusX() const noexcept
    {
        return radius_x_;
    }

    /**
     * @brief Returns the semi-axis along Y.
     * @return The semi-axis along Y.
     */
    [[nodiscard]] float radiusY() const noexcept
    {
        return radius_y_;
    }

    /**
     * @brief Returns the semi-axis along Z.
     * @return The semi-axis along Z.
     */
    [[nodiscard]] float radiusZ() const noexcept
    {
        return radius_z_;
    }

    /**
     * @brief Sets the semi-axis along X.
     * @param radius The new semi-axis; a value at or below 0 leaves the shape invalid.
     */
    void setRadiusX(float radius) noexcept
    {
        radius_x_ = radius;
    }

    /**
     * @brief Sets the semi-axis along Y.
     * @param radius The new semi-axis; a value at or below 0 leaves the shape invalid.
     */
    void setRadiusY(float radius) noexcept
    {
        radius_y_ = radius;
    }

    /**
     * @brief Sets the semi-axis along Z.
     * @param radius The new semi-axis; a value at or below 0 leaves the shape invalid.
     */
    void setRadiusZ(float radius) noexcept
    {
        radius_z_ = radius;
    }

    /**
     * @brief Returns the analytic solid this ellipsoid describes.
     * @return The ellipsoid solid with the stored semi-axes.
     */
    [[nodiscard]] vn::geom3d::EllipsoidSolidf solid() const noexcept
    {
        return vn::geom3d::EllipsoidSolidf{ radius_x_, radius_y_, radius_z_ };
    }

    /**
     * @brief Returns whether every semi-axis is positive.
     * @return `true` when the ellipsoid can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the ellipsoid encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the ellipsoid encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the ellipsoid in its local space.
     * @return The box of the three semi-axes around the origin.
     */
    [[nodiscard]] vn::geom3d::Aabbf bounds() const override;

  private:
    /// Semi-axis along X; 1 for the unit ellipsoid.
    float radius_x_{ 1.0f };

    /// Semi-axis along Y; 1 for the unit ellipsoid.
    float radius_y_{ 1.0f };

    /// Semi-axis along Z; 1 for the unit ellipsoid.
    float radius_z_{ 1.0f };
};

VN_MODELS_NS_END
