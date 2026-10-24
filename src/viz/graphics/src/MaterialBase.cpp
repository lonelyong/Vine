#include <vine/graphics/MaterialBase.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(MaterialBase, vn::Object);

String MaterialBase::name() const
{
    return name_;
}

void MaterialBase::setName(const String& name)
{
    // A NAME IS NOT SHADING: it never moves the revision (see revision()), so renaming a material cannot make a
    // consumer rebuild what it already answered.
    name_ = name;
}

Colorf MaterialBase::baseColor() const noexcept
{
    return resolved().base_color;
}

Colorf MaterialBase::emissive() const noexcept
{
    return resolved().emissive;
}

float MaterialBase::metallic() const noexcept
{
    return resolved().metallic;
}

float MaterialBase::roughness() const noexcept
{
    return resolved().roughness;
}

float MaterialBase::occlusion() const noexcept
{
    return resolved().occlusion;
}

float MaterialBase::alphaCutoff() const noexcept
{
    return resolved().alpha_cutoff;
}

raw_ptr<Texture> MaterialBase::texture(TextureSlot slot) const noexcept
{
    return resolved().textures[static_cast<std::size_t>(slot)].texture.get();
}

const Sampler& MaterialBase::sampler(TextureSlot slot) const noexcept
{
    return resolved().textures[static_cast<std::size_t>(slot)].sampler;
}

bool MaterialBase::hasTexture(TextureSlot slot) const noexcept
{
    return resolved().textures[static_cast<std::size_t>(slot)].texture != nullptr;
}

VN_GRAPHICS_NS_END
