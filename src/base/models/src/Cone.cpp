#include <vine/models/Cone.hpp>

VN_MODELS_NS_BEGIN

VN_OBJECT_META_IMPL(Cone, Primitive)

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

vn::geom3d::Aabbf Cone::bounds() const
{
    return solid().bounds();
}

VN_MODELS_NS_END
