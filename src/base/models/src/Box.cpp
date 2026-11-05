#include <vine/models/Box.hpp>

VN_MODELS_NS_BEGIN

VN_OBJECT_META_IMPL(Box, Primitive)

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

vn::geom3d::Aabbf Box::bounds() const
{
    return solid().bounds();
}

VN_MODELS_NS_END
