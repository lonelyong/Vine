#pragma once
#include "graphics_global.hpp"

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

V_GRAPHICS_NS_END
