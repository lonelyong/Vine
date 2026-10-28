#pragma once
#include "graphics_global.hpp"

#include <array>
#include <cstdint>

#include <vine/Colorf.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "MaterialBase.hpp"
#include "Sampler.hpp"
#include "Texture.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief A surface's appearance, as an ASSET: one set of values shared by every drawable that names it.
 *
 * @details
 * - The engine's ONE shading model is metallic-roughness with glTF 2.0's vocabulary - base colour, emissive,
 *   metallic, roughness, occlusion - because the import boundary can express an asset in it without an
 *   approximation, and both drawing paths (forward and deferred) read the same bytes (graphics-material.md §2.1).
 * - Phong does not exist here: a Phong source asset is converted by the importer (`ModelScene.cpp`'s
 *   `materialFrom`), so the engine has one set of surface parameters instead of two branches per path.
 * - The material counts its own edits (`revision()`), because every shading value travels through a setter; a
 *   consumer that answers for a material caches on that number instead of reading the values back every frame.
 * - Textures are a five-slot SET (`TextureSlot`): each slot is a map plus its `Sampler`, the slot's identity
 *   decides its binding and colour space, and a material carrying any slot takes the program's `VN_TEXTURED`
 *   variant (five samplers; an absent slot is short-circuited by the block's flags - graphics-material.md §2.5).
 * - It is a MaterialBase, not the only one: `MaterialInstance` overrides some of these values for one drawable
 *   without copying the asset (graphics-material.md 2.2). A host that wants a per-object tint or a swapped normal
 *   map says so with an instance; editing the asset still reaches every instance that does not override that item.
 *
 * @note
 * - Transparency is not a material value: `AlphaMode` says how alpha is USED, and the per-drawable opacity
 *   (`Geometry::setOpacity` / `Node::setOpacity`) is the only magnitude, because one material is shared by every
 *   drawable that uses it (graphics-material.md §2.7).
 * - Emissive is HDR: its colour is linear and unbounded, and its ALPHA is the strength the colour is scaled by
 *   (`Colorf`'s fourth component is a scalar slot here, not an opacity, exactly as the block's `emissive.a`
 *   states).
 */
class VN_GRAPHICS_API Material : public MaterialBase {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a material with the engine's default appearance and no texture. */
    Material();

  public:
    /** @brief Sets the base (albedo) colour.
     *
     * @details
     * - This is the diffuse albedo of a dielectric and the reflection tint of a metal; the split between the two
     *   is `metallic()`.
     * - Setting it does NOT make the drawable translucent on its own: the effective alpha is the mode's business
     *   and the magnitude is the drawable's opacity (see the class note).
     *
     * @param color Linear RGBA; alpha in [0, 1].
     */
    void setBaseColor(const Colorf& color);

    /** @brief Sets the emissive colour and strength.
     *
     * @param color Linear RGB plus the strength in A; the product is what the surface emits on top of shading.
     */
    void setEmissive(const Colorf& color);

    /** @brief Sets the metallic factor.
     *
     * @param metallic Metallic in [0, 1].
     */
    void setMetallic(float metallic);

    /** @brief Sets the perceived roughness.
     *
     * @details
     * - 0 is a mirror and 1 fully diffuse; the value is the one an asset authors (glTF's `roughnessFactor` and
     *   Filament's `roughness` are the same quantity). How it turns into the BRDF's width is the shading model's
     *   business, not this class's.
     *
     * @param roughness Roughness in [0, 1].
     */
    void setRoughness(float roughness);

    /** @brief Sets the ambient-occlusion factor.
     *
     * @param occlusion Occlusion in [0, 1]; it scales the ambient term only, never a direct light.
     */
    void setOcclusion(float occlusion);

    /** @brief Sets how the material's alpha is interpreted.
     *
     * @param mode New alpha mode (the getter is `MaterialBase::alphaMode()`, which answers with the resolved value).
     */
    void setAlphaMode(AlphaMode mode);

    /** @brief Sets the alpha threshold used by AlphaMode::Mask.
     *
     * @param cutoff Cutoff in [0, 1]; fragments with a lower effective alpha are discarded.
     */
    void setAlphaCutoff(float cutoff);

    /** @brief Sets (or replaces) the texture and its read state in one slot.
     *
     * @details
     * - Passing a null @p texture clears the slot, exactly like clearTexture().
     * - The texture itself is not validated here beyond its kind: a cube map in a slot other than BaseColor is
     *   a contract violation the SHADING side answers with the slot's declared-kind fallback (graphics-material.md
     *   C2), because the material does not own a diagnostic channel. The per-slot report is F11.
     *
     * @param slot    The slot to write.
     * @param texture The map, or null to clear the slot.
     * @param sampler How the map is read (defaults when omitted).
     */
    void setTexture(TextureSlot slot, intrusive_ptr<Texture> texture, const Sampler& sampler = {});

    /** @brief Clears one slot (its texture is released; the sampler value is kept).
     *
     * @param slot The slot to clear.
     */
    void clearTexture(TextureSlot slot);

    /** @brief Gets the values every drawable naming this material is shaded with.
     *
     * @return This material's own parameters.
     */
    [[nodiscard]] const MaterialParameters& resolved() const noexcept override;

    /** @brief Gets the revision, which moves whenever a shading value of this material changes.
     *
     * @details
     * - THE MATERIAL MOVES ITS OWN REVISION. The other content types are announced by their WRITER instead
     *   (`Geometry::setRevision()`), because their bytes sit in buffers a caller can refill behind the SDK's
     *   back - a material has no such hole: every one of its shading values travels through a setter, so the
     *   setter can report the change itself and nobody has to remember to.
     * - WHAT MOVES IT: the base colour, emissive, metallic, roughness, occlusion, alpha mode, alpha cutoff and
     *   every texture slot (its map and its sampler) - each only when the value actually DIFFERS (assigning what
     *   a material already holds is not a change, so a host that re-applies its settings every frame costs
     *   nothing). The NAME does not move it: a label is not shading.
     * - WHAT READS IT: a consumer that answers for this material caches on the number instead of reading the
     *   values back every frame (see the backend's ContentStore::updateMaterial).
     *
     * @return Monotonic revision, 0 until the first change.
     */
    [[nodiscard]] std::uint64_t revision() const noexcept override;

  private:
    MaterialParameters parameters_{};
    /// Moves when a shading value changes (see revision()); the name is deliberately not part of it.
    std::uint64_t revision_ = 0;
};

using MaterialPtr = intrusive_ptr<Material>;

VN_GRAPHICS_NS_END
