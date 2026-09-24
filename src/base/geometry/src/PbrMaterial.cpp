#include <vine/geometry/PbrMaterial.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(PbrMaterial, Material)

PbrMaterial::PbrMaterial()
{
    material_type_ = MaterialType::Pbr;
}

PbrMaterial::PbrMaterial(const vn::Colorf& base_color, float metallic, float roughness, float opacity)
  : base_color_(base_color)
  , metallic_(metallic)
  , roughness_(roughness)
  , opacity_(opacity)
{
    material_type_ = MaterialType::Pbr;
}

const vn::Colorf& PbrMaterial::baseColor() const
{
    return base_color_;
}

void PbrMaterial::setBaseColor(const vn::Colorf& color)
{
    base_color_ = color;
}

const vn::Colorf& PbrMaterial::emissive() const
{
    return emissive_;
}

void PbrMaterial::setEmissive(const vn::Colorf& color)
{
    emissive_ = color;
}

float PbrMaterial::metallic() const
{
    return metallic_;
}

void PbrMaterial::setMetallic(float metallic)
{
    metallic_ = metallic;
}

float PbrMaterial::roughness() const
{
    return roughness_;
}

void PbrMaterial::setRoughness(float roughness)
{
    roughness_ = roughness;
}

float PbrMaterial::opacity() const
{
    return opacity_;
}

void PbrMaterial::setOpacity(float opacity)
{
    opacity_ = opacity;
}

const char* PbrMaterial::typeName() const
{
    return "PbrMaterial";
}

VN_GEOMETRY_NS_END
