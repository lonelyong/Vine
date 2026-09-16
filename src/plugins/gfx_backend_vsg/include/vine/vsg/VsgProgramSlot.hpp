#pragma once

/**
 * @brief The fullscreen program slot: the host's program drawn over a sampled target.
 *
 * It is the ONE screen draw this backend has (the picture-in-picture "copy a target into a rectangle"
 * path went away once ScreenPass grew a program): a ScreenPass carrying a program binds the source's
 * colour attachments (plus its depth while that one is sampleable) to the program's declared ABI
 * bindings and records as its own view under the destination's graph.
 *
 * WHERE it draws is resolved in one place (resolveProgramSlotDestination) and placed in one place
 * (placeProgramSlotView): the destination is the target the pass is bound to (setRenderTarget;
 * nullptr = the window), the graph its view records into, and the surface size its rectangle is
 * expressed in. The draw's own state — the retained slot, its stale predicate and the per-frame
 * rewriting of its shadow block — lives in detail::drawScreenProgram.
 *
 * The light blocks both shading paths read are packed in VsgLights, next to nothing else: that is a
 * property of the shading ABI, not of this draw, and the forward content path packs the same block.
 *
 * @ref ProgramSlotDestination is namespace-scope on purpose: helpers read it, and a private nested
 * type could not be named by them.
 */

#include <vine/vsg/vsg_global.hpp>

#include <vector>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/core/ref_ptr.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

/** @brief Where the fullscreen program draw records: its target, graph and surface size.
 *
 * Resolved once per draw from the pass' own scope: the target the pass is bound to
 * (setRenderTarget; nullptr = the window), the graph the slot's view must record into, and the
 * surface size the draw's rectangle is expressed in. One rule, so a draw that re-binds its target
 * does not also have to restate where its view goes (see resolveProgramSlotDestination).
 */
struct ProgramSlotDestination
{
    vine::graphics::RenderTarget*      target = nullptr; ///< Destination target (nullptr = the window).
    ::vsg::ref_ptr<::vsg::RenderGraph> graph;            ///< Graph the view records into; null = refuse to draw.
    int                                surf_w = 0;        ///< Surface width the rectangle is clamped to.
    int                                surf_h = 0;        ///< Surface height the rectangle is clamped to.
};

namespace detail
{

/** @brief Resolves and prepares the destination of an overlay draw.
 *
 * Rejects the source == destination feedback loop (sampling the very
 * attachments the pass writes), refuses a destination without a usable
 * colour attachment, (re)builds an off-screen destination whose size changed,
 * and hands back the graph the draw records into. Also re-targets the pass:
 * a pass that drew into another target before drops the slot it left there,
 * so it stops compositing into it.
 *
 * @param source Sampled target (also what the feedback loop is checked against).
 * @param key    Slot key of the draw (identifies the graph of an off-screen pass).
 * @param what   Draw name for the diagnostics ("drawScreenProgram").
 * @return The destination; @c graph is null when the draw must not record.
 */
ProgramSlotDestination resolveProgramSlotDestination(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                                                    vine::graphics::RenderTarget* source, const SlotKey& key,
                                                    const char* what);

/** @brief Places an overlay view in its destination's graph by its explicit order.
 *
 * The view is already compiled (against the destination's render pass), so this
 * only changes the RECORD order — the stacking position among the target's other
 * slot views (see placeViewByOrder). A view that (re)starts recording under an
 * off-screen destination also puts that pass' graph back into the command graph,
 * which is why the off-screen order is reconciled here.
 *
 * @param dest  Resolved destination of the draw.
 * @param view  The slot's retained view (compiled).
 * @param order The pass' explicit pipeline order.
 */
void placeProgramSlotView(VsgRendererState& state, const ProgramSlotDestination& dest,
                      const ::vsg::ref_ptr<::vsg::View>& view, int order);


/** @brief Draws the host's fullscreen program over a sampled target.
 *
 * The sampled target's colour attachments (plus its depth while that one is sampleable) are bound to
 * the program's declared ABI bindings, the pass' lights are baked into the 128-byte push block, and
 * the program is recorded as its own view under the destination's graph.
 *
 * @param state       Session the draw records into.
 * @param diagnostics Route a refused draw is reported on.
 * @param source      Target whose attachments are sampled.
 * @param program     Fullscreen program to draw.
 * @param camera      Camera whose view transforms the lights (may be null).
 */
void drawScreenProgram(VsgRendererState& state, const VsgDiagnostics& diagnostics, vine::graphics::RenderTarget* source,
                       vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                       vine::raw_ptr<const vine::graphics::Camera>        camera);

} // namespace detail

V_VSG_NS_END

