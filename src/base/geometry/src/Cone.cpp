#include <vine/geometry/Cone.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Cone, Primitive)

Cone::Cone()
{
    shape_type_ = ShapeType::Cone;
}

Cone::Cone(double radius, double height)
  : radius_(radius)
  , height_(height)
{
    shape_type_ = ShapeType::Cone;
}

double Cone::radius() const
{
    return radius_;
}

void Cone::setRadius(double radius)
{
    radius_ = radius;
}

double Cone::height() const
{
    return height_;
}

void Cone::setHeight(double height)
{
    height_ = height;
}

bool Cone::isValid() const
{
    return radius_ > 0.0 && height_ > 0.0;
}

bool Cone::hasVolume(double eps) const
{
    return radius_ > eps && height_ > eps;
}

VN_GEOMETRY_NS_END
