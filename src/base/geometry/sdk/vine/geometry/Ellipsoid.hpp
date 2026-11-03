#pragma once

#include "geometry_global.hpp"

#include <vine/math/Types.hpp>

#include "Primitive.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief An axis-aligned ellipsoid primitive centered at the origin, sized in the shape's local space.
 */
class VN_GEOMETRY_API Ellipsoid : public Primitive {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a unit ellipsoid (all radii 0.5).
     */
    Ellipsoid()
    {
        shape_type_ = ShapeType::Ellipsoid;
    }

    /**
     * @brief Constructs an ellipsoid from its semi-axis lengths.
     *
     * @param radius_x Semi-axis length along X.
     * @param radius_y Semi-axis length along Y.
     * @param radius_z Semi-axis length along Z.
     */
    Ellipsoid(float radius_x, float radius_y, float radius_z)
      : radius_x_(radius_x)
      , radius_y_(radius_y)
      , radius_z_(radius_z)
    {
        shape_type_ = ShapeType::Ellipsoid;
    }

    /**
     * @brief Returns the semi-axis length along X.
     *
     * @return Radius along X.
     */
    float radiusX() const
    {
        return radius_x_;
    }

    /**
     * @brief Sets the semi-axis length along X.
     *
     * @param radius New radius along X.
     */
    void setRadiusX(float radius)
    {
        radius_x_ = radius;
    }

    /**
     * @brief Returns the semi-axis length along Y.
     *
     * @return Radius along Y.
     */
    float radiusY() const
    {
        return radius_y_;
    }

    /**
     * @brief Sets the semi-axis length along Y.
     *
     * @param radius New radius along Y.
     */
    void setRadiusY(float radius)
    {
        radius_y_ = radius;
    }

    /**
     * @brief Returns the semi-axis length along Z.
     *
     * @return Radius along Z.
     */
    float radiusZ() const
    {
        return radius_z_;
    }

    /**
     * @brief Sets the semi-axis length along Z.
     *
     * @param radius New radius along Z.
     */
    void setRadiusZ(float radius)
    {
        radius_z_ = radius;
    }

    [[nodiscard]]
    bool isValid() const override
    {
        return radius_x_ > 0.0f && radius_y_ > 0.0f && radius_z_ > 0.0f;
    }

    /**
     * @brief Returns whether the ellipsoid encloses a volume.
     *
     * @param eps Numerical tolerance for the semi-axis lengths.
     * @return true when all three semi-axis lengths are larger than eps.
     */
    [[nodiscard]]
    bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override
    {
        const float tolerance = static_cast<float>(eps);
        return radius_x_ > tolerance && radius_y_ > tolerance && radius_z_ > tolerance;
    }

  private:
    /// Semi-axis length along X.
    float radius_x_ = 0.5f;
    /// Semi-axis length along Y.
    float radius_y_ = 0.5f;
    /// Semi-axis length along Z.
    float radius_z_ = 0.5f;
};

VN_GEOMETRY_NS_END
