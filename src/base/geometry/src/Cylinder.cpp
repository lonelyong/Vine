#include <vine/geometry/Cylinder.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Cylinder, Primitive)

Cylinder::Cylinder()
{
    shape_type_ = ShapeType::Cylinder;
}

Cylinder::Cylinder(double radius, double height)
  : radius_(radius)
  , height_(height)
{
    shape_type_ = ShapeType::Cylinder;
}

double Cylinder::radius() const
{
    return radius_;
}

void Cylinder::setRadius(double radius)
{
    radius_ = radius;
}

double Cylinder::height() const
{
    return height_;
}

void Cylinder::setHeight(double height)
{
    height_ = height;
}

bool Cylinder::isValid() const
{
    return radius_ > 0.0 && height_ > 0.0;
}

bool Cylinder::hasVolume(double eps) const
{
    return radius_ > eps && height_ > eps;
}

VN_GEOMETRY_NS_END
