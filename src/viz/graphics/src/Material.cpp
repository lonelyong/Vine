#include <vine/graphics/Material.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(Material, MaterialBase);

MaterialDescription::MaterialDescription(std::uint64_t revision, MaterialParameters values)
    : revision_(revision)
    , values_(std::move(values))
{
}

std::uint64_t MaterialDescription::revision() const noexcept
{
    return revision_;
}

const MaterialParameters& MaterialDescription::values() const noexcept
{
    return values_;
}

MaterialDescriptionPtr MaterialBase::description() const
{
    const std::uint64_t revision = this->revision();
    if (described_ && described_revision_ == revision && description_ != nullptr) {
        return description_;
    }
    // The revision IS the value's identity (see revision()): a description is
    // built once per revision and then shared, so a steady frame compares an
    // integer and a backend row keyed on the value stays put.
    description_        = intrusive_ptr<const MaterialDescription>(new MaterialDescription(revision, resolved()));
    described_revision_ = revision;
    described_          = true;
    return description_;
}

MaterialDescriptionPtr MaterialBase::cachedDescription() const noexcept
{
    if (!described_ || described_revision_ != revision()) {
        return nullptr;
    }
    return description_;
}

Material::Material() = default;

void Material::setBaseColor(const Colorf& color)
{
    // Each setter compares FIRST (see revision()): a host that re-applies the values it already set - the common
    // "recompute my settings every frame" shape - costs nothing, and a consumer keyed on the revision keeps what
    // it has instead of rebuilding an identical row.
    if (parameters_.base_color == color) {
        return;
    }
    parameters_.base_color = color;
    ++revision_;
}

void Material::setEmissive(const Colorf& color)
{
    if (parameters_.emissive == color) {
        return;
    }
    parameters_.emissive = color;
    ++revision_;
}

void Material::setMetallic(float metallic)
{
    if (parameters_.metallic == metallic) {
        return;
    }
    parameters_.metallic = metallic;
    ++revision_;
}

void Material::setRoughness(float roughness)
{
    if (parameters_.roughness == roughness) {
        return;
    }
    parameters_.roughness = roughness;
    ++revision_;
}

void Material::setOcclusion(float occlusion)
{
    if (parameters_.occlusion == occlusion) {
        return;
    }
    parameters_.occlusion = occlusion;
    ++revision_;
}

void Material::setAlphaMode(AlphaMode mode)
{
    if (parameters_.alpha_mode == mode) {
        return;
    }
    parameters_.alpha_mode = mode;
    ++revision_;
}

void Material::setAlphaCutoff(float cutoff)
{
    if (parameters_.alpha_cutoff == cutoff) {
        return;
    }
    parameters_.alpha_cutoff = cutoff;
    ++revision_;
}

void Material::setTexture(TextureSlot slot, intrusive_ptr<Texture> texture, const Sampler& sampler)
{
    TextureSlotBinding& binding = parameters_.textures[static_cast<std::size_t>(slot)];
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
    setTexture(slot, nullptr, parameters_.textures[static_cast<std::size_t>(slot)].sampler);
}

const MaterialParameters& Material::resolved() const noexcept
{
    return parameters_;
}

std::uint64_t Material::revision() const noexcept
{
    return revision_;
}

VN_GRAPHICS_NS_END
