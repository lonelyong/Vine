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

bool borrowNeedsRebuild(const VsgRendererState& state, const VsgRenderTargetEntry& t,
                      const vine::graphics::RenderTarget* target_key)
{
    if (target_key == nullptr) {
        return false; // the window target never borrows (see the declaration's notes)
    }
    // Non-const: the targets table is keyed by the plain pointer (and the SDK's accessor
    // returns one even through a const target).
    vine::graphics::RenderTarget* const wanted_source = target_key->depthSource();
    if (wanted_source != nullptr && t.depth_source != wanted_source && t.unusable_depth_source != wanted_source) {
        const auto src_it = state.targets.find(wanted_source);
        if (src_it != state.targets.end() && src_it->second.depth_view != nullptr) {
            return true; // the borrow was only WAITING for its source
        }
    }
    if (t.depth_source != nullptr && t.unusable_depth_source != t.depth_source) {
        const auto src_it = state.targets.find(t.depth_source);
        if (src_it == state.targets.end() || src_it->second.depth_view != t.depth_source_view) {
            return true; // the image this framebuffer borrowed is gone
        }
    }
    return false;
}

void resetTargetAttachments(VsgRenderTargetEntry& t)
{
    t.content_slots.clear();
    t.screen_slots.clear();
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
    t.depth_borrow_pending_reported = false;
    t.graph                = {};
    t.depth_on_shader_set  = {};
    t.depth_testonly_shader_set = {};
    t.depth_off_shader_set = {};
    t.width     = 0;
    t.height    = 0;
    t.build_key = {};
}

void dropConsumersSampling(VsgRendererState& state, const vine::graphics::RenderTarget* target)
{
    for (auto& entry : state.targets) {
        auto& other = entry.second;
        if (entry.first == target || (!other.attachments_built && other.graph == nullptr)) {
            continue;
        }
        for (auto it = other.screen_slots.begin(); it != other.screen_slots.end();) {
            if (it->second.source_target == target) {
                detachSlotView(state, other, entry.first, it->first, it->second.view);
                it = other.screen_slots.erase(it);
            }
            else {
                ++it;
            }
        }
        for (auto it = other.program_slots.begin(); it != other.program_slots.end();) {
            if (it->second.source_target == target) {
                detachSlotView(state, other, entry.first, it->first, it->second.view);
                it = other.program_slots.erase(it);
            }
            else {
                ++it;
            }
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
        const auto& view  = slot_entry.second.view;
        auto&       queue = state.pending_compile_views;
        queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
    }
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
    auto& queue = state.pending_compile_views;
    queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
}

bool resolveDepthBorrow(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                        vine::graphics::RenderTarget& target, uint32_t w, uint32_t h)
{
    auto& t = state.entryFor(&target);
    // A borrow whose source has since been RELEASED cannot be honoured either — the
    // source's VkImage is gone — so such a target builds with its own depth instead
    // of failing to build for ever (see releaseRenderTarget).
    vine::graphics::RenderTarget* const depth_src = target.depthSource();
    if (depth_src == nullptr || t.unusable_depth_source == depth_src) {
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
        if (!t.depth_borrow_pending_reported) {
            diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                               vine::graphics::DiagnosticCategory::ContentSkipped,
                               formatDiagnostic(u8"shared-depth target '%s': source '%s' has no depth image yet;"
                                                u8" this target builds its own depth and retries the borrow",
                                                target.name().empty() ? "(unnamed)" : target.name().stdstr().c_str(),
                                                depth_src->name().empty() ? "(unnamed)" : depth_src->name().stdstr().c_str()));
            t.depth_borrow_pending_reported = true;
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
        t.depth_borrow_pending_reported = false;
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
    t.unusable_depth_source = depth_src;
    return false;
}

void buildOffscreenTarget(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                          vine::graphics::RenderTarget* target)
{
    // (Re)build an off-screen target's GPU attachments + render graph, sized
    // to the target. The graph is created EMPTY: content-slot Views are
    // appended by setupContentSlot() as passes render into this target (C6.4:
    // one RT can hold several content slots, like the window target).
    // EXPERIMENTAL: must be validated on a real Vulkan device before
    // production use.
    if (target == nullptr || state.window == nullptr) {
        return;
    }
    auto& t = state.entryFor(target);

    // A rebuild (target resized or its attachment shape changed) must first stop the previous
    // pass graphs being recorded and release what hangs off them, then forget the previous
    // build's images / views / flags (the counted device wait lives in that unhook).
    if (t.attachments_built) {
        unhookTargetPasses(state, t);
        resetTargetAttachments(t);
    }

    const uint32_t w = static_cast<uint32_t>(target->width());
    const uint32_t h = static_cast<uint32_t>(target->height());
    if (w == 0 || h == 0) {
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
    t.depth_clear_value = 0.0f;
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
    // (PiP screen slots / fullscreen-program slots) still holds the OLD views — see
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

void erasePassSlotsFromTarget(VsgRendererState& state, vine::graphics::RenderTarget* target,
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
        erasePassSlotsFromTarget(state, entry.first, pass);
    }
}

void releaseRenderTarget(VsgRendererState& state, const VsgDiagnostics& diagnostics, vine::graphics::RenderTarget* target)
{
    if (target == nullptr || !state.initialized) {
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
        // set: a later shareDepth() with a live source clears the condition by
        // being a different pointer, and the memory is bounded by the live
        // targets rather than by every target ever released.
        other.unusable_depth_source = target;
        other.depth_source          = nullptr;
        other.depth_share_barrier   = {};
        other.width                 = 0;
        other.height                = 0;
        released                    = true;
    }
    // A slot that SAMPLES the removed target (a screen / program slot lives under
    // the target that DRAWS it, and its source is a slot attribute) would keep a
    // dead image bound: drop it wherever it lives. Content slots own no sampling
    // edge, so they are skipped by the requires-clause.
    for (auto& target_entry : state.targets) {
        auto& t = target_entry.second;
        // Collect first: erasing while the visitor walks the tables would
        // invalidate the walk.
        std::vector<std::pair<VsgRenderTargetEntry::SlotKind, SlotKey>> drop;
        t.forEachSlot([&](const SlotKey& key, auto& slot, VsgRenderTargetEntry::SlotKind kind) {
            if constexpr (requires { slot.source_target; }) {
                if (slot.source_target == target) {
                    detachSlotView(state, t, target_entry.first, key, slot.view);
                    drop.emplace_back(kind, key);
                }
            }
        });
        for (const auto& [kind, key] : drop) {
            // Destructive (the slot's node goes with it), so this keeps the
            // counted device wait rather than parking the view — see
            // erasePassSlotsFromTarget.
            state.retireRing.waitForIdle(state.viewer);
            t.eraseSlot(kind, key);
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

void releaseWindowLayer(VsgRendererState& state, vine::raw_ptr<const vine::graphics::Camera> camera, int order)
{
    if (camera == nullptr || !state.initialized) {
        return;
    }
    // Window content slots live in the window target (nullptr key) of the
    // output-target table, keyed by (camera, explicit pass order). Off-screen
    // slots are released together with their whole target (releaseRenderTarget).
    auto& t  = state.entryFor(nullptr);
    // Legacy key (camera, order): only state created by a direct driver that
    // never opened a pass scope uses it. Engine-driven slots are released by
    // releasePass() (keyed by the pass itself).
    auto  it = t.content_slots.find(SlotKey::cameraOrder(camera, order));
    if (it == t.content_slots.end()) {
        return;
    }
    // Detach the slot's View from the window render graph so it is no longer
    // recorded each frame, then drop it (releases its compiled pipelines and
    // the per-slot bridge cache). Slot removal is rare, so a device wait
    // before the drop keeps the release safe against an in-flight frame.
    removeGraphChild(t.graph.get(), it->second.view);
    // The slot's bridge is destroyed with it, so the bridge's own ring goes too:
    // the cache drop needs the counted device wait (see detail::unhookTargetPasses).
    state.retireRing.waitForIdle(state.viewer);
    it->second.bridge.clearCache();
    t.content_slots.erase(it);
}

} // namespace detail

V_VSG_NS_END

