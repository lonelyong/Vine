#pragma once

/**
 * @brief Turning a decided pass into GPU objects: the pass materialisation layer.
 *
 * A pass bakes ONE pair of attachment load-ops, so what a pass records is a decision (which
 * variant) followed by materialisation (render pass + framebuffer + render graph), and the
 * decision's inputs live on the TARGET (its attachments, its pass table, its flags) while a
 * few come from the SESSION (the device, the open pass scope, the passes that already ran
 * this frame).
 *
 * These functions take exactly those inputs instead of being methods on the session state:
 * the state is then data a frame is driven through (VsgRendererState.hpp), not the owner of
 * every operation that touches it (§48 in .ai/design/vsg-pass-lifecycle.md).
 *
 * The policy the load-op decision follows — a pass' OWN clear request decides its load-ops,
 * plus the one-frame bootstrap a brand-new target needs — is documented on planPass().
 */


#include <vine/vsg/vsg_global.hpp>

#include <map>
#include <set>
#include <utility>

#include <vsg/app/RenderGraph.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/maths/vec4.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgFramePlan.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/** @brief Decides what one pass records this frame — device-free, no side effects.
 *
 * The whole per-pass decision: the attachment set, the load-op variant, the pass' own
 * clear requests, and whether its depth image still carries the layout a PROMOTING pass
 * left behind.
 *
 * THE LOAD-OP POLICY. A pass' OWN clear request (the open pass scope) decides its
 * load-ops — nothing else. A pass that never asked for a clear must LOAD what an earlier
 * pass left, or the stacked-pass pipelines break: the engine's deferred +
 * forward-composite pipeline stacks fullscreen lighting and the forward transparent
 * content on ONE off-screen target whose passes all set clearEnabled=false, so "clear it
 * anyway" wipes the lit result the next pass was meant to composite over. A pass that DID
 * ask for a clear clears, whatever its siblings asked for. The one exception is the
 * bootstrap: a target whose colour image has never been defined holds an UNDEFINED image,
 * and a render pass may not LOAD an UNDEFINED image, so the first pass into a NEW target
 * clears it ONCE (planPassVariant()'s transient bootstrap variant, swapped for the steady
 * one at the end of the frame like the depth seed) — otherwise the bootstrap would turn
 * into "this pass clears colour for ever", wiping what an earlier pass of the target drew
 * every frame.
 *
 * PROMOTION STATE. A pass that LOADs depth has to name the layout its image really is in,
 * and promotion is the only way the depth ends anywhere but the attachment layout. It is
 * in force while no pass of this target LOADs depth (@ref VsgRenderTargetEntry::depth_sampleable — the
 * cascade in passGraph revokes it) and the image is defined (@ref VsgRenderTargetEntry::depth_seeded);
 * it can then only have been replaced by a pass that RAN EARLIER in this frame. Passes
 * announce themselves in passes_active_this_frame as they render and record in their
 * explicit order, so a pass of this target that is announced AND ordered before this one
 * has already run (see depthStillPromoted).
 *
 * The variant itself is the device-free planPassVariant(), and the steady-frame guard is
 * passVariantIsStale(), which compares the pass' REQUESTS — never the materialised
 * load-ops: those carry the bootstrap too, and the very same frame builds one pass twice
 * (setupContentSlot() and render()), so a materialised comparison would rebuild the second
 * build into a LOAD against images nothing has defined yet.
 *
     * @param state Session the pass is materialised against: its window (the device), its target
     *              table (borrow-source lookup), its open pass scope (clear policy) and its retire
     *              ring (replaced objects are parked).
 * @param t      Target entry the pass belongs to.
 * @param key    Slot key of the pass.
 * @param target The render target itself (its description names the formats).
 * @return The plan; @ref detail::PassPlan::current is null for a pass that has not recorded yet.
 */
[[nodiscard]] PassPlan planPass(const VsgRendererState& state, const VsgRenderTargetEntry& t, const SlotKey& key,
                                const vine::graphics::RenderTarget& target);

/** @brief Describes the attachment set passes into @p target will attach.
 *
     * @param state Session the pass is materialised against: its window (the device), its target
     *              table (borrow-source lookup), its open pass scope (clear policy) and its retire
     *              ring (replaced objects are parked).
 * @param t      Target entry the pass builds objects for.
 * @param target Render target whose description names the formats.
 * @return The description; @c device is null before the window session exists.
 */
[[nodiscard]] PassAttachments passAttachments(const VsgRendererState& state, const VsgRenderTargetEntry& t,
                                              const vine::graphics::RenderTarget& target);

/** @brief Creates the render pass + framebuffer for ONE load-op combination.
 *
 * A render pass bakes one pair of attachment load-ops, so a pass that clears and
 * a pass that preserves cannot share one: this is where a variant becomes
 * objects. The pair is returned together because the framebuffer names the
 * attachments the render pass declares — they are replaced together or not at
 * all.
 *
     * @param state Session the pass is materialised against: its window (the device), its target
     *              table (borrow-source lookup), its open pass scope (clear policy) and its retire
     *              ring (replaced objects are parked).
 * @param t                Target entry the pass belongs to.
 * @param att              Its attachment description (see passAttachments).
 * @param pass_color_clear Whether this pass clears (rather than loads) colour.
 * @param depth_load       Whether the depth attachment is loaded, not cleared.
 * @param promote          Whether the pass may leave the depth sampleable.
 * @param depth_initial    Layout the depth image is in when this pass starts. A
 *                         CLEAR pass starts from UNDEFINED whatever its image
 *                         held, so this only matters for a depth-LOAD pass,
 *                         which must name the layout its image really is in.
 * @return The render pass and its framebuffer.
 */
[[nodiscard]] std::pair<::vsg::ref_ptr<::vsg::RenderPass>, ::vsg::ref_ptr<::vsg::Framebuffer>>
makePassObjects(const VsgRendererState& state, const VsgRenderTargetEntry& t, const PassAttachments& att,
                bool pass_color_clear, bool depth_load, bool promote, VkImageLayout depth_initial);

/** @brief Creates the render graph of a NEW pass, with its clear values.
 *
 * One graph per pass (§28): a pass owns the load-ops of its own scope and therefore
 * cannot record into its target's graph. Everything the graph needs is derived from
 * the target's built attachments.
 *
 * The clear values follow the ATTACHMENT ORDER the framebuffer was built with —
 * colour attachments in order, then depth — as VkRenderPassBeginInfo requires:
 * attachment 0 carries THIS pass' clear colour, the extra MRT attachments stay
 * transparent black (their regions stay black until a fragment writes them), and the
 * depth entry carries the target's depth clear value, the reverse-Z far plane for
 * every target (see buildOffscreenTarget).
 *
 * @param t           Target entry whose attachments the graph renders into.
 * @param has_depth   Whether the framebuffer has a depth attachment (its clear value
 *                    is appended last).
 * @param clear_color Colour attachment 0 clears to (the pass' own request).
 * @return The graph, with a null render pass / framebuffer to be set by the caller.
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> makePassGraph(const VsgRenderTargetEntry& t, bool has_depth,
                                                              const ::vsg::vec4& clear_color);

/** @brief Reuses a pass' recorded variant when its clear policy did not change.
 *
 * The whole steady-frame cost of a pass: its objects already encode the load-ops
 * its requests ask for, so only the clear VALUE can have changed — two map lookups,
 * no device call, no allocation. A pass that re-requests a clear updates ITS OWN
 * graph and never its siblings': a pass clears to its own request (§28), so one
 * pass' clear must not become another pass' background colour.
 *
 * The caller has already moved the pass to its record position when its explicit
 * pipeline order changed (VsgRenderer::reconcileOffscreenOrder).
 *
     * @param state Session the pass is materialised against: its window (the device), its target
     *              table (borrow-source lookup), its open pass scope (clear policy) and its retire
     *              ring (replaced objects are parked).
 * @param objects           The pass' recorded objects (its variant).
 * @param want_color_clear  Whether the pass asks to clear colour this frame.
 * @param want_depth_clear  Whether the pass asks to clear depth this frame.
 * @param has_color         Whether the target has a colour attachment (clear value 0
 *                          is the COLOUR entry only then: a depth-only target's single
 *                          entry is its DEPTH value, and VkClearValue is a union, so
 *                          writing .color there would clear the depth to a colour's
 *                          bit pattern).
 * @param clear_color       The colour the pass clears to (its own request).
 * @return The pass' graph, or null when the pass changed its clear policy and its
 *         variant has to be rebuilt.
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> reuseSteadyPass(const VsgRendererState& state,
                                                              VsgRenderTargetEntry::PassObjects& objects,
                                                              bool want_color_clear, bool want_depth_clear,
                                                              bool has_color, const ::vsg::vec4& clear_color);

/** @brief Records a (re)built pass and what it establishes for its target.
 *
 * The pass now exists and will record: its objects are stored under its slot key, the
 * target learns that its attachments have been written (a later pass may LOAD them) and
 * a pass that LOADs depth withdraws the target's depth promotion, because from here on
 * the image ends in the attachment layout (readDepthBuffer and a later borrow
 * validation are told by the same flag).
 *
 * Adding the pass' graph to the command graph and ordering it is the caller's job
 * (VsgRenderer::passGraph): only VsgRenderer owns the command graph.
 *
 * @param t         Target entry that owns the pass.
 * @param key       Slot key of the pass.
 * @param objects   The pass' materialised objects.
 * @param has_color Whether the target has a colour attachment.
 */
void publishPass(VsgRenderTargetEntry& t, const SlotKey& key, const VsgRenderTargetEntry::PassObjects& objects,
                 bool has_color);

/** @brief Whether the target's depth still carries the layout a promoting pass left.
 *
 * A pass that LOADs depth has to name the layout its image really is in, and
 * promotion is the only way the depth ends anywhere but the attachment layout.
 * Promotion is in force while no pass of the target LOADs depth
 * (@ref VsgRenderTargetEntry::depth_sampleable — revokeDepthPromotion() withdraws it) and the
 * image is defined (@ref VsgRenderTargetEntry::depth_seeded); it can then only have been
 * replaced by a pass that RAN EARLIER in this frame — passes announce
 * themselves in passes_active_this_frame as they render and record in their
 * explicit order, so a pass of the target that is announced AND ordered before
 * this one has already run.
 *
     * @param state Session the pass is materialised against: its window (the device), its target
     *              table (borrow-source lookup), its open pass scope (clear policy) and its retire
     *              ring (replaced objects are parked).
 * @param t       Target entry the pass belongs to.
 * @param current The pass being built (skipped; null when it has no objects yet).
 * @param order   This pass' explicit record order.
 * @return true when the depth is still promoted, so a LOAD must name that layout.
 */
[[nodiscard]] bool depthStillPromoted(const VsgRendererState& state, const VsgRenderTargetEntry& t,
                                      const VsgRenderTargetEntry::PassObjects* current, int order);

/** @brief Re-creates every pass of @p t WITHOUT depth promotion.
 *
 * A pass that LOADs depth must find the image in a layout it named, so no pass
 * of the target may promote it any more: passes that were allowed to promote
 * (the first pass cleared depth and nothing loaded it) are rebuilt without it
 * and the target's @ref VsgRenderTargetEntry::depth_sampleable is withdrawn. A depth-only
 * target is exempt — promotion is not a choice there (its depth always ends
 * sampleable), so nothing has to be revoked.
 *
 * The pass being (re)built is skipped: its own variant is decided by the caller,
 * and rebuilding it here would be thrown away — and would hide the layout its
 * image is in (a pass that STOPS promoting is exactly the one whose depth may
 * still carry the promoted layout).
 *
     * @param state Session the pass is materialised against: its window (the device), its target
     *              table (borrow-source lookup), its open pass scope (clear policy) and its retire
     *              ring (replaced objects are parked).
 * @param t                    Target entry being revoked.
 * @param current              The pass being built, skipped (may be null).
 * @param steady_depth_initial Layout a depth-LOAD pass of this target expects.
 */
void revokeDepthPromotion(VsgRendererState& state, VsgRenderTargetEntry& t,
                          const VsgRenderTargetEntry::PassObjects* current, VkImageLayout steady_depth_initial);

/** @brief Gets the render graph one pass records into, creating it on first use.
 *
 * One graph per pass (§28): a pass owns the load-ops of its own scope, so it cannot record
 * into its target's graph. The graph is a direct child of the command graph and records in
 * the passes' explicit pipeline order (setPassOrder) — the position the pass' content would
 * have occupied as a View of a single target-wide render pass (see VsgRecordOrder.hpp). A
 * target without attachments yet has no graph (null), and the window target has ONE shared
 * swapchain graph instead of one per pass.
 *
 * @param state       Session whose command graph and target table are used.
 * @param diagnostics Route a revoked promotion is reported on.
 * @param target      Off-screen target, or nullptr for the window session.
 * @param key    Slot key of the pass that owns the graph.
 * @return The pass' graph, or null when the target has no attachments yet.
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> passGraph(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                                                          vine::graphics::RenderTarget* target,
                                                          const SlotKey& key);

/** @brief Drops the fullscreen-program slots that BIND @p target's depth.
 *
 * Its promotion was just revoked (see passGraph), and a slot built while the promotion
 * stood names the promoted layout in its descriptor set, so it cannot go on sampling it: the
 * slot is dropped and its owner's next drawScreenProgram() call rebuilds it against the new
 * policy.
 *
 * @param state       Session whose target table holds the slots.
 * @param diagnostics Route a slot that cannot be dropped is reported on.
 * @param target      Target whose depth is no longer sampleable.
 */
void dropDepthSamplingProgramSlots(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                                   vine::graphics::RenderTarget* target);

} // namespace detail

V_VSG_NS_END
