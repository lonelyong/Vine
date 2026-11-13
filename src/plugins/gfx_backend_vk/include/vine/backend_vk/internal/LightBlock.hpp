#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <array>
#include <cstddef>
#include <span>

#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/ShaderAbi.hpp>

VN_BACKEND_VK_NS_BEGIN

/** @brief The kind tag of an UNUSED slot (a used slot carries a `LightType` value). */
inline constexpr float kLightKindEmpty = -1.0F;

/**
 * @brief The shading's light block: one ambient plus `kMaxLights` typed lights, all in view space.
 *
 * @details
 * - The C++ half of the GLSL `VineLightsBlock` (`shaders/vine_params.glsl`, set 0 binding 6 on the content path and
 *   on the full-screen path alike). vsg declares the same shape next to its packer; this backend cannot include that
 *   header (backend internals do not cross), so it states the block here with the same sizes and offsets, and the
 *   static_asserts below are the numbers both sides must keep.
 * - Every slot's `position_kind.w` is its kind, and an unused slot says `kLightKindEmpty` rather than 0 - a shader
 *   that trusted a zeroed slot would read a kind the array does not carry and light a light that is not there.
 * - Positions AND directions are in VIEW space: the content vertex stages already work there, so the fragment stage
 *   needs no world matrix to evaluate the light.
 */
struct alignas(16) VineLightsBlock {
    /// rgb + intensity.
    std::array<float, 4> ambient{};
    /// xyz = the light's view-space position, w = its kind (`LightType` value, `kLightKindEmpty` when unused).
    std::array<std::array<float, 4>, vn::graphics::kMaxLights> position_kind{};
    /// xyz = view-space direction (unused for a point light), w = range (<= 0 = no cutoff).
    std::array<std::array<float, 4>, vn::graphics::kMaxLights> direction_range{};
    /// rgb + intensity.
    std::array<std::array<float, 4>, vn::graphics::kMaxLights> color_intensity{};
    /// cos(inner) / cos(outer) for a spot light, unused otherwise.
    std::array<std::array<float, 4>, vn::graphics::kMaxLights> cone{};
};

// The struct IS the shader ABI (vine_params.glsl's VineLightsBlock): a field added here without updating that text
// (or vice versa) must fail the build, not silently mis-read at run time.
static_assert(sizeof(VineLightsBlock) == 16U + 4U * vn::graphics::kMaxLights * 16U,
              "VineLightsBlock must be one ambient vec4 plus four arrays of kMaxLights vec4");
static_assert(alignof(VineLightsBlock) == 16U, "VineLightsBlock must stay std140 / D3D-cbuffer aligned");
static_assert(offsetof(VineLightsBlock, position_kind) == 16U, "VineLightsBlock std140 offset");
static_assert(offsetof(VineLightsBlock, direction_range) == 16U + vn::graphics::kMaxLights * 16U, "VineLightsBlock std140 offset");
static_assert(offsetof(VineLightsBlock, color_intensity) == 16U + 2U * vn::graphics::kMaxLights * 16U, "VineLightsBlock std140 offset");
static_assert(offsetof(VineLightsBlock, cone) == 16U + 3U * vn::graphics::kMaxLights * 16U, "VineLightsBlock std140 offset");

/**
 * @brief Packs an announced light list into the shading block.
 *
 * @details
 * - The walk is the engine's reference one (the same rule the vsg backend states): enabled ambient lights fill the
 *   ambient slot (the last one wins), enabled directional / point / spot lights take slots 0..`kMaxLights`-1 in
 *   announcement order, a disabled light takes none, and a positional light nobody placed takes none either.
 * - A list that announces no ambient light gets the small ambient fill, so an unlit scene is visible instead of
 *   multiplied by zero. The fill is NOT part of the returned count: it is not a light the host announced.
 * - Every field is written, unused slots included: a block left as whatever the previous owner of these bytes wrote
 *   is a shader reading another frame's lights.
 * - No camera means no block: with nothing to rotate into, the block stays empty and the count is zero.
 *
 * @param lights The lights the drawing call announced (see `DrawDescription::light_values`); empty means "the
 *               backend default", an ambient-lit scene.
 * @param camera The camera announced with the same call (its view matrix is the world -> view transform).
 * @param out    Receives the block; every field is written, the ambient fill included.
 * @return How many of @p lights the block represents, not counting the ambient fill.
 */
std::size_t packLightBlock(std::span<const vn::graphics::LightValue> lights,
                           const vn::graphics::CameraSnapshot&      camera,
                           VineLightsBlock&                         out) noexcept;

VN_BACKEND_VK_NS_END
