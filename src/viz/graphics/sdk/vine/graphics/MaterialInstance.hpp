#pragma once
#include "graphics_global.hpp"

#include <cstdint>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Material.hpp"
#include "MaterialBase.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief One drawable's DELTA over an asset: a reference to a `Material` plus the items it states itself.
 *
 * @details
 * - The two-level model (graphics-material.md 2.2): a material is an asset - one definition, shared, where an
 *   edit reaches everything that does not override that item; an instance is a sparse overlay on exactly that
 *   asset, for the per-object cases (a highlight, a hover tint, a swapped normal map).
 * - WHY A SECOND TYPE instead of "a material with an optional parent": the question "does editing this change
 *   anything else?" is answered by the TYPE here, and an instance can answer "which items do I state?" -
 *   `overrideMask()`. With one class both answers become run-time data a host has to reason about.
 * - AN INSTANCE IS ALSO A DRAWABLE'S SHADING (it is a `MaterialBase`), so `RenderCommand` / `Geometry` /
 *   `Primitive` / `StateNode` hold a `MaterialBasePtr` and nothing downstream branches on which kind it got.
 *
 * @note
 * - The parent is a `Material`, never another instance (C1): one hop, so resolution is one copy and "what does
 *   this inherit from" has one answer.
 * - Resolution is CACHED: `resolved()` recomputes only when the parent's revision or this instance's own
 *   revision moved (R1), so reading a resolved instance every frame costs two number comparisons.
 * - Transparency follows the resolved value: an instance that overrides the alpha mode to `Blend` is sorted as
 *   blended even when its asset is opaque (`MaterialBase::alphaMode()`).
 */
class VN_GRAPHICS_API MaterialInstance : public MaterialBase
{
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs an instance over @p parent (the engine's default material when it is null).
     *
     * @param parent The asset this instance starts by inheriting everything from.
     */
    explicit MaterialInstance(intrusive_ptr<Material> parent = nullptr);

  public:
    /**
     * @brief Gets the asset this instance inherits from.
     *
     * @return The parent, or null (the instance then starts from the engine's default material).
     */
    raw_ptr<Material> parent() const;

    /**
     * @brief Points this instance at another asset.
     *
     * @details
     * - Every item this instance does not override follows the new parent from the next resolution.
     * - The swap moves the revision even when the pointers are the same kind: the values behind them may differ.
     *
     * @param parent The new asset, or nullptr for the engine's default material.
     */
    void setParent(intrusive_ptr<Material> parent);

    /**
     * @brief States a base colour for this instance (the parent's stops applying to it).
     *
     * @param color Linear RGBA; alpha is the material's own alpha (see AlphaMode).
     */
    void setBaseColor(const Colorf& color);

    /**
     * @brief States an emissive colour and strength for this instance.
     *
     * @param color Linear RGB plus the strength in A.
     */
    void setEmissive(const Colorf& color);

    /**
     * @brief States a metallic factor for this instance.
     *
     * @param metallic Metallic in [0, 1].
     */
    void setMetallic(float metallic);

    /**
     * @brief States a roughness factor for this instance.
     *
     * @param roughness Perceptual roughness in [0, 1].
     */
    void setRoughness(float roughness);

    /**
     * @brief States an occlusion factor for this instance.
     *
     * @param occlusion Occlusion in [0, 1].
     */
    void setOcclusion(float occlusion);

    /**
     * @brief States an alpha mode for this instance.
     *
     * @param mode New alpha mode (the getter is `MaterialBase::alphaMode()`).
     */
    void setAlphaMode(AlphaMode mode);

    /**
     * @brief States an alpha cutoff for this instance.
     *
     * @param cutoff Cutoff in [0, 1].
     */
    void setAlphaCutoff(float cutoff);

    /**
     * @brief Replaces one texture slot for this instance.
     *
     * @details
     * - A null @p texture still STATES the slot - as absent - which is how an instance removes a map its asset
     *   carries. To go back to the asset's map, clear the override instead (clearOverride()).
     *
     * @param slot    The slot to write.
     * @param texture The map, or null to state that the slot is absent for this instance.
     * @param sampler How the map is read (defaults when omitted).
     */
    void setTexture(TextureSlot slot, intrusive_ptr<Texture> texture, const Sampler& sampler = {});

    /**
     * @brief Gets whether this instance states one item itself.
     *
     * @param item The item to test.
     * @return true when the item is overridden (the parent's value does not apply).
     */
    bool hasOverride(MaterialItem item) const;

    /**
     * @brief Gets every item this instance states, as the bits of `MaterialItem`.
     *
     * @return The override mask (0 when it inherits everything).
     */
    std::uint32_t overrideMask() const;

    /**
     * @brief Stops overriding one item: the parent's value applies again from the next resolution.
     *
     * @details
     * - Clearing an item that was not overridden is not a change and does not move the revision.
     * - Clearing a texture item restores the asset's whole slot: its map AND its sampler, because a slot is one
     *   item (see `TextureSlotBinding`).
     *
     * @param item The item to stop stating.
     */
    void clearOverride(MaterialItem item);

    /**
     * @brief Gets this instance's values: the parent's, with every overridden item replaced.
     *
     * @details
     * - Recomputed only when the parent's revision or this instance's own revision moved (R1); the returned
     *   reference is valid until the next such change.
     *
     * @return The resolved parameters.
     */
    [[nodiscard]] const MaterialParameters& resolved() const noexcept override;

    /**
     * @brief Gets the revision of the resolved values.
     *
     * @details
     * - A COMPOSITE number (2.11): every resolution in which the parent's values or this instance's own changed
     *   moves it, so a consumer keyed on it rewrites the instance's block when either side moved - an asset edit
     *   reaching every instance is exactly this number changing, with no walk over the instances.
     *
     * @return A per-instance monotonic number, 0 until the first resolution.
     */
    [[nodiscard]] std::uint64_t revision() const noexcept override;

  private:
    /** @brief Recomputes the cache from the parent plus this instance's overrides. */
    void resolve() const noexcept;

    /** @brief Marks one item as overridden and moves the revision. */
    void markOverridden(MaterialItem item);

  private:
    intrusive_ptr<Material> parent_;
    /// Only the items whose bit is set in `overrides_mask_` are meaningful (a sparse overlay, see the class note).
    MaterialParameters overrides_{};
    std::uint32_t      overrides_mask_ = 0;
    /// Moves on every edit of this instance's own overrides or of the parent asset it points at.
    std::uint64_t own_revision_ = 0;

    /// The resolution cache and the two keys it was computed with (R1).
    mutable MaterialParameters resolved_{};
    mutable std::uint64_t      resolved_parent_revision_ = 0;
    mutable std::uint64_t      resolved_own_revision_    = 0;
    mutable std::uint64_t      resolved_revision_        = 0;
};

using MaterialInstancePtr = intrusive_ptr<MaterialInstance>;

VN_GRAPHICS_NS_END
