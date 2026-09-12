#pragma once

/**
 * @brief The overlay draws: what a pass composites on top of (or into) a target — a sampled
 * target's colour (picture-in-picture) and the host's fullscreen program.
 *
 * Both draws share ONE destination rule, which is why they live together: the target the pass is
 * bound to (setRenderTarget; nullptr = the window), the graph the overlay's view records into, and
 * the surface size its rectangle is expressed in. The two kinds differ in WHAT they draw and in how
 * they fit a rectangle, never in where they draw — so the destination is resolved once
 * (resolveOverlayDestination), placed once (placeOverlayView) and the slot that keeps the draw
 * alive is installed the same way.
 *
 * @ref VsgOverlayDestination is namespace-scope on purpose: helpers read it, and a private nested
 * type could not be named by them (the same reason VsgContentSlotRequest was liberated).
 */

#include <vine/vsg/vsg_global.hpp>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/core/ref_ptr.h>

#include <vector>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

/** @brief CPU mirror of the deferred-lighting push-constant block (defined in VsgPipelineFactory.hpp). */
namespace detail
{
struct LightPushBlock;
}
/** @brief What an overlay draw (PiP / fullscreen program) records into.
 *
 * Every overlay draw resolves the same destination: the target the pass is
 * bound to (setRenderTarget; nullptr = the window), the graph its view must
 * record into, and the surface size its rectangle is expressed in. The two
 * overlay kinds differ in WHAT they draw and in how they fit a rectangle —
 * never in where they draw, which is why the rules live in one place (see
 * resolveOverlayDestination).
 */
struct VsgOverlayDestination
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
 * @param what   Draw name for the diagnostics ("drawScreenTexture" / "drawScreenProgram").
 * @return The destination; @c graph is null when the draw must not record.
 */
VsgOverlayDestination resolveOverlayDestination(VsgRendererState& state, const VsgDiagnostics& diagnostics,
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
void placeOverlayView(VsgRendererState& state, const VsgOverlayDestination& dest,
                      const ::vsg::ref_ptr<::vsg::View>& view, int order);


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

/** @brief Draws a sampled target's colour attachment as a picture-in-picture overlay.
 *
 * Selected by the pass' sub-viewport (setViewport) or auto-anchored bottom-right when that
 * rectangle does not fit the destination, drawing the source's requested colour attachment as a
 * full-screen textured triangle whose viewport clips it.
 *
 * @param state       Session the draw records into.
 * @param diagnostics Route a refused draw is reported on.
 * @param source      Target whose colour attachment is sampled.
 * @param attachment  Colour attachment index to sample (clamped, reported when out of range).
 */
void drawScreenTexture(VsgRendererState& state, const VsgDiagnostics& diagnostics, vine::graphics::RenderTarget* source,
                       int attachment);

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

