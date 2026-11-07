#include <vine/models/Ellipsoid.hpp>

VN_MODELS_NS_BEGIN

VN_OBJECT_META_IMPL(Ellipsoid, Primitive)

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

vn::geom::spatial::Aabbf Ellipsoid::bounds() const
{
    return solid().bounds();
}

VN_MODELS_NS_END
