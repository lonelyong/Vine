#include <vine/geometry/Primitive.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(Primitive, Shape)

VN_OBJECT_META_IMPL(Box, Primitive)
VN_OBJECT_META_IMPL(Sphere, Primitive)
VN_OBJECT_META_IMPL(Cylinder, Primitive)
VN_OBJECT_META_IMPL(Cone, Primitive)
VN_OBJECT_META_IMPL(Ellipsoid, Primitive)

Primitive::Primitive(ShapeType type) noexcept
{
    shape_type_ = type;
}

Box::Box()
  : Primitive(ShapeType::Box)
{}

Box::Box(float width, float height, float depth)
  : Primitive(ShapeType::Box)
  , width_(width)
  , height_(height)
  , depth_(depth)
{}

bool Box::isValid() const
{
    return solid().isValid();
}

bool Box::hasVolume(double eps) const
{
    return solid().hasVolume(static_cast<float>(eps));
}

Aabbf Box::bounds() const
{
    return solid().bounds();
}

Sphere::Sphere()
  : Primitive(ShapeType::Sphere)
{}

Sphere::Sphere(float radius)
  : Primitive(ShapeType::Sphere)
  , radius_(radius)
{}

bool Sphere::isValid() const
{
    return solid().isValid();
}

bool Sphere::hasVolume(double eps) const
{
    return solid().hasVolume(static_cast<float>(eps));
}

Aabbf Sphere::bounds() const
{
    return solid().bounds();
}

Cylinder::Cylinder()
  : Primitive(ShapeType::Cylinder)
{}

Cylinder::Cylinder(float radius, float height)
  : Primitive(ShapeType::Cylinder)
  , radius_(radius)
  , height_(height)
{}

bool Cylinder::isValid() const
{
    return solid().isValid();
}

bool Cylinder::hasVolume(double eps) const
{
    return solid().hasVolume(static_cast<float>(eps));
}

Aabbf Cylinder::bounds() const
{
    return solid().bounds();
}

Cone::Cone()
  : Primitive(ShapeType::Cone)
{}

Cone::Cone(float radius, float height)
  : Primitive(ShapeType::Cone)
  , radius_(radius)
  , height_(height)
{}

bool Cone::isValid() const
{
    return solid().isValid();
}

bool Cone::hasVolume(double eps) const
{
    return solid().hasVolume(static_cast<float>(eps));
}

Aabbf Cone::bounds() const
{
    return solid().bounds();
}

Ellipsoid::Ellipsoid()
  : Primitive(ShapeType::Ellipsoid)
{}

Ellipsoid::Ellipsoid(float radius_x, float radius_y, float radius_z)
  : Primitive(ShapeType::Ellipsoid)
  , radius_x_(radius_x)
  , radius_y_(radius_y)
  , radius_z_(radius_z)
{}

bool Ellipsoid::isValid() const
{
    return solid().isValid();
}

bool Ellipsoid::hasVolume(double eps) const
{
    return solid().hasVolume(static_cast<float>(eps));
}

Aabbf Ellipsoid::bounds() const
{
    return solid().bounds();
}

VN_GEOMETRY_NS_END
