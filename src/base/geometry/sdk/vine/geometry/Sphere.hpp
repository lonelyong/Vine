#pragma once

#include "geometry_global.hpp"

#include <vine/math/Types.hpp>

#include "Primitive.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A sphere primitive centered at the origin, sized in the shape's local space.
 */
class VN_GEOMETRY_API Sphere : public Primitive {
    VN_OBJECT_META(Sphere, Primitive);

  public:
    /**
     * @brief Constructs a unit sphere (radius 0.5).
     */
    Sphere()
    {
        shape_type_ = ShapeType::Sphere;
    }

    /**
     * @brief Constructs a sphere from its radius.
     *
     * @param radius Radius of the sphere.
     */
    explicit Sphere(float radius)
      : radius_(radius)
    {
        shape_type_ = ShapeType::Sphere;
    }

    /**
     * @brief Returns the radius of the sphere.
     *
     * @return Radius.
     */
    float radius() const
    {
        return radius_;
    }

    /**
     * @brief Sets the radius of the sphere.
     *
     * @param radius New radius.
     */
    void setRadius(float radius)
    {
        radius_ = radius;
    }

    [[nodiscard]]
    bool isValid() const override
    {
        return radius_ > 0.0f;
    }

    /**
     * @brief Returns whether the sphere encloses a volume.
     *
     * @param eps Numerical tolerance for the radius.
     * @return true when the radius is larger than eps.
     */
    [[nodiscard]]
    bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override
    {
        return radius_ > static_cast<float>(eps);
    }

  private:
    /// Radius of the sphere.
    float radius_ = 0.5f;
};

VN_GEOMETRY_NS_END
