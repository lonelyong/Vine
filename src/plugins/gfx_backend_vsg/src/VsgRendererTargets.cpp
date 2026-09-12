#include <vine/vsg/VsgRenderer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/commands/BlitImage.h>
#include <vsg/commands/CopyImageToBuffer.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/lighting/Light.h>
#include <vsg/state/Buffer.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/ViewportState.h>
#include <vsg/state/material.h>
#include <vsg/vk/CommandPool.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/DeviceMemory.h>
#include <vsg/vk/Fence.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/PhysicalDevice.h>
#include <vsg/vk/SubmitCommands.h>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

#include "VsgUtils.hpp"
#include "VsgRendererImpl.hpp"
#include "VsgBackendUtility.hpp"
#include "VsgPipelineFactory.hpp"

V_VSG_NS_BEGIN

// This translation unit is one of several that share a single free-function
// layer (VsgPipelineFactory.hpp / VsgBackendUtility.hpp); the directive keeps
// its call sites unqualified.
using namespace detail;

VsgRenderer::Impl::PassAttachments VsgRenderer::Impl::passAttachments(const Target&                   t,
                                                                     const vine::graphics::RenderTarget& target) const
{
    PassAttachments att;
    att.device    = window->getOrCreateDevice();
    att.borrowed  = t.depth_source != nullptr;
    att.has_color = !t.color_views.empty();
    att.has_depth = att.borrowed || t.depth_view != nullptr;
    if (att.has_depth) {
        att.depth_format =
            att.borrowed ? toDepthFormat(t.depth_source->depthFormat()) : toDepthFormat(target.depthFormat());
    }
    att.color_formats.reserve(t.color_views.size());
    for (std::size_t i = 0; i < t.color_views.size(); ++i) {
        att.color_formats.push_back(toColorFormat(target.colorFormat(static_cast<int>(i))));
    }
    return att;
}

std::pair<::vsg::ref_ptr<::vsg::RenderPass>, ::vsg::ref_ptr<::vsg::Framebuffer>> VsgRenderer::Impl::makePassObjects(
    const Target& t, const PassAttachments& att, bool pass_color_clear, bool depth_load, bool promote,
    VkImageLayout depth_initial) const
{
    ::vsg::ref_ptr<::vsg::RenderPass> render_pass;
    if (!att.has_color) {
        // A depth-only target keeps its depth sampleable whatever the pass does
        // with it, so a preserving pass LOADs a SHADER_READ_ONLY image and a
        // clearing one starts from UNDEFINED.
        render_pass = makeDepthOnlyRenderPass(att.device.get(), att.depth_format, depth_initial);
    }
    else if (att.has_depth && depth_load) {
        render_pass = makeColorDepthRenderPass(att.device.get(), att.color_formats, att.depth_format, depth_initial,
                                               /*promote_depth*/ false, pass_color_clear);
    }
    else {
        render_pass = makeColorDepthRenderPass(att.device.get(), att.color_formats, att.depth_format,
                                               VK_IMAGE_LAYOUT_UNDEFINED, promote, pass_color_clear);
    }
    // The attachment order the render pass declares: colour attachments in order,
    // then depth — borrowed depth is the SOURCE's view (the borrow was validated
    // in buildOffscreenTarget).
    ::vsg::ImageViews attachments;
    attachments.reserve(t.color_views.size() + (att.has_depth ? 1u : 0u));
    for (const auto& view : t.color_views) {
        attachments.push_back(view);
    }
    if (att.has_depth) {
        const auto source = targets.find(t.depth_source);
        attachments.push_back(att.borrowed && source != targets.end() ? source->second.depth_view : t.depth_view);
    }
    return std::pair{ render_pass,
                      ::vsg::Framebuffer::create(render_pass, attachments, static_cast<uint32_t>(t.width),
                                                 static_cast<uint32_t>(t.height), 1) };
}

bool VsgRenderer::Impl::borrowNeedsRebuild(const Target& t, const vine::graphics::RenderTarget* target_key) const
{
    if (target_key == nullptr) {
        return false; // the window target never borrows (see the declaration's notes)
    }
    // Non-const: the targets table is keyed by the plain pointer (and the SDK's accessor
    // returns one even through a const target).
    vine::graphics::RenderTarget* const wanted_source = target_key->depthSource();
    if (wanted_source != nullptr && t.depth_source != wanted_source && t.unusable_depth_source != wanted_source) {
        const auto src_it = targets.find(wanted_source);
        if (src_it != targets.end() && src_it->second.depth_view != nullptr) {
            return true; // the borrow was only WAITING for its source
        }
    }
    if (t.depth_source != nullptr && t.unusable_depth_source != t.depth_source) {
        const auto src_it = targets.find(t.depth_source);
        if (src_it == targets.end() || src_it->second.depth_view != t.depth_source_view) {
            return true; // the image this framebuffer borrowed is gone
        }
    }
    return false;
}

bool VsgRenderer::Impl::depthStillPromoted(const Target& t, const Target::PassObjects* current, int order) const
{
    if (!t.depth_sampleable || !t.depth_seeded) {
        // Nothing promoted it (or a LOAD pass revoked promotion), or the image is
        // UNDEFINED and needs the CLEAR seed instead.
        return false;
    }
    for (const auto& other : t.passes) {
        if (&other.second == current || other.second.order >= order) {
            continue; // this pass itself, or one that records after it
        }
        if (passes_active_this_frame.count(other.first.owner) != 0) {
            return false; // it ran first this frame and left its own layout
        }
    }
    return true;
}

void VsgRenderer::Impl::revokeDepthPromotion(Target& t, const Target::PassObjects* current,
                                             VkImageLayout steady_depth_initial)
{
    const PassAttachments att = passAttachments(t, *t.owner);
    for (auto& pass : t.passes) {
        if (&pass.second == current) {
            continue;
        }
        auto [render_pass, framebuffer] =
            makePassObjects(t, att, pass.second.color_clear, pass.second.load_depth,
                            /*promote*/ false,
                            pass.second.load_depth ? steady_depth_initial : VK_IMAGE_LAYOUT_UNDEFINED);
        // The variant this pass RECORDED may still be named by an in-flight command
        // buffer, so park what is being replaced instead of stopping the device for
        // it (see retireObject).
        retireObject(pass.second.render_pass);
        retireObject(pass.second.render_pass_transient);
        retireObject(pass.second.framebuffer);
        pass.second.render_pass           = render_pass;
        pass.second.render_pass_transient = {};
        pass.second.framebuffer           = framebuffer;
        pass.second.transient             = false;
        if (pass.second.graph != nullptr) {
            pass.second.graph->renderPass  = render_pass;
            pass.second.graph->framebuffer = framebuffer;
        }
    }
    t.depth_sampleable = false;
}

void VsgRenderer::dropDepthSamplingProgramSlots(vine::graphics::RenderTarget* target)
{
    // Drop the fullscreen-program slots that BIND this target's depth, now that a
    // pass of it revoked the promotion (see passGraph): the slot was built while
    // the promotion stood, so its descriptor set names the promoted layout, and
    // the revoke leaves the image in the attachment layout from here on —
    // recording it would name a layout the image is no longer in, once per draw,
    // for a frame the host has no way to know about. The owner's next
    // drawScreenProgram call rebuilds the slot from the revoke on, with the depth
    // binding gone (or refusing the program when it needs one, which is what the
    // compile path reports).
    //
    // Only slots that really BIND the depth are dropped: a colour-only program's
    // pipeline layout has no depth sampler, so nothing it records names that
    // layout, and it keeps drawing — this frame and every one after.
    for (auto& dest_entry : impl->targets) {
        auto& slots = dest_entry.second.program_slots;
        for (auto slot_it = slots.begin(); slot_it != slots.end();) {
            auto& slot = slot_it->second;
            if (!slot.ready || slot.source_target != target || !slot.binds_source_depth) {
                ++slot_it;
                continue;
            }
            removeGraphChild(slot.dest_graph.get(), slot.view);
            // The node owns the program's pipeline / descriptor objects, and the
            // command buffer that recorded this slot may still be pending: park it
            // rather than release it in flight (a view holds no Vulkan object).
            impl->retireObject(slot.node);
            reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                          vine::graphics::DiagnosticCategory::ContentSkipped,
                          formatDiagnostic(u8"drawScreenProgram: sampled target '%s' started preserving depth, which"
                                           u8" revokes its depth promotion: the program sampling that depth is"
                                           u8" dropped for this frame (its slot is rebuilt on the owner's next"
                                           u8" draw)",
                                           target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str()));
            slot_it = slots.erase(slot_it);
        }
    }
}

bool VsgRenderer::resolveDepthBorrow(vine::graphics::RenderTarget& target, uint32_t w, uint32_t h)
{
    auto& t = impl->entryFor(&target);
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
    const auto  src_it    = impl->targets.find(depth_src);
    const bool  src_ready = src_it != impl->targets.end() && src_it->second.depth_view != nullptr;
    const char* reason    = nullptr;
    if (!src_ready) {
        // TRANSIENT: the source has no depth image yet (its pass has not built this
        // frame — e.g. an engine warm-up that ran this target's consumer before the
        // producer). Not remembered as unusable: this frame builds with its own
        // depth and the borrow is retried as soon as the source exists (render()'s
        // rebuild predicate). Reported once per episode, so a source that never
        // arrives is not silent either.
        if (!t.depth_borrow_pending_reported) {
            reportFailure(vine::graphics::DiagnosticSeverity::Warning,
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
    reportFailure(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                  formatDiagnostic(u8"shared-depth target '%s': source '%s' cannot be borrowed (%s); this target"
                                   u8" builds its own depth",
                                   target.name().empty() ? "(unnamed)" : target.name().stdstr().c_str(),
                                   depth_src->name().empty() ? "(unnamed)" : depth_src->name().stdstr().c_str(),
                                   reason));
    t.unusable_depth_source = depth_src;
    return false;
}

bool VsgRenderer::Impl::submitOneShot(const ::vsg::ref_ptr<::vsg::Commands>& commands) const
{
    // Generous because the wait is what makes the readback synchronous: a timeout
    // here means the GPU never finished the transfer, not that it was slow.
    constexpr std::uint64_t kReadbackTimeoutNs = 100'000'000'000ull;

    auto device   = window != nullptr ? window->getDevice() : ::vsg::ref_ptr<::vsg::Device>();
    auto physical = window != nullptr ? window->getPhysicalDevice() : ::vsg::ref_ptr<::vsg::PhysicalDevice>();
    if (device == nullptr || physical == nullptr) {
        return false; // no usable device: there is nothing to submit to
    }
    const auto queue_family = physical->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
    auto       command_pool = ::vsg::CommandPool::create(device, queue_family);
    auto       fence        = ::vsg::Fence::create(device);
    auto       queue        = device->getQueue(queue_family);
    ::vsg::submitCommandsToQueue(command_pool, fence, kReadbackTimeoutNs, queue,
                                [&commands](::vsg::CommandBuffer& command_buffer) { commands->record(command_buffer); });
    return true;
}

::vsg::ref_ptr<::vsg::DeviceMemory> VsgRenderer::Impl::hostVisibleMemory(::vsg::Device*               device,
                                                                        const VkMemoryRequirements& requirements) const
{
    return ::vsg::DeviceMemory::create(device, requirements,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

const VsgRenderer::Impl::Target* VsgRenderer::Impl::readbackTarget(vine::graphics::RenderTarget* target) const
{
    if (target == nullptr || viewer == nullptr || window == nullptr) {
        // Nothing has been rendered off-screen in this session: the request is
        // unsupported, which is what the readback's false means (base contract).
        return nullptr;
    }
    const auto entry = targets.find(target);
    if (entry == targets.end() || !entry->second.attachments_built) {
        return nullptr;
    }
    const Target& built = entry->second;
    return (built.width > 0 && built.height > 0) ? &built : nullptr;
}

void VsgRenderer::Impl::retireObject(::vsg::ref_ptr<::vsg::Object> object)
{
    if (object == nullptr) {
        return;
    }
    retire_ring[retire_head].emplace_back(std::move(object));
}

void VsgRenderer::Impl::advanceRetireRing()
{
    // Move to the next slot and release it: it was filled kRetireRingDepth
    // advances ago, so every command-buffer slot that could have recorded one of
    // its objects has been re-recorded since (and start() waited on that slot's
    // fence before re-recording it), and the GPU no longer executes them.
    retire_head = (retire_head + 1u) % SceneBridge::kRetireRingDepth;
    retired_object_count += retire_ring[retire_head].size();
    retire_ring[retire_head].clear();
}

void VsgRenderer::Impl::waitForIdle()
{
    if (viewer == nullptr) {
        return;
    }
    ++device_wait_count;
    viewer->deviceWaitIdle();
}

void VsgRenderer::Impl::resetTargetAttachments(Target& t)
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

void VsgRenderer::Impl::dropConsumersSampling(const vine::graphics::RenderTarget* target)
{
    for (auto& entry : targets) {
        auto& other = entry.second;
        if (entry.first == target || (!other.attachments_built && other.graph == nullptr)) {
            continue;
        }
        for (auto it = other.screen_slots.begin(); it != other.screen_slots.end();) {
            if (it->second.source_target == target) {
                detachSlotView(other, entry.first, it->first, it->second.view);
                it = other.screen_slots.erase(it);
            }
            else {
                ++it;
            }
        }
        for (auto it = other.program_slots.begin(); it != other.program_slots.end();) {
            if (it->second.source_target == target) {
                detachSlotView(other, entry.first, it->first, it->second.view);
                it = other.program_slots.erase(it);
            }
            else {
                ++it;
            }
        }
    }
}

void VsgRenderer::Impl::createTargetAttachments(Target& t, ::vsg::Device* device,
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
        const auto src_it   = targets.find(depth_src);
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

void VsgRenderer::buildOffscreenTarget(vine::graphics::RenderTarget* target)
{
    // (Re)build an off-screen target's GPU attachments + render graph, sized
    // to the target. The graph is created EMPTY: content-slot Views are
    // appended by setupContentSlot() as passes render into this target (C6.4:
    // one RT can hold several content slots, like the window target).
    // EXPERIMENTAL: must be validated on a real Vulkan device before
    // production use.
    if (target == nullptr || impl->window == nullptr) {
        return;
    }
    auto& t = impl->entryFor(target);

    // A rebuild (target resized or its attachment shape changed) must first stop the previous
    // pass graphs being recorded and release what hangs off them, then forget the previous
    // build's images / views / flags (the counted device wait lives in that unhook).
    if (t.attachments_built) {
        impl->unhookTargetPasses(t);
        impl->resetTargetAttachments(t);
    }

    const uint32_t w = static_cast<uint32_t>(target->width());
    const uint32_t h = static_cast<uint32_t>(target->height());
    if (w == 0 || h == 0) {
        return;
    }
    t.width             = static_cast<int>(w);
    t.height            = static_cast<int>(h);
    auto       device     = impl->window->getOrCreateDevice();
    const int  color_count = target->colorCount();
    const bool has_color   = color_count > 0;
    const bool has_depth   = target->hasDepth();
    // Depth may be OWNED (allocated below) or BORROWED from an earlier target in the
    // same frame (RenderTarget::shareDepth): the deferred-lit composite reuses the
    // G-buffer's depth so forward content can test against it. Whether the borrow is
    // possible is decided (and reported) here; anything unusable must not reach
    // vkCreateFramebuffer.
    const bool borrowed = resolveDepthBorrow(*target, w, h);
    // The image the borrow points at, when it was honoured (the barrier and the
    // attachment selection below need it).
    vine::graphics::RenderTarget* const depth_src = borrowed ? target->depthSource() : nullptr;

    // One colour image + view per attachment, plus this target's depth (owned, or the source's
    // image when the borrow was honoured).
    impl->createTargetAttachments(t, device.get(), *target, w, h, depth_src);

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
    // Impl::dropConsumersSampling.
    impl->dropConsumersSampling(target);

    if (borrowed) {
        // The barrier the command graph needs so this pass LOADs / tests the depth
        // after the source's passes wrote it (reconcileOffscreenOrder inserts it
        // right after the source's last graph).
        t.depth_share_barrier = impl->makeDepthShareBarrier(depth_src);
        t.depth_source        = depth_src;
    }

    // Record the shape these attachments were built from, so render() rebuilds
    // when the host changes any of it (see Target::BuildKey).
    t.build_key = Impl::Target::BuildKey::of(*target);
    V_LOGI("[VsgRenderer] EXPERIMENTAL off-screen target '{}' {}x{} attached",
           target->name().empty() ? "(unnamed)" : target->name().stdstr(), w, h);
    ++impl->offscreen_build_count;
    // NOTE: no compile here — no pass graph exists until the first pass into
    // this target asks for one (passGraph); setupContentSlot() compiles then.
}

::vsg::ref_ptr<::vsg::PipelineBarrier> VsgRenderer::Impl::makeDepthShareBarrier(vine::graphics::RenderTarget* source) const
{
    const auto src_it = targets.find(source);
    if (src_it == targets.end() || src_it->second.depth_image == nullptr) {
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

::vsg::ref_ptr<::vsg::RenderGraph> VsgRenderer::Impl::makePassGraph(const Target& t, bool has_depth,
                                                                   const ::vsg::vec4& clear_color) const
{
    auto graph        = ::vsg::RenderGraph::create();
    graph->renderArea =
        VkRect2D{ { 0, 0 }, { static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) } };
    graph->contents      = VK_SUBPASS_CONTENTS_INLINE;
    graph->viewportState = ::vsg::ViewportState::create(
        VkExtent2D{ static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) });
    graph->clearValues.clear();
    for (std::size_t i = 0; i < t.color_views.size(); ++i) {
        VkClearValue value = {};
        if (i == 0u) {
            value.color = VkClearColorValue{ { clear_color.r, clear_color.g, clear_color.b, clear_color.a } };
        }
        graph->clearValues.push_back(value);
    }
    if (has_depth) {
        VkClearValue value = {};
        value.depthStencil = VkClearDepthStencilValue{ t.depth_clear_value, 0 };
        graph->clearValues.push_back(value);
    }
    return graph;
}

::vsg::ref_ptr<::vsg::RenderGraph> VsgRenderer::Impl::reuseSteadyPass(Target::PassObjects& objects,
                                                                   bool want_color_clear, bool want_depth_clear,
                                                                   bool has_color, const ::vsg::vec4& clear_color)
{
    if (passVariantIsStale(objects.want_color_clear, objects.want_depth_clear, want_color_clear, want_depth_clear)) {
        return {}; // clear policy changed: the variant has to be rebuilt
    }
    if (has_color && request.presenting && objects.clear_color != clear_color && objects.graph != nullptr &&
        !objects.graph->clearValues.empty()) {
        objects.clear_color                 = clear_color;
        objects.graph->clearValues[0].color = VkClearColorValue{
            { clear_color.r, clear_color.g, clear_color.b, clear_color.a }
        };
    }
    return objects.graph;
}

void VsgRenderer::Impl::publishPass(Target& t, const SlotKey& key, const Target::PassObjects& objects, bool has_color)
{
    t.passes.insert_or_assign(key, objects);
    // The image is defined from now on and this pass' policy is part of the target's: a
    // later pass may LOAD the colour, and a pass that LOADs depth forbids promotion for
    // the whole target.
    t.color_seeded  = true;
    t.depth_seeded  = true;
    t.any_load_pass = t.any_load_pass || objects.load_depth;
    if (has_color && objects.load_depth) {
        // A pass that had to LOAD the depth leaves it in the attachment layout, so the
        // target's depth can no longer be sampled: readDepthBuffer and a later borrow
        // validation must both be told. A depth-only target keeps its depth sampleable
        // regardless — its pass always ends in SHADER_READ_ONLY — so the flag stays TRUE
        // there (it is what readDepthBuffer derives the image's layout from).
        t.depth_sampleable = false;
    }
}

VsgRenderer::Impl::PassPlan VsgRenderer::Impl::planPass(const Target& t, const SlotKey& key,
                                                       const vine::graphics::RenderTarget& target) const
{
    PassPlan plan;
    // The target's attachment set (owned by buildOffscreenTarget): every pass of this target
    // attaches exactly these images, only its load-ops differ.
    plan.att       = passAttachments(t, target);
    plan.has_color = plan.att.has_color;
    plan.has_depth = plan.att.has_depth;

    // This pass' own clear request (the open pass scope) is what the variant's load-ops are
    // derived from — see this function's notes for the policy.
    plan.want_color_clear = request.presenting;
    plan.want_depth_clear = request.presenting && request.clear_depth;

    // Taken BEFORE the pass is published: the pointer addresses the target's pass table, and
    // the flags planPassVariant() reads below are the ones publishing the new objects changes.
    const auto built = t.passes.find(key);
    plan.current     = built != t.passes.end() ? &built->second : nullptr;

    const bool depth_still_promoted = depthStillPromoted(t, plan.current, request.order);
    plan.variant =
        planPassVariant(plan.has_color, plan.has_depth, plan.att.borrowed,
                        plan.has_depth && !plan.att.borrowed && target.depthPromotion(), t.any_load_pass,
                        t.depth_seeded, t.color_seeded, plan.want_color_clear, plan.want_depth_clear,
                        depth_still_promoted);
    plan.color_clear = plan.variant.color_load == VK_ATTACHMENT_LOAD_OP_CLEAR;
    plan.depth_load  = plan.has_depth && plan.variant.depth_load == VK_ATTACHMENT_LOAD_OP_LOAD;
    return plan;
}

::vsg::ref_ptr<::vsg::RenderGraph> VsgRenderer::passGraph(vine::graphics::RenderTarget* target, const SlotKey& key)
{
    auto& t = impl->entryFor(target);
    if (target == nullptr) {
        // The window session has ONE graph. Its render pass is the swapchain's
        // (vsg creates it with the window), so every window pass shares it —
        // window passes have no per-pass load-op to express anyway.
        return t.graph;
    }
    if (!t.attachments_built) {
        // No attachments: the target failed to build, or has not rendered yet.
        return {};
    }

    // What this pass needs THIS frame: attachment set, load-op variant, clear requests and
    // whether its depth is still promoted — one decision, taken in one place (see
    // Impl::planPass). Everything below APPLIES it.
    const Impl::PassPlan plan = impl->planPass(t, key, *target);

    const auto built = t.passes.find(key);

    if (built != t.passes.end()) {
        // A pass that changed its explicit pipeline order moves its graph to the matching
        // record position (see reconcileOffscreenOrder).
        if (built->second.order != impl->request.order) {
            built->second.order = impl->request.order;
            reconcileOffscreenOrder();
        }
        // Steady frame: reuse the recorded variant unless this pass changed its clear
        // policy (RenderPass::setClearEnabled / setShouldClearDepth flipped at run time).
        // Every other per-pass property (order, depth mode, lights, viewport) is re-applied
        // each frame; the load-ops have to follow suit, or a pass that starts clearing keeps
        // LOADing and its request is silently ignored.
        if (auto graph = impl->reuseSteadyPass(built->second, plan.want_color_clear, plan.want_depth_clear,
                                               plan.has_color, impl->request.clear_color)) {
            return graph;
        }
    }
    auto device = plan.att.device;
    if (device == nullptr) {
        return built != t.passes.end() ? built->second.graph : ::vsg::ref_ptr<::vsg::RenderGraph>();
    }

    // The clear COLOUR this pass' graph starts from: the pass' own clear()
    // request, or the target's last requested colour when the pass never asked
    // (see clear()); the constant is the historical default for a target nobody
    // has ever cleared.
    const ::vsg::vec4 clear_color =
        (t.clear_seen || impl->request.presenting) ? t.clear_color : ::vsg::vec4{ 0.2f, 0.2f, 0.2f, 1.0f };

    // A pass that LOADs depth must find the image in a layout it named, so no
    // pass of this target may promote it any more. A target whose earlier passes
    // were allowed to promote (the first pass cleared depth and nothing loaded
    // it) has to re-create them without promotion: their framebuffer goes with
    // the render pass, so the swap waits for idle. A program slot that was built
    // earlier in the frame and BINDS that depth goes with it (see below): nothing
    // of this frame may still name the layout the promotion left behind.
    //
    // A depth-only target is exempt: promotion is not a choice there (its depth
    // always ends sampleable), so nothing has to be revoked.
    //
    // The pass being (re)built is skipped: its own variant is decided above, and
    // rebuilding it here would both be thrown away and hide the layout its image
    // is in (a pass that STOPS promoting is exactly the one whose depth may still
    // carry the promoted layout — see planPassVariant).
    if (plan.has_color && plan.depth_load && t.depth_sampleable) {
        impl->revokeDepthPromotion(t, plan.current, plan.variant.steady_depth_initial);
        // A fullscreen program that SAMPLES this depth was built earlier in this
        // frame, while the promotion still stood, so its descriptor set names the
        // promoted layout — and the revoke (plus this pass) leaves the image in
        // the attachment layout from here on. Recording the slot would name a
        // layout the image is no longer in, once per draw, for a frame the host has
        // no way to know about, so it is dropped for this frame (see
        // dropDepthSamplingProgramSlots).
        dropDepthSamplingProgramSlots(target);
    }

    // A pass that preserves depth may find its image in a transitional layout that
    // no pass of this target has replaced with the attachment layout yet:
    //
    //  - UNDEFINED: nothing has defined the depth image (the pass records the
    //    CLEAR seed variant of the LOAD pass, see planPassVariant), or
    //  - SHADER_READ_ONLY: an earlier frame's pass PROMOTED the depth to a sampled
    //    texture and this pass is the first of this frame to use it. It LOADs the
    //    promoted content and hands the image back in the layout the next pass
    //    expects.
    //
    // Both variants are recorded for THIS frame only; submitFrame() swaps the
    // graph to the steady LOAD variant afterwards, which is the same render pass
    // with the depth attachment's initial layout corrected. The variants differ
    // only in the depth load-op and the depth attachment's initial layout, so
    // they are render-pass compatible and share the framebuffer.
    auto [render_pass, framebuffer] = impl->makePassObjects(t, plan.att, plan.color_clear, plan.depth_load,
                                                          plan.variant.promote_depth, plan.variant.depth_initial);
    ::vsg::ref_ptr<::vsg::RenderPass> render_pass_transient;
    if (plan.variant.transient) {
        render_pass_transient = render_pass;
        render_pass            = impl->makePassObjects(t, plan.att,
                                                       plan.variant.steady_color_load == VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                       plan.depth_load, plan.variant.promote_depth,
                                                       plan.variant.steady_depth_initial)
                                     .first;
    }

    // A rebuilt pass keeps its RenderGraph object (its content View stays
    // attached to it) and only swaps the render pass / framebuffer inside it.
    // The replaced objects may still be referenced by an in-flight command
    // buffer, so they are PARKED (released once every slot has been re-recorded,
    // see Impl::retireObject) instead of stopping the device mid-frame — this is
    // the policy-change path, and a host that animates a clear / depth policy
    // would otherwise stall the GPU every frame it does so. Setting them is legal
    // without recompiling the pipelines: a render pass is COMPATIBLE with another
    // when the attachments match (format / samples), and load-ops are not part of
    // that (VUID-vkCmdDraw-renderPass-02684).
    ::vsg::ref_ptr<::vsg::RenderGraph> graph =
        (built != t.passes.end()) ? built->second.graph : ::vsg::ref_ptr<::vsg::RenderGraph>();
    if (built != t.passes.end()) {
        impl->retireObject(built->second.render_pass);
        impl->retireObject(built->second.render_pass_transient);
        impl->retireObject(built->second.framebuffer);
    }
    if (graph == nullptr) {
        graph = impl->makePassGraph(t, plan.has_depth, clear_color);
    }
    graph->renderPass  = plan.variant.transient ? render_pass_transient : render_pass;
    graph->framebuffer = framebuffer;
    if (plan.has_color && !graph->clearValues.empty()) {
        // The colour clear value follows the pass' current request (a rebuilt
        // pass may have just STARTED clearing). The depth entry — if any — keeps
        // the target's depth clear value.
        graph->clearValues[0].color = VkClearColorValue{
            { clear_color.r, clear_color.g, clear_color.b, clear_color.a }
        };
    }

    Impl::Target::PassObjects objects;
    objects.render_pass            = render_pass;
    objects.render_pass_transient = render_pass_transient;
    objects.framebuffer           = framebuffer;
    objects.graph                 = graph;
    objects.load_depth            = plan.depth_load;
    objects.color_clear           = plan.want_color_clear;
    objects.want_color_clear      = plan.want_color_clear;
    objects.want_depth_clear      = plan.want_depth_clear;
    objects.clear_color           = clear_color;
    objects.order                 = impl->request.order;
    objects.transient             = plan.variant.transient;
    impl->publishPass(t, key, objects, plan.has_color);

    if (impl->command_graph != nullptr) {
        if (built == t.passes.end()) {
            // Only a NEW pass' graph has to be added: a rebuilt pass' graph never
            // left the command graph (it is the same object).
            impl->command_graph->children.push_back(graph);
        }
        // Reorder every off-screen graph into a dependency-valid sequence — each
        // consumer is recorded after the targets it samples (see
        // reconcileOffscreenOrder() for why creation order alone is not enough).
        reconcileOffscreenOrder();
    }
    return graph;
}

void VsgRenderer::reconcileOffscreenOrder()
{
    // Keeps the command graph's child render graphs in a dependency-valid RECORD
    // order: a screen pass that samples another target reads that target's colour
    // texture, and the sample is only CURRENT when the producer's graph is recorded
    // first. The three phases (collect → order → apply) are named units; each
    // explains what it guarantees.
    if (impl->command_graph == nullptr) {
        return;
    }
    const auto win = impl->targets.find(nullptr);
    if (win == impl->targets.end() || win->second.graph == nullptr) {
        return;
    }
    Impl::RecordPlan plan;
    plan.window_graph = win->second.graph;
    impl->fillRecordPlan(plan);
    impl->orderRecordPlan(plan);
    impl->applyRecordPlan(plan);
}

void VsgRenderer::Impl::fillRecordPlan(RecordPlan& plan) const
{
    const auto& children = command_graph->children;
    // A pass whose retained slot was RETIRED (its views detached because the pass
    // did not execute this frame) must not be recorded at all. Each pass graph is
    // its own render pass now, so leaving a retired one in the command graph would
    // execute that pass' load-ops every frame with NO content — a disabled clearing
    // pass would go on clearing a target the active passes just drew into, which is
    // the opposite of retiring it (see retireInactivePassSlots).
    const auto pass_records = [](const Target& owner, const SlotKey& key) {
        if (const auto it = owner.content_slots.find(key); it != owner.content_slots.end()) {
            return !it->second.detached;
        }
        if (const auto it = owner.screen_slots.find(key); it != owner.screen_slots.end()) {
            return !it->second.detached;
        }
        if (const auto it = owner.program_slots.find(key); it != owner.program_slots.end()) {
            return !it->second.detached;
        }
        return true; // no retained slot (a direct-driver pass): nothing to retire
    };
    // Inside a target the graphs must record in the passes' explicit pipeline order
    // (setPassOrder) — the position each pass' content would have occupied as a View
    // of a single target-wide render pass. Taking that order from the slot map
    // (pointer order) would let a target's SECOND pass record before its first, so
    // the second pass' colour clear would wipe the first pass' draws.
    const auto graph_order = [](const Target& owner, const ::vsg::ref_ptr<::vsg::RenderGraph>& graph) {
        for (const auto& pass : owner.passes) {
            if (pass.second.graph == graph) {
                return pass.second.order;
            }
        }
        return std::numeric_limits<int>::max();
    };
    for (const auto& entry : targets) {
        if (entry.first == nullptr) {
            continue;
        }
        // Seed from the order the graphs are recorded in RIGHT NOW, so passes
        // carrying the same explicit order keep their relative position, then append
        // the ones created since the last reconcile.
        std::vector<::vsg::ref_ptr<::vsg::RenderGraph>> graphs;
        for (const auto& child : children) {
            if (child == plan.window_graph) {
                continue;
            }
            for (const auto& pass : entry.second.passes) {
                if (pass.second.graph == child && pass_records(entry.second, pass.first)) {
                    graphs.push_back(pass.second.graph);
                    break;
                }
            }
        }
        for (const auto& pass : entry.second.passes) {
            if (pass.second.graph != nullptr && pass_records(entry.second, pass.first) &&
                std::find(graphs.begin(), graphs.end(), pass.second.graph) == graphs.end()) {
                graphs.push_back(pass.second.graph);
            }
        }
        if (!graphs.empty()) {
            std::stable_sort(graphs.begin(), graphs.end(),
                             [&entry, &graph_order](const ::vsg::ref_ptr<::vsg::RenderGraph>& lhs,
                                                    const ::vsg::ref_ptr<::vsg::RenderGraph>& rhs) {
                                 return graph_order(entry.second, lhs) < graph_order(entry.second, rhs);
                             });
            plan.graphs_of.emplace(entry.first, std::move(graphs));
        }
    }
    // The targets recorded RIGHT NOW, in child order: the stable tie-break seed.
    std::set<vine::graphics::RenderTarget*> seen;
    for (const auto& child : children) {
        if (child == plan.window_graph) {
            continue;
        }
        for (const auto& entry : plan.graphs_of) {
            if (std::find(entry.second.begin(), entry.second.end(), child) == entry.second.end()) {
                continue;
            }
            if (seen.insert(entry.first).second) {
                plan.present.push_back(entry.first);
            }
            break;
        }
    }
}

void VsgRenderer::Impl::orderRecordPlan(RecordPlan& plan) const
{
    // Sampling edges: a consumer depends on every source it samples (screen slot
    // keys carry the sampled target; program slots are keyed by it). Self-sampling
    // is rejected on attach and mutual same-frame sampling (ping-pong inside one
    // frame) is not a supported pattern, so the edge graph is acyclic in practice; a
    // cycle would only leave targets in their current order.
    std::map<vine::graphics::RenderTarget*, std::size_t> index_of;
    for (std::size_t i = 0; i < plan.present.size(); ++i) {
        index_of.emplace(plan.present[i], i);
    }
    std::vector<GraphOrderEdge> edges;
    for (auto* t : plan.present) {
        const auto entry = targets.find(t);
        if (entry == targets.end()) {
            continue;
        }
        const auto& target     = entry->second;
        const auto  add_source = [&](vine::graphics::RenderTarget* source) {
            if (source == nullptr || source == t) {
                return;
            }
            const auto src = index_of.find(source);
            if (src == index_of.end()) {
                return; // the source is not recorded this frame: no edge
            }
            edges.push_back(GraphOrderEdge{ index_of[t], src->second });
        };
        // A DEPTH BORROW is a dependency too, and not a sampling one: the borrower's
        // pass LOADs (tests against) the depth the source's pass writes this frame.
        // Without an edge here the order came from the build order alone, so a
        // borrower whose graph happened to be created before the source's was
        // RECORDED FIRST and tested against the previous frame's depth — a silent
        // one-frame lag, which no validation layer reports (the layouts match; only
        // the write→read dependency is wrong). The barrier applied in phase 3 then
        // also sits after the source, i.e. between the two, as it must.
        if (target.depth_source != nullptr) {
            add_source(target.depth_source);
        }
        // A slot's sampled target is a slot attribute (its key is the owning pass),
        // so the dependency edges come from the attribute. A retired (detached) slot
        // is not recorded, so it contributes no edge.
        for (const auto& slot : target.screen_slots) {
            if (!slot.second.detached) {
                add_source(const_cast<vine::graphics::RenderTarget*>(slot.second.source_target));
            }
        }
        for (const auto& slot : target.program_slots) {
            if (!slot.second.detached) {
                add_source(const_cast<vine::graphics::RenderTarget*>(slot.second.source_target));
            }
        }
    }
    plan.order.reserve(plan.present.size());
    for (const std::size_t index : stableTopologicalOrder(plan.present.size(), edges)) {
        plan.order.push_back(plan.present[index]);
    }
}

void VsgRenderer::Impl::applyRecordPlan(const RecordPlan& plan)
{
    auto& children = command_graph->children;
    children.clear();
    for (auto* t : plan.order) {
        const auto graphs = plan.graphs_of.find(t);
        if (graphs == plan.graphs_of.end()) {
            continue;
        }
        for (const auto& graph : graphs->second) {
            children.push_back(graph);
        }
        // After the LAST pass graph of a target whose depth another target borrows,
        // insert that borrower's depth-share barrier so its LOAD / depth test sees
        // this target's writes (both share one depth image in the attachment layout).
        for (const auto& entry : targets) {
            const auto& other = entry.second;
            if (other.depth_source == t && other.depth_share_barrier != nullptr) {
                children.push_back(other.depth_share_barrier);
            }
        }
    }
    children.push_back(plan.window_graph);
}

void VsgRenderer::releaseWindowLayer(vine::raw_ptr<const vine::graphics::Camera> camera, int order)
{
    if (camera == nullptr || !impl->initialized) {
        return;
    }
    // Window content slots live in the window target (nullptr key) of the
    // output-target table, keyed by (camera, explicit pass order). Off-screen
    // slots are released together with their whole target (releaseRenderTarget).
    auto& t  = impl->entryFor(nullptr);
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
    // the cache drop needs the counted device wait (see Impl::unhookTargetPasses).
    impl->waitForIdle();
    it->second.bridge.clearCache();
    t.content_slots.erase(it);
}

void VsgRenderer::releaseRenderTarget(vine::graphics::RenderTarget* target)
{
    if (target == nullptr || !impl->initialized) {
        return;
    }
    bool released = false;
    auto ot       = impl->targets.find(target);
    if (ot != impl->targets.end()) {
        // Stop recording the target's off-screen pass graphs and make the release
        // safe before dropping its images / views / render passes / slots (the
        // counted device wait lives in that unhook).
        impl->unhookTargetPasses(ot->second);
        // Drop the target's whole table entry (its off-screen attachments /
        // render graph / content slots). Any sampling slot that reads it lives
        // in another target's slot tables and is removed right below.
        impl->targets.erase(ot);
        released = true;
    }
    // A target that BORROWED the removed target's depth (RenderTarget::
    // shareDepth) now references a destroyed depth image, and the barrier that
    // ordered the two graphs still points at it. Drop the borrow and force the
    // borrower to rebuild with its own depth: otherwise its framebuffer keeps
    // a dead attachment (and the command graph is ordered around a dead
    // barrier). Clearing the recorded size re-enters the rebuild path on its
    // next draw.
    for (auto& entry : impl->targets) {
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
    for (auto& target_entry : impl->targets) {
        auto& t = target_entry.second;
        // Collect first: erasing while the visitor walks the tables would
        // invalidate the walk.
        std::vector<std::pair<Impl::Target::SlotKind, SlotKey>> drop;
        t.forEachSlot([&](const SlotKey& key, auto& slot, Impl::Target::SlotKind kind) {
            if constexpr (requires { slot.source_target; }) {
                if (slot.source_target == target) {
                    impl->detachSlotView(t, target_entry.first, key, slot.view);
                    drop.emplace_back(kind, key);
                }
            }
        });
        for (const auto& [kind, key] : drop) {
            // Destructive (the slot's node goes with it), so this keeps the
            // counted device wait rather than parking the view — see
            // erasePassSlotsFromTarget.
            impl->waitForIdle();
            t.eraseSlot(kind, key);
            released = true;
        }
    }
    if (released) {
        // The remaining command-graph child order may have changed (a sampling
        // edge disappeared, a graph was detached).
        reconcileOffscreenOrder();
        V_LOGI("[VsgRenderer] released GPU resources for removed render target");
    }
}

void VsgRenderer::placeViewByOrder(::vsg::ref_ptr<::vsg::RenderGraph> graph,
                                   vine::graphics::RenderTarget* target,
                                   const ::vsg::ref_ptr<::vsg::View>& view,
                                   int order)
{
    auto& t = impl->entryFor(target);
    if (graph == nullptr || view == nullptr) {
        return;
    }
    auto& children = graph->children; // RenderGraph is a Group: children are ref_ptr<Node>
    // Drop any previous position, then insert so the children stay ascending
    // by each slot's explicit order: content slots carry theirs, fullscreen-
    // program and PiP / present screen slots carry theirs, and any child with
    // no known slot sorts last. Reordering render-graph children only changes
    // per-frame record order within the (single) render pass, so no
    // recompilation is needed.
    for (auto it = children.begin(); it != children.end();) {
        if (it->get() == view.get()) {
            it = children.erase(it);
        }
        else {
            ++it;
        }
    }
    const auto child_order = [&](const ::vsg::ref_ptr<::vsg::Node>& child) -> int {
        for (const auto& kv : t.content_slots) {
            if (kv.second.ready && kv.second.view.get() == child.get()) {
                return kv.second.order;
            }
        }
        for (const auto& kv : t.program_slots) {
            if (kv.second.ready && kv.second.view.get() == child.get()) {
                return kv.second.order;
            }
        }
        for (const auto& kv : t.screen_slots) {
            if (kv.second.ready && kv.second.view.get() == child.get()) {
                return kv.second.order;
            }
        }
        return std::numeric_limits<int>::max();
    };
    auto it = children.begin();
    for (; it != children.end(); ++it) {
        if (child_order(*it) > order) {
            break;
        }
    }
    children.insert(it, view); // ref_ptr<View> -> ref_ptr<Node> (View is a Node)
}

bool VsgRenderer::readColorBuffer(vine::graphics::RenderTarget* target, int attachment,
                                  std::vector<std::uint8_t>& outPixels)
{
    auto* built = impl->readbackTarget(target);
    if (built == nullptr) {
        return false;
    }
    if (attachment < 0 || static_cast<std::size_t>(attachment) >= built->color_images.size()) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error,
                      vine::graphics::DiagnosticCategory::ContentSkipped,
                      formatDiagnostic(u8"readColorBuffer: attachment %d is out of range for this target", attachment));
        return false;
    }
    // The packed-RGBA8 output contract needs a same-format blit. A float
    // attachment would have to be converted (and the caller told how), so it is
    // reported as unsupported instead of returning wrongly packed bytes.
    if (target->colorFormat(attachment) != vine::graphics::RenderTarget::ColorFormat::RGBA8) {
        reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                      vine::graphics::DiagnosticCategory::ContentSkipped,
                      formatDiagnostic(u8"readColorBuffer: attachment %d is not RGBA8 (packed RGBA8 readback only)", attachment));
        return false;
    }

    const std::uint32_t width  = static_cast<std::uint32_t>(built->width);
    const std::uint32_t height = static_cast<std::uint32_t>(built->height);

    // The frame that wrote this target must be complete before its image is
    // copied out; this call is synchronous by contract. Counted, like every other
    // device-wide idle this backend takes (deviceWaitCount).
    impl->waitForIdle();

    auto device   = impl->window->getDevice();
    auto physical = impl->window->getPhysicalDevice();
    auto source   = built->color_images[attachment];
    if (device == nullptr || physical == nullptr || source == nullptr) {
        return false;
    }
    const VkFormat format = source->format;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(*physical, format, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0 ||
        (properties.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0) {
        reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                      vine::graphics::DiagnosticCategory::ContentSkipped,
                      formatDiagnostic(u8"readColorBuffer: format %d cannot be blitted", static_cast<int>(format)));
        return false;
    }

    // A linear host-visible image the pixels land in (rows may carry padding,
    // see rowPitch below): the same shape the standalone colour probe uses.
    auto destination         = ::vsg::Image::create();
    destination->imageType   = VK_IMAGE_TYPE_2D;
    destination->format      = format;
    destination->extent      = VkExtent3D{ width, height, 1 };
    destination->mipLevels   = 1;
    destination->arrayLayers = 1;
    destination->samples     = VK_SAMPLE_COUNT_1_BIT;
    destination->tiling      = VK_IMAGE_TILING_LINEAR;
    destination->usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    destination->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    destination->compile(device);
    auto memory = impl->hostVisibleMemory(device.get(), destination->getMemoryRequirements(device->deviceID));
    destination->bind(memory, 0);

    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    auto                          commands = ::vsg::Commands::create();
    // The attachment is sampleable when the render pass ends (its final layout),
    // so it is made a transfer source here and handed back below: the next
    // frame samples it again.
    commands->addChild(::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, source, range),
        ::vsg::ImageMemoryBarrier::create(0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range)));

    VkImageBlit region{};
    region.srcSubresource = VkImageSubresourceLayers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1]  = VkOffset3D{ static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), 1 };
    region.dstSubresource = VkImageSubresourceLayers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[1]  = VkOffset3D{ static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), 1 };
    auto blit            = ::vsg::BlitImage::create();
    blit->srcImage       = source;
    blit->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit->dstImage       = destination;
    blit->dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blit->regions.push_back(region);
    blit->filter = VK_FILTER_NEAREST;
    commands->addChild(blit);

    commands->addChild(::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, source, range),
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range)));

    if (!impl->submitOneShot(commands)) {
        return false;
    }

    VkImageSubresource  sub_resource{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout sub_layout{};
    vkGetImageSubresourceLayout(*device, destination->vk(device->deviceID), &sub_resource, &sub_layout);
    auto mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(memory, sub_layout.offset, 0,
                                                               ::vsg::Data::Properties{ format }, sub_layout.rowPitch * height);

    // Pack the rows: a linear image may pad them (rowPitch), the caller gets
    // tightly packed RGBA8.
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    outPixels.resize(row_bytes * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(outPixels.data() + static_cast<std::size_t>(row) * row_bytes,
                    mapped->dataPointer(static_cast<std::size_t>(row) * sub_layout.rowPitch), row_bytes);
    }
    return true;
}

bool VsgRenderer::readDepthBuffer(vine::graphics::RenderTarget* target, std::vector<float>& outDepths)
{
    auto* built = impl->readbackTarget(target);
    if (built == nullptr) {
        return false;
    }
    if (built->depth_image == nullptr) {
        if (built->depth_source != nullptr) {
            // The depth is real but owned by the target it was borrowed from
            // (shareDepth): it is read through the SOURCE, not the borrower, so
            // this is a documented unsupported case rather than a silent one.
            reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                          vine::graphics::DiagnosticCategory::ChannelIgnored,
                          u8"readDepthBuffer: this target borrows its depth (shareDepth); read the source target instead");
        }
        return false;
    }

    // Only the formats whose texels ARE the value are read back: the packed
    // D24_UNORM_S8_UINT needs the implementation's bit convention to decode, so
    // it is reported as unsupported rather than decoded on a guess.
    const VkFormat      format      = built->depth_image->format;
    const std::size_t   texel_bytes = (format == VK_FORMAT_D32_SFLOAT) ? 4u : (format == VK_FORMAT_D16_UNORM) ? 2u : 0u;
    if (texel_bytes == 0u) {
        reportFailure(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                      formatDiagnostic(u8"readDepthBuffer: format %d is packed (depth + stencil in one texel) and is "
                                       u8"not decoded; use D32_SFLOAT or D16_UNORM for depth readback",
                                       static_cast<int>(format)));
        return false;
    }

    const std::uint32_t width  = static_cast<std::uint32_t>(built->width);
    const std::uint32_t height = static_cast<std::uint32_t>(built->height);

    // Synchronous by contract, and counted: see readColorBuffer.
    impl->waitForIdle();

    // The transfer needs the device; the physical device / queue family come from
    // the one-shot submit, which guards for them itself.
    auto device = impl->window->getDevice();
    if (device == nullptr) {
        return false;
    }

    // A depth image cannot be blitted (no blit support in the format's feature
    // set), and a LINEAR depth image is not guaranteed either, so the copy goes
    // through a host-visible staging buffer: vkCmdCopyImageToBuffer is mandatory
    // for depth formats.
    const VkDeviceSize byte_count = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) *
                                    static_cast<VkDeviceSize>(texel_bytes);
    auto buffer = ::vsg::Buffer::create(byte_count, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE);
    buffer->compile(device);
    auto memory = impl->hostVisibleMemory(device.get(), buffer->getMemoryRequirements(device->deviceID));
    buffer->bind(memory, 0);

    // The depth image is left in whatever layout the last pass of this target
    // ends in: a pass that promoted it leaves SHADER_READ_ONLY_OPTIMAL, an
    // ordinary one leaves the attachment layout. There is no single
    // target-level render pass any more, so the layout comes from the target's
    // recorded depth policy.
    const VkImageLayout depth_layout = built->depth_sampleable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    auto                          commands = ::vsg::Commands::create();
    commands->addChild(::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                          depth_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, built->depth_image, range)));

    VkBufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                = 0;
    region.bufferImageHeight              = 0;
    region.imageSubresource                = VkImageSubresourceLayers{ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    region.imageOffset                     = VkOffset3D{ 0, 0, 0 };
    region.imageExtent                     = VkExtent3D{ width, height, 1 };
    auto copy                              = ::vsg::CopyImageToBuffer::create();
    copy->srcImage                         = built->depth_image;
    copy->srcImageLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copy->dstBuffer                        = buffer;
    copy->regions.push_back(region);
    commands->addChild(copy);

    commands->addChild(::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_HOST_BIT,
        0,
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_layout,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, built->depth_image, range)));

    if (!impl->submitOneShot(commands)) {
        return false;
    }

    auto mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(memory, 0, 0,
                                                               ::vsg::Data::Properties{ VK_FORMAT_R8_UNORM }, byte_count);
    outDepths.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < outDepths.size(); ++i) {
        if (texel_bytes == 4u) {
            float value = 0.0f;
            std::memcpy(&value, mapped->dataPointer(i * 4u), 4u);
            outDepths[i] = value;
        }
        else {
            std::uint16_t value = 0u;
            std::memcpy(&value, mapped->dataPointer(i * 2u), 2u);
            outDepths[i] = static_cast<float>(value) / 65535.0f;
        }
    }
    return true;
}

V_VSG_NS_END
