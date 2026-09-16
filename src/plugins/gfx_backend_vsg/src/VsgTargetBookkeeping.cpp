#include <vine/vsg/VsgTargetBookkeeping.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/vk/Device.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>

#include <vine/logging/Log.hpp>

#include <vine/vsg/SceneBridge.hpp>

#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgRecordOrder.hpp>
#include <vine/vsg/VsgUtils.hpp>
#include <vine/vsg/VsgViewCompiler.hpp>

V_VSG_NS_BEGIN

// A render target's whole lifecycle: the (re)build of its GPU attachments, the release of its
// pass graphs and of the slots that draw into it. The declarations and the policy notes are in
// VsgTargetBookkeeping.hpp.
//
// This translation unit is one of several that share a single free-function layer
// (VsgPipelineFactory.hpp / VsgBackendUtility.hpp); the directive keeps its call sites unqualified.
using namespace detail;

namespace detail
{

bool beginTargetSizeMissingEpisode(std::uint32_t width, std::uint32_t height, ReportOnce& reported)
{
    if (width != 0u && height != 0u) {
        reported.rearm(); // a usable size ends the episode (the host sized the target)
        return false;
    }
    return reported.shouldReport();
}

bool borrowNeedsRebuild(const VsgRendererState& state, const VsgRenderTargetEntry& t,
                      const vine::graphics::RenderTarget* target_key)
{
    if (target_key == nullptr) {
        return false; // the window target never borrows (see the declaration's notes)
    }
    // Non-const: the targets table is keyed by the plain pointer (and the SDK's accessor
    // returns one even through a const target).
    vine::graphics::RenderTarget* const wanted_source = target_key->depthSource();
    if (wanted_source != nullptr && t.depth_source != wanted_source &&
        t.unusable_depth_source.get() != wanted_source) {
        const auto src_it = state.targets.find(wanted_source);
        if (src_it != state.targets.end() && src_it->second.depth_view != nullptr) {
            return true; // the borrow was only WAITING for its source
        }
    }
    if (t.depth_source != nullptr && t.unusable_depth_source.get() != t.depth_source) {
        const auto src_it = state.targets.find(t.depth_source);
        if (src_it == state.targets.end() || src_it->second.depth_view != t.depth_source_view) {
            return true; // the image this framebuffer borrowed is gone
        }
    }
    return false;
}

namespace
{

/**
 * @brief Forgets everything a previous build of a target's attachments produced.
 *
 * The forgetting half of clearTargetAttachments, and deliberately NOT callable on its own: every
 * image / view / slot table and every flag the build set goes back to its initial value, while the
 * target's own entry stays — and doing that while the old pass graphs are still recorded, or before
 * the device was waited on, is what that function's body exists to make impossible. Written as ONE
 * list because it is exactly what a build OWNS: a Target field added later and forgotten here would
 * survive a rebuild as a stale image, a stale "already built" flag or a stale borrow source, and
 * nothing would report it.
 *
 * @param t Target entry being emptied (its key stays registered).
 */
void resetTargetAttachments(VsgRenderTargetEntry& t)
{
    t.content_slots.clear();
    t.program_slots.clear();
    t.color_images.clear();
    t.color_views.clear();
    t.depth_image          = {};
    t.depth_view           = {};
    t.passes.clear();
    t.attachments_built = false;
    t.depth_seeded      = false;
    t.any_load_pass     = false;
    t.color_seeded      = false;
    t.depth_source      = nullptr;
    t.depth_source_view    = {};
    t.depth_share_barrier  = {};
    t.depth_sampleable     = false;
    t.depth_borrow_pending_reported.rearm();
    t.graph                = {};
    t.depth_on_shader_set  = {};
    t.depth_testonly_shader_set = {};
    t.depth_off_shader_set = {};
    t.width     = 0;
    t.height    = 0;
    t.build_key = {};
}

} // namespace

void eraseProgramSlot(VsgRendererState& state, VsgRenderTargetEntry& owner,
                      vine::graphics::RenderTarget* owner_key, const SlotKey& key)
{
    const auto slot = owner.program_slots.find(key);
    if (slot == owner.program_slots.end()) {
        return;
    }
    // The order is the rule (see the declaration): stop recording the view, park the node that
    // the frames in flight may still name, then let the slot go.
    detachSlotView(state, owner, owner_key, key, slot->second.view);
    state.retireRing.park(slot->second.node);
    owner.program_slots.erase(slot);
}

void dropConsumersSampling(VsgRendererState& state, const vine::graphics::RenderTarget* target)
{
    for (auto& entry : state.targets) {
        auto& other = entry.second;
        if (entry.first == target || (!other.attachments_built && other.graph == nullptr)) {
            continue;
        }
        // Collected first: erasing while the walk runs would invalidate it.
        std::vector<SlotKey> drop;
        for (const auto& slot : other.program_slots) {
            if (slot.second.source_target == target) {
                drop.push_back(slot.first);
            }
        }
        for (const SlotKey& key : drop) {
            eraseProgramSlot(state, other, entry.first, key);
        }
    }
}

void createTargetAttachments(VsgRendererState& state, VsgRenderTargetEntry& t, ::vsg::Device* device,
                             const vine::graphics::RenderTarget& target, uint32_t w, uint32_t h,
                             vine::graphics::RenderTarget* depth_src)
{
    const int color_count = target.colorCount();
    t.color_images.resize(static_cast<std::size_t>(color_count));
    t.color_views.resize(static_cast<std::size_t>(color_count));
    for (int i = 0; i < color_count; ++i) {
        auto color           = ::vsg::Image::create();
        color->imageType     = VK_IMAGE_TYPE_2D;
        color->format        = toColorFormat(target.colorFormat(i));
        color->extent        = VkExtent3D{ w, h, 1 };
        color->mipLevels     = 1;
        color->arrayLayers   = 1;
        color->tiling        = VK_IMAGE_TILING_OPTIMAL;
        color->usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        color->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.color_images[static_cast<std::size_t>(i)] = color;
        t.color_views[static_cast<std::size_t>(i)] =
            ::vsg::createImageView(device, color, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!target.hasDepth()) {
        return;
    }
    if (depth_src != nullptr) {
        // Borrow the source's depth image/view (it renders earlier this frame): the
        // framebuffer attaches the shared depth, loaded rather than cleared. The borrow was
        // validated by resolveDepthBorrow, so the source image exists and matches this
        // framebuffer.
        const auto src_it   = state.targets.find(depth_src);
        t.depth_source_view = src_it->second.depth_view;
        return;
    }
    auto depth           = ::vsg::Image::create();
    depth->imageType     = VK_IMAGE_TYPE_2D;
    depth->format        = toDepthFormat(target.depthFormat());
    depth->extent        = VkExtent3D{ w, h, 1 };
    depth->mipLevels     = 1;
    depth->arrayLayers   = 1;
    depth->tiling        = VK_IMAGE_TILING_OPTIMAL;
    depth->usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    depth->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    t.depth_image        = depth;
    t.depth_view         = ::vsg::createImageView(device, depth, VK_IMAGE_ASPECT_DEPTH_BIT);
}

::vsg::ref_ptr<::vsg::PipelineBarrier> makeDepthShareBarrier(const VsgRendererState& state,
                                                             vine::graphics::RenderTarget* source)
{
    const auto src_it = state.targets.find(source);
    if (src_it == state.targets.end() || src_it->second.depth_image == nullptr) {
        return {}; // no depth image to share (yet): the borrower rebuilds later
    }
    // The depth image may be a COMBINED depth/stencil format (D24 ->
    // VK_FORMAT_D24_UNORM_S8_UINT). With separateDepthStencilLayouts disabled, a
    // barrier's subresource range must cover BOTH aspects of such a format
    // (VUID-VkImageMemoryBarrier-image-03320), so the aspect mask follows the
    // source's format instead of assuming a depth-only image.
    const VkFormat src_depth_format = toDepthFormat(source->depthFormat());
    const bool     has_stencil =
        src_depth_format == VK_FORMAT_D24_UNORM_S8_UINT || src_depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        src_depth_format == VK_FORMAT_D16_UNORM_S8_UINT;
    const VkImageAspectFlags aspect_flags =
        VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    auto imb = ::vsg::ImageMemoryBarrier::create(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                                src_it->second.depth_image,
                                                VkImageSubresourceRange{ aspect_flags, 0, 1, 0, 1 });
    // Both stages are the early/late fragment tests: the source WRITES the depth in
    // its own depth tests, the borrower READS it in its own.
    return ::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0, imb);
}

void unhookTargetPasses(VsgRendererState& state, VsgRenderTargetEntry& t)
{
    for (auto& pass : t.passes) {
        removeGraphChild(state.command_graph.get(), pass.second.graph);
    }
    // Destructive teardown keeps the counted device wait (§3): the bridge caches
    // dropped below release the shared object registry, whose pipelines / samplers
    // the retained nodes do not necessarily keep alive as the only owner. Parking
    // the views instead was measured to trip vkDestroyPipeline-00765 /
    // vkDestroySampler-01082, so this is the wait, not a park.
    state.retireRing.waitForIdle(state.viewer);
    for (auto& slot_entry : t.content_slots) {
        slot_entry.second.bridge.clearCache();
        // A dropped slot must not stay queued for the frame's incremental compile:
        // its view no longer belongs to any target.
        dropQueuedCompileView(state, slot_entry.second.view);
    }
}

void clearTargetAttachments(VsgRendererState& state, VsgRenderTargetEntry& t)
{
    // A target that was never built has nothing to unhook: no pass graph was ever created for it
    // (passGraph refuses without attachments) and no slot either, so the forgetting half is the
    // whole job — which is also what the guard buys: a first build pays no device wait.
    if (!t.attachments_built) {
        return;
    }
    // The order IS the function body (see the declaration): stop the old graphs recording and make
    // the release safe FIRST, then forget what they owned.
    unhookTargetPasses(state, t);
    resetTargetAttachments(t);
}

void detachSlotView(VsgRendererState& state, VsgRenderTargetEntry& owner, vine::graphics::RenderTarget* owner_key,
                    const SlotKey& key, const ::vsg::ref_ptr<::vsg::View>& view)
{
    if (auto graph = owner.slotGraph(owner, owner_key, key); graph != nullptr) {
        removeGraphChild(graph.get(), view);
    }
    // A dropped view must not stay queued for the frame's incremental compile —
    // only content slots queue their views, so this is a no-op for the other
    // kinds (which compile the moment they are built).
    dropQueuedCompileView(state, view);
}

void dropQueuedCompileView(VsgRendererState& state, const ::vsg::ref_ptr<::vsg::View>& view)
{
    auto& queue = state.pending_compile_views;
    queue.erase(std::remove_if(queue.begin(), queue.end(),
                               [&view](const PendingCompileView& entry) { return entry.view == view; }),
                queue.end());
}

void resetContentShaderSlots(VsgRendererState& state)
{
    // One wait for every drop below: the slots' bridges are destroyed here and
    // their caches release the shared object registry, which is the counted
    // device wait (see unhookTargetPasses) rather than a park.
    state.retireRing.waitForIdle(state.viewer);
    for (auto& entry : state.targets) {
        auto& t = entry.second;
        for (auto& slot_entry : t.content_slots) {
            // Through detachSlotView so the view stops being recorded: a slot
            // whose view is still attached to its pass graph keeps drawing with
            // the set it was built with, which is exactly what this replaces.
            detachSlotView(state, t, entry.first, slot_entry.first, slot_entry.second.view);
            slot_entry.second.bridge.clearCache();
        }
        t.content_slots.clear();
        // The per-size sets a target bakes for its own slots follow the program
        // too, so they are forgotten with the slots that used them.
        t.depth_on_shader_set       = {};
        t.depth_testonly_shader_set = {};
        t.depth_off_shader_set      = {};
    }
}

bool resolveDepthBorrow(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                        vine::graphics::RenderTarget& target, uint32_t w, uint32_t h)
{
    auto& t = state.entryFor(&target);
    // A borrow whose source has since been RELEASED cannot be honoured either — the
    // source's VkImage is gone — so such a target builds with its own depth instead
    // of failing to build for ever (see releaseRenderTarget).
    vine::graphics::RenderTarget* const depth_src = target.depthSource();
    if (depth_src == nullptr || t.unusable_depth_source.get() == depth_src) {
        return false;
    }
    // Three ways a borrow is unusable, each of which was silent before:
    //  - the source has not rendered yet this frame (no depth image yet);
    //  - the extents differ: Vulkan requires every framebuffer attachment to have
    //    the framebuffer's dimensions, so a half-resolution composite borrowing a
    //    full-resolution depth built an INVALID framebuffer
    //    (VUID-VkFramebufferCreateInfo-pAttachments-00880) and then rendered
    //    undefined;
    //  - the source promoted its depth to a sampled texture: its image is in
    //    SHADER_READ_ONLY_OPTIMAL, which no render pass may attach, so every frame
    //    tripped VUID-VkImageMemoryBarrier-oldLayout-01197 (the depth-share barrier
    //    assumes the attachment layout) and drew nothing.
    const auto  src_it    = state.targets.find(depth_src);
    const bool  src_ready = src_it != state.targets.end() && src_it->second.depth_view != nullptr;
    const char* reason    = nullptr;
    if (!src_ready) {
        // TRANSIENT: the source has no depth image yet (its pass has not built this
        // frame — e.g. an engine warm-up that ran this target's consumer before the
        // producer). Not remembered as unusable: this frame builds with its own
        // depth and the borrow is retried as soon as the source exists (render()'s
        // rebuild predicate). Reported once per episode, so a source that never
        // arrives is not silent either.
        if (t.depth_borrow_pending_reported.shouldReport()) {
            diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                               vine::graphics::DiagnosticCategory::ContentSkipped,
                               formatDiagnostic(u8"shared-depth target '%s': source '%s' has no depth image yet;"
                                                u8" this target builds its own depth and retries the borrow",
                                                target.name().empty() ? "(unnamed)" : target.name().stdstr().c_str(),
                                                depth_src->name().empty() ? "(unnamed)" : depth_src->name().stdstr().c_str()));
        }
        return false;
    }
    if (src_it->second.width != static_cast<int>(w) || src_it->second.height != static_cast<int>(h)) {
        reason = "its source has a different size (a framebuffer attachment must have the framebuffer's dimensions)";
    }
    else if (src_it->second.depth_sampleable) {
        reason = "its source promoted its depth to a sampled texture (a sampled depth cannot be attached)";
    }
    if (reason == nullptr) {
        // Honoured (or nothing to retry): re-arm the transient report.
        t.depth_borrow_pending_reported.rearm();
        return true;
    }
    // PERSISTENT: a property of the setup, not of this frame — the same source stays
    // unusable until the host changes it, so it is remembered (reported once) and
    // retried only when the host points the borrow at a different source.
    diagnostics.report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                       formatDiagnostic(u8"shared-depth target '%s': source '%s' cannot be borrowed (%s); this target"
                                        u8" builds its own depth",
                                        target.name().empty() ? "(unnamed)" : target.name().stdstr().c_str(),
                                        depth_src->name().empty() ? "(unnamed)" : depth_src->name().stdstr().c_str(),
                                        reason));
    t.unusable_depth_source = vine::intrusive_ptr<const vine::graphics::RenderTarget>(depth_src);
    return false;
}

void buildOffscreenTarget(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                          vine::graphics::RenderTarget* target)
{
    // (Re)build an off-screen target's GPU attachments + render graph, sized
    // to the target. The graph is created EMPTY: content-slot Views are
    // appended by setupContentSlot() as passes render into this target (C6.4:
    // one RT can hold several content slots, like the window target).
    if (target == nullptr) {
        return;
    }
    auto& t = state.entryFor(target);

    // A rebuild (target resized or its attachment shape changed) first stops the previous pass
    // graphs being recorded and releases what hangs off them (the counted device wait lives in
    // that unhook), then forgets the previous build's images / views / flags — ONE call, because the
    // order between the two halves is what makes the release safe (see clearTargetAttachments).
    clearTargetAttachments(state, t);

    const uint32_t w = static_cast<uint32_t>(target->width());
    const uint32_t h = static_cast<uint32_t>(target->height());
    if (beginTargetSizeMissingEpisode(w, h, t.size_missing_reported)) {
        // One report per episode: an unsized target is a host mistake with a clear fix, and a
        // pass that silently draws nothing is exactly what the diagnostics channel exists for.
        diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                           vine::graphics::DiagnosticCategory::TargetBuildFailed,
                           formatDiagnostic(u8"render target '%s' has no size (%ux%u): no attachments are"
                                            u8" created, so the passes drawing into it draw nothing until it is"
                                            u8" sized (RenderTarget::setSize)",
                                            target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str(),
                                            w, h));
    }
    if (w == 0 || h == 0) {
        return;
    }
    // The size check above is a fact about the REQUEST (it is reported whether or not a device
    // is up); building the attachments is the part that needs the session.
    if (state.window == nullptr) {
        return;
    }
    t.width             = static_cast<int>(w);
    t.height            = static_cast<int>(h);
    auto       device     = state.window->getOrCreateDevice();
    const int  color_count = target->colorCount();
    const bool has_color   = color_count > 0;
    const bool has_depth   = target->hasDepth();
    // Depth may be OWNED (allocated below) or BORROWED from an earlier target in the
    // same frame (RenderTarget::shareDepth): the deferred-lit composite reuses the
    // G-buffer's depth so forward content can test against it. Whether the borrow is
    // possible is decided (and reported) here; anything unusable must not reach
    // vkCreateFramebuffer.
    const bool borrowed = resolveDepthBorrow(state, diagnostics, *target, w, h);
    // The image the borrow points at, when it was honoured (the barrier and the
    // attachment selection below need it).
    vine::graphics::RenderTarget* const depth_src = borrowed ? target->depthSource() : nullptr;

    // One colour image + view per attachment, plus this target's depth (owned, or the source's
    // image when the borrow was honoured).
    createTargetAttachments(state, t, device.get(), *target, w, h, depth_src);

    // What is shared — and what this function owns — is the ATTACHMENT SET:
    // the render pass, framebuffer and graph are per pass now (see passGraph),
    // because one render pass bakes ONE pair of attachment load-ops and a
    // clearing pass and a preserving pass cannot share one. Record the depth
    // value every pass of this target clears to: the reverse-Z FAR plane (0.0)
    // for EVERY target, colour or depth-only. The depth compare is
    // VK_COMPARE_OP_GREATER (near = 1, far = 0), so a buffer initialised to the
    // near plane would reject every fragment (`depth > 1.0` is never true) and
    // a depth-only target would stay empty — the value has to be the far plane
    // the test expects, exactly as the window swapchain pass clears to 0.0.
    t.depth_clear_value = kReverseZFarPlane;
    // Whether this target's depth may be promoted to a sampleable texture is
    // part of the target's DESCRIPTION (RenderTarget::depthPromotion), so it is
    // known HERE rather than only once a pass is created: a consumer that
    // borrows this depth is validated in the same frame, before ANY of this
    // target's passes exists, so recording it at pass-creation time would be too
    // late. A target that had to fall back to its own depth promotes on its own
    // terms (it is the owner of the image a pass would sample).
    t.depth_sampleable  = has_depth && !borrowed && target->depthPromotion();
    t.attachments_built = has_color || has_depth;

    // A (re)built target created FRESH colour views: any OTHER target that samples this one
    // (a full-screen program slot) still holds the OLD views — see
    // detail::dropConsumersSampling.
    dropConsumersSampling(state, target);

    if (borrowed) {
        // The barrier the command graph needs so this pass LOADs / tests the depth
        // after the source's passes wrote it (reconcileOffscreenOrder inserts it
        // right after the source's last graph).
        t.depth_share_barrier = makeDepthShareBarrier(state, depth_src);
        t.depth_source        = depth_src;
    }

    // Record the shape these attachments were built from, so render() rebuilds
    // when the host changes any of it (see VsgRenderTargetEntry::BuildKey).
    t.build_key = VsgRenderTargetEntry::BuildKey::of(*target);
    V_LOGI("[VsgRenderer] EXPERIMENTAL off-screen target '{}' {}x{} attached",
           target->name().empty() ? "(unnamed)" : target->name().stdstr(), w, h);
    ++state.offscreen_build_count;
    // NOTE: no compile here — no pass graph exists until the first pass into
    // this target asks for one (passGraph); setupContentSlot() compiles then.
}

void erasePassFromTarget(VsgRendererState& state, vine::graphics::RenderTarget* target,
                         const vine::graphics::RenderPass* pass)
{
    if (pass == nullptr) {
        return;
    }
    const auto target_entry = state.targets.find(target);
    if (target_entry == state.targets.end()) {
        return;
    }
    auto&         t   = target_entry->second;
    const SlotKey key = SlotKey::ownerPass(pass);

    // The pass' own materialised objects go with it. They used to survive their pass (only a
    // target rebuild cleared them), which broke the two rules the interface states for retained
    // state: it must not grow with the frame count (a host that adds and removes a pass would
    // accumulate one render pass + framebuffer + graph per ever-seen pass), and it must be keyed
    // by something it owns — the key is a raw RenderPass* a released pass can be replaced at (the
    // rule the target / material / geometry / program caches already follow). Parked, not
    // destroyed: an in-flight command buffer may still name them and nothing else owns them (the
    // same policy as a load-op rebuild in VsgPassMaterialiser).
    if (const auto objects = t.passes.find(key); objects != t.passes.end()) {
        state.retireRing.park(objects->second.graph);
        state.retireRing.park(objects->second.render_pass);
        state.retireRing.park(objects->second.render_pass_transient);
        state.retireRing.park(objects->second.framebuffer);
        t.passes.erase(objects);
    }

    // Nothing to do for a pass this target holds no slot for: avoid a device wait
    // on the common path (a pass that moved targets usually owns a slot in only
    // one of them).
    if (!t.hasSlot(key)) {
        return;
    }
    // Destructive: the slots dropped below are erased together with their
    // bridges, and SceneBridge::clearCache() releases the shared object registry
    // (whose pipelines / samplers the retained nodes need not be the only owner
    // of), so this keeps the counted device wait rather than parking a view —
    // parking was measured to trip vkDestroyPipeline-00765 (see the
    // policy-churn notes).
    state.retireRing.waitForIdle(state.viewer);

    t.visitSlot(key, [&](auto& slot, VsgRenderTargetEntry::SlotKind kind) {
        detachSlotView(state, t, target, key, slot.view);
        // Only a content slot owns a bridge (its caches go with the slot).
        if constexpr (requires { slot.bridge; }) {
            slot.bridge.clearCache();
        }
        t.eraseSlot(kind, key);
    });
}

void retargetPass(VsgRendererState& state, const vine::graphics::RenderPass* pass, vine::graphics::RenderTarget* target)
{
    if (pass == nullptr) {
        return;
    }
    // A pass keeps exactly one retained slot per target. When it draws into a
    // different target than before, the slot it left behind would otherwise
    // keep drawing its content there forever.
    for (auto& entry : state.targets) {
        if (entry.first == target) {
            continue;
        }
        erasePassFromTarget(state, entry.first, pass);
    }
}

void releaseRenderTarget(VsgRendererState& state, const VsgDiagnostics& diagnostics, vine::graphics::RenderTarget* target)
{
    if (target == nullptr) {
        return;
    }
    // The target has to OUTLIVE this call, even when nothing else holds it: the table entry that
    // owns it is erased below (that is what "released" means here), so without this reference the
    // erase can be the last release — and the rest of this function still reads the released
    // target (its name, its identity compared against every borrowing / sampling slot, and the
    // tombstone that keeps a borrowing target from retrying the borrow). That is a use-after-free
    // with a heap-visible consequence (the tombstone would write into freed memory), and it is
    // reachable from the path that exists exactly for "the host dropped the target itself" (see
    // VsgRenderer::releaseAbandonedTargets, which releases targets whose only owner is the entry).
    // One reference, for the length of the call, is what makes the identity below a fact.
    const vine::intrusive_ptr<vine::graphics::RenderTarget> alive(target);

    // The scope being executed may still name this target: the request belongs to that scope, and
    // the caller releases the target because its last owner is going away — so the announced
    // pointer must not be used again (the class contract forbids keeping it). Marking it rather
    // than only clearing it is what lets the next call that needed it say why it was skipped
    // instead of silently drawing into the window. Mirrors releasePass(), which drops the pass it
    // announces the same way.
    if (state.request.target == target) {
        state.request.target         = nullptr;
        state.request.target_released = true;
    }
    if (!state.initialized) {
        return;
    }
    bool released = false;
    auto ot       = state.targets.find(target);
    if (ot != state.targets.end()) {
        // Stop recording the target's off-screen pass graphs and make the release
        // safe before dropping its images / views / render passes / slots (the
        // counted device wait lives in that unhook).
        unhookTargetPasses(state, ot->second);
        // Drop the target's whole table entry (its off-screen attachments /
        // render graph / content slots). Any sampling slot that reads it lives
        // in another target's slot tables and is removed right below.
        state.targets.erase(ot);
        released = true;
    }
    // A target that BORROWED the removed target's depth (RenderTarget::
    // shareDepth) now references a destroyed depth image, and the barrier that
    // ordered the two graphs still points at it. Drop the borrow and force the
    // borrower to rebuild with its own depth: otherwise its framebuffer keeps
    // a dead attachment (and the command graph is ordered around a dead
    // barrier). Clearing the recorded size re-enters the rebuild path on its
    // next draw.
    for (auto& entry : state.targets) {
        auto& other = entry.second;
        if (other.depth_source != target) {
            continue;
        }
        V_LOGW("[VsgRenderer] target '{}' borrowed the released target '{}' depth;"
               " dropping the borrow (it rebuilds with its own depth)",
               entry.first->name().empty() ? "(unnamed)" : entry.first->name().stdstr(),
               target->name().empty() ? "(unnamed)" : target->name().stdstr());
        // Remember WHICH source became unusable instead of a global tombstone
        // set: a later shareDepth() naming a live source clears the condition (the
        // remembered one is a DIFFERENT object, and the entry owns it, so no new
        // target can be mistaken for it), and the memory is bounded by the live
        // targets rather than by every target ever released.
        other.unusable_depth_source = vine::intrusive_ptr<const vine::graphics::RenderTarget>(target);
        other.depth_source          = nullptr;
        other.depth_share_barrier   = {};
        other.width                 = 0;
        other.height                = 0;
        released                    = true;
    }
    // A slot that SAMPLES the removed target (a program slot lives under the target that DRAWS
    // it, and its source is a slot attribute) would keep a dead image bound: drop it wherever it
    // lives. Content slots own no sampling edge, so they are skipped by the requires-clause.
    for (auto& target_entry : state.targets) {
        auto& t = target_entry.second;
        // Collect first: erasing while the visitor walks the tables would
        // invalidate the walk.
        std::vector<SlotKey> drop;
        t.forEachSlot([&](const SlotKey& key, auto& slot, VsgRenderTargetEntry::SlotKind) {
            if constexpr (requires { slot.source_target; }) {
                if (slot.source_target == target) {
                    drop.push_back(key);
                }
            }
        });
        for (const SlotKey& key : drop) {
            // One home for the drop (see eraseProgramSlot): it detaches the view, PARKS the node
            // and erases the slot. Parking is what this loop used to pay a device-wide idle PER
            // SLOT for — the node holds the descriptor set that names the removed target's images,
            // so it has to outlive the frames that may still record it, and the ring is what keeps
            // it alive for exactly that long.
            eraseProgramSlot(state, t, target_entry.first, key);
            released = true;
        }
    }
    if (released) {
        // The remaining command-graph child order may have changed (a sampling
        // edge disappeared, a graph was detached).
        detail::reconcileOffscreenOrder(state);
        V_LOGI("[VsgRenderer] released GPU resources for removed render target");
    }
}

} // namespace detail

V_VSG_NS_END

