#pragma once

#include <array>
#include <cstddef>
#include <span>

#include <vine/graphics/backend/FrameRecorder.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Packing the frame's lights into the block the shading reads - one block, both drawing paths.
 *
 * WHAT THE BLOCK IS. `VineLightsBlock` (vine_params.glsl is its one GLSL declaration) carries ONE ambient
 * plus `kLightSlots` typed lights, every position and direction in VIEW space:
 *
 *   * `position_kind[i]`   xyz = the light's view-space position, w = its kind (a `LightType` value:
 *     Directional = 1, Point = 2, Spot = 3; -1 marks an UNUSED slot);
 *   * `direction_range[i]` xyz = its view-space direction (unused for a point light), w = its range
 *     (<= 0 = no cutoff);
 *   * `color_intensity[i]` rgb + intensity;
 *   * `cone[i]`            cos(inner) / cos(outer) for a spot light, unused otherwise.
 *
 * The parallel arrays carry no `struct VineLight[]` on purpose: the ABI scanner (`api/ProgramAbi`) sizes
 * built-in member types only, so a struct array would be a declaration it cannot measure and must refuse.
 *
 * WHY A UBO AND NOT THE PUSH RANGE. The content path's forward vertex stage already spends the guaranteed
 * 128-byte push range on its camera matrices, and `kLightSlots` typed lights do not fit there anyway (the old
 * full-screen push spent 16 of its 128 bytes on a field nothing read). So BOTH paths bind the block as a
 * uniform buffer, and the full-screen path keeps its push range for a host program that declares one.
 *
 * THE VIEW-SPACE TRANSFORM IS THE POINT. The lights arrive in world space and leave multiplied by the camera's
 * axes - and, for a positional light, translated by its view matrix - so the shading lights in view space and
 * never needs a world matrix. A packing that forgot the rotation is invisible with an axis-aligned camera (the
 * two spaces coincide) and wrong the moment the camera turns: the sun stays where the world put it while
 * everything else rotates, which reads as "the light is attached to the scene wrong", not as "the rotation is
 * missing". A positional light that forgot the TRANSLATION is worse: it lights from a point that is only right
 * while the camera sits at the origin.
 *
 * THE AMBIENT FILL IS NOT AN ANNOUNCED LIGHT. An empty (or entirely unusable) light list keeps a small ambient
 * so a scene is visible instead of multiplied by zero - but that fill must not be COUNTED, or the caller
 * reports a dropped light the host never announced.
 *
 * The count this returns is therefore about the host's lights only: how many of the announced ones the block
 * represents. The caller compares it with how many were announced and reports the difference once per episode
 * (the SDK's rule for the drop report - see ContentPass).
 *
 * NO CAMERA, NO BLOCK: with nothing to rotate into, the block stays empty and the count is zero. That is the
 * reference behaviour (a pass that announced no camera is unlit rather than lit by a guess), and it is visible:
 * the shading writes black, not the ambient fill.
 */
VN_VSG_NS_BEGIN

/** @brief How many typed lights the block carries (a light past the last slot is not lit at all). */
inline constexpr std::size_t kLightSlots = 8U;

/** @brief The kind tag of an UNUSED slot (a used slot carries a `LightType` value). */
inline constexpr float kLightKindEmpty = -1.0F;

/** @brief The shading's light block: one ambient plus `kLightSlots` typed lights, all in view space. */
struct alignas(16) VineLightsBlock
{
    std::array<float, 4>                          ambient{};         ///< rgb + intensity.
    std::array<std::array<float, 4>, kLightSlots> position_kind{};   ///< xyz = view-space position, w = kind.
    std::array<std::array<float, 4>, kLightSlots> direction_range{}; ///< xyz = view-space direction, w = range.
    std::array<std::array<float, 4>, kLightSlots> color_intensity{}; ///< rgb + intensity.
    std::array<std::array<float, 4>, kLightSlots> cone{};            ///< cos(inner) / cos(outer), spot only.
};

// The struct IS the shader ABI (vine_params.glsl's VineLightsBlock): a field added here without updating that text
// (or vice versa) must fail the build, not silently mis-read at run time.
static_assert(sizeof(VineLightsBlock) == 528U, "VineLightsBlock must be 1 + four arrays of 8 vec4");
static_assert(alignof(VineLightsBlock) == 16U, "VineLightsBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(VineLightsBlock, position_kind) == 16U, "VineLightsBlock std140 offset");
static_assert(offsetof(VineLightsBlock, direction_range) == 144U, "VineLightsBlock std140 offset");
static_assert(offsetof(VineLightsBlock, color_intensity) == 272U, "VineLightsBlock std140 offset");
static_assert(offsetof(VineLightsBlock, cone) == 400U, "VineLightsBlock std140 offset");

/**
 * @brief Gets the block slot one announced light was packed into.
 *
 * The walk is the packing's own: enabled lights take slots 0..kLightSlots-1 in announcement order, the
 * positional and the directional ones alike, and everything else - disabled lights and a light the block has no
 * room for - takes none. It exists so a consumer that has to NAME a light (the shadow block's `params.w` says
 * which slot a map belongs to) can ask the same list the same question, instead of writing the order down a
 * second time.
 *
 * @param lights The lights the drawing call announced.
 * @param light_identity The identity of the light whose slot is wanted (see LightRef::identity).
 * @return The slot in [0, kLightSlots), or kLightSlots when the light has none.
 */
std::size_t packedSlotOf(std::span<const core::LightRef> lights, const void* light_identity) noexcept;

/**
 * @brief Packs a drawing call's lights into the shading block.
 *
 * The walk is the reference one: enabled ambient lights fill the ambient slot (the last one wins), enabled
 * directional / point / spot lights take slots 0..kLightSlots-1 in announcement order, every disabled light is
 * skipped, and a light the block has no room for is skipped as well rather than packed somewhere a shader
 * cannot name it. Every slot's kind is written, an unused one as `kLightKindEmpty`.
 *
 * @param lights The lights the drawing call announced; empty means the backend default (an ambient-lit scene).
 * @param camera The camera announced with the same call (its view matrix holds the world -> view transform).
 * @param out    Receives the block; every field is written (the ambient fill included).
 * @return How many of @p lights the block represents, not counting the ambient fill.
 */
std::size_t packLightBlock(std::span<const core::LightRef> lights, const core::CameraSnapshot& camera,
                           VineLightsBlock& out) noexcept;

VN_VSG_NS_END
