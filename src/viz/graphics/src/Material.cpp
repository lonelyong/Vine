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
    // A NAME IS NOT SHADING: it never moves the revision (see revision()), so renaming a material cannot make a
    // consumer rebuild what it already answered.
    name_ = name;
}

Colorf Material::diffuse() const
{
    return diffuse_;
}

void Material::setDiffuse(const Colorf& color)
{
    // Each setter compares FIRST (see revision()): a host that re-applies the values it already set - the common
    // "recompute my settings every frame" shape - costs nothing, and a consumer keyed on the revision keeps what
    // it has instead of rebuilding an identical row.
    if (diffuse_ == color)
    {
        return;
    }
    diffuse_ = color;
    ++revision_;
}

Colorf Material::specular() const
{
    return specular_;
}

void Material::setSpecular(const Colorf& color)
{
    if (specular_ == color)
    {
        return;
    }
    specular_ = color;
    ++revision_;
}

Colorf Material::ambient() const
{
    return ambient_;
}

void Material::setAmbient(const Colorf& color)
{
    if (ambient_ == color)
    {
        return;
    }
    ambient_ = color;
    ++revision_;
}

float Material::shininess() const
{
    return shininess_;
}

void Material::setShininess(float shine)
{
    if (shininess_ == shine)
    {
        return;
    }
    shininess_ = shine;
    ++revision_;
}

raw_ptr<Texture> Material::texture() const
{
    return texture_.get();
}

void Material::setTexture(intrusive_ptr<Texture> texture)
{
    if (texture_.get() == texture.get())
    {
        return;
    }
    texture_ = texture;
    ++revision_;
}

std::uint64_t Material::revision() const
{
    return revision_;
}

VN_GRAPHICS_NS_END
