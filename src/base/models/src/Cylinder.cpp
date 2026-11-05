#include <vine/models/Cylinder.hpp>

VN_MODELS_NS_BEGIN

VN_OBJECT_META_IMPL(Cylinder, Primitive)

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

vn::geometry::Aabbf Cylinder::bounds() const
{
    return solid().bounds();
}

VN_MODELS_NS_END
