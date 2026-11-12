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

#include "ContentId.hpp"
#include "Sampler.hpp"
#include "Texture.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief How a material's alpha is interpreted where the surface is shaded.
 *
 * @details
 * - The MODE is a material fact while the MAGNITUDE is a per-drawable one (`VineDrawBlock::params.x`); the
 *   effective alpha is the product of the two times the base-colour texture's own alpha (graphics-material.md §2.7),
 *   so the sort and the shading read the same inputs instead of two different notions of "translucent".
 * - `Opaque` keeps the drawable in the opaque queue and ignores alpha; `Mask` cuts out fragments below
 *   `MaterialParameters::alpha_cutoff` but is still opaque to the sort (a cut-out leaf must occlude itself);
 *   `Blend` is the only mode the transparent queue accepts, and it is drawn by the forward overlay - the
 *   deferred G-buffer records opaque and masked content only.
 */
enum class AlphaMode
{
    Opaque, // Alpha is ignored: drawn in the opaque queue.
    Mask,   // Alpha below the cutoff is discarded; the surviving fragment is opaque to the sort.
    Blend,  // Alpha is blended: the drawable belongs in the transparent queue (forward overlay).
};

/**
 * @brief Returns whether a draw with alpha @p mode and drawable @p opacity belongs in the transparent queue.
 *
 * @details
 * - ONE DEFINITION OF A TWO-INPUT RULE (graphics-material.md §2.7): `Blend` is transparent whatever the opacity;
 *   `Mask` is opaque to the sort even at opacity < 1 (its alpha cuts fragments out instead of fading the surface);
 *   everything else is transparent exactly when the effective opacity is below 1.
 * - Both readers call it: the command's own `isTransparent()` (its live fallback) and `MaterialDescription::isTransparent()`
 *   (the value the frame crosses with), so the sort order and the recorded blend enable cannot disagree.
 *
 * @param mode    The material's resolved alpha mode.
 * @param opacity The drawable's effective opacity, in [0, 1].
 * @return true when the draw belongs in the transparent queue.
 */
[[nodiscard]] VN_GRAPHICS_API bool alphaModeSortsTransparent(AlphaMode mode, float opacity) noexcept;

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
    BaseColor,         // Albedo / reflectance tint (RGB; alpha drives AlphaMode).
    MetallicRoughness, // G = roughness, B = metallic (linear data).
    Normal,            // Tangent-space normal (linear data).
    Occlusion,         // Ambient occlusion in R (linear data).
    Emissive,          // Emission colour (sRGB colour).
};

/** @brief The number of texture slots a material carries (see TextureSlot). */
constexpr std::uint32_t kTextureSlotCount = 5U;

/** @brief One texture slot: the texture (when the slot exists) and how it is READ. */
struct VN_GRAPHICS_API TextureSlotBinding {
    intrusive_ptr<Texture> texture;   // The map, or null when the slot is absent.
    Sampler                sampler{}; // Filter / wrap / mip state (a value: equal state = one sampler).

    /** @brief Compares two bindings (the identity is the map and the read state). */
    [[nodiscard]] friend bool operator==(const TextureSlotBinding&, const TextureSlotBinding&) noexcept = default;
};

/**
 * @brief The engine's ONE set of surface values: what a material (or a resolved instance) IS.
 *
 * @details
 * - One definition serves three consumers: `Material` holds one, `MaterialInstance` resolves into one, and the
 *   backend's packer reads one (`buildMaterialFacts`). A second spelling of "the shading values" is how an
 *   instance's override and the asset it inherits from would come to disagree about what a field means.
 * - The defaults ARE the engine's default material, and they are the same values `VineMaterialBlock` defaults
 *   to: a material nobody configured shades exactly like a drawable without one.
 * - The values are stored as authored. Nothing here clamps or sanitises: a material is host data, and the
 *   shader clamps what it draws (graphics-material.md N2).
 *
 * @note
 * - Transparency is not a value here: `alpha_mode` says how alpha is USED, the per-drawable opacity is the only
 *   magnitude, and `base_color.a` is the material's own alpha that the mode decides what to do with (§2.7).
 * - Emission is HDR: `emissive`'s colour is linear and unbounded, and its ALPHA is the strength the colour is
 *   scaled by (a scalar slot, not an opacity - exactly as the block's `emissive.a` states).
 */
struct VN_GRAPHICS_API MaterialParameters {
    Colorf                                            base_color{ 0.8f, 0.8f, 0.8f, 1.0f }; // Linear RGB + the material's own alpha.
    Colorf                                            emissive{ 0.0f, 0.0f, 0.0f, 1.0f };   // Linear RGB + the emission strength in A.
    float                                             metallic{ 0.0f };                     // 0 = dielectric, 1 = metal.
    float                                             roughness{ 0.5f };                    // Perceptual roughness, 0 = mirror, 1 = diffuse.
    float                                             occlusion{ 1.0f };                    // Ambient-occlusion factor (scales ambient only).
    AlphaMode                                         alpha_mode{ AlphaMode::Opaque };      // How alpha is used where the surface is shaded.
    float                                             alpha_cutoff{ 0.5f };                 // Mask threshold (ignored by the other modes).
    std::array<TextureSlotBinding, kTextureSlotCount> textures{};                           // One entry per TextureSlot, in that order.
};

/**
 * @brief A material's values AT ONE REVISION, as an immutable value.
 *
 * @details
 * - THE SAME SHAPE AS `GeometryDescription`, for the same reason: what a frame draws must not be read from a live object
 *   on the frame's own thread. A command carries this value, and the backend describes its tables from it - so a material
 *   edit is a NEW VALUE that reaches the render side the way every other content fact does: at a publication boundary
 *   (one frame, or one edit batch - see `Scene::beginEdit`), never by a live re-read in the middle of one.
 * - ONE PER REVISION: `MaterialBase::description()` returns the cached instance for the material's current revision, so a
 *   frame with no material edit shares one object and the backend's rows stay put.
 * - NOT thread-safe to mutate, and it never is: every field is set by the constructor and read-only afterwards.
 *
 * @note
 * - `values` is the RESOLVED set (an instance answers with its parent's values overlaid), so an asset and an instance
 *   describe themselves the same way and nothing downstream can tell them apart.
 */
class VN_GRAPHICS_API MaterialDescription final : public RefCounted<MaterialDescription> {
  public:
    /**
     * @brief Constructs a description.
     *
     * @param revision The material revision these values were read at.
     * @param values   The resolved values.
     */
    MaterialDescription(std::uint64_t revision, MaterialParameters values);

    /** @brief Gets the revision these values were read at (see MaterialBase::revision). */
    [[nodiscard]] std::uint64_t revision() const noexcept;

    /** @brief Gets the resolved values. */
    [[nodiscard]] const MaterialParameters& values() const noexcept;

    /**
     * @brief Returns whether these values belong in the transparent queue at @p opacity.
     *
     * @details
     * - The value-side spelling of the sort rule (see `alphaModeSortsTransparent`): this is what a recorder asks
     *   instead of reading the live material's alpha mode, so the blend enable and the sort order agree even when the
     *   material is edited while a frame is in flight.
     *
     * @param opacity The drawable's effective opacity, in [0, 1].
     * @return true when the draw belongs in the transparent queue.
     */
    [[nodiscard]] bool isTransparent(float opacity) const noexcept;

  private:
    std::uint64_t      revision_;
    MaterialParameters values_;
};

using MaterialDescriptionPtr = intrusive_ptr<const MaterialDescription>;

/**
 * @brief One item of `MaterialParameters`, as a bit an instance can override.
 *
 * @details
 * - An instance overrides ITEMS, not the whole material: `true` for one bit means "this instance states this
 *   value, the parent's does not apply" (graphics-material.md 2.3). The bits are therefore that class's
 *   vocabulary, and each texture slot is its own item because a host may replace a normal map and keep the base
 *   colour.
 * - The values are STABLE BITS, not ordinals: `overrideMask()` reports them, a host stores them, and a bit that
 *   moved would silently change what an override means.
 */
enum class MaterialItem : std::uint32_t
{
    BaseColor                = 1U << 0,  // `base_color`.
    Emissive                 = 1U << 1,  // `emissive`.
    Metallic                 = 1U << 2,  // `metallic`.
    Roughness                = 1U << 3,  // `roughness`.
    Occlusion                = 1U << 4,  // `occlusion`.
    AlphaMode                = 1U << 5,  // `alpha_mode`.
    AlphaCutoff              = 1U << 6,  // `alpha_cutoff`.
    BaseColorTexture         = 1U << 7,  // `textures[TextureSlot::BaseColor]`.
    MetallicRoughnessTexture = 1U << 8,  // `textures[TextureSlot::MetallicRoughness]`.
    NormalTexture            = 1U << 9,  // `textures[TextureSlot::Normal]`.
    OcclusionTexture         = 1U << 10, // `textures[TextureSlot::Occlusion]`.
    EmissiveTexture          = 1U << 11, // `textures[TextureSlot::Emissive]`.
};

/** @brief Gets the bit of one item (the value `overrideMask()` reports). */
[[nodiscard]] constexpr std::uint32_t materialItemBit(MaterialItem item) noexcept
{
    return static_cast<std::uint32_t>(item);
}

/**
 * @brief Gets the override item of one texture slot.
 *
 * @param slot The slot to name.
 * @return The bit that overrides that slot.
 */
[[nodiscard]] constexpr MaterialItem materialItemOf(TextureSlot slot) noexcept
{
    switch (slot) {
    case TextureSlot::BaseColor: return MaterialItem::BaseColorTexture;
    case TextureSlot::MetallicRoughness: return MaterialItem::MetallicRoughnessTexture;
    case TextureSlot::Normal: return MaterialItem::NormalTexture;
    case TextureSlot::Occlusion: return MaterialItem::OcclusionTexture;
    case TextureSlot::Emissive: return MaterialItem::EmissiveTexture;
    }
    return MaterialItem::BaseColorTexture;
}

/**
 * @brief What a draw can be shaded with: an asset (`Material`) or one of its instances (`MaterialInstance`).
 *
 * @details
 * - ONE TYPE FOR "what shades this draw" (graphics-material.md 2.2): `RenderCommand` / `Geometry` / `Primitive`
 *   and `StateNode` hold a `MaterialBasePtr`, so a host never has to ask whether the thing it assigned can be
 *   edited without re-shading somebody else.
 * - The values are the RESOLVED ones: an asset answers with what it stores, an instance with its parent's values
 *   overlaid by what it overrides. Everything downstream - the block packer, the variant decision, the
 *   transparency sort - reads `resolved()` and so cannot tell the two apart, which is the point.
 * - `revision()` is the identity of those values for a cache: an asset counts its own edits, an instance counts
 *   every resolution in which its parent's values changed too (2.11 / R1).
 */
class VN_GRAPHICS_API MaterialBase : public Object, public RefCounted<MaterialBase> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a material: unnamed, opaque, with an identity of its own. */
    MaterialBase();
    ~MaterialBase();

  public:
    /** @brief Gets this material's identity.
     *
     * @details
     * - ALLOCATED AT CONSTRUCTION, released with the object, so a table that outlived the material reads "gone"
     *   instead of resolving to whatever is built at the same address next (see ContentId.hpp).
     * - THE NAME IS NOT THE IDENTITY (see name()): two materials may share a name, and a rename keeps the identity.
     *
     * @return The material id; never null for a constructed material.
     */
    [[nodiscard]] MaterialId id() const noexcept;

    /**
     * @brief Gets the name this material (or instance) was given.
     *
     * @return The name, empty when never set.
     */
    String name() const;

    /**
     * @brief Sets a name for this material (or instance).
     *
     * @details
     * - A name is a label, not shading: it never moves `revision()`.
     *
     * @param name New name.
     */
    void setName(const String& name);

    /**
     * @brief Gets the values this drawable is shaded with (an asset's own, or an instance's resolved values).
     *
     * @return The resolved parameters; the reference stays valid until the material changes.
     */
    [[nodiscard]] virtual const MaterialParameters& resolved() const noexcept = 0;

    /**
     * @brief Gets the revision of the values `resolved()` returns.
     *
     * @details
     * - A consumer caches on this number instead of comparing values: it is what tells the backend "these bytes
     *   are stale" (see `ContentStore::updateMaterial`).
     * - An instance's number moves when EITHER its own overrides or its parent's values changed, which is what
     *   makes "edit the asset, every instance follows" one write instead of a walk over the instances (R1).
     *
     * @return A monotonic number, 0 until the values first changed.
     */
    [[nodiscard]] virtual std::uint64_t revision() const noexcept = 0;

    /**
     * @brief Gets these values as an immutable value, building it when the revision moved.
     *
     * @details
     * - THE CONVERSION, the same shape as `Geometry::description`: call it where the values may still be read - the
     *   publish that copies a scene's rows, a row fact built for a delta. It caches, so the second call in a revision is
     *   an integer compare.
     * - What the RENDER side reads is the value that was handed to it (`RenderCommand::materialDescription`), never this
     *   call: a material edit must reach a frame at a publication boundary, not in the middle of one (design
     *   graphics-threading.md C18).
     *
     * @return The cached description for the current revision.
     */
    [[nodiscard]] MaterialDescriptionPtr description() const;

    /**
     * @brief Gets the cached description WITHOUT building one - null when the revision moved since it was built.
     *
     * @details
     * - What a frame-side reader uses: it answers "the values this material was published with" without ever writing the
     *   cache from a thread that does not own the content. A null answer means the material moved on since it was
     *   published, and the picture keeps the values it was given (one publication of lag - the rule every other content
     *   fact follows).
     *
     * @return The value built for the current revision, or null.
     */
    [[nodiscard]] MaterialDescriptionPtr cachedDescription() const noexcept;

    /**
     * @brief Gets the resolved alpha mode, which is what the transparency sort asks for.
     *
     * @details
     * - One spelling for both kinds: `RenderCommand::isTransparent()` must classify an instance exactly like the
     *   asset it inherits from, and reading it through the base is what keeps that from being a branch per kind.
     *
     * @return The resolved mode (Opaque when nothing overrode it).
     */
    [[nodiscard]] AlphaMode alphaMode() const noexcept
    {
        return resolved().alpha_mode;
    }

    /**
     * @brief Gets the resolved base (albedo) colour.
     *
     * @return Linear RGBA; alpha is the material's own alpha (see AlphaMode).
     */
    [[nodiscard]] Colorf baseColor() const noexcept;

    /**
     * @brief Gets the resolved emissive colour and strength.
     *
     * @return Linear RGB plus the strength in A (0 = no emission).
     */
    [[nodiscard]] Colorf emissive() const noexcept;

    /**
     * @brief Gets the resolved metallic factor.
     *
     * @return Metallic in [0, 1]: 0 is a dielectric, 1 a metal.
     */
    [[nodiscard]] float metallic() const noexcept;

    /**
     * @brief Gets the resolved perceptual roughness.
     *
     * @return Roughness in [0, 1]: 0 is a mirror, 1 fully diffuse.
     */
    [[nodiscard]] float roughness() const noexcept;

    /**
     * @brief Gets the resolved ambient-occlusion factor.
     *
     * @return Occlusion in [0, 1] (1 = not occluded).
     */
    [[nodiscard]] float occlusion() const noexcept;

    /**
     * @brief Gets the resolved Mask threshold.
     *
     * @return The cutoff in [0, 1].
     */
    [[nodiscard]] float alphaCutoff() const noexcept;

    /**
     * @brief Gets the texture one slot samples, after resolution.
     *
     * @param slot The slot to read.
     * @return The texture, or null when that slot is absent.
     */
    [[nodiscard]] raw_ptr<Texture> texture(TextureSlot slot) const noexcept;

    /**
     * @brief Gets how one slot is read, after resolution.
     *
     * @param slot The slot to read.
     * @return The sampler of that slot (the defaults when it is absent).
     */
    [[nodiscard]] const Sampler& sampler(TextureSlot slot) const noexcept;

    /**
     * @brief Whether one slot has a texture, after resolution.
     *
     * @param slot The slot to test.
     * @return true when the slot exists.
     */
    [[nodiscard]] bool hasTexture(TextureSlot slot) const noexcept;

  private:
    MaterialId id_;
    String     name_;

    /// The one value per revision (see description()); written on the content side only.
    mutable MaterialDescriptionPtr description_;
    mutable std::uint64_t          described_revision_{ 0 };
    mutable bool                   described_{ false };
};

using MaterialBasePtr = intrusive_ptr<MaterialBase>;

VN_GRAPHICS_NS_END
