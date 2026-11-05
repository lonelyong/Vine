#include <vine/models/Primitive.hpp>

VN_MODELS_NS_BEGIN

VN_OBJECT_META_IMPL(Primitive, Shape)

Primitive::Primitive(ShapeType type) noexcept
{
    shape_type_ = type;
}

VN_MODELS_NS_END
