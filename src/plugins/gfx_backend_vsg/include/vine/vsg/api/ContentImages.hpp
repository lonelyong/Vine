#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <vsg/core/ref_ptr.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/Sampler.h>

#include <vine/vsg/api/ProgramAbi.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Which image a declared sampler binding gets, decided BY NAME - the one spelling of that policy.
 *
 * WHY A NAME TABLE AND NOT A SET INDEX. A program's text says where its samplers live (`layout(set = ...,
 * binding = ...)`) and what they are CALLED, and the layer serves the text as written (see api/ProgramAbi).
 * The engine's own programs spend the names this way: `diffuseMap` is the drawable's own texture,
 * `shadow_map` is the map its pass declared, `skyMap` is the frame's environment, and every other name
 * (`albedo_tex`, `normal_tex`, `spec_tex`, `pos_tex`, `screen_tex`, ...) is a texture an earlier pass
 * produced - one of the pass' declared INPUTS. The NAME is the contract: the same name in different sets is
 * the same thing, and a set index is only where a text happens to put it (the engine puts the material, the
 * diffuse map and the shadow map in one set, and a G-buffer's four pictures in another).
 *
 * WHAT THE LAYER ENFORCES TODAY, and what it hands the caller. Two rows have a producer inside this backend:
 *   * `Environment` (`skyMap`): the environment image has not landed, so a program that samples it is REFUSED
 *     where the pipeline would be built (ContentPipeline::create) rather than compiled against a stand-in
 *     that would show something the host did not author.
 *   * `Shadow` (`shadow_map`): the map is the one the pass' plan resolved (see api/ContentImages.hpp's
 *     `shadowImageOf`), and a pass whose plan resolved one reports a program that declares none - the map
 *     would never reach its drawables otherwise, silently.
 * The other two rows are the CALLER's, because the caller is the one that owns the image:
 *   * `Material` (`diffuseMap`): the material's own texture, or the white fallback when it has none
 *     (api/WhiteImage - "no map" is WHITE, not an unwritten binding).
 *   * `Input`: the pass' declared inputs, in declaration order - input by input, each one's colour
 *     attachments in attachment order and then its depth, the same order the pass' own input set binds them.
 *   * ... and a `shadow_map` with no resolved map takes the SAME white stand-in: the engine's programs read
 *     the map under the shadow block's switch (VineShadowBlock::params.x), and while that switch is off the
 *     picture is exactly the material's - so the stand-in's value has to be the multiply's identity.
 *
 * NO DEVICE IS NEEDED for anything here: the images are views and samplers (create-infos until a context
 * compiles them), and the plan is values.
 */
V_VSG_NS_BEGIN

/** @brief The images one compiled input offers, as the layer that owns the target reports them.
 *
 * One entry per plan input, in the plan's order: the caller walks `CompiledPass::inputs` and answers for each
 * one. A caller with nothing to offer for an input (it produced nothing this frame) hands over an empty entry
 * rather than skipping it, so the two lists stay indexable against each other.
 */
struct InputImages
{
    std::span<const ::vsg::ref_ptr<::vsg::ImageView>> colors;  ///< Colour attachments, in attachment order.
    ::vsg::ref_ptr<::vsg::ImageView>                  depth;   ///< The input's DEPTH view, when it offers one.
};

/** @brief Where a declared sampler binding takes its image from (the by-name policy of the file note). */
enum class ImageOrigin
{
    Material,     ///< The drawable's own texture, or the white fallback (`diffuseMap`).
    Environment,  ///< The frame's environment image (`skyMap`); not built yet, so refused where it is declared.
    Shadow,       ///< The map the pass' plan resolved (`shadow_map`); a pass without one takes the white stand-in.
    Input,        ///< Any other name: a texture the pass' declared inputs offer, in declaration order.
};

/** @brief Gets where a declared sampler's @p name takes its image from.
 *
 * @param name The declared instance name (`diffuseMap`, `shadow_map`, `skyMap`, ...); an empty name is a
 *             declaration the scan could not name, which is served like any other input texture.
 * @return The origin the name states (see the file note for what each one is filled from).
 */
[[nodiscard]] ImageOrigin imageOriginOf(std::string_view name) noexcept;

/** @brief One image, as a caller needs it to fill a declared sampler binding. */
struct SamplerImage
{
    ::vsg::ref_ptr<::vsg::ImageView> view;     ///< The image read there.
    ::vsg::ref_ptr<::vsg::Sampler>   sampler;  ///< The filter it is read with (NEAREST for depths).
};

/** @brief Gets whether @p abi samples the map its plan resolved: it declares a sampler named `shadow_map`.
 *
 * This is the by-name half of "does the shadow reach this program", and it does NOT ask which set or binding
 * the text names: a program that declares the map anywhere can read it, and one that declares it nowhere
 * cannot, whatever its other declarations say (see the file note).
 *
 * @param abi The bindings the program's text declares.
 * @return true when at least one declared sampler is the shadow map's name.
 */
[[nodiscard]] bool samplesShadowMap(const ProgramAbi& abi) noexcept;

/** @brief Picks the map a pass samples, as the layer's depth sampler reads it.
 *
 * The map is the INPUT the plan resolved (core::FrameCompiler's `resolveShadow`: the first input whose target
 * states it is a map, whose depth is sampleable and whose producer published how to read it) - so the picker
 * asks the plan, and falls back to walking the inputs with the SAME three facts only to find which offered
 * image that is. A caller binds the answer at the binding its text names for `shadow_map`, and binds
 * @ref SamplerImage empty (the white stand-in) when this returns false.
 *
 * @param pass          The compiled pass (its resolved shadow, and the inputs it was resolved from).
 * @param inputs        The images the caller offers, one entry per `pass.inputs` entry and in the same order.
 * @param depth_sampler The sampler a depth texture is read with (NEAREST - see ContentPipeline).
 * @param out           Receives the map's view and sampler when one is found (untouched otherwise).
 * @return true when the pass samples a map the caller really offered.
 */
[[nodiscard]] bool shadowImageOf(const core::CompiledPass& pass, std::span<const InputImages> inputs,
                                 const ::vsg::ref_ptr<::vsg::Sampler>& depth_sampler, SamplerImage& out) noexcept;

V_VSG_NS_END
