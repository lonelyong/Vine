#pragma once
#include "graphics_global.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

V_GRAPHICS_NS_BEGIN

/**
 * @brief Canonical vertex attributes the built-in shading consumes.
 *
 * This is the L1 (backend-neutral) half of the shader ABI: a backend maps a role
 * to its own binding spelling — a vsg attribute-binding name, a D3D semantic, a
 * GL attribute index — while the SHADER declares the attribute at the location
 * attributeLocation() returns. See .ai/design/graphics-shader.md §11.
 *
 * A caller-provided attribute (a forwarded custom channel) is not in this enum:
 * it keeps its SOURCE location as its shader location (>= 3, never the reserved
 * texcoord slot), which is what lets a program address its own attributes
 * without the engine renumbering them.
 */
enum class VertexAttribute
{
    Position,  ///< Object-space position (the only required attribute).
    Normal,    ///< Shading normal (derived when the geometry authors none).
    Color,     ///< Optional per-vertex colour (define-gated on the vsg set).
    TexCoord0, ///< Optional texture coordinate (define-gated on the vsg set).
};

/**
 * @brief Shader location of a canonical attribute.
 *
 * The values are the engine's ABI, not a free choice: 8 is the reserved texcoord
 * slot, deliberately not the next number after the custom-channel range, so a
 * forwarded custom channel (which reuses its source location) cannot collide
 * with a canonical one. Changing a value here changes what the built-in shaders
 * must declare, so the two move together — a unit test pins that agreement.
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
    }
    return 0u;
}

/**
 * @brief Whether @p location is one of the canonical attributes.
 *
 * The check every consumer needs when it walks a geometry's channels and asks "is this one of the
 * engine's own, or a forwarded custom one?". Asking it HERE is what keeps the answer in step with
 * attributeLocation(): a range test (`location <= 2`) or a list of literals would be a second copy
 * of the ABI, and the reserved texcoord slot is exactly the value a range test gets wrong.
 *
 * @param location Shader location to classify.
 * @return true when it is a canonical attribute's location.
 */
constexpr bool isCanonicalAttributeLocation(std::uint32_t location) noexcept
{
    return location == attributeLocation(VertexAttribute::Position) ||
           location == attributeLocation(VertexAttribute::Normal) ||
           location == attributeLocation(VertexAttribute::Color) ||
           location == attributeLocation(VertexAttribute::TexCoord0);
}

/**
 * @brief Per-view data block the built-in shading reads (L1 shape).
 *
 * The engine owns this SHAPE; a backend owns where it lives — a uniform / constant
 * buffer, or (as the vsg forward path does today) the proj/model subset inside the
 * 128-byte push range. Every member is a mat4/vec4 and the struct is 16-byte
 * aligned, so std140, D3D cbuffer packing and root constants all agree (see
 * .ai/design/graphics-shader.md §11/§12): a `vec3` followed by a `float` is the one
 * packing trap this rule avoids.
 *
 * Matrices are column-major, matching the math module's convention; a backend
 * whose shader language needs row-major translates when it fills the block.
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
 * The user parameter slot is reserved for the per-drawable values (P10: material
 * overrides / user parameters); until then it packs as zero.
 */
struct alignas(16) VineDrawBlock
{
    std::array<float, 16> model{};  ///< Object -> world (column-major mat4).
    std::array<float, 4>  params{}; ///< User parameter slot (reserved).
};

// The structs ARE the shader ABI: a member added here without updating the GLSL
// (or vice versa) must fail the build, not silently mis-read at run time.
static_assert(sizeof(VineViewBlock) == 288u, "VineViewBlock must be 4 mat4 + 2 vec4");
static_assert(alignof(VineViewBlock) == 16u, "VineViewBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(VineViewBlock, cam_pos) == 256u, "VineViewBlock std140 offset");
static_assert(offsetof(VineViewBlock, frame) == 272u, "VineViewBlock std140 offset");
static_assert(sizeof(VineDrawBlock) == 80u, "VineDrawBlock must be 1 mat4 + 1 vec4");
static_assert(alignof(VineDrawBlock) == 16u, "VineDrawBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(VineDrawBlock, params) == 64u, "VineDrawBlock std140 offset");

V_GRAPHICS_NS_END
