#pragma once
#include "graphics_global.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Canonical vertex attributes the built-in shading consumes.
 *
 * @details
 * - This is the L1 (backend-neutral) half of the shader ABI: a backend maps a role to its own binding spelling — a
 *   vsg attribute-binding name, a D3D semantic, a GL attribute index — while the SHADER declares the attribute at the
 *   location attributeLocation() returns. See .ai/design/graphics-rendering.md §11.
 * - A caller-provided attribute (a forwarded custom channel) is not in this enum: it keeps its SOURCE location as its
 *   shader location (>= 3, never the reserved texcoord slot 8 and never the four instance-matrix columns 12-15), which
 *   is what lets a program address its own attributes without the engine renumbering them.
 * - The instance matrix is ONE role with four consecutive locations: the vertex stage declares it as a `mat4`, and
 *   a backend that declares one attribute per column walks `instanceMatrixLocation(0..3)`.
 */
enum class VertexAttribute
{
    Position,      ///< Object-space position (the only required attribute).
    Normal,        ///< Shading normal (derived when the geometry authors none).
    Color,         ///< Optional per-vertex colour (define-gated on the vsg set).
    TexCoord0,     ///< Optional texture coordinate (define-gated on the vsg set).
    InstanceModel, ///< One instance's local model matrix (four consecutive locations).
};

/** @brief The number of shader locations one instance matrix occupies. */
constexpr std::uint32_t kInstanceMatrixColumns = 4u;

/** @brief The number of scalars in one instance matrix (a column-major mat4). */
constexpr std::uint32_t kInstanceMatrixComponents = kInstanceMatrixColumns * 4u;

/**
 * @brief Shader location of a canonical attribute.
 *
 * @details
 * - The values are the engine's ABI, not a free choice: 8 is the reserved texcoord slot and 12-15 are the instance
 *   matrix, deliberately not the next numbers after the custom-channel range, so a forwarded custom channel (which
 *   reuses its source location) cannot collide with a canonical one.
 * - Changing a value here changes what the built-in shaders must declare, so the two move together — a unit test pins
 *   that agreement.
 *
 * @param attribute Attribute role.
 * @return Shader location the role is declared at.
 */
constexpr std::uint32_t attributeLocation(VertexAttribute attribute) noexcept
{
    switch (attribute)
    {
    case VertexAttribute::Position: return 0u;
    case VertexAttribute::Normal: return 1u;
    case VertexAttribute::Color: return 2u;
    case VertexAttribute::TexCoord0: return 8u;
    case VertexAttribute::InstanceModel: return 12u;
    }
    return 0u;
}

/**
 * @brief Gets the shader location of one instance-matrix column.
 *
 * @details
 * - The four columns are CONSECUTIVE in the ABI, and the vertex stage declares them as one `mat4`; a
 *   backend that declares one attribute per column starts at the first and adds the column offset.
 *
 * @param column Column in [0, kInstanceMatrixColumns).
 * @return Shader location the column is declared at.
 */
constexpr std::uint32_t instanceMatrixLocation(std::uint32_t column) noexcept
{
    return attributeLocation(VertexAttribute::InstanceModel) + column;
}

/**
 * @brief Whether @p location is one of the canonical attributes.
 *
 * @details
 * - The check every consumer needs when it walks a geometry's channels and asks "is this one of the engine's own, or
 *   a forwarded custom one?".
 * - Asking it HERE is what keeps the answer in step with attributeLocation(): a range test (`location <= 2`) or a
 *   list of literals would be a second copy of the ABI, and the reserved texcoord slot is exactly the value a range
 *   test gets wrong.
 *
 * @param location Shader location to classify.
 * @return true when it is a canonical attribute's location.
 */
constexpr bool isCanonicalAttributeLocation(std::uint32_t location) noexcept
{
    if (location >= attributeLocation(VertexAttribute::InstanceModel) &&
        location < attributeLocation(VertexAttribute::InstanceModel) + kInstanceMatrixColumns)
    {
        return true;  // the four instance-matrix columns
    }
    return location == attributeLocation(VertexAttribute::Position) ||
           location == attributeLocation(VertexAttribute::Normal) ||
           location == attributeLocation(VertexAttribute::Color) ||
           location == attributeLocation(VertexAttribute::TexCoord0);
}

/**
 * @brief Per-view data block the built-in shading reads (L1 shape).
 *
 * @details
 * - The engine owns this SHAPE; a backend owns where it lives — a uniform / constant buffer, or (as the vsg forward
 *   path does today) the proj/model subset inside the 128-byte push range.
 * - Every member is a mat4/vec4 and the struct is 16-byte aligned, so std140, D3D cbuffer packing and root constants
 *   all agree (see .ai/design/graphics-rendering.md §11/§12): a `vec3` followed by a `float` is the one packing trap
 *   this rule avoids.
 * - Matrices are column-major, matching the math module's convention; a backend whose shader language needs row-major
 *   translates when it fills the block.
 */
struct alignas(16) VineViewBlock
{
    std::array<float, 16> view{};      ///< World -> view (column-major mat4).
    std::array<float, 16> inv_view{};  ///< View -> world (column-major mat4).
    std::array<float, 16> proj{};      ///< View -> clip (column-major mat4).
    std::array<float, 16> view_proj{}; ///< World -> clip (column-major mat4).
    std::array<float, 4>  cam_pos{};   ///< World-space camera position; w reserved.
    std::array<float, 4>  frame{};     ///< x = time (s), y/z = viewport size, w = flags.
};

/**
 * @brief Per-draw data block the built-in shading reads (L1 shape).
 *
 * @note The user parameter slot is reserved for the per-drawable values (P10: material overrides / user parameters);
 *       until then it packs as zero.
 */
struct alignas(16) VineDrawBlock
{
    std::array<float, 16> model{};  ///< Object -> world (column-major mat4).
    std::array<float, 4>  params{}; ///< User parameter slot (reserved).
};

/**
 * @brief Per-material data block the built-in shading reads (L1 shape).
 *
 * @details
 * - The engine's material ABI: a backend fills this from vn::graphics::Material and binds it at the shader's material
 *   binding.
 * - Field order and std140 offsets are the contract (see the GLSL `VineMaterialBlock` in the SDK's
 *   `shaders/vine_params.glsl`, the ONE declaration of the parameter blocks), so the two sides cannot drift: a member
 *   added here without the GLSL fails the static_asserts below or the structural test, never silently mis-reads.
 *
 * @note
 * - Transparency is NOT part of it: opacity is a per-drawable value (VineDrawBlock::params.x), because one material
 *   is shared by every drawable that uses it.
 * - `diffuse.w` still carries the material's own alpha — the byte is transported for a host program that wants its
 *   own transparency model — but no engine program reads it, and in particular the forward stage does not multiply it
 *   into the fragment alpha (doing so made a shared material translucent for every drawable at once, while the sort
 *   order the engine computes is the opacity's).
 * - Fields the shading never read were removed rather than left declared: an ABI member that promises an effect has
 *   to have one (the removed trio was emissive / alphaMask / alphaMaskCutoff — no SDK accessor could set them and no
 *   stage read them, so a host that believed in them saw nothing).
 */
struct alignas(16) VineMaterialBlock
{
    std::array<float, 4> ambient{};  ///< xyz = colour, w = 1.
    std::array<float, 4> diffuse{};  ///< xyz = colour, w = the material's own alpha (unread by the
                                     ///< engine's programs, see the note above).
    std::array<float, 4> specular{}; ///< xyz = colour, w = strength.
    float                shininess{ 32.0f }; ///< Phong exponent.

    /**
     * @brief Field-wise equality of two blocks.
     *
     * @details
     * - A backend refreshes a material by comparing the block it WOULD send with the one the GPU already has, so this
     *   comparison is a type-level fact and not a backend detail.
     * - Defaulted on purpose: a member added here (PBR's metallic/roughness, say) then enters the comparison by
     *   itself, whereas a hand-written field list in the backend is a second copy of this layout that can silently
     *   fall behind it — and its failure mode is a material edit that never reaches the GPU, i.e. a slider that does
     *   nothing with nothing to see in a log.
     *
     * @param a Left block.
     * @param b Right block.
     * @return true when every member is equal.
     */
    friend constexpr bool operator==(const VineMaterialBlock& a, const VineMaterialBlock& b) noexcept = default;
};

/**
 * @brief Per-pass shadow data block the shading reads (L1 shape).
 *
 * @details
 * - A shadow-casting light is sampled by mapping the shaded fragment into the LIGHT's clip space, so the shading
 *   needs the matrix that takes a VIEW-space position there — `light_vp * inverse(view)` — which is why this block
 *   carries a matrix and not just a bias: the shading already has the view position (a varying in the forward stage,
 *   a G-buffer attachment in the deferred one) and nothing else about the light camera.
 * - It is a PASS-level block, not a per-view one: the pipeline that builds a shadow pass owns the light camera that
 *   produced its map, and a program that samples no shadow declares no such binding at all (the deferred lighting
 *   program has a shadowed variant that declares it, and the forward program declares it unconditionally and switches
 *   at run time - see BuiltinShaders::shadowBindings).
 * - `params.x` is 1 while a map is bound (a shader must be able to take the unshadowed path with the same text) and 0
 *   otherwise; `y` is the depth bias applied at the comparison; `z` is the shadow strength (0 disables the darkening
 *   without unbinding anything); `w` names which of the block's directional lights the map belongs to, so the map
 *   scales ONE light's term and never every light's (a light that casts none would otherwise lose its contribution
 *   wherever that one light is blocked — see the shadow term).
 * - THE MATRIX IS IN THE SDK'S CLIP CONVENTION, THE MAP IS NOT: `view_to_light` maps a view-space position to the
 *   SDK's OWN clip space — x right, y up, z in [-1, 1] with -1 at the light's near plane and +1 at its far one (see
 *   Camera; directionalShadowMatrix builds it as `projection * view` in that convention and
 *   states it on the map through RenderTarget::setProducerViewProjection) — so the matrix means the same thing on
 *   every backend.
 * - The MAP it is compared against is a GPU image the backend rasterised, and its two axes are the backend's
 *   business: the vsg backend renders reverse-Z (near = 1, far = 0) into a top-down image (v = 0 is world up, the
 *   same fact that makes its G-buffer upright).
 * - A shader therefore converts x/y by `xy * vec2(0.5, -0.5) + 0.5` and z by `1 - (z * 0.5 + 0.5)` before comparing —
 *   the z line IS the [-1, 1] -> [1, 0] remap, so a producer that stated a [0, 1] matrix here would sample the wrong
 *   end of the map — and getting either sign wrong is invisible: the picture loses its sun, or samples a mirrored
 *   texel. Both were measured, one at a time, by vsg_backend_selftest's deferred shadow phase.
 */
struct alignas(16) VineShadowBlock
{
    std::array<float, 16> view_to_light{}; ///< View space -> the SDK's light clip, column-major (see the conventions
                                           ///< above before sampling a map with it).
    std::array<float, 4>  params{};        ///< x = enabled, y = bias, z = strength, w = the light the map belongs to.
};

// The structs ARE the shader ABI: a member added here without updating the GLSL
// (the parameter blocks are declared once, in shaders/vine_params.glsl) must fail the build, not silently mis-read at
// run time.
static_assert(sizeof(VineViewBlock) == 288u, "VineViewBlock must be 4 mat4 + 2 vec4");
static_assert(alignof(VineViewBlock) == 16u, "VineViewBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(VineViewBlock, cam_pos) == 256u, "VineViewBlock std140 offset");
static_assert(offsetof(VineViewBlock, frame) == 272u, "VineViewBlock std140 offset");
static_assert(sizeof(VineDrawBlock) == 80u, "VineDrawBlock must be 1 mat4 + 1 vec4");
static_assert(alignof(VineDrawBlock) == 16u, "VineDrawBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(VineDrawBlock, params) == 64u, "VineDrawBlock std140 offset");
static_assert(sizeof(VineMaterialBlock) == 64u, "VineMaterialBlock must be 3 vec4 + 1 float (std140 pads it to 64)");
static_assert(alignof(VineMaterialBlock) == 16u, "VineMaterialBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(VineMaterialBlock, shininess) == 48u, "VineMaterialBlock std140 offset");
static_assert(sizeof(VineShadowBlock) == 80u, "VineShadowBlock must be 1 mat4 + 1 vec4");
static_assert(alignof(VineShadowBlock) == 16u, "VineShadowBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(VineShadowBlock, params) == 64u, "VineShadowBlock std140 offset");

VN_GRAPHICS_NS_END
