#pragma once

#include "geometry_global.hpp"

#include <vine/math/Types.hpp>

#include "Primitive.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A cylinder primitive centered on the Z axis, spanning -height/2 to +height/2 in the shape's local space.
 */
class VN_GEOMETRY_API Cylinder : public Primitive {
    VN_OBJECT_META(Cylinder, Primitive);

  public:
    /**
     * @brief Constructs a unit cylinder (radius 0.5, height 1.0).
     */
    Cylinder()
    {
        shape_type_ = ShapeType::Cylinder;
    }

    /**
     * @brief Constructs a cylinder from its dimensions.
     *
     * @param radius Radius of the circular cross-section.
     * @param height Extent along the Z axis.
     */
    Cylinder(float radius, float height)
      : radius_(radius)
      , height_(height)
    {
        shape_type_ = ShapeType::Cylinder;
    }

    /**
     * @brief Returns the radius of the circular cross-section.
     *
     * @return Radius.
     */
    float radius() const
    {
        return radius_;
    }

    /**
     * @brief Sets the radius of the circular cross-section.
     *
     * @param radius New radius.
     */
    void setRadius(float radius)
    {
        radius_ = radius;
    }

    /**
     * @brief Returns the extent along the Z axis.
     *
     * @return Height.
     */
    float height() const
    {
        return height_;
    }

    /**
     * @brief Sets the extent along the Z axis.
     *
     * @param height New height.
     */
    void setHeight(float height)
    {
        height_ = height;
    }

    [[nodiscard]]
    bool isValid() const override
    {
        return radius_ > 0.0f && height_ > 0.0f;
    }

    /**
     * @brief Returns whether the cylinder encloses a volume.
     *
     * @param eps Numerical tolerance for the dimensions.
     * @return true when both radius and height are larger than eps.
     */
    [[nodiscard]]
    bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override
    {
        const float tolerance = static_cast<float>(eps);
        return radius_ > tolerance && height_ > tolerance;
    }

  private:
    /// Radius of the circular cross-section.
    float radius_ = 0.5f;
    /// Extent along the Z axis.
    float height_ = 1.0f;
};

VN_GEOMETRY_NS_END
