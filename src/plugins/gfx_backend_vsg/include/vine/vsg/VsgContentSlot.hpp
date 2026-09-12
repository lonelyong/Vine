#pragma once

/**
 * @brief One content slot: the retained View a pass draws its content into.
 *
 * A content slot is the unit a pass' content lives in: its own View under the TARGET's render
 * graph (the window's swapchain graph, or the pass' own off-screen graph — §28), its own
 * SceneBridge (vsg compiles pipelines per viewID, so two slots never share compiled state),
 * its own camera / light group and its stacking position (the pass' explicit pipeline order).
 *
 * The slot is keyed by the pass that OWNS it (@ref SlotKey), so a pass' camera or target may
 * change without orphaning its retained content, and two passes never alias. A direct driver
 * that skips the pass protocol falls back to the historical (camera, order) identity.
 *
 * @ref VsgContentSlotRequest carries everything one draw call announces — it is what
 * `VsgRenderer::render` fills from the pass scope and what the slot code consumes — and it is
 * namespace-scope on purpose: the request is read by helpers too, which could not name it
 * while it was a private nested type (§37 had to pass eight loose fields around because of
 * that).
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

/// Everything one content-slot draw announces (see renderContentSlot).
struct VsgContentSlotRequest
{
    vine::graphics::RenderTarget*                  target   = nullptr; ///< Target key (nullptr = window).
    vine::raw_ptr<const vine::graphics::Camera>    camera   = nullptr; ///< Camera identifying the slot.
    const std::vector<vine::graphics::RenderCommand>* commands = nullptr; ///< Commands to reconcile (borrowed).
    const std::vector<const vine::graphics::Light*>* lights  = nullptr; ///< Content lights (borrowed, may be empty).
    vine::graphics::DepthMode                      depth_mode = vine::graphics::DepthMode::TestAndWrite; ///< The pass' depth policy.
    bool                                           presenting = false; ///< True for the full-target pass that cleared.
    bool                                           clear_depth = false; ///< The pass' own depth-clear request (a mixed target clears it per pass).
    int                                            order      = 0;     ///< The pass' explicit pipeline order (stacking).
    std::optional<vine::graphics::Viewport>        viewport;  ///< Sub-viewport, or nullopt for the full target.
};

namespace detail
{

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
 * @param request     What the draw call announced (target, camera, depth policy, order, role).
 */
void setupContentSlot(VsgRendererState& state, VsgRendererPersistent& persistent, const VsgDiagnostics& diagnostics,
                      const SlotKey& key, const VsgContentSlotRequest& request);

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
 * @param request     Draw request (see VsgContentSlotRequest).
 */
void renderContentSlot(VsgRendererState& state, VsgRendererPersistent& persistent, const VsgDiagnostics& diagnostics,
                       const VsgContentSlotRequest& request);

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
 * A slot's lights come from its pass' content scene every frame, and a light the backend cannot
 * map (disabled, or a kind with no vsg translation) is dropped while the slot keeps drawing.
 * That must be said: an announced list whose EVERY entry is unusable leaves the slot on its
 * seeded default light (and the whole view would shade to black if the fallback did not keep
 * it), while a partly usable list lights the rest and drops the others — the normal way to hit
 * this, and the case that used to be silent.
 *
 * The EPISODE is "at least one announced light is not lit". The report fires on the frame the
 * episode starts and re-arms only once every announced light was attached again (or nothing was
 * announced), so a slot whose scene keeps an unusable light year after year — the list is
 * rebuilt per frame — says so once instead of every frame. The caller owns @p reported (it
 * lives on the slot) and builds the message from the counts it already has.
 *
 * @param announced Lights the pass announced this frame (its scene's list size).
 * @param attached  Lights @ref setGroupLights actually put into the slot's light group.
 * @param reported  Per-slot episode flag (true while the current episode was reported).
 * @return true on the ONE frame the caller has to report.
 */
[[nodiscard]] bool beginLightsDroppedEpisode(std::size_t announced, std::size_t attached, bool& reported);

} // namespace detail

V_VSG_NS_END
