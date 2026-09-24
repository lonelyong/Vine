#include <vine/geometry/PhongMaterial.hpp>

VN_GEOMETRY_NS_BEGIN

VN_OBJECT_META_IMPL(PhongMaterial, Material)

PhongMaterial::PhongMaterial()
{
    material_type_ = MaterialType::Phong;
}

PhongMaterial::PhongMaterial(const vn::Colorf& ambient, const vn::Colorf& diffuse,
    const vn::Colorf& specular, float shininess)
  : ambient_(ambient)
  , diffuse_(diffuse)
  , specular_(specular)
  , shininess_(shininess)
{
    material_type_ = MaterialType::Phong;
}

const vn::Colorf& PhongMaterial::ambient() const
{
    return ambient_;
}

void PhongMaterial::setAmbient(const vn::Colorf& ambient)
{
    ambient_ = ambient;
}

const vn::Colorf& PhongMaterial::diffuse() const
{
    return diffuse_;
}

void PhongMaterial::setDiffuse(const vn::Colorf& diffuse)
{
    diffuse_ = diffuse;
}

const vn::Colorf& PhongMaterial::specular() const
{
    return specular_;
}

void PhongMaterial::setSpecular(const vn::Colorf& specular)
{
    specular_ = specular;
}

float PhongMaterial::shininess() const
{
    return shininess_;
}

void PhongMaterial::setShininess(float shininess)
{
    shininess_ = shininess;
}

const char* PhongMaterial::typeName() const
{
    return "PhongMaterial";
}

VN_GEOMETRY_NS_END
