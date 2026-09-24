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
 * The engine's own programs spend the names this way: `diffuseMap` is the drawable's own texture, `skyMap`
 * is the drawable's own texture TOO (the sky box carries its map in its material and the sky program names
 * that map `skyMap` - see builtin_skybox.frag), `shadow_map` is the map its pass declared, and every other
 * name (`albedo_tex`, `normal_tex`, `spec_tex`, `pos_tex`, `screen_tex`, ...) is a texture an earlier pass
 * produced - one of the pass' declared INPUTS. The NAME is the contract: the same name in different sets is
 * the same thing, and a set index is only where a text happens to put it (the engine puts the material, the
 * diffuse map and the shadow map in one set, and a G-buffer's four pictures in another).
 *
 * WHAT THE LAYER FILLS TODAY, and who owns each row. ONE row has its producer inside this backend:
 *   * `Shadow` (`shadow_map`): the map is the one the pass' plan resolved (`shadowImageOf`), and a pass whose
 *     plan resolved one reports a program that declares none - the map would never reach its drawables
 *     otherwise, silently.
 * The other two rows are the CALLER's, because the caller is the one that owns the image:
 *   * `Material` (`diffuseMap` AND `skyMap`): the DRAWABLE's own texture. The engine's sky program names the
 *     sky box's own cube map `skyMap` and samples it at the ABI's material slot, which is the same slot
 *     `diffuseMap` names on every other content program. A material without a usable texture takes the
 *     white fallback OF THE DECLARED KIND (api/WhiteImage, api/MaterialImages::whiteCube - "no map" is
 *     WHITE, not an unwritten binding, and a 2D view where the text declares `samplerCube` is an invalid
 *     descriptor rather than an untextured draw).
 *   * `Input`: the pass' declared inputs, in declaration order - input by input, each one's colour
 *     attachments in attachment order and then its depth, the same order the pass' own input set binds them.
 *   * ... and a `shadow_map` with no resolved map takes the SAME white stand-in: the engine's programs read
 *     the map under the shadow block's switch (VineShadowBlock::params.x), and while that switch is off the
 *     picture is exactly the material's - so the stand-in's value has to be the multiply's identity.
 *
 * NO DEVICE IS NEEDED for anything here: the images are views and samplers (create-infos until a context
 * compiles them), and the plan is values.
 */
VN_VSG_NS_BEGIN

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
    Material,  ///< The drawable's own texture, or the white fallback of the declared kind (`diffuseMap`, `skyMap`).
    Shadow,    ///< The map the pass' plan resolved (`shadow_map`); a pass without one takes the white stand-in.
    Input,     ///< Any other name: a texture the pass' declared inputs offer, in declaration order.
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

/** @brief Gets the binding a program declares the shadow map at, in one set.
 *
 * The full-screen ABI needs the NUMBER (its set carries the map at the binding the text names - 5 for the
 * engine's four-colour G-buffer), while the content ABI's caller fills the binding itself; this is the one
 * spelling of "which binding is the map's".
 *
 * @param abi     The bindings the program's text declares.
 * @param set     Descriptor set index.
 * @param binding Receives the declared binding when the text names one there.
 * @return true when the set declares a sampler named `shadow_map`.
 */
[[nodiscard]] bool shadowBindingOf(const ProgramAbi& abi, std::uint32_t set, std::uint32_t& binding) noexcept;

/** @brief Gets the index of the pass' input that IS the map its plan resolved, in the offered list.
 *
 * The plan names the map by the light its target states (see core::FrameCompiler's `resolveShadow`), and the
 * offered list is walked with the same three facts - so the answer is the input the plan resolved, never
 * "the first input that has a depth" (a G-buffer has one too, and that is the measured defect this asks the
 * plan to avoid).
 *
 * @param pass  The compiled pass (its resolved shadow).
 * @param inputs The images the caller offers, one entry per `pass.inputs` entry and in the same order.
 * @return The index into @p inputs, or `inputs.size()` when the pass samples no map it can read.
 */
[[nodiscard]] std::size_t shadowInputIndexOf(const core::CompiledPass& pass,
                                            std::span<const InputImages> inputs) noexcept;

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

VN_VSG_NS_END
