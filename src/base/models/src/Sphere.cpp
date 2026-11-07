#include <vine/models/Sphere.hpp>

VN_MODELS_NS_BEGIN

VN_OBJECT_META_IMPL(Sphere, Primitive)

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

vn::geom::spatial::Aabbf Sphere::bounds() const
{
    return solid().bounds();
}

VN_MODELS_NS_END
