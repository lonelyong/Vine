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

Colorf Material::baseColor() const
{
    return base_color_;
}

void Material::setBaseColor(const Colorf& color)
{
    // Each setter compares FIRST (see revision()): a host that re-applies the values it already set - the common
    // "recompute my settings every frame" shape - costs nothing, and a consumer keyed on the revision keeps what
    // it has instead of rebuilding an identical row.
    if (base_color_ == color)
    {
        return;
    }
    base_color_ = color;
    ++revision_;
}

Colorf Material::emissive() const
{
    return emissive_;
}

void Material::setEmissive(const Colorf& color)
{
    if (emissive_ == color)
    {
        return;
    }
    emissive_ = color;
    ++revision_;
}

float Material::metallic() const
{
    return metallic_;
}

void Material::setMetallic(float metallic)
{
    if (metallic_ == metallic)
    {
        return;
    }
    metallic_ = metallic;
    ++revision_;
}

float Material::roughness() const
{
    return roughness_;
}

void Material::setRoughness(float roughness)
{
    if (roughness_ == roughness)
    {
        return;
    }
    roughness_ = roughness;
    ++revision_;
}

float Material::occlusion() const
{
    return occlusion_;
}

void Material::setOcclusion(float occlusion)
{
    if (occlusion_ == occlusion)
    {
        return;
    }
    occlusion_ = occlusion;
    ++revision_;
}

AlphaMode Material::alphaMode() const
{
    return alpha_mode_;
}

void Material::setAlphaMode(AlphaMode mode)
{
    if (alpha_mode_ == mode)
    {
        return;
    }
    alpha_mode_ = mode;
    ++revision_;
}

float Material::alphaCutoff() const
{
    return alpha_cutoff_;
}

void Material::setAlphaCutoff(float cutoff)
{
    if (alpha_cutoff_ == cutoff)
    {
        return;
    }
    alpha_cutoff_ = cutoff;
    ++revision_;
}

raw_ptr<Texture> Material::texture(TextureSlot slot) const
{
    return textures_[static_cast<std::size_t>(slot)].texture.get();
}

const Sampler& Material::sampler(TextureSlot slot) const
{
    return textures_[static_cast<std::size_t>(slot)].sampler;
}

bool Material::hasTexture(TextureSlot slot) const
{
    return textures_[static_cast<std::size_t>(slot)].texture != nullptr;
}

void Material::setTexture(TextureSlot slot, intrusive_ptr<Texture> texture, const Sampler& sampler)
{
    TextureSlotBinding& binding = textures_[static_cast<std::size_t>(slot)];
    // A slot is (map, state): either one changing is one edit, and re-stating what the material already holds
    // is none - the same rule every other setter follows (see revision()).
    if (binding.texture.get() == texture.get() && binding.sampler == sampler) {
        return;
    }
    binding.texture = std::move(texture);
    binding.sampler = sampler;
    ++revision_;
}

void Material::clearTexture(TextureSlot slot)
{
    // The STATE stays with the slot: clearing the map is not a statement about the next map's filtering.
    setTexture(slot, nullptr, textures_[static_cast<std::size_t>(slot)].sampler);
}

std::uint64_t Material::revision() const
{
    return revision_;
}

VN_GRAPHICS_NS_END
