#pragma once

/**
 * @brief One content slot: the retained View a pass draws its content into.
 *
 * A content slot is the unit a pass' content lives in: its own View under the TARGET's render
 * graph (the window's swapchain graph, or the pass' own off-screen graph — §28), its own
 * SceneBridge (vsg compiles pipelines per viewID, and its per-view implementation reuse never
 * compares the render pass — which this backend varies per pass variant — so two slots must not
 * share a pipeline-state registry),
 * its own camera / light group and its stacking position (the pass' explicit pipeline order).
 *
 * The slot is keyed by the pass that OWNS it (@ref SlotKey), so a pass' camera or target may
 * change without orphaning its retained content, and two passes never alias.
 *
 * What a draw call announces reaches this code in two pieces, and the split is the point:
 *
 *  - the pass SCOPE attributes (target, order, depth policy, presenting role) are read from the
 *    session's own request (@ref VsgPassRequest, which the renderer filled from the scope), so there is
 *    ONE description of a pass' attributes rather than a second copy built per draw call;
 *  - the PER-DRAW-CALL state (the command stream, the announced lights, the taken sub-viewport) arrives
 *    as arguments, because it is consumed by the call instead of remembered.
 *
 * What a slot REMEMBERS it keeps as one value (@ref PassAttributes), so "what was applied" cannot drift
 * from "what was compared".
 */

#include <vine/vsg/vsg_global.hpp>

#include <optional>
#include <vector>

#include <vsg/app/RenderGraph.h>
#include <vsg/core/ref_ptr.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Viewport.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/** @brief Keeps a content slot's viewport (and its one vsg::ViewportState) in step with its role.
 *
 * The rectangular area a slot records into has two sources — the pass' announced sub-viewport, and the
 * target's own size when the pass presents full-target content — and two callers: the per-frame render
 * path, and the renderer's resize() (which refreshes the presenting slots before their next render).
 * One implementation, so the two cannot disagree about which rectangle a slot has.
 *
 * The state is written IN PLACE when the rectangle changes and left alone when it does not: building a
 * fresh vsg::ViewportState per slot per frame allocated on every steady-state frame, and the object it
 * replaced was not even required — vsg re-emits vkCmdSetViewport from the state on every recording, so
 * re-asserting the same rectangle needs no new object.
 *
 * @param content    Slot whose camera viewport to update (its cached state is reused).
 * @param presenting Whether the slot presents full-target content (wins over @p viewport).
 * @param viewport   The pass' sub-viewport, when it announced one and is not presenting. A zero-size one
 *                   is not usable either, so the slot keeps the rule it always had and fills the target.
 * @param surf_w     Target (or live swapchain) width in pixels; a zero width (a surface with no size yet)
 *                   asserts nothing, leaving the rectangle the slot already records.
 * @param surf_h     Target (or live swapchain) height in pixels.
 */
void updateSlotViewport(ContentSlot& content, bool presenting, const std::optional<vine::graphics::Viewport>& viewport,
                        int surf_w, int surf_h);

/** @brief Creates the content slot @p key identifies under @p request's target, if missing.
 *
 * The slot's identity (its camera bridge, its View, its render graph position) is established
 * here and kept: content slots are retained Views of the target's render graph, so a later
 * frame re-uses the slot instead of re-uploading the mesh and recompiling. A slot that cannot
 * be built (no render graph, a camera bridge that failed) is dropped and REPORTED — drawing
 * nothing silently is not an option.
 *
 * @param state       Session whose target table owns the slot.
 * @param persistent  Cross-session services the slot needs (its camera bridge).
 * @param diagnostics Route a slot that cannot be built is reported on.
 * @param key         Slot key (the owning pass, or the historical fallback identity).
 * @param target      Target the pass draws into (nullptr = the window).
 * @param camera      Camera the slot renders through.
 *
 * Its pass attributes are seeded from the session's request (@ref VsgPassRequest::attributes): a slot is
 * created by a draw call of the pass it belongs to, so "what the pass announced" is what the slot applies
 * first.
 */
void setupContentSlot(VsgRendererState& state, VsgRendererPersistent& persistent, const VsgDiagnostics& diagnostics,
                      const SlotKey& key, vine::graphics::RenderTarget* target,
                      vine::raw_ptr<const vine::graphics::Camera> camera);

/** @brief Renders one content slot (a View of a target's render graph).
 *
 * The slot is owned by the pass that draws it (@ref SlotKey) and its view is stacked by that
 * pass' explicit pipeline order, so several passes sharing one camera and one order stay
 * separate content. The request's depth policy is the explicit content depth handling
 * (independent of clearing; it fills the depth state of commands that did not author one), and
 * its presenting flag marks the full-target pass that cleared the target (such content fills
 * the whole target and seeds the window headlight when there is no scene light). Lights come
 * from the content scene each frame.
 *
 * @param state       Session whose target table holds the slot.
 * @param persistent  Cross-session services the slot sync needs.
 * @param diagnostics Route a failed content sync is reported on.
 * @param camera      Camera this draw call renders through.
 * @param commands    Commands to reconcile (the call's own stream).
 * @param lights      Lights the pass announced for this call (may be empty).
 * @param viewport    Sub-viewport taken for this call, or empty for the full target.
 */
void renderContentSlot(VsgRendererState& state, VsgRendererPersistent& persistent, const VsgDiagnostics& diagnostics,
                       vine::raw_ptr<const vine::graphics::Camera> camera,
                       const std::vector<vine::graphics::RenderCommand>& commands,
                       const std::vector<const vine::graphics::Light*>& lights,
                       const std::optional<vine::graphics::Viewport>& viewport);

/** @brief Places one slot view in a target's graph at its pass' explicit pipeline order.
 *
 * Within a target the slot views are stacked in ASCENDING pass order — the order the caller
 * gave addPass(), which is also the order the engine runs the passes in — so a pass positioned
 * by the host at any order draws exactly there, and a pass that (re)starts recording under an
 * order it was not built with is moved rather than drawn at the wrong depth in the stack.
 *
 * @param state  Session whose command graph holds the target's graphs.
 * @param graph  Graph whose child order is asserted.
 * @param target Target the graph belongs to (nullptr = the window session).
 * @param view   Slot view to move (null = no-op).
 * @param order  The pass' explicit pipeline order (stacking position).
 */
void placeViewByOrder(VsgRendererState& state, ::vsg::ref_ptr<::vsg::RenderGraph> graph,
                      vine::graphics::RenderTarget* target, const ::vsg::ref_ptr<::vsg::View>& view, int order);

/** @brief Whether a slot's announced lights have to be reported as (partly) unusable.
 *
 * A slot's lights come from its pass' content scene every frame, and a light its light block cannot
 * carry (disabled, neither ambient nor directional, a second ambient, or the fourth directional) is
 * not lit while the slot keeps drawing. That must be said: a list whose EVERY entry is unusable leaves
 * the slot drawing under the block's ambient fill, while a partly usable list lights the rest and drops
 * the others — the normal way to hit this, and the case that used to be silent.
 *
 * The EPISODE is "at least one announced light is not lit". The report fires on the frame the
 * episode starts and re-arms only once every announced light was represented in the block again (or
 * nothing was announced), so a slot whose scene keeps an unusable light year after year — the list is
 * rebuilt per frame — says so once instead of every frame. The caller owns @p reported (it
 * lives on the slot) and builds the message from the counts it already has.
 *
 * @param announced Lights the pass announced this frame (its scene's list size).
 * @param attached  Lights @ref fillVineLightsBlock reported as represented in the block.
 * @param reported  Per-slot episode flag (true while the current episode was reported).
 * @return true on the ONE frame the caller has to report.
 */
[[nodiscard]] bool beginLightsDroppedEpisode(std::size_t announced, std::size_t attached, bool& reported);

} // namespace detail

V_VSG_NS_END
