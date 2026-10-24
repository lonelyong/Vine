#include <vine/graphics/MaterialInstance.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(MaterialInstance, MaterialBase);

MaterialInstance::MaterialInstance(intrusive_ptr<Material> parent)
  : parent_(std::move(parent))
{
}

raw_ptr<Material> MaterialInstance::parent() const
{
    return parent_.get();
}

void MaterialInstance::setParent(intrusive_ptr<Material> parent)
{
    if (parent_.get() == parent.get()) {
        return;
    }
    parent_ = std::move(parent);
    // The values behind the two pointers may differ even when the KIND is the same, so a swap is an edit: the
    // next resolution compares this number, sees the move and recomputes.
    ++own_revision_;
}

void MaterialInstance::setBaseColor(const Colorf& color)
{
    if (hasOverride(MaterialItem::BaseColor) && overrides_.base_color == color) {
        return;
    }
    overrides_.base_color = color;
    markOverridden(MaterialItem::BaseColor);
}

void MaterialInstance::setEmissive(const Colorf& color)
{
    if (hasOverride(MaterialItem::Emissive) && overrides_.emissive == color) {
        return;
    }
    overrides_.emissive = color;
    markOverridden(MaterialItem::Emissive);
}

void MaterialInstance::setMetallic(float metallic)
{
    if (hasOverride(MaterialItem::Metallic) && overrides_.metallic == metallic) {
        return;
    }
    overrides_.metallic = metallic;
    markOverridden(MaterialItem::Metallic);
}

void MaterialInstance::setRoughness(float roughness)
{
    if (hasOverride(MaterialItem::Roughness) && overrides_.roughness == roughness) {
        return;
    }
    overrides_.roughness = roughness;
    markOverridden(MaterialItem::Roughness);
}

void MaterialInstance::setOcclusion(float occlusion)
{
    if (hasOverride(MaterialItem::Occlusion) && overrides_.occlusion == occlusion) {
        return;
    }
    overrides_.occlusion = occlusion;
    markOverridden(MaterialItem::Occlusion);
}

void MaterialInstance::setAlphaMode(AlphaMode mode)
{
    if (hasOverride(MaterialItem::AlphaMode) && overrides_.alpha_mode == mode) {
        return;
    }
    overrides_.alpha_mode = mode;
    markOverridden(MaterialItem::AlphaMode);
}

void MaterialInstance::setAlphaCutoff(float cutoff)
{
    if (hasOverride(MaterialItem::AlphaCutoff) && overrides_.alpha_cutoff == cutoff) {
        return;
    }
    overrides_.alpha_cutoff = cutoff;
    markOverridden(MaterialItem::AlphaCutoff);
}

void MaterialInstance::setTexture(TextureSlot slot, intrusive_ptr<Texture> texture, const Sampler& sampler)
{
    const std::size_t        index = static_cast<std::size_t>(slot);
    TextureSlotBinding&      binding = overrides_.textures[index];
    const MaterialItem       item    = materialItemOf(slot);
    if (hasOverride(item) && binding.texture.get() == texture.get() && binding.sampler == sampler) {
        return;
    }
    // A NULL texture is a statement too: it says "this slot is absent for this instance", which is how an
    // instance removes a map its asset carries (see the header).
    binding.texture = std::move(texture);
    binding.sampler = sampler;
    markOverridden(item);
}

bool MaterialInstance::hasOverride(MaterialItem item) const
{
    return (overrides_mask_ & materialItemBit(item)) != 0U;
}

std::uint32_t MaterialInstance::overrideMask() const
{
    return overrides_mask_;
}

void MaterialInstance::clearOverride(MaterialItem item)
{
    if (!hasOverride(item)) {
        return;
    }
    overrides_mask_ &= ~materialItemBit(item);
    ++own_revision_;
}

const MaterialParameters& MaterialInstance::resolved() const noexcept
{
    // The parent's own revision is the cheap half of the key: an asset counts its edits itself, so "the asset
    // moved" is one number read, not a comparison of values (R1 / P3).
    const std::uint64_t parent_revision = parent_ != nullptr ? parent_->revision() : 0U;
    if (parent_revision != resolved_parent_revision_ || own_revision_ != resolved_own_revision_) {
        resolve();
        resolved_parent_revision_ = parent_revision;
        resolved_own_revision_    = own_revision_;
        // The COMPOSITE revision: the cache's own generation. It moves exactly when a resolution found new
        // values, which is what a consumer keys on (2.11) - two number comparisons per read, no per-frame copy.
        ++resolved_revision_;
    }
    return resolved_;
}

std::uint64_t MaterialInstance::revision() const noexcept
{
    (void)resolved();  // the read is what refreshes the cache and, with it, the number
    return resolved_revision_;
}

void MaterialInstance::resolve() const noexcept
{
    resolved_ = parent_ != nullptr ? parent_->resolved() : MaterialParameters{};
    if (hasOverride(MaterialItem::BaseColor)) {
        resolved_.base_color = overrides_.base_color;
    }
    if (hasOverride(MaterialItem::Emissive)) {
        resolved_.emissive = overrides_.emissive;
    }
    if (hasOverride(MaterialItem::Metallic)) {
        resolved_.metallic = overrides_.metallic;
    }
    if (hasOverride(MaterialItem::Roughness)) {
        resolved_.roughness = overrides_.roughness;
    }
    if (hasOverride(MaterialItem::Occlusion)) {
        resolved_.occlusion = overrides_.occlusion;
    }
    if (hasOverride(MaterialItem::AlphaMode)) {
        resolved_.alpha_mode = overrides_.alpha_mode;
    }
    if (hasOverride(MaterialItem::AlphaCutoff)) {
        resolved_.alpha_cutoff = overrides_.alpha_cutoff;
    }
    // A slot is one item: its map AND its read state come from the same side, so a host that swaps a map cannot
    // end up filtering it with the asset's state.
    for (std::uint32_t index = 0U; index < kTextureSlotCount; ++index) {
        if (hasOverride(materialItemOf(static_cast<TextureSlot>(index)))) {
            resolved_.textures[index] = overrides_.textures[index];
        }
    }
}

void MaterialInstance::markOverridden(MaterialItem item)
{
    overrides_mask_ |= materialItemBit(item);
    ++own_revision_;
}

VN_GRAPHICS_NS_END
