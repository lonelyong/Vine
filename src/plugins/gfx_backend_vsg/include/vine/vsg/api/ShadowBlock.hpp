#pragma once

#include <vine/graphics/ShaderAbi.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Packing the shadow block a drawing call shades with (`VineShadowBlock`) from the plan's own facts.
 *
 * THE MAP ARRIVES AS A SAMPLED INPUT; THIS BLOCK IS WHAT MAKES IT A SHADOW. A pass samples a shadow by declaring the
 * target the map was rendered into (see the engine's `setPassInputs`), which the sampling path already binds. What
 * the shader cannot get from a texture is WHERE a fragment lands in it: the map belongs to a LIGHT, its pixels were
 * rasterised by that light's camera, and the consuming pass draws from its own camera - so the block carries the one
 * matrix that takes a VIEW-space position into the producer's clip space (`light_vp * inverse(view)`) plus the
 * scalars the comparison needs.
 *
 * FOUR FACTS DECIDE WHETHER IT IS SHADED AT ALL, and each of them is somebody else's statement:
 *
 *   * the TARGET says whose map it is (`RenderTarget::shadowOf`; the plan copied it into `ShadowFacts`) - a map found
 *     by "the first sampleable depth input" would shade a G-buffer's depth as if it were the sun's (a measured
 *     defect of the reference backend, see the ShadowFacts note);
 *   * the producer published how to read it (`setProducerViewProjection`): a map without a matrix is not readable,
 *     and shading nothing is the honest answer;
 *   * the LIGHT still casts (`Light::castShadow`) and is one this call announced - the switch lives on the light, so
 *     turning it off has to stop the shading, not just the rendering;
 *   * the light has a SLOT in the light block (one of its three directional entries): a caster the block cannot name
 *     would scale a light the map does not belong to, so the switch stays off instead.
 *
 * `params.x` is that switch (1 = shaded, 0 = the same shader text takes the unshadowed path), `y` the bias - the
 * casting light's OWN `ShadowSettings` (a private backend default would be the second answer this ABI exists to
 * prevent), `z` the strength (1: the SDK has no knob for it yet), `w` the light's slot.
 */
V_VSG_NS_BEGIN

/**
 * @brief Packs the shadow block one drawing call shades with.
 *
 * @param shadow The map the PLAN resolved for the pass (whose it is, and how to read it).
 * @param draw   The drawing call: its camera (the view space the matrix maps from) and its lights (the caster).
 * @param out    Receives the block; every field is written, and the switch is off when the map cannot be used.
 * @return true when the shading is on (`params.x == 1`), false when the call shades unshadowed.
 */
bool packShadowBlock(const core::ShadowFacts& shadow, const core::CompiledDraw& draw,
                     vine::graphics::VineShadowBlock& out) noexcept;

V_VSG_NS_END
