#pragma once
#include "graphics_global.hpp"

#include <array>
#include <cstddef>
#include <span>

#include "CameraSnapshot.hpp"
#include "Light.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief How many typed lights one drawing call can have lit - the light block's slot count.
 *
 * @details
 * - The light block (`VineLightsBlock` below, declared once in the GLSL `shaders/vine_params.glsl` and packed by
 *   packLightBlock) carries one ambient plus this many typed lights, in announcement order: directional, point and
 *   spot alike. A drawing call that announces more has the extra ones reported (a `ChannelIgnored` warning, once per
 *   episode - a backend reports the difference `packLightBlock` returns) rather than refused.
 * - It is a SHADING capacity, not a scene limit: `Scene::addLight` takes any number of lights, because the same
 *   content may be drawn by a program with a different capacity (a host's own lighting stage, or a later block with
 *   more slots). Ask this constant when building a rig; do not cap the content to it.
 * - ONE number, several consumers: the GLSL arrays are this length, `VineLightsBlock` is sized by it, and a test reads
 *   the declared length back out of the embedded shader text - so a change reaches both sides or fails.
 *
 * @note The ambient term is not a slot: an enabled ambient light fills the block's ambient field, so any number of
 *       them are represented by one value (the last one wins) and only typed lights consume a slot.
 */
inline constexpr std::size_t kMaxLights = 8U;

/**
 * @brief The kind tag of an UNUSED light slot (a used slot carries a `LightType` value).
 *
 * @details
 * - A zeroed slot would read as `LightType::Ambient` (0) - a kind the slot ARRAY does not carry - so every unused slot
 *   states this tag instead and a shader can skip it without guessing.
 * - It is part of the ABI: the GLSL side compares the slot's kind against -1 (see `vine_params.glsl`).
 */
inline constexpr float kLightKindEmpty = -1.0F;

/**
 * @brief Per-view light data block the shading reads (L1 shape).
 *
 * @details
 * - One ambient light plus `kMaxLights` typed lights, every position and direction in VIEW space: the content vertex
 *   stages already work there, so the fragment stage needs no world matrix to evaluate a light.
 * - `position_kind[i]` is xyz = the light's view-space position, w = its kind (`LightType` value, `kLightKindEmpty`
 *   when the slot is unused); `direction_range[i]` is xyz = its view-space direction (unused for a point light), w =
 *   its range (<= 0 = no cutoff); `color_intensity[i]` is rgb + intensity; `cone[i]` is cos(inner) / cos(outer) for a
 *   spot light, unused otherwise.
 * - The parallel arrays carry no `struct VineLight[]` on purpose: the ABI scanner (`ProgramAbi`) sizes built-in member
 *   types only, so a struct array would be a declaration it cannot measure and must refuse.
 * - One block serves both drawing paths (the forward content path binds it at set 0 binding 6, the full-screen path at
 *   the same binding) and one function fills it, because the packing rule is the engine's rather than a backend's: a
 *   light's term must mean the same thing whichever backend draws it.
 * - THIS HEADER, NOT `ShaderAbi.hpp`, BECAUSE THE BLOCK IS FILLED FROM THE SCENE: the shape, the capacity and the
 *   packing are one concept, and only this facet needs `LightValue` / `CameraSnapshot`; the block shapes that need
 *   nothing but bytes stay in `ShaderAbi.hpp`, which stays a leaf (see its file note). The dependency direction is
 *   therefore `ShaderAbi.hpp` <- this header <- a backend, never the reverse.
 *
 * @note Fill it with packLightBlock; a block written by hand is a second copy of the rule.
 */
struct alignas(16) VineLightsBlock {
    /// rgb + intensity.
    std::array<float, 4> ambient{};
    /// xyz = the light's view-space position, w = its kind (`LightType` value, `kLightKindEmpty` when unused).
    std::array<std::array<float, 4>, kMaxLights> position_kind{};
    /// xyz = view-space direction (unused for a point light), w = range (<= 0 = no cutoff).
    std::array<std::array<float, 4>, kMaxLights> direction_range{};
    /// rgb + intensity.
    std::array<std::array<float, 4>, kMaxLights> color_intensity{};
    /// cos(inner) / cos(outer) for a spot light, unused otherwise.
    std::array<std::array<float, 4>, kMaxLights> cone{};
};

// The struct IS the shader ABI (vine_params.glsl's VineLightsBlock): a field added here without updating that text
// (or vice versa) must fail the build, not silently mis-read at run time. Every size and offset is DERIVED from
// kMaxLights, so raising the capacity is one edit here and one in the GLSL - not a hunt for the literal 8.
static_assert(sizeof(VineLightsBlock) == 16U + 4U * kMaxLights * 16U, "VineLightsBlock must be one ambient vec4 plus four arrays of kMaxLights vec4");
static_assert(alignof(VineLightsBlock) == 16U, "VineLightsBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(VineLightsBlock, position_kind) == 16U, "VineLightsBlock std140 offset");
static_assert(offsetof(VineLightsBlock, direction_range) == 16U + kMaxLights * 16U, "VineLightsBlock std140 offset");
static_assert(offsetof(VineLightsBlock, color_intensity) == 16U + 2U * kMaxLights * 16U, "VineLightsBlock std140 offset");
static_assert(offsetof(VineLightsBlock, cone) == 16U + 3U * kMaxLights * 16U, "VineLightsBlock std140 offset");

/**
 * @brief Packs a drawing call's announced lights into the shading block, in view space.
 *
 * @details
 * - THE ONE PACKING RULE: enabled ambient lights fill the ambient slot (the last one wins), enabled directional /
 *   point / spot lights take slots 0..`kMaxLights`-1 in announcement order, a disabled light takes none, and a
 *   positional light nobody placed takes none either (it would light from the camera's eye). Every backend packs
 *   through this function, so a light's term cannot mean two things.
 * - A list that announces no ambient light gets the small ambient fill, so an unlit scene is visible instead of
 *   multiplied by zero. The fill is NOT part of the returned count: it is not a light the host announced.
 * - Every field is written, unused slots included (their kind is `kLightKindEmpty`): a block left as whatever the
 *   previous owner of these bytes wrote is a shader reading another frame's lights.
 * - Directions are rotated into view space and NORMALISED (the shading compares them against a normal); positions are
 *   rotated AND translated; a spot cone arrives as two cosines, so the shading does no trigonometry per fragment.
 * - No camera means no block: with nothing to rotate into, the block stays empty (black) and the count is zero - a pass
 *   that announced no camera is unlit rather than lit by a guess.
 *
 * @param lights The lights the drawing call announced (see `DrawDescription::light_values`); empty means "the backend
 *               default", an ambient-lit scene.
 * @param camera The camera announced with the same call (its view matrix is the world -> view transform).
 * @param out    Receives the block; every field is written, the ambient fill included.
 * @return How many of @p lights the block represents, not counting the ambient fill; a caller compares it with how
 *         many were announced and reports the difference once per episode.
 */
VN_GRAPHICS_API std::size_t packLightBlock(std::span<const LightValue> lights, const CameraSnapshot& camera, VineLightsBlock& out) noexcept;

/**
 * @brief Gets the block slot a light was packed into, or `kMaxLights` when it took none.
 *
 * @details
 * - THE OTHER HALF OF packLightBlock, by the same walk: what a shadow map needs is not the light's numbers but the
 *   INDEX its term sits at (the shadow block's `params.w` names it), and a second derivation of that rule is how a
 *   map ends up scaling a different light's term.
 * - A light takes a slot only when packLightBlock would give it one: enabled, not ambient, the block not full by then,
 *   and a positional light that was placed.
 *
 * @param lights   The lights the same drawing call announced.
 * @param identity The light's identity (see `LightValue::identity`), as a map states whose shadow it is.
 * @return The slot in [0, kMaxLights), or `kMaxLights` when that light took none (and for a null @p identity).
 */
VN_GRAPHICS_API std::size_t packedSlotOf(std::span<const LightValue> lights, const void* identity) noexcept;

VN_GRAPHICS_NS_END
