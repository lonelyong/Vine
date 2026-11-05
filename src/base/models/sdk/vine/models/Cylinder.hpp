#pragma once

#include <vine/models/models_global.hpp>

#include <optional>

#include <vine/geom3d/CylinderSolid.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include <vine/models/Primitive.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief A cylinder with identity: a radius and a height, axis on +Z, spanning `[-height / 2, +height / 2]`.
 *
 * @details
 * - The cylinder stores the full height a description file states; `solid()` halves it for the analytic cylinder.
 */
class VN_MODELS_API Cylinder final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit cylinder (radius 1, height 1) around the origin.
     */
    Cylinder();

    /**
     * @brief Constructs a cylinder from its radius and full height.
     *
     * @param radius The radius; a value at or below 0 leaves the shape invalid.
     * @param height The full height; a value at or below 0 leaves the shape invalid.
     */
    Cylinder(float radius, float height);

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
     * @brief Returns the full height.
     * @return The full height.
     */
    [[nodiscard]] float height() const noexcept
    {
        return height_;
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
     * @brief Sets the height.
     * @param height The new full height; a value at or below 0 leaves the shape invalid.
     */
    void setHeight(float height) noexcept
    {
        height_ = height;
    }

    /**
     * @brief Returns the analytic solid this cylinder describes.
     * @return The cylinder solid with the stored radius and half height.
     */
    [[nodiscard]] vn::geom3d::CylinderSolidf solid() const noexcept
    {
        return vn::geom3d::CylinderSolidf{ radius_, height_ * 0.5f };
    }

    /**
     * @brief Returns whether the radius and height are positive.
     * @return `true` when the cylinder can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the cylinder encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the cylinder encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the cylinder in its local space.
     * @return The box of the full diameter around the axis.
     */
    [[nodiscard]] vn::geom3d::Aabbf bounds() const override;

  private:
    /// Radius; 1 for the unit cylinder.
    float radius_{ 1.0f };

    /// Full height; 1 for the unit cylinder.
    float height_{ 1.0f };
};

VN_MODELS_NS_END
