#pragma once

#include <vine/models/models_global.hpp>

#include <optional>

#include <vine/geometry/ConeSolid.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include <vine/models/Primitive.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief A cone with identity: a base radius and a height, axis on +Z, base at `-height / 2`, apex at `+height / 2`.
 *
 * @details
 * - The cone stores the full height a description file states; `solid()` halves it for the analytic cone.
 */
class VN_MODELS_API Cone final : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit cone (base radius 1, height 1) around the origin.
     */
    Cone();

    /**
     * @brief Constructs a cone from its base radius and full height.
     *
     * @param radius The base radius; a value at or below 0 leaves the shape invalid.
     * @param height The full height; a value at or below 0 leaves the shape invalid.
     */
    Cone(float radius, float height);

  public:
    /**
     * @brief Returns the base radius.
     * @return The base radius.
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
     * @brief Sets the base radius.
     * @param radius The new base radius; a value at or below 0 leaves the shape invalid.
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
     * @brief Returns the analytic solid this cone describes.
     * @return The cone solid with the stored base radius and half height.
     */
    [[nodiscard]] vn::geometry::ConeSolidf solid() const noexcept
    {
        return vn::geometry::ConeSolidf{ radius_, height_ * 0.5f };
    }

    /**
     * @brief Returns whether the radius and height are positive.
     * @return `true` when the cone can be queried.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the cone encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the cone encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the cone in its local space.
     * @return The box of the base disk around the axis.
     */
    [[nodiscard]] vn::geometry::Aabbf bounds() const override;

  private:
    /// Base radius; 1 for the unit cone.
    float radius_{ 1.0f };

    /// Full height; 1 for the unit cone.
    float height_{ 1.0f };
};

VN_MODELS_NS_END
