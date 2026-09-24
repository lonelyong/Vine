#include <vine/geometry/Sphere.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Sphere, Primitive)

Sphere::Sphere()
{
    shape_type_ = ShapeType::Sphere;
}

Sphere::Sphere(double radius)
  : radius_(radius)
{
    shape_type_ = ShapeType::Sphere;
}

double Sphere::radius() const
{
    return radius_;
}

void Sphere::setRadius(double radius)
{
    radius_ = radius;
}

bool Sphere::isValid() const
{
    return radius_ > 0.0;
}

bool Sphere::hasVolume(double eps) const
{
    return radius_ > eps;
}

VN_GEOMETRY_NS_END
