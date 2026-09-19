#pragma once

/**
 * @brief Assembling and taking apart a render target: its attachments, its pass graphs and the
 * slots that draw into it.
 *
 * A target's GPU attachment set is built once and shared by every pass that records into it (the
 * render pass and the framebuffer are per PASS and belong to VsgPassMaterialiser), so the whole
 * lifecycle of one entry in the session's target table belongs together: the build (the borrow
 * validation, the attachments, the borrowed-depth barrier), the rebuild (a target that was resized
 * or whose attachment shape changed), the teardown (a released target, a released window layer) and
 * the two per-frame decisions the frame makes about an entry (does the borrow force a rebuild;
 * which consumers sampled a rebuilt target).
 *
 * Three rules run through all of it. What a build OWNS is exactly what a rebuild must forget, which
 * is why the forgetting half (clearTargetAttachments) is written as one list — and why it is ONE
 * call: the unhook before it is what makes the release safe, and that order used to be a rule of its
 * own. Every path that stops a slot drawing goes through detachSlotView. And every path that drops a
 * full-screen program slot goes through eraseProgramSlot, which parks the node on the way out.
 *
 * The destructive paths keep the COUNTED device wait (VsgRetireRing::waitForIdle) rather than
 * parking their objects; the notes on unhookTargetPasses and erasePassFromTarget record why
 * (clearCache() releases the shared object registry, whose pipelines / samplers the retained nodes
 * are not necessarily the only owner of). Dropping a PROGRAM slot is the exception that proves the
 * rule: it parks, because nothing else holds its node's pipeline / descriptor sets (see
 * eraseProgramSlot).
 *
 * The compile contexts of the slots a teardown drops are NOT released here: a registration is owned by the
 * slot that made it (detail::VsgCompileRegistration), whose destruction is what releases it -- so these
 * teardowns owe the compile manager nothing at all.
 */


#include <vine/vsg/vsg_global.hpp>

#include <cstdint>

#include <vsg/app/View.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/vk/Device.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRenderTargetEntry.hpp>
#include <vine/vsg/VsgRendererState.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/** @brief Builds (or rebuilds) an off-screen target's GPU attachments and
 * empty render graph, sized to the target.
 *
 * Called from render() when an off-screen target is first rendered or was
 * resized. The graph is created without Views; content-slot Views are
 * appended by setupContentSlot() as passes render into the target (C6.4:
 * the same multi-slot mechanism the window target uses, so one RT can
 * bake several content groups with different programs / depth policy). A
 * rebuild first releases the previous graph and every content slot
 * compiled against it.
 *
 * @param target Off-screen target to (re)build.
 */
void buildOffscreenTarget(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                          vine::graphics::RenderTarget* target);

/**
 * @brief Brings a target's attachments in line with its description: build when its SHAPE changed,
 * resize in place when only its SIZE did.
 *
 * THE decision about an off-screen target's attachments, in one place, because it is asked from two —
 * the pass path (VsgRenderer::render, for the target a pass draws into) and the fullscreen-program path
 * (resolveProgramSlotDestination, for the target a program draws INTO) — and the two used to answer it
 * with the same "rebuild everything" code copied out. Which path applies is a fact about what moved:
 *
 *  - the target's shape (attachment count / formats / depth policy), or the borrow itself: EVERYTHING
 *    anchored on it is rebuilt (buildOffscreenTarget);
 *  - its size, or the image its borrowed depth points at: the passes, slots and pipelines stay and only
 *    the attachments, framebuffers and descriptor bindings are replaced (resizeOffscreenTarget).
 *
 * @param state       Session that owns the target's entry.
 * @param diagnostics Route a refused build / unusable size is reported on.
 * @param target      Off-screen target to sync (null is the window target: nothing to do).
 * @return true when the target has usable attachments afterwards.
 */
[[nodiscard]] bool syncOffscreenTarget(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                                       vine::graphics::RenderTarget* target);

/**
 * @brief Resizes an off-screen target's attachments IN PLACE: same passes, same slots, same pipelines —
 * new images.
 *
 * The sibling of buildOffscreenTarget, and the difference is the whole point (§0 of
 * .ai/design/vsg-target-resize-in-place.md): a build answers "this target's SHAPE changed" (attachment
 * count / formats / depth policy) and therefore tears everything anchored on it down (passes, graphs,
 * slots, per-size shader sets); a resize answers "the same target, at a new size" and keeps ALL of that,
 * because none of it is size-bound:
 *
 *  - a render pass declares attachment FORMATS and sample counts, never a size ⇒ the passes stay, and
 *    they stay compatible with the pipelines compiled against them (VUID-vkCmdDraw-renderPass-02684);
 *  - a pipeline is cached on the NODE object, per viewID (GraphicsPipeline::compile) ⇒ keeping the
 *    slots' views and nodes is what keeps their compiled pipelines;
 *  - a framebuffer DOES name the attachments ⇒ each pass' framebuffer is replaced (by its own passGraph
 *    call on the next draw, from the target's new attachments);
 *  - a descriptor set DOES name the sampled image views ⇒ what samples this target re-points (the
 *    program slots' own generation check, see .ai/design/vsg-target-resize-in-place.md §3.3);
 *  - the new images are UNDEFINED ⇒ the first pass of the target seeds them again for one frame, which
 *    is why every pass' recorded variant is sent back through the plan (attachments_generation).
 *
 * The replaced images / views / barrier are PARKED, never destroyed in place, and this path takes no
 * device wait: a submitted command buffer may still name them, and parking is what this backend does
 * instead of stopping the device (see VsgRetireRing) — that is also what makes a resize cheap enough
 * to run on every window resize.
 *
 * @param state       Session that owns the target's entry and its retire ring.
 * @param diagnostics Route an unusable size is reported on.
 * @param target      Off-screen target to resize (null, unbuilt or unchanged is a no-op).
 */
void resizeOffscreenTarget(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                           vine::graphics::RenderTarget* target);

/** @brief Whether a target's BORROWED depth is no longer the source's current image.
 *
 * The third thing that can go stale about a target's attachments, next to its own size and its shape: a
 * target that borrows another's depth (RenderTarget::shareDepth) has that image baked into its
 * framebuffers, so the source replacing it (a resize of the source) leaves the borrower recording
 * against an image nothing writes any more. The borrower is then resized in place for the same reason
 * the source was — its own attachments have to be remade against the new image.
 *
 * Deliberately narrow: only the case where the borrow itself is unchanged and only the painted image
 * moved. A borrow that is still waiting for its source, or one that has become unusable, is a change of
 * the target's ATTACHMENT SHAPE and is left to buildOffscreenTarget (see borrowNeedsRebuild).
 *
 * @param state      Session whose target table holds both entries.
 * @param t          Target entry whose borrow is being judged.
 * @param target_key The target's own description (null for the window, which never borrows).
 * @return true when the borrow is in force but points at an image the source no longer has.
 */
[[nodiscard]] bool borrowPointsAtAnotherImage(const VsgRendererState& state, const VsgRenderTargetEntry& t,
                                              const vine::graphics::RenderTarget* target_key);

/** @brief Decides whether a target's shared depth can be borrowed.
 *
 * RenderTarget::shareDepth points a target at another target's depth image so
 * forward content can test against a G-buffer's depth. That is only possible
 * when the source's image is usable as THIS framebuffer's depth attachment as
 * it stands; every other case is reported (once per episode) and the target
 * builds its own depth instead of attaching an unusable image.
 *
 * @param target Target requesting the borrow.
 * @param w      Width the framebuffer is being built with.
 * @param h      Height the framebuffer is being built with.
 * @return true when the target must attach its source's depth image.
 */
bool resolveDepthBorrow(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                        vine::graphics::RenderTarget& target, uint32_t w, uint32_t h);

/** @brief Creates a target's GPU attachments: one colour image + view per attachment, plus
 * its depth (owned or borrowed from an earlier target this frame).
 *
 * The USAGE flags are the reason this lives in one place — they are what makes a colour
 * attachment also usable as a sampled texture (PiP / fullscreen-program sources) or as a
 * blit source (readColorBuffer), and what lets a depth image be copied out
 * (readDepthBuffer): without TRANSFER_SRC the depth cannot even be transitioned to
 * TRANSFER_SRC_OPTIMAL (VUID-VkImageMemoryBarrier-oldLayout-01212).
 *
 * Every view is created through createImageView(), which compiles the Image (creates the
 * VkImage and allocates / binds its memory) AND the ImageView: without it both handles stay
 * VK_NULL_HANDLE and a framebuffer built from them holds corrupt handles, which only shows
 * up as a crash in vkCmdBeginRenderPass.
 *
 * A BORROWED depth (see RenderTarget::shareDepth) attaches the SOURCE's image, so what this
 * target records is which source VIEW its framebuffer was baked with — the source replacing
 * that image invalidates the framebuffer and render() rebuilds this target by comparing the
 * two.
 *
 * @param t        Target entry whose images / views are set (it is being built).
 * @param device   Device that compiles the images and creates the views.
 * @param target   Render target description (attachment count / formats / depth).
 * @param w        Width to create the images with.
 * @param h        Height to create the images with.
 * @param depth_src Source whose depth is borrowed, or null to create this target's own.
 */
void createTargetAttachments(VsgRendererState& state, VsgRenderTargetEntry& t, ::vsg::Device* device,
                             const vine::graphics::RenderTarget& target, uint32_t w, uint32_t h,
                             vine::graphics::RenderTarget* depth_src);

/** @brief Creates the barrier that orders a borrowed depth image's writes before
 * the borrower's pass reads / tests it.
 *
 * A depth borrow (RenderTarget::shareDepth) makes two targets share ONE depth
 * image in the attachment layout. The image is written by the source's passes and
 * then LOADed by the borrower's, so the write must be made visible before the
 * read: reconcileOffscreenOrder() inserts this barrier right after the source's
 * last pass graph (VsgRenderTargetEntry::depth_share_barrier).
 *
 * The subresource range covers both aspects when the source's depth format is a
 * COMBINED depth/stencil one (D24 / D32S8 / D16S8): with separateDepthStencilLayouts
 * disabled a barrier may not name only one aspect of such a format
 * (VUID-VkImageMemoryBarrier-image-03320).
 *
 * @param source Target whose depth image is borrowed.
 * @return The barrier, or null when @p source has no depth image to share.
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::PipelineBarrier> makeDepthShareBarrier(const VsgRendererState& state,
                                                                          vine::graphics::RenderTarget* source);

/** @brief What a target's depth borrow would do RIGHT NOW (see borrowVerdict).
 *
 * The values that matter to a caller are "honoured" and "not": the refused ones are separated because
 * the two families are handled differently — a source that is gone or not ready yet is retried, while a
 * size mismatch or a sampled depth is a property of the setup that is remembered and reported once
 * (see resolveDepthBorrow).
 */
enum class BorrowVerdict
{
    None,              ///< No borrow was asked for (RenderTarget::depthSource is null).
    Honoured,          ///< The source's depth can be attached: the borrow is (or becomes) in force.
    SourceGone,        ///< The source is not in the session's table: it was released, or never rendered.
    SourceNotReady,    ///< The source has no depth image YET (its pass has not built this frame).
    SourceSizeMismatch, ///< Different extents: a framebuffer attachment must have the framebuffer's dimensions.
    SourceSampled,     ///< The source promoted its depth to a sampled texture: a sampled depth cannot be attached.
};

/** @brief The ONE rule for whether a depth field can be shared, asked by both the build and its predicate.
 *
 * A depth borrow is decided twice per session: once by the BUILD (resolveDepthBorrow, which attaches the
 * source's image) and once by the REBUILD PREDICATE (borrowNeedsRebuild, which has to notice that the
 * answer changed without a build ever happening). Written twice, the two drift — and the drift is
 * invisible: the borrower keeps a framebuffer attached to an image the source no longer writes, or it
 * rebuilds every frame for a borrow that is perfectly fine. So the checks live here, and both callers
 * ask this function.
 *
 * @param state      Session that owns the target table.
 * @param source     Target whose depth would be borrowed (the caller checks this for null).
 * @param width      Width the borrower would be built at.
 * @param height     Height the borrower would be built at.
 * @return What the borrow would do now, and (when it is refused) why.
 */
[[nodiscard]] BorrowVerdict borrowVerdict(const VsgRendererState& state, vine::graphics::RenderTarget* source,
                                          int width, int height);

/** @brief Whether a target's recorded attachments have to be (re)built because of its
 * DEPTH BORROW.
 *
 * The borrower's framebuffer bakes the source's depth: a borrow that is no longer the one a build would
 * take has to be rebuilt, and the only question is whether the borrow's DECISION changed or only the
 * image it names. Two cases, answered together because they mean the same thing to this caller:
 *
 *  - the decision would change (borrowVerdict): a disabled producer that produced its depth, a source
 *    that started promoting its depth to a sampled texture or changed size, or a source that was
 *    released — each of those is a different attachment set, and one of them (the promotion) makes the
 *    borrowed image one no render pass may attach at all;
 *  - the borrow itself moved (RenderTarget::shareDepth now names a different source, or none), which is
 *    a different barrier and a different image.
 *
 * A source that merely REPLACED its image while the decision stayed the same (a resize in place) is
 * deliberately NOT a rebuild: the borrow is unchanged, so the borrower is resized in place as well and
 * its framebuffers are remade against the source's new image (see borrowPointsAtAnotherImage).
 *
 * @param t          Target entry to inspect.
 * @param target_key The target itself (nullptr = the window, which never borrows).
 * @return true when the caller has to rebuild the target's attachments.
 */
[[nodiscard]] bool borrowNeedsRebuild(const VsgRendererState& state, const VsgRenderTargetEntry& t,
                                      const vine::graphics::RenderTarget* target_key);

/** @brief Whether an honoured borrow names an image the source has since replaced (a resize in place).
 *
 * The other half of the borrow rule: the decision (borrowVerdict) is still "honoured" and the source is
 * still the one asked for, so nothing has to be rebuilt — but the framebuffer currently attaches the
 * source's PREVIOUS depth view, and the rendered image no longer writes to it. The caller replaces the
 * borrower's attachments against the source's current image (resizeOffscreenTarget), which keeps its
 * passes, its slots and their compiled pipelines.
 *
 * @param state      Session that owns the target table.
 * @param t          Target entry to inspect.
 * @param target_key The target itself (nullptr = the window, which never borrows).
 * @return true when the borrower has to be resized in place.
 */
[[nodiscard]] bool borrowPointsAtAnotherImage(const VsgRendererState& state, const VsgRenderTargetEntry& t,
                                             const vine::graphics::RenderTarget* target_key);

/** @brief Forgets a target's attachments and everything that hangs off them — ONE call, ONE order.
 *
 * A rebuild has to do two things to what the previous build produced, and the order between them is
 * what makes the release safe: stop the old pass graphs being recorded and release what hangs off
 * them with the COUNTED device wait (the bridges' caches release the shared object registry, whose
 * pipelines / samplers the retained nodes are not necessarily the only owner of), and only then
 * forget the images / views / slots / flags. As two public functions with that requirement written
 * in prose, calling the second without the first destroyed every slot's bridge — and therefore its
 * pipelines — without the wait, which is the "destroy while a command buffer may still name it"
 * failure the whole retire-ring policy exists to prevent. One entry point means the order is not
 * something a caller can get wrong: it is the body of this function.
 *
 * A target that was never built has nothing to unhook (no pass graph was ever created for it, and
 * no slot was ever created either — a slot needs the target's attachments), so this is a no-op
 * there.
 *
 * Written as ONE list for the forgetting half: it is exactly what a build OWNS, and a Target field
 * added later and forgotten there would survive a rebuild as a stale image, a stale "already built"
 * flag or a stale borrow source, with nothing to report it.
 *
 * @param state Session whose command graph / retire ring / compile queue the unhook touches.
 * @param t     Target entry being emptied (its key stays registered).
 */
void clearTargetAttachments(VsgRendererState& state, VsgRenderTargetEntry& t);

/** @brief Stops a target's passes being recorded and makes their release safe.
 *
 * The destructive unhook two paths need: a target about to be rebuilt (see
 * @ref clearTargetAttachments, which is the one a rebuild should call) and a target about to be
 * released (releaseRenderTarget). Each pass graph is removed from the command graph, the
 * device is waited on, and every content slot's bridge cache is dropped together
 * with its queued compile view.
 *
 * The wait is REQUIRED here and is the counted one (VsgRetireRing::waitForIdle), not a
 * park: clearCache() releases the bridge's shared object registry, whose
 * pipelines / samplers the retained nodes do not necessarily keep alive as the
 * only owner — parking the views instead was measured to trip
 * vkDestroyPipeline-00765 / vkDestroySampler-01082 (see the policy-churn notes).
 * Non-destructive paths (a replaced render pass, a dropped program slot) park
 * their objects with VsgRetireRing::park instead.
 *
 * @param t Target entry whose passes stop being recorded.
 */
void unhookTargetPasses(VsgRendererState& state, VsgRenderTargetEntry& t);

/** @brief Detaches one slot's view from the graph it records into.
 *
 * The single place that knows how a slot stops being recorded: the view is
 * removed from the target's (or the pass') render graph and dropped from the
 * pending incremental-compile queue. Every path that retires, moves or erases
 * a slot goes through it — a slot whose view is left attached keeps drawing.
 *
 * @param owner     Target entry that holds the slot.
 * @param owner_key Key @p owner is registered under (nullptr = window).
 * @param key       Slot key whose graph the view was attached to.
 * @param view      The slot's retained view (null is a no-op).
 */
void detachSlotView(VsgRendererState& state, VsgRenderTargetEntry& owner, vine::graphics::RenderTarget* owner_key,
                    const SlotKey& key, const ::vsg::ref_ptr<::vsg::View>& view);

/** @brief Drops @p view from the frame's incremental-compile queue, if it is queued.
 *
 * The other half of "this view is no longer recorded" (see detachSlotView): the queue's entry names the
 * slot to compile, so an entry left behind after its slot was dropped would be compiled against a
 * framebuffer the view no longer belongs to. The compiler re-checks that anyway (see
 * PendingCompileView), which makes this the cheap path rather than the only guard.
 *
 * @param view View to drop from the queue (null is a no-op).
 */
void dropQueuedCompileView(VsgRendererState& state, const ::vsg::ref_ptr<::vsg::View>& view);

/** @brief Drops every content slot's baked shader set so the next frame builds them again.
 *
 * A slot bakes its shader set at build time and the set carries more than one shading choice
 * with it: which program shades the slot, its attribute and descriptor ABI, and the pipeline
 * states baked at the target's size and depth policy. None of that
 * can be patched afterwards, so changing the default content program mid-session means the slots have to
 * be built again — this is that drop, and the existing lazy slot creation rebuilds them on the
 * next frame each pass renders.
 *
 * Only the SLOTS are dropped, never the target's attachments or pass graphs: the pixels the
 * host sees still come from the same framebuffer, so a switch does not reset a target's depth
 * or its history. Every dropped slot goes through detachSlotView (a view left attached to a
 * graph keeps drawing with its old set) and keeps the COUNTED device wait for the same reason
 * unhookTargetPasses does.
 *
 * @param state Session whose content slots stop being recorded.
 */
void resetContentShaderSlots(VsgRendererState& state);

/** @brief Whether a build attempt whose target has no size has to be reported now.
 *
 * An off-screen target with a zero width or height cannot be built: the passes drawing into
 * it draw nothing, and the host would otherwise see a pass that never appears with no reason
 * for it (the same shape D60 fixed on the readback side). The EPISODE is "this target has no
 * size": the report fires on the first build attempt that finds it, and a later attempt with
 * a usable size re-arms it — so a host that never sizes the target is told once, not every
 * frame, while one that sizes it, then loses the size, is told again.
 *
 * The caller owns @p reported (it lives on the target's entry) and builds the message from
 * the numbers it already has.
 *
 * @param width    Width the target reports for the build attempt.
 * @param height   Height the target reports for the build attempt.
 * @param reported Per-target episode flag (see @ref ReportOnce: who re-arms it is the caller's call).
 * @return true on the ONE attempt the caller has to report.
 */
[[nodiscard]] bool beginTargetSizeMissingEpisode(std::uint32_t width, std::uint32_t height, ReportOnce& reported);

/** @brief Erases everything one target retains for a pass: its slots AND its materialised objects.
 *
 * The one "this pass is gone from this target" path: the pass is removed
 * (releasePass) or it moved to another target (retargetPass). Its SLOTS (the content
 * view, the full-screen program slot) are detached and dropped, and the pass' OWN
 * materialised objects (its render pass + one-frame transient variant, its framebuffer and
 * its RenderGraph — see VsgRenderTargetEntry::PassObjects) go with them.
 *
 * Dropping the pass objects is what keeps retained state from growing with the number of
 * passes a session has ever seen (the interface states it must reach a steady state) and
 * what makes the raw RenderPass* key safe to reuse: the entry never outlives its pass. They
 * are PARKED, not waited on — an in-flight command buffer may still name them and nothing
 * else owns them (the same policy as a load-op rebuild, see VsgRetireRing) — and the
 * caller's reconcileOffscreenOrder() drops the graph from the record sequence.
 *
 * Only the SLOT teardown keeps the counted device wait (VsgRetireRing::waitForIdle):
 * clearCache() releases the bridge's shared object registry, whose pipelines / samplers the
 * retained nodes are not necessarily the only owner of.
 *
 * @param target Target key whose slots and pass objects to inspect (nullptr = the window).
 * @param pass   Pass whose retained state to drop.
 */
void erasePassFromTarget(VsgRendererState& state, vine::graphics::RenderTarget* target,
                         const vine::graphics::RenderPass* pass);

/** @brief Restricts a pass to @p target by dropping its slots elsewhere.
 *
 * A pass owns exactly one retained slot per target; when a pass renders
 * into a different target than before (its render target changed at run
 * time) the slot it left behind would otherwise keep drawing its content
 * forever. @p target may be nullptr (the window target), so callers pass
 * the target the pass is (re)drawing into.
 *
 * @param pass   Pass being re-targeted.
 * @param target Target the pass now renders into (nullptr = window).
 */
void retargetPass(VsgRendererState& state, const vine::graphics::RenderPass* pass, vine::graphics::RenderTarget* target);

/** @brief Drops one retained full-screen program slot, the ONE way such a slot goes.
 *
 * A program slot owns three things, and each of them used to be a step its drop site could
 * forget (two of the four sites did):
 *
 *  - its VIEW has to be detached from the graph it records into — a view left attached keeps
 *    drawing;
 *  - its NODE has to be PARKED on the retire ring, not destroyed: a SUBMITTED command buffer
 *    may still name the pipeline, the descriptor sets and (through them) the sampled image
 *    views the node holds, so destroying it at this moment is the "destroy while in flight"
 *    that the ring exists to prevent (measured earlier on the teardown paths as
 *    `vkDestroyPipeline-00765`);
 *  - the slot itself has to be erased, so its owner's next call builds a new one.
 *
 * Parking is also what makes the drop WAIT-FREE: the old node keeps its Vulkan objects (and the
 * image views it samples) alive until every slot that could have recorded it has been
 * re-recorded, so no path that drops a program slot has to stop the device first. The two paths
 * that used to wait for a device-wide idle per dropped slot (a released render target, a rebuilt
 * source) now pay nothing for it.
 *
 * @param state     Session whose retire ring parks the node.
 * @param owner     Target entry that holds the slot.
 * @param owner_key Key @p owner is registered under (nullptr = window).
 * @param key       Slot key of the program slot to drop (absent key is a no-op).
 */
void eraseProgramSlot(VsgRendererState& state, VsgRenderTargetEntry& owner,
                      vine::graphics::RenderTarget* owner_key, const SlotKey& key);

/** @brief Drops every slot that SAMPLES @p target, which was just (re)built.
 *
 * A rebuild creates FRESH colour views, and a consumer's stale check only watches the
 * source's SIZE — which a same-size rebuild does not change — so a full-screen program
 * slot built against the old views would go on sampling an image nothing draws into any
 * more. Dropping the slot makes its owner's next drawScreenProgram call reattach against the new
 * attachments.
 *
 * Consumers are found by inspecting the slot ATTRIBUTE (source_target), because a slot's
 * key is the pass that OWNS it, not the target it samples. They go through
 * @ref eraseProgramSlot like every other drop.
 *
 * @param target Target whose attachments were just rebuilt (the sampled source).
 */
void dropConsumersSampling(VsgRendererState& state, const vine::graphics::RenderTarget* target);

/** @brief Releases every GPU object a removed render target owns.
 *
 * The engine's releaseRenderTarget() entry point: the target's pass graphs stop being recorded, its
 * attachments and every slot that draws into it are dropped, and any target that BORROWED its depth
 * forgets the borrow so that it rebuilds with its own depth next frame.
 *
 * @param state       Session whose target table is swept.
 * @param diagnostics Route the borrow invalidation is reported on.
 * @param target      Target being removed (null is a no-op).
 */
void releaseRenderTarget(VsgRendererState& state, const VsgDiagnostics& diagnostics, vine::graphics::RenderTarget* target);

} // namespace detail

V_VSG_NS_END

