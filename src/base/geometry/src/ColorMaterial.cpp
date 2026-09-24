#include <vine/geometry/ColorMaterial.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(ColorMaterial, Material)

ColorMaterial::ColorMaterial()
{
    material_type_ = MaterialType::Color;
}

ColorMaterial::ColorMaterial(const vn::Colorf& color)
  : color_(color)
{
    material_type_ = MaterialType::Color;
}

const vn::Colorf& ColorMaterial::color() const
{
    return color_;
}

void ColorMaterial::setColor(const vn::Colorf& color)
{
    color_ = color;
}

const char* ColorMaterial::typeName() const
{
    return "ColorMaterial";
}

VN_GEOMETRY_NS_END
