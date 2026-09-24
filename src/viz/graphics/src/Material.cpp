#include <vine/graphics/Material.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(Material, vn::Object);

Material::Material() = default;

String Material::name() const
{
    return name_;
}

void Material::setName(const String& name)
{
    name_ = name;
}

Colorf Material::diffuse() const
{
    return diffuse_;
}

void Material::setDiffuse(const Colorf& color)
{
    diffuse_ = color;
}

Colorf Material::specular() const
{
    return specular_;
}

void Material::setSpecular(const Colorf& color)
{
    specular_ = color;
}

Colorf Material::ambient() const
{
    return ambient_;
}

void Material::setAmbient(const Colorf& color)
{
    ambient_ = color;
}

float Material::shininess() const
{
    return shininess_;
}

void Material::setShininess(float shine)
{
    shininess_ = shine;
}

raw_ptr<Texture> Material::texture() const
{
    return texture_.get();
}

void Material::setTexture(intrusive_ptr<Texture> texture)
{
    texture_ = texture;
}

VN_GRAPHICS_NS_END
