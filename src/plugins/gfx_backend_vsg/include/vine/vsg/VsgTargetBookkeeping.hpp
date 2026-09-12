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
 * Two rules run through all of it. What a build OWNS is exactly what a rebuild must forget, which
 * is why resetTargetAttachments is written as one list. And every path that stops a slot drawing
 * goes through detachSlotView: a slot whose view is still attached to a graph keeps drawing.
 *
 * The destructive paths keep the COUNTED device wait (VsgRetireRing::waitForIdle) rather than
 * parking their objects; the notes on unhookTargetPasses and erasePassSlotsFromTarget record why
 * (clearCache() releases the shared object registry, whose pipelines / samplers the retained nodes
 * are not necessarily the only owner of).
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
 * compiled against it. EXPERIMENTAL: needs on-device validation.
 *
 * @param target Off-screen target to (re)build.
 */
void buildOffscreenTarget(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                          vine::graphics::RenderTarget* target);

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

/** @brief Whether a target's recorded attachments have to be (re)built because of its
 * DEPTH BORROW.
 *
 * Two separate ways a borrowed depth outlives its usefulness, answered together because
 * they mean the same thing to the caller: the framebuffer recorded for this target no
 * longer matches the source it has to test against.
 *
 *  - PENDING: the requested borrow could not be honoured yet (the source had no depth
 *    image when this target was built), so the baked borrow differs from the requested one
 *    and is retried as soon as the source has an image. A source that is permanently
 *    unusable is remembered as such (@ref VsgRenderTargetEntry::unusable_depth_source), so this retries
 *    only while the borrow is merely WAITING — a disabled or never-built producer costs one
 *    map lookup per frame, not a rebuild loop.
 *  - STALE: an honoured borrow attaches the source's depth VIEW, and a source that is
 *    rebuilt (a size change, or the depth-policy change this same predicate watches for its
 *    own targets) replaces its depth image. The borrower's framebuffer would go on testing
 *    the replaced image, which nobody writes any more: the borrowed depth silently freezes
 *    while the old image stays alive. Comparing the source's current view against the one
 *    this target was baked with detects that, and the rebuild re-runs the borrow validation
 *    against the new image.
 *
 * @param t          Target entry to inspect.
 * @param target_key The target itself (nullptr = the window, which never borrows).
 * @return true when the caller has to rebuild the target's attachments.
 */
[[nodiscard]] bool borrowNeedsRebuild(const VsgRendererState& state, const VsgRenderTargetEntry& t,
                                      const vine::graphics::RenderTarget* target_key);

/** @brief Forgets everything a previous build of a target's attachments produced.
 *
 * The second half of a rebuild (the first is unhookTargetPasses, which stops the old
 * passes being recorded and makes the release safe): every image / view / slot table and
 * every flag the build set goes back to its initial value, while the target's own entry
 * stays. Written as ONE list because it is exactly what a build OWNS — a Target field
 * added later and forgotten here would survive a rebuild as a stale image, a stale
 * "already built" flag or a stale borrow source, and nothing would report it.
 *
 * @param t Target entry being emptied (its key stays registered).
 */
void resetTargetAttachments(VsgRenderTargetEntry& t);

/** @brief Stops a target's passes being recorded and makes their release safe.
 *
 * The destructive unhook both teardown paths need: a target about to be rebuilt
 * (buildOffscreenTarget) and a target about to be released
 * (releaseRenderTarget). Each pass graph is removed from the command graph, the
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

/** @brief Detaches and drops every retained slot a pass owns under one
 * target (its content view, PiP screen slot and fullscreen-program slot).
 *
 * Used when a pass' retained state must be discarded: the pass is removed
 * (releasePass), it was not active this frame (retireInactivePassSlots) or it
 * moved to another target (retargetPass). The owning graph's child list is
 * swept first, then the device is waited on so no in-flight command buffer
 * still references the dropped view / pipelines.
 *
 * @param target Target key whose slots to inspect (nullptr = the window).
 * @param pass   Pass whose slots to drop.
 */
void erasePassSlotsFromTarget(VsgRendererState& state, vine::graphics::RenderTarget* target,
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

/** @brief Drops every slot that SAMPLES @p target, which was just (re)built.
 *
 * A rebuild creates FRESH colour views, and a consumer's stale check only watches the
 * source's SIZE — which a same-size rebuild does not change — so a PiP / fullscreen-program
 * slot built against the old views would go on sampling an image nothing draws into any
 * more. Dropping the slot makes its owner's next drawScreenTexture / drawScreenProgram call
 * reattach against the new attachments.
 *
 * Consumers are found by inspecting the slot ATTRIBUTE (source_target), because a slot's
 * key is the pass that OWNS it, not the target it samples.
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

/** @brief Releases the window content slot a removed pass left under its legacy (camera, order) key.
 *
 * Only a direct driver that never opened a pass scope owns such a slot (see
 * RenderBackend::releaseWindowLayer), so this is a lookup in the window entry's content slots and a
 * no-op for an engine-driven pass, whose slot releasePass() handles.
 *
 * @param state  Session whose window entry holds the slot.
 * @param camera The removed pass' camera (the legacy content-slot key), or null.
 * @param order  The removed pass' explicit pipeline order (the legacy key within that camera).
 */
void releaseWindowLayer(VsgRendererState& state, vine::raw_ptr<const vine::graphics::Camera> camera, int order);

} // namespace detail

V_VSG_NS_END

