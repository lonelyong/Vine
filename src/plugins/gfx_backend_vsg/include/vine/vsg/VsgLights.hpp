#pragma once
#include <vine/vsg/vsg_global.hpp>

// Internal header: what packs the light blocks the shading reads.
//
// The blocks themselves are the shading ABI and live next to the pipeline states that read them
// (LightPushBlock / VineLightsBlock, VsgPipelineFactory.hpp). What they contain comes from a
// camera and a light list — world -> view on the CPU, because both shader paths light in view
// space and neither owns a view matrix — and that packing is what this unit owns. It was part of
// the fullscreen-program draw's translation unit until the two were split: the forward content
// path packs the same block (fillVineLightsBlock), so the packing was never the draw's.

#include <array>
#include <cstddef>
#include <vector>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Light.hpp>

V_VSG_NS_BEGIN

namespace detail
{
/** @brief CPU mirrors of the two light blocks (defined in VsgPipelineFactory.hpp). */
struct LightPushBlock;
struct VineLightsBlock;
} // namespace detail

namespace detail
{

/**
 * @brief Computes the world -> view rotation basis for a look-at camera. *
 * @param camera Vine camera (eye / target / up).
 * @param r      Receives the view-space X axis in world coords (right).
 * @param u      Receives the view-space Y axis in world coords (up).
 * @param f      Receives the view-space -Z axis in world coords (forward).
 */
void viewRotation(const vine::graphics::Camera* camera, double r[3], double u[3], double f[3]);

/**
 * @brief Fills a fullscreen light push block for a deferred-lighting pass.
 *
 * The G-buffer stores view-space normals / positions, so directional lights
 * are pre-transformed from world to view space on the CPU (the fragment
 * shader then never needs a view matrix). Supports the first ambient plus up
 * to three directional lights (the push block is exactly 128 bytes); further
 * lights are ignored (documented S4 limitation). When the pass carries no
 * ambient light a small default ambient is seeded, mirroring how a scene pass
 * with an empty light list keeps its view's default light: without it a
 * fullscreen program pass bound to no lights would shade everything to black
 * (ambient 0 x albedo) — a silent, hard-to-diagnose blank frame.
 *
 * @param camera Camera whose view transforms the lights (may be null).
 * @param lights Scene lights to bake (borrowed).
 * @param block  Receives the packed block (zeroed first).
 */
void fillLightPushBlock(const vine::graphics::Camera* camera,
                        const std::vector<const vine::graphics::Light*>& lights, LightPushBlock& block);

/**
 * @brief Fills the forward path's per-view light block.
 *
 * The same packing as fillLightPushBlock's light half (one ambient plus up to
 * three directionals, world -> view on the CPU), but written into the uniform
 * block the forward shader reads instead of into the push-constant range the
 * full-screen path owns: the forward path needs that range for the camera
 * matrices (see VineLightsBlock).
 *
 * The count is what the CALLER has to be able to say out loud: a light that is disabled, is neither
 * ambient nor directional, is a second ambient (the block carries ONE ambient, the last one
 * announced), or is the fourth directional is not lit, and a pass that announces it draws a picture
 * the host did not ask for unless it is told (see beginLightsDroppedEpisode).
 *
 * @param camera Camera whose view transforms the lights (may be null).
 * @param lights Scene lights to bake (borrowed).
 * @param block  Receives the packed block (zeroed first).
 * @return How many of the announced lights the block represents.
 */
std::size_t fillVineLightsBlock(const vine::graphics::Camera* camera,
                                const std::vector<const vine::graphics::Light*>& lights, VineLightsBlock& block);

/**
 * @brief The directional slot the shadow-casting light occupies, and the light itself.
 *
 * The shadow ABI carries the index of the light a map belongs to (`VineShadowBlock::params.w`), because
 * the shader's shadow term is inserted INSIDE the loop over the block's directional lights: without the
 * index it scales every light's term, and a light that casts nothing (the demo's fill) is extinguished
 * wherever the casting light is blocked - measured: a face the sun never reaches fell to ambient-only in
 * the shape of the sun's shadow (the lit-face phase of vsg_backend_selftest is the gate).
 *
 * The walk is the packing's own rule (enabled directionals in announcement order, three slots), so the
 * index cannot drift from the block the shader reads.
 */
struct ShadowLightSlot
{
    const vine::graphics::Light* light = nullptr; ///< The first enabled shadow-casting directional.
    std::size_t                  slot  = 3u;      ///< Its block slot (0..2), or 3 when the block cannot carry it.
};

/**
 * @brief Finds the light a shadow map would belong to, and the slot a shader can name it by.
 *
 * @param lights Scene lights to walk (borrowed; null entries and disabled lights are skipped).
 * @return The light and its slot; `light` is null when nothing casts, `slot` is 3 when the caster would
 *         fall outside the block's three directional slots (nothing the shader could scale).
 */
ShadowLightSlot shadowLightSlot(const std::vector<const vine::graphics::Light*>& lights);

} // namespace detail

V_VSG_NS_END
