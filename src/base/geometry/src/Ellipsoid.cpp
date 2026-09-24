#include <vine/geometry/Ellipsoid.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Ellipsoid, Primitive)

Ellipsoid::Ellipsoid()
{
    shape_type_ = ShapeType::Ellipsoid;
}

Ellipsoid::Ellipsoid(double radius_x, double radius_y, double radius_z)
  : radius_x_(radius_x)
  , radius_y_(radius_y)
  , radius_z_(radius_z)
{
    shape_type_ = ShapeType::Ellipsoid;
}

double Ellipsoid::radiusX() const
{
    return radius_x_;
}

void Ellipsoid::setRadiusX(double radius)
{
    radius_x_ = radius;
}

double Ellipsoid::radiusY() const
{
    return radius_y_;
}

void Ellipsoid::setRadiusY(double radius)
{
    radius_y_ = radius;
}

double Ellipsoid::radiusZ() const
{
    return radius_z_;
}

void Ellipsoid::setRadiusZ(double radius)
{
    radius_z_ = radius;
}

bool Ellipsoid::isValid() const
{
    return radius_x_ > 0.0 && radius_y_ > 0.0 && radius_z_ > 0.0;
}

bool Ellipsoid::hasVolume(double eps) const
{
    return radius_x_ > eps && radius_y_ > eps && radius_z_ > eps;
}

VN_GEOMETRY_NS_END
