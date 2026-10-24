#pragma once
#include "graphics_global.hpp"

#include <array>
#include <cstdint>

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/RefCounted.hpp>
#include <vine/Colorf.hpp>
#include <vine/String.hpp>

#include "Sampler.hpp"
#include "Texture.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief How a material's alpha is interpreted where the surface is shaded.
 *
 * @details
 * - The MODE is a material fact while the MAGNITUDE is a per-drawable one (`VineDrawBlock::params.x`); the
 *   effective alpha is the product of the two times the base-colour texture's own alpha (graphics-material.md §2.7), so
 *   the sort and the shading read the same inputs instead of two different notions of "translucent".
 * - `Opaque` keeps the drawable in the opaque queue and ignores alpha; `Mask` cuts out fragments below
 *   `Material::alphaCutoff()` but is still opaque to the sort (a cut-out leaf must occlude itself); `Blend` is
 *   the only mode the transparent queue accepts, and it is drawn by the forward overlay - the deferred G-buffer
 *   records opaque and masked content only.
 *
 * @note
 * - The block already carries the mode (`VineMaterialBlock::flags`, bits 8-9); the engine's own programs start
 *   reading it with the alpha-mode work (graphics-material.md F7), and until then every material behaves as
 *   `Opaque`, which is the default.
 */
enum class AlphaMode
{
    Opaque, ///< Alpha is ignored: drawn in the opaque queue.
    Mask,   ///< Alpha below alphaCutoff() is discarded; still opaque to the sort.
    Blend,  ///< Alpha is blended: the drawable belongs in the transparent queue (forward overlay).
};

/**
 * @brief The five semantic texture slots a material carries.
 *
 * @details
 * - The slot's IDENTITY decides its binding and its colour space, not the host: base colour and emissive are
 *   sampled as colour (sRGB formats, hardware-decoded), metallic-roughness / normal / occlusion as data (linear
 *   formats). A texture that does not fit its slot is reported, never silently interpreted as another colour
 *   space (graphics-material.md §2.9).
 * - `metallic_roughness` is glTF's packing: G = roughness, B = metallic; `occlusion` is read from R.
 * - Only `BaseColor` may be a cube map (the sampler kind follows the geometry's texcoord width, which is a fact
 *   about the geometry); the other four are 2D only. The UV set is always 0 - a second one would be a geometry
 *   format change, not a material one.
 */
enum class TextureSlot
{
    BaseColor,        ///< Albedo / reflectance tint (RGB; alpha drives AlphaMode).
    MetallicRoughness, ///< G = roughness, B = metallic (linear data).
    Normal,           ///< Tangent-space normal (linear data).
    Occlusion,        ///< Ambient occlusion in R (linear data).
    Emissive,         ///< Emission colour (sRGB colour).
};

/** @brief The number of texture slots a material carries (see TextureSlot). */
constexpr std::uint32_t kTextureSlotCount = 5U;

/** @brief One texture slot: the texture (when the slot exists) and how it is READ. */
struct VN_GRAPHICS_API TextureSlotBinding
{
    intrusive_ptr<Texture> texture;  ///< The map, or null when the slot is absent.
    Sampler                sampler{}; ///< Filter / wrap / mip state (a value: equal state = one sampler).
};

/**
 * @brief A surface's appearance: metallic-roughness PBR factors and an optional base-colour texture.
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
 *   decides its binding and colour space, and a material carrying any slot takes the program's `VINE_TEXTURED`
 *   variant (five samplers, missing slots bound to their type's constant fallback - graphics-material.md §2.5).
 *
 * @note
 * - Transparency is not a material value: `AlphaMode` says how alpha is USED, and the per-drawable opacity
 *   (`Geometry::setOpacity` / `Node::setOpacity`) is the only magnitude, because one material is shared by every
 *   drawable that uses it (graphics-material.md §2.7).
 * - Emissive is HDR: its colour is linear and unbounded, and its ALPHA is the strength the colour is scaled by
 *   (`Colorf`'s fourth component is a scalar slot here, not an opacity, exactly as the block's `emissive.a`
 *   states).
 */
class VN_GRAPHICS_API Material : public Object, public RefCounted<Material> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a material with the engine's default appearance and no texture. */
    Material();

  public:
    /** @brief Gets the material name.
     *
     * @return The name (empty when never set).
     */
    String name() const;

    /** @brief Sets the material name.
     *
     * @details
     * - A name is a label, not shading: it never moves `revision()`.
     *
     * @param name New material name.
     */
    void setName(const String& name);

    /** @brief Gets the base (albedo) colour.
     *
     * @return Linear RGBA; alpha is the material's own alpha (see AlphaMode).
     */
    Colorf baseColor() const;

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

    /** @brief Gets the emissive colour and strength.
     *
     * @return Linear RGB plus the strength in A (0 = no emission).
     */
    Colorf emissive() const;

    /** @brief Sets the emissive colour and strength.
     *
     * @param color Linear RGB plus the strength in A; the product is what the surface emits on top of shading.
     */
    void setEmissive(const Colorf& color);

    /** @brief Gets the metallic factor.
     *
     * @return Metallic in [0, 1]: 0 is a dielectric, 1 a metal.
     */
    float metallic() const;

    /** @brief Sets the metallic factor.
     *
     * @param metallic Metallic in [0, 1].
     */
    void setMetallic(float metallic);

    /** @brief Gets the perceived roughness.
     *
     * @return Roughness in [0, 1]: 0 is a mirror, 1 fully diffuse.
     */
    float roughness() const;

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

    /** @brief Gets the ambient-occlusion factor.
     *
     * @return Occlusion in [0, 1] (1 = not occluded).
     */
    float occlusion() const;

    /** @brief Sets the ambient-occlusion factor.
     *
     * @param occlusion Occlusion in [0, 1]; it scales the ambient term only, never a direct light.
     */
    void setOcclusion(float occlusion);

    /** @brief Gets how the material's alpha is interpreted.
     *
     * @return The alpha mode (Opaque by default).
     */
    AlphaMode alphaMode() const;

    /** @brief Sets how the material's alpha is interpreted.
     *
     * @param mode New alpha mode.
     */
    void setAlphaMode(AlphaMode mode);

    /** @brief Gets the alpha threshold used by AlphaMode::Mask.
     *
     * @return The cutoff in [0, 1].
     */
    float alphaCutoff() const;

    /** @brief Sets the alpha threshold used by AlphaMode::Mask.
     *
     * @param cutoff Cutoff in [0, 1]; fragments with a lower effective alpha are discarded.
     */
    void setAlphaCutoff(float cutoff);

    /** @brief Gets the texture one slot samples.
     *
     * @param slot The slot to read.
     * @return The texture, or null when that slot is absent.
     */
    raw_ptr<Texture> texture(TextureSlot slot) const;

    /** @brief Gets how one slot is read (filter / wrap / mip state).
     *
     * @param slot The slot to read.
     * @return The sampler of that slot (the defaults when it is absent).
     */
    const Sampler& sampler(TextureSlot slot) const;

    /** @brief Whether one slot has a texture.
     *
     * @param slot The slot to test.
     * @return true when the slot exists.
     */
    bool hasTexture(TextureSlot slot) const;

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
    std::uint64_t revision() const;

  private:
    String                 name_;
    Colorf                 base_color_{ 0.8f, 0.8f, 0.8f, 1.0f };
    /// Linear RGB plus the strength in A (0 = no emission).
    Colorf                 emissive_{ 0.0f, 0.0f, 0.0f, 1.0f };
    float                  metallic_ = 0.0f;
    /// Perceptual roughness; the same default the geometry-side PbrMaterial states.
    float                  roughness_ = 0.5f;
    float                  occlusion_ = 1.0f;
    AlphaMode              alpha_mode_ = AlphaMode::Opaque;
    float                  alpha_cutoff_ = 0.5f;
    std::array<TextureSlotBinding, kTextureSlotCount> textures_{};
    /// Moves when a shading value changes (see revision()); the name is deliberately not part of it.
    std::uint64_t revision_ = 0;
};

using MaterialPtr = intrusive_ptr<Material>;

VN_GRAPHICS_NS_END
