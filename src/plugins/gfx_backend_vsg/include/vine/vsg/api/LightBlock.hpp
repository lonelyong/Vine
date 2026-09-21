#pragma once

#include <array>
#include <cstddef>
#include <span>

#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Packing the forward path's light block from the frame's own light facts - the bytes the shading lights with.
 *
 * WHY A UBO AND NOT THE PUSH RANGE. A content pipeline needs the view matrices, and the engine's forward vertex stage
 * takes them from the 128-byte push range, which is exactly Vulkan's guaranteed budget. There is nothing left to push
 * lights with, so the forward path spends a UNIFORM BUFFER instead: one block, bound per drawing call, written from
 * the lights that call announced. (The full-screen path needs no matrices and spends the whole push range on its own
 * light representation; the two are different ABIs with the same numbers, which is why the packing here is the
 * forward half of one rule and not a copy of the other.)
 *
 * THE BLOCK IS THE CONTRACT WITH THE SHADER (`VineLightsBlock`, the name the GLSL block in builtin_forward.frag
 * declares), and two of its facts fail silently when they are wrong:
 *
 *   * THE DIRECTIONS ARE VIEW-SPACE. The lights arrive in world space and leave multiplied by the camera's axes, so
 *     the shading can light in view space and never touch a world matrix. A packing that forgot the rotation is
 *     invisible with an axis-aligned camera (the two spaces coincide) and wrong the moment the camera turns - the
 *     sun stays where the world put it while everything else rotates, which reads as "the light is attached to the
 *     scene wrong", not as "the rotation is missing";
 *   * THE AMBIENT FILL IS NOT AN ANNOUNCED LIGHT. An empty (or entirely unusable) light list keeps a small ambient
 *     so a scene is visible instead of multiplied by zero - but that fill must not be COUNTED, or the caller reports
 *     a dropped light that the host never announced.
 *
 * The count this returns is therefore about the host's lights only: how many of the announced ones the block
 * represents. The caller compares it with how many were announced and reports the difference once per episode (the
 * SDK's rule for the drop report - see ContentPass).
 *
 * NO CAMERA, NO BLOCK: with nothing to rotate into, the block stays empty and the count is zero. That is the reference
 * behaviour (a pass that announced no camera is unlit rather than lit by a guess), and it is visible: the shading
 * writes black, not the ambient fill.
 */
V_VSG_NS_BEGIN

/** @brief The forward path's light block: one ambient plus up to three directional lights, all in view space. */
struct alignas(16) VineLightsBlock
{
    std::array<float, 4>                ambient{};  ///< rgb + intensity.
    std::array<std::array<float, 4>, 3> dirs{};     ///< View-space directions (w unused).
    std::array<std::array<float, 4>, 3> cols{};     ///< rgb + intensity of the light each direction belongs to.
};

// The struct IS the shader ABI (builtin_forward.frag's VineLightsBlock): a field added here without updating that
// text (or vice versa) must fail the build, not silently mis-read at run time.
static_assert(sizeof(VineLightsBlock) == 112U, "VineLightsBlock must be 1 vec4 + 3 vec4 + 3 vec4");
static_assert(alignof(VineLightsBlock) == 16U, "VineLightsBlock must stay std140 / D3D-cbuffer aligned");

/** @brief How many directional lights the block holds (a fourth one is not lit at all). */
inline constexpr std::size_t kLightDirectionalSlots = 3U;

/** @brief The full-screen path's light representation: the same lights, in the 128-byte PUSH range.
 *
 * WHY A SECOND SHAPE AT ALL. The full-screen path needs no view matrices (its vertex stage generates the triangle),
 * so its whole push budget is free - and the engine's deferred lighting program spends it on the lights, which is
 * why a full-screen pass has no light UBO to bind. The values are the SAME ones the forward block carries (one
 * view-space ambient plus three directionals: `packLightPushBlock` is `packLightBlock` in this layout), because the
 * SDK's two programs light the same scene; only the transport differs.
 *
 * `projparms` is RESERVED AND STAYS ZERO: it is where a program that reconstructs a view position from a depth
 * buffer would find near / far / proj[0][0] / proj[1][1], and no shipped program reads it - the engine's own
 * lighting program samples the G-buffer's view-position attachment instead (see builtin_deferred_lighting.frag,
 * which says so at the declaration). The reference backend filled it for a perspective camera and nothing read it,
 * which is the "a field that promises an effect has to have one" failure this file's sibling notes keep naming;
 * inventing a reader here would be worse than leaving the reservation visible.
 */
struct alignas(16) LightPushBlock
{
    std::array<float, 4>                ambient{};    ///< rgb + intensity (the light block's ambient slot).
    std::array<float, 4>                projparms{};  ///< RESERVED: zeros, see the declaration.
    std::array<std::array<float, 4>, 3> dirs{};       ///< View-space directions (w unused).
    std::array<std::array<float, 4>, 3> cols{};       ///< rgb + intensity of the light each direction belongs to.
};

// The struct IS the shader ABI (builtin_deferred_lighting.frag's PushConstants) AND the range the pipeline declares:
// a field added here without updating either must fail the build.
static_assert(sizeof(LightPushBlock) == 128U, "LightPushBlock must match the 128-byte push-constant range");
static_assert(alignof(LightPushBlock) == 16U, "LightPushBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(LightPushBlock, projparms) == 16U, "LightPushBlock std140 offset");
static_assert(offsetof(LightPushBlock, dirs) == 32U, "LightPushBlock std140 offset");
static_assert(offsetof(LightPushBlock, cols) == 80U, "LightPushBlock std140 offset");

/**
 * @brief Gets the directional slot one announced light was packed into.
 *
 * The walk is the packing's own: enabled directional lights take slots 0..2 in announcement order, everything else -
 * disabled lights, other kinds, and a light the block has no room for - takes none. It exists so a consumer that has
 * to NAME a light (the shadow block's `params.w` says which of the block's directional lights a map belongs to) can
 * ask the same list the same question, instead of writing the order down a second time.
 *
 * @param lights The lights the drawing call announced.
 * @param light_identity The identity of the light whose slot is wanted (see LightRef::identity).
 * @return The slot in [0, kLightDirectionalSlots), or kLightDirectionalSlots when the light has none.
 */
std::size_t directionalSlotOf(std::span<const core::LightRef> lights, const void* light_identity) noexcept;

/**
 * @brief Packs a drawing call's lights into the forward light block.
 *
 * The walk is the reference one: enabled ambient lights fill the ambient slot (the last one wins), enabled directional
 * lights take slots 0..2 in announcement order, every other kind and every disabled light is skipped, and a light the
 * block has no room for is skipped as well rather than packed somewhere a shader cannot name it.
 *
 * @param lights The lights the drawing call announced; empty means the backend default (an ambient-lit scene).
 * @param camera The camera announced with the same call (its view matrix holds the world -> view rotation).
 * @param out    Receives the block; every field is written (the ambient fill included).
 * @return How many of @p lights the block represents, not counting the ambient fill.
 */
std::size_t packLightBlock(std::span<const core::LightRef> lights, const core::CameraSnapshot& camera,
                           VineLightsBlock& out) noexcept;

/**
 * @brief Packs a full-screen drawing call's lights into the push block.
 *
 * The values are the forward block's (see @ref packLightBlock), so the two paths light a scene identically; the
 * reserved field stays zero.
 *
 * @param lights The lights the drawing call announced; empty means the backend default (an ambient-lit scene).
 * @param camera The camera announced with the same call (its view matrix holds the world -> view rotation).
 * @param out    Receives the block; every field is written (the reservation included).
 * @return How many of @p lights the block represents, not counting the ambient fill.
 */
std::size_t packLightPushBlock(std::span<const core::LightRef> lights, const core::CameraSnapshot& camera,
                               LightPushBlock& out) noexcept;

V_VSG_NS_END
