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

    // A rebuild (target resized or its attachment shape changed) must first
    // release the previous pass graphs: they may still be referenced by an
    // in-flight command buffer, and every content slot compiled against them
    // must be dropped with them (per-view pipelines bind the old render pass).
    if (t.attachments_built) {
        for (auto& pass : t.passes) {
            removeGraphChild(impl->command_graph.get(), pass.second.graph);
        }
        // Wait for any in-flight command buffer that may still reference the
        // old framebuffer/images before their Vk handles are destroyed.
        waitForIdle(impl->viewer.get());
        // Every slot this target held (content views, PiP screen views and
        // fullscreen-program views) was a child of the graph being dropped:
        // they must go with it, or their retained view would never be recorded
        // again while still reporting itself as ready.
        for (auto& slot_entry : t.content_slots) {
            slot_entry.second.bridge.clearCache();
            const auto& view = slot_entry.second.view;
            auto&       queue = impl->pending_compile_views;
            queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
        }
        t.content_slots.clear();
        t.screen_slots.clear();
        t.program_slots.clear();
        t.color_images.clear();
        t.color_views.clear();
        t.depth_image          = {};
        t.depth_view           = {};
        t.passes.clear();
        t.attachments_built    = false;
        t.depth_seeded         = false;
        t.any_load_pass        = false;
        t.color_seeded         = false;
        t.depth_source         = nullptr;
        t.depth_source_view    = {};
        t.depth_share_barrier  = {};
        t.depth_sampleable     = false;
        t.depth_borrow_pending_reported = false;
        t.graph                = {};
        t.depth_on_shader_set  = {};
        t.depth_testonly_shader_set = {};
        t.depth_off_shader_set = {};
        t.width                = 0;
        t.height               = 0;
        t.build_key            = {};
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
    // Depth may be OWNED (allocated below) or BORROWED from an earlier target
    // in the same frame (RenderTarget::shareDepth): the deferred-lit composite
    // reuses the G-buffer's depth so forward content can test against it.
    // A borrow is only possible when the source's depth image is usable as THIS
    // framebuffer's depth attachment exactly as it stands, which the checks
    // below establish; anything else must not reach vkCreateFramebuffer.
    // A borrow whose source has since been RELEASED cannot be honoured either —
    // the source's VkImage is gone — so such a target builds with its own depth
    // instead of failing to build forever (see releaseRenderTarget).
    vine::graphics::RenderTarget* const depth_src = target->depthSource();
    bool borrowed = depth_src != nullptr && t.unusable_depth_source != depth_src;
    if (borrowed) {
        // Three ways a borrow is unusable, each of which was silent before:
        //  - the source has not rendered yet this frame (no depth image yet);
        //  - the extents differ: Vulkan requires every framebuffer attachment to
        //    have the framebuffer's dimensions, so a half-resolution composite
        //    borrowing a full-resolution depth built an INVALID framebuffer
        //    (VUID-VkFramebufferCreateInfo-pAttachments-00880) and then rendered
        //    undefined;
        //  - the source promoted its depth to a sampled texture: its image is in
        //    SHADER_READ_ONLY_OPTIMAL, which no render pass may attach, so every
        //    frame tripped VUID-VkImageMemoryBarrier-oldLayout-01197 (the
        //    depth-share barrier assumes the attachment layout) and drew
        //    nothing.
        const auto  src_it   = impl->targets.find(depth_src);
        const bool  src_ready = src_it != impl->targets.end() && src_it->second.depth_view != nullptr;
        const char* reason    = nullptr;
        if (!src_ready) {
            // TRANSIENT: the source has no depth image yet (its pass has not
            // built this frame — e.g. an engine warm-up that ran this target's
            // consumer before the producer). Not remembered as unusable: this
            // frame builds with its own depth and the borrow is retried as soon
            // as the source exists (render()'s rebuild predicate). Reported once
            // per episode, so a source that never arrives is not silent either.
            if (!t.depth_borrow_pending_reported) {
                reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                              vine::graphics::DiagnosticCategory::ContentSkipped,
                              formatDiagnostic(u8"shared-depth target '%s': source '%s' has no depth image yet;"
                                               u8" this target builds its own depth and retries the borrow",
                                               target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str(),
                                               depth_src->name().empty() ? "(unnamed)" : depth_src->name().stdstr().c_str()));
                t.depth_borrow_pending_reported = true;
            }
            borrowed = false;
        }
        else if (src_it->second.width != static_cast<int>(w) || src_it->second.height != static_cast<int>(h)) {
            reason = "its source has a different size (a framebuffer attachment must have the framebuffer's dimensions)";
        }
        else if (src_it->second.depth_sampleable) {
            reason = "its source promoted its depth to a sampled texture (a sampled depth cannot be attached)";
        }
        if (reason != nullptr) {
            // PERSISTENT: a property of the setup, not of this frame — the same
            // source will stay unusable until the host changes it, so it is
            // remembered (reported once) and retried only when the host points
            // the borrow at a different source.
            reportFailure(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                          formatDiagnostic(u8"shared-depth target '%s': source '%s' cannot be borrowed (%s); this target"
                                           u8" builds its own depth",
                                           target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str(),
                                           depth_src->name().empty() ? "(unnamed)" : depth_src->name().stdstr().c_str(),
                                           reason));
            t.unusable_depth_source = depth_src;
            borrowed                = false;
        }
        else {
            // Honoured (or nothing to retry): re-arm the transient report.
            t.depth_borrow_pending_reported = false;
        }
    }

    std::vector<VkFormat> color_formats;
    color_formats.reserve(static_cast<std::size_t>(color_count));
    for (int i = 0; i < color_count; ++i) {
        color_formats.push_back(toColorFormat(target->colorFormat(i)));
    }

    // One colour image + view per attachment: each is written by fragment
    // output location i, then usable as a sampled texture on its own
    // (VK_IMAGE_USAGE_SAMPLED_BIT) or as a blit source for compositing.
    t.color_images.resize(static_cast<std::size_t>(color_count));
    t.color_views.resize(static_cast<std::size_t>(color_count));
    for (int i = 0; i < color_count; ++i) {
        auto color           = ::vsg::Image::create();
        color->imageType     = VK_IMAGE_TYPE_2D;
        color->format        = color_formats[static_cast<std::size_t>(i)];
        color->extent        = VkExtent3D{ w, h, 1 };
        color->mipLevels     = 1;
        color->arrayLayers   = 1;
        color->tiling        = VK_IMAGE_TILING_OPTIMAL;
        color->usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        color->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.color_images[static_cast<std::size_t>(i)] = color;
        // createImageView compiles the Image (creates VkImage + allocates/binds
        // device memory) and creates+compiles the ImageView (VkImageView).
        // WITHOUT this the VkImage/VkImageView stay VK_NULL_HANDLE and the
        // Framebuffer holds a corrupt handle -> vkCmdBeginRenderPass crashes.
        t.color_views[static_cast<std::size_t>(i)] = ::vsg::createImageView(device.get(), color, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (has_depth && !borrowed) {
        auto depth           = ::vsg::Image::create();
        depth->imageType     = VK_IMAGE_TYPE_2D;
        depth->format        = toDepthFormat(target->depthFormat());
        depth->extent        = VkExtent3D{ w, h, 1 };
        depth->mipLevels     = 1;
        depth->arrayLayers   = 1;
        depth->tiling        = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC is what readDepthBuffer() copies from (and what any
        // future depth dump needs): without it the image cannot even be
        // transitioned to TRANSFER_SRC_OPTIMAL, which validation reports as
        // VUID-VkImageMemoryBarrier-oldLayout-01212.
        depth->usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        depth->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.depth_image        = depth;
        t.depth_view         = ::vsg::createImageView(device.get(), depth, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    else if (borrowed) {
        // Borrow the source's depth image/view (it renders earlier this frame):
        // the framebuffer below attaches the shared depth, loaded not cleared.
        // The borrow was validated above, so the source image exists and matches
        // this framebuffer.
        const auto src_it = impl->targets.find(depth_src);
        // Remember WHICH source image this framebuffer borrowed: the source
        // replacing it (a rebuild) invalidates this framebuffer, and render()
        // detects that by comparing the two.
        t.depth_source_view = src_it->second.depth_view;
    }

    // What is shared — and what this function owns — is the ATTACHMENT SET:
    // the render pass, framebuffer and graph are per pass now (see passGraph),
    // because one render pass bakes ONE pair of attachment load-ops and a
    // clearing pass and a preserving pass cannot share one. Record the depth
    // value every pass of this target clears to (colour targets clear to the
    // reverse-Z far plane 0.0, depth-only targets to 1.0 — the value the window
    // forward path's geometry test expects, so off-screen content rasterises
    // the same way) and mark the attachments built.
    t.depth_clear_value = has_color ? 0.0f : 1.0f;
    // Whether this target's depth may be promoted to a sampleable texture is
    // part of the target's DESCRIPTION (RenderTarget::depthPromotion), so it is
    // known HERE rather than only once a pass is created: a consumer that
    // borrows this depth is validated in the same frame, before ANY of this
    // target's passes exists, so recording it at pass-creation time would be too
    // late. A target that had to fall back to its own depth promotes on its own
    // terms (it is the owner of the image a pass would sample).
    t.depth_sampleable  = has_depth && !borrowed && target->depthPromotion();
    t.attachments_built = has_color || has_depth;

    // A (re)built target created FRESH colour views: any OTHER target that
    // samples this one (PiP screen slots / fullscreen-program slots) still
    // holds the OLD views and would sample a stale, no-longer-drawn image
    // (its stale check only watches source size, which a same-size rebuild
    // does not change). Drop those slots so the next drawScreenTexture /
    // drawScreenProgram call reattaches against the new attachments.
    for (auto& entry : impl->targets) {
        auto& other = entry.second;
        if (entry.first == target || (!other.attachments_built && other.graph == nullptr)) {
            continue;
        }
        // A slot's sampled target is a slot ATTRIBUTE, so consumers are found
        // by inspecting it (a slot's key is its owning pass).
        const auto forget_view = [this](const ::vsg::ref_ptr<::vsg::View>& view) {
            auto& queue = impl->pending_compile_views;
            queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
        };
        // The graph a slot's view was attached to: the window target's shared
        // swapchain graph, or the off-screen pass' own graph.
        const auto slot_graph = [&other](const SlotKey& key) -> ::vsg::ref_ptr<::vsg::RenderGraph> {
            const auto pass = other.passes.find(key);
            return pass == other.passes.end() ? other.graph : pass->second.graph;
        };
        for (auto it = other.screen_slots.begin(); it != other.screen_slots.end();) {
            if (it->second.source_target == target) {
                if (auto graph = slot_graph(it->first); graph != nullptr) {
                    removeGraphChild(graph.get(), it->second.view);
                }
                it = other.screen_slots.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = other.program_slots.begin(); it != other.program_slots.end();) {
            if (it->second.source_target == target) {
                if (auto graph = slot_graph(it->first); graph != nullptr) {
                    removeGraphChild(graph.get(), it->second.view);
                }
                forget_view(it->second.view);
                it = other.program_slots.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (borrowed) {
        // Create the depth-share barrier now (the source's graph is already
        // built earlier this frame). reconcileOffscreenOrder() inserts it into
        // the command graph right after the source's render graph so the depth
        // writes are visible before this pass LOADs / tests them.
        auto src_it = impl->targets.find(depth_src);
        if (src_it != impl->targets.end() && src_it->second.depth_image != nullptr) {
            // The depth image may be a COMBINED depth/stencil format (D24 ->
            // VK_FORMAT_D24_UNORM_S8_UINT). With separateDepthStencilLayouts
            // disabled, a barrier's subresource range must cover BOTH aspects
            // of such a format (VUID-VkImageMemoryBarrier-image-03320), so the
            // aspect mask follows the source's format instead of assuming a
            // depth-only image.
            const VkFormat src_depth_format = toDepthFormat(depth_src->depthFormat());
            const bool     has_stencil =
                src_depth_format == VK_FORMAT_D24_UNORM_S8_UINT ||
                src_depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                src_depth_format == VK_FORMAT_D16_UNORM_S8_UINT;
            const VkImageAspectFlags aspect_flags =
                VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
            auto imb = ::vsg::ImageMemoryBarrier::create(
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                src_it->second.depth_image,
                VkImageSubresourceRange{ aspect_flags, 0, 1, 0, 1 });
            t.depth_share_barrier = ::vsg::PipelineBarrier::create(
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                0,
                imb);
        }
        t.depth_source = depth_src;
    }

    // Record the shape these attachments were built from, so render() rebuilds
    // when the host changes any of it (see Target::BuildKey).
    t.build_key = Impl::Target::BuildKey::of(*target);
    std::fprintf(stderr, "[VsgRenderer] EXPERIMENTAL off-screen target '%s' %ux%u attached\n",
                 target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str(), w, h);
    ++impl->offscreen_build_count;
    // NOTE: no compile here — no pass graph exists until the first pass into
    // this target asks for one (passGraph); setupContentSlot() compiles then.
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
    auto device = impl->window->getOrCreateDevice();
    if (device == nullptr) {
        return {};
    }

    // The target's attachment set (owned by buildOffscreenTarget): every pass of
    // this target attaches exactly these images, only its load-ops differ.
    const bool     has_color = !t.color_views.empty();
    const bool     borrowed  = t.depth_source != nullptr;
    const bool     has_depth = borrowed || t.depth_view != nullptr;
    const VkFormat depth_format =
        has_depth ? (borrowed ? toDepthFormat(t.depth_source->depthFormat()) : toDepthFormat(target->depthFormat()))
                  : VK_FORMAT_UNDEFINED;
    std::vector<VkFormat> color_formats;
    color_formats.reserve(t.color_views.size());
    for (std::size_t i = 0; i < t.color_views.size(); ++i) {
        color_formats.push_back(toColorFormat(target->colorFormat(static_cast<int>(i))));
    }

    // Creates the render pass + framebuffer for one load-op combination over
    // this target's attachments.
    //
    // @param pass_color_clear Whether this pass clears (rather than loads) colour.
    // @param depth_load       Whether the depth attachment is loaded, not cleared.
    // @param promote          Whether the pass may leave depth sampleable.
    // @param seed             Selects the CLEAR variant of a depth-LOAD pass.
    const auto make_pass_objects = [&](bool pass_color_clear, bool depth_load, bool promote, bool seed) {
        ::vsg::ref_ptr<::vsg::RenderPass> render_pass;
        if (!has_color) {
            render_pass = makeDepthOnlyRenderPass(device.get(), depth_format);
        }
        else if (has_depth && depth_load) {
            render_pass = makeDepthLoadRenderPass(device.get(), color_formats, depth_format, seed, pass_color_clear);
        }
        else {
            render_pass = makeSampleableRenderPass(device.get(), color_formats, depth_format, promote, pass_color_clear);
        }
        ::vsg::ImageViews attachments;
        attachments.reserve(t.color_views.size() + (has_depth ? 1u : 0u));
        for (const auto& view : t.color_views) {
            attachments.push_back(view);
        }
        if (has_depth) {
            const auto source = impl->targets.find(t.depth_source);
            attachments.push_back(borrowed && source != impl->targets.end() ? source->second.depth_view : t.depth_view);
        }
        return std::pair{ render_pass,
                          ::vsg::Framebuffer::create(render_pass, attachments, static_cast<uint32_t>(t.width),
                                                     static_cast<uint32_t>(t.height), 1) };
    };

    // The pass already has its objects: reuse them, updating whatever the pass
    // re-requested this frame (its record position and its own clear colour).
    if (const auto built = t.passes.find(key); built != t.passes.end()) {
        // A pass that changed its explicit pipeline order moves its graph to the
        // matching record position (see reconcileOffscreenOrder).
        if (built->second.order != impl->request.order) {
            built->second.order = impl->request.order;
            reconcileOffscreenOrder();
        }
        // A pass re-requesting a clear updates ITS OWN graph — never its
        // siblings': a pass clears to its own request (§28), so one pass' clear
        // must not become another pass' background colour.
        const bool want_color_clear = !t.clear_seen || impl->request.presenting;
        const ::vsg::vec4 wanted_color =
            (t.clear_seen || impl->request.presenting) ? t.clear_color : ::vsg::vec4{ 0.2f, 0.2f, 0.2f, 1.0f };
        if (built->second.color_clear != want_color_clear) {
            // The colour load-op is baked into the render pass, so a pass that
            // starts (or stops) clearing needs its render pass and framebuffer
            // re-created; the pass' views stay children of its graph.
            waitForIdle(impl->viewer.get());
            auto [render_pass, framebuffer] =
                make_pass_objects(want_color_clear, built->second.load_depth, /*promote*/ false, /*seed*/ false);
            built->second.render_pass = render_pass;
            built->second.framebuffer = framebuffer;
            built->second.color_clear = want_color_clear;
            if (built->second.graph != nullptr) {
                built->second.graph->renderPass  = render_pass;
                built->second.graph->framebuffer = framebuffer;
            }
        }
        if (built->second.clear_color != wanted_color && built->second.graph != nullptr &&
            !built->second.graph->clearValues.empty()) {
            built->second.clear_color                 = wanted_color;
            built->second.graph->clearValues[0].color = VkClearColorValue{
                { wanted_color.r, wanted_color.g, wanted_color.b, wanted_color.a }
            };
        }
        return built->second.graph;
    }

    // This pass' OWN clear request (the open pass scope) decides its load-ops.
    // Two rules keep the historical behaviour for existing hosts:
    //  - a target on which clear() was NEVER called clears both its colour and
    //    its depth every pass, exactly as the former single render pass did —
    //    such a target is normally a sampling destination (a PiP / post-chain
    //    stage) that expects a clean plate every frame;
    //  - a target whose colour image has never been cleared must clear it
    //    whatever this pass asked for: the image is UNDEFINED and a render pass
    //    may not LOAD an UNDEFINED image.
    const bool want_color_clear = !t.clear_seen || impl->request.presenting;
    const bool want_depth_clear = !t.clear_seen || (impl->request.presenting && impl->request.clear_depth);
    const bool color_clear      = want_color_clear || !t.color_seeded;
    const ::vsg::vec4 clear_color =
        (t.clear_seen || impl->request.presenting) ? t.clear_color : ::vsg::vec4{ 0.2f, 0.2f, 0.2f, 1.0f };

    const PassRenderPassPlan plan = planPassRenderPass(color_clear, want_depth_clear, has_depth,
                                                       has_depth && !borrowed && target->depthPromotion(),
                                                       t.any_load_pass, t.depth_seeded, borrowed);
    const bool depth_load = has_depth && plan.depth_load == VK_ATTACHMENT_LOAD_OP_LOAD;

    // A pass that LOADs depth must find the image in the ATTACHMENT layout, so
    // no pass of this target may promote it. A target whose earlier passes were
    // allowed to promote (the first pass cleared depth and nothing loaded it)
    // has to re-create them without promotion before this pass can load: their
    // framebuffer goes with the render pass, so the swap waits for idle.
    if (depth_load && t.depth_sampleable) {
        waitForIdle(impl->viewer.get());
        for (auto& built : t.passes) {
            auto [render_pass, framebuffer] =
                make_pass_objects(built.second.color_clear, built.second.load_depth, /*promote*/ false, /*seed*/ false);
            built.second.render_pass      = render_pass;
            built.second.render_pass_seed = {};
            built.second.framebuffer      = framebuffer;
            built.second.seeded           = false;
            if (built.second.graph != nullptr) {
                built.second.graph->renderPass  = render_pass;
                built.second.graph->framebuffer = framebuffer;
            }
        }
        t.depth_sampleable = false;
    }

    // A pass that preserves an as-yet undefined depth image records the CLEAR
    // (seed) variant for THIS frame; submitFrame() swaps it for the steady LOAD
    // variant afterwards. The two differ only in the depth load-op, so they are
    // render-pass compatible and share the framebuffer.
    const bool                        needs_seed = has_depth && !borrowed && plan.seed_required;
    auto [render_pass, framebuffer] = make_pass_objects(color_clear, depth_load, plan.promote_depth, needs_seed);
    ::vsg::ref_ptr<::vsg::RenderPass> render_pass_seed;
    if (needs_seed) {
        render_pass_seed = render_pass;
        render_pass = make_pass_objects(color_clear, depth_load, plan.promote_depth, /*seed*/ false).first;
    }

    auto graph           = ::vsg::RenderGraph::create();
    graph->framebuffer   = framebuffer;
    graph->renderPass    = needs_seed ? render_pass_seed : render_pass;
    graph->renderArea    = VkRect2D{ { 0, 0 }, { static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) } };
    graph->contents      = VK_SUBPASS_CONTENTS_INLINE;
    graph->viewportState = ::vsg::ViewportState::create(
        VkExtent2D{ static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) });
    // Clear values match the attachment order (colour attachments in order,
    // then depth). Attachment 0 takes this pass' clear colour; extra MRT
    // attachments clear transparent black (empty regions stay black until a
    // fragment writes them).
    graph->clearValues.clear();
    for (std::size_t i = 0; i < t.color_views.size(); ++i) {
        VkClearValue value = {};
        if (i == 0u) {
            value.color = VkClearColorValue{ { clear_color.r, clear_color.g, clear_color.b, clear_color.a } };
        }
        graph->clearValues.push_back(value);
    }
    if (has_depth) {
        VkClearValue value  = {};
        value.depthStencil  = VkClearDepthStencilValue{ t.depth_clear_value, 0 };
        graph->clearValues.push_back(value);
    }

    Impl::Target::PassObjects& objects = t.passes[key];
    objects.render_pass      = render_pass;
    objects.render_pass_seed = render_pass_seed;
    objects.framebuffer      = framebuffer;
    objects.graph            = graph;
    objects.load_depth       = depth_load;
    objects.color_clear      = color_clear;
    objects.clear_color      = clear_color;
    objects.order            = impl->request.order;
    objects.seeded           = needs_seed;

    // The image is defined from now on and this pass' policy is part of the
    // target's: a later pass may LOAD the colour, and a pass that LOADs depth
    // forbids promotion for the whole target.
    t.color_seeded  = true;
    t.depth_seeded  = true;
    t.any_load_pass = t.any_load_pass || depth_load;
    if (depth_load) {
        // A pass that had to LOAD the depth leaves it in the attachment layout,
        // so the target's depth can no longer be sampled: readDepthBuffer and a
        // later borrow validation must both be told.
        t.depth_sampleable = false;
    }

    if (impl->command_graph != nullptr) {
        // Add the pass graph as the command graph's last child, then reorder
        // every off-screen graph into a dependency-valid sequence — each
        // consumer is recorded after the targets it samples (see
        // reconcileOffscreenOrder() for why creation order alone is not enough).
        impl->command_graph->children.push_back(graph);
        reconcileOffscreenOrder();
    }
    return graph;
}

void VsgRenderer::reconcileOffscreenOrder()
{
    // Keeps the command graph's child render graphs in a dependency-valid
    // RECORD order. The engine can build a target's graph out of dependency
    // order — a producer (re)built after its consumers existed (resize or
    // depth-policy change in render()), or a consumer wired to a producer
    // built later — so creation order alone is not enough: a screen pass that
    // samples another target (drawScreenTexture / drawScreenProgram) reads
    // that target's colour texture, and the sample is only CURRENT when the
    // producer's graph is recorded before the consumer's in the same frame.
    // This orders every off-screen graph by its sampling edges (source before
    // the targets that sample it), seeding ties with the current child order
    // so unrelated targets keep a stable sequence; the window swapchain graph
    // stays the last child (it may itself sample off-screen targets).
    if (impl->command_graph == nullptr) {
        return;
    }
    auto& children = impl->command_graph->children;
    const auto win = impl->targets.find(nullptr);
    if (win == impl->targets.end() || win->second.graph == nullptr) {
        return;
    }
    const auto window_graph = win->second.graph;

    // Index every off-screen PASS graph by its target, and collect the targets
    // currently in the command graph, preserving their current relative order
    // as the stable tie-break seed.
    //
    // Inside a target the graphs must record in the passes' explicit pipeline
    // order (setPassOrder) — the position each pass' content would have
    // occupied as a View of a single target-wide render pass. Taking that order
    // from the slot map (pointer order) would let a target's SECOND pass record
    // before its first, so the second pass' colour clear would wipe the first
    // pass' draws.
    const auto graph_order = [](const Impl::Target& owner, const ::vsg::ref_ptr<::vsg::RenderGraph>& graph) {
        for (const auto& pass : owner.passes) {
            if (pass.second.graph == graph) {
                return pass.second.order;
            }
        }
        return std::numeric_limits<int>::max();
    };
    std::map<vine::graphics::RenderTarget*, std::vector<::vsg::ref_ptr<::vsg::RenderGraph>>> graphs_of;
    for (const auto& entry : impl->targets) {
        if (entry.first == nullptr) {
            continue;
        }
        // Seed from the order the graphs are recorded in RIGHT NOW, so passes
        // carrying the same explicit order keep their relative position, then
        // append the ones created since the last reconcile.
        std::vector<::vsg::ref_ptr<::vsg::RenderGraph>> graphs;
        for (const auto& child : children) {
            if (child == window_graph) {
                continue;
            }
            for (const auto& pass : entry.second.passes) {
                if (pass.second.graph == child) {
                    graphs.push_back(pass.second.graph);
                    break;
                }
            }
        }
        for (const auto& pass : entry.second.passes) {
            if (pass.second.graph != nullptr &&
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
            graphs_of.emplace(entry.first, std::move(graphs));
        }
    }
    const auto owner_of_child = [&graphs_of](const ::vsg::ref_ptr<::vsg::Node>& child) -> vine::graphics::RenderTarget* {
        for (const auto& entry : graphs_of) {
            for (const auto& graph : entry.second) {
                if (graph == child) {
                    return entry.first;
                }
            }
        }
        return nullptr;
    };
    std::vector<vine::graphics::RenderTarget*> present;
    present.reserve(graphs_of.size());
    std::set<vine::graphics::RenderTarget*> seen;
    for (const auto& child : children) {
        if (child == window_graph) {
            continue;
        }
        if (auto* owner = owner_of_child(child); owner != nullptr && seen.insert(owner).second) {
            present.push_back(owner);
        }
    }

    // Sampling edges: a consumer depends on every source it samples (screen
    // slot keys carry the sampled target; program slots are keyed by it).
    // Self-sampling is rejected on attach and mutual same-frame sampling
    // (ping-pong inside one frame) is not a supported pattern, so the edge
    // graph is acyclic in practice; a cycle would only leave targets in their
    // current order below.
    // Index the off-screen targets by their CURRENT record position, turn the
    // sampling / depth-borrow edges into index pairs, and let the pure stable
    // topological helper order them (see stableTopologicalOrder).
    std::map<vine::graphics::RenderTarget*, std::size_t> index_of;
    for (std::size_t i = 0; i < present.size(); ++i) {
        index_of.emplace(present[i], i);
    }
    std::vector<GraphOrderEdge> edges;
    for (auto* t : present) {
        const auto entry = impl->targets.find(t);
        if (entry == impl->targets.end()) {
            continue;
        }
        const auto& target = entry->second;
        const auto add_source = [&](vine::graphics::RenderTarget* source) {
            if (source == nullptr || source == t) {
                return;
            }
            const auto src = index_of.find(source);
            if (src == index_of.end()) {
                return; // the source is not recorded this frame: no edge
            }
            edges.push_back(GraphOrderEdge{ index_of[t], src->second });
        };
        // A DEPTH BORROW is a dependency too, and not a sampling one: the
        // borrower's pass LOADs (tests against) the depth the source's pass
        // writes this frame. Without an edge here the order came from the build
        // order alone, so a borrower whose graph happened to be created before
        // the source's (a consumer pass ordered before its producer, or a source
        // that was rebuilt later in the frame) was RECORDED FIRST and tested
        // against the previous frame's depth — a silent one-frame lag, which no
        // validation layer reports (the layouts match; only the write→read
        // dependency is wrong). The barrier inserted below then also sits after
        // the source, i.e. between the two, as it must.
        if (target.depth_source != nullptr) {
            add_source(target.depth_source);
        }
        // A slot's sampled target is a slot attribute (its key is the owning
        // pass), so the dependency edges come from the attribute. A retired
        // (detached) slot is not recorded, so it contributes no edge.
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

    std::vector<vine::graphics::RenderTarget*> order;
    order.reserve(present.size());
    for (const std::size_t index : stableTopologicalOrder(present.size(), edges)) {
        order.push_back(present[index]);
    }

    // Rewrite the command graph's children: each target's pass graphs in
    // dependency order, then the window graph last. Reordering render-graph
    // children only changes per-frame record order — each pass graph is its own
    // render pass, so no recompilation is needed.
    children.clear();
    for (auto* t : order) {
        const auto graphs = graphs_of.find(t);
        if (graphs == graphs_of.end()) {
            continue;
        }
        for (const auto& graph : graphs->second) {
            children.push_back(graph);
        }
        // After the LAST pass graph of a target whose depth another target
        // borrows, insert that borrower's depth-share barrier so its LOAD /
        // depth test sees this target's writes (both share one depth image in
        // the attachment layout).
        for (const auto& entry : impl->targets) {
            const auto& other = entry.second;
            if (other.depth_source == t && other.depth_share_barrier != nullptr) {
                children.push_back(other.depth_share_barrier);
            }
        }
    }
    children.push_back(window_graph);
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
    waitForIdle(impl->viewer.get());
    it->second.bridge.clearCache();
    t.content_slots.erase(it);
}

void VsgRenderer::releaseRenderTarget(vine::graphics::RenderTarget* target)
{
    if (target == nullptr || !impl->initialized) {
        return;
    }
    bool released = false;
    // The graph a slot's view was attached to: the window target's shared
    // swapchain graph, or the off-screen pass' own graph.
    const auto graph_of_slot = [](Impl::Target& owner, const SlotKey& key) -> ::vsg::ref_ptr<::vsg::RenderGraph> {
        const auto pass = owner.passes.find(key);
        return pass == owner.passes.end() ? owner.graph : pass->second.graph;
    };
    auto ot       = impl->targets.find(target);
    if (ot != impl->targets.end()) {
        // Remove the target's off-screen pass graphs from the command graph
        // before dropping its images / views / render passes / slots.
        auto& t = ot->second;
        for (auto& pass : t.passes) {
            removeGraphChild(impl->command_graph.get(), pass.second.graph);
        }
        waitForIdle(impl->viewer.get());
        for (auto& slot_entry : t.content_slots) {
            slot_entry.second.bridge.clearCache();
            // A dropped slot must not stay queued for the frame's incremental
            // compile: its view no longer belongs to any target.
            const auto& view  = slot_entry.second.view;
            auto&       queue = impl->pending_compile_views;
            queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
        }
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
        std::fprintf(stderr,
                     "[VsgRenderer] target '%s' borrowed the released target '%s' depth; "
                     "dropping the borrow (it rebuilds with its own depth)\n",
                     entry.first->name().empty() ? "(unnamed)" : entry.first->name().stdstr().c_str(),
                     target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str());
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
    // PiP (screen) slots live in the target that draws them; drop any that
    // sample this target's colour (any of its colour attachments). The slot's
    // sampled target is an attribute now (the key is its owning pass).
    for (auto& target_entry : impl->targets) {
        auto& slots = target_entry.second.screen_slots;
        for (auto it = slots.begin(); it != slots.end();) {
            if (it->second.source_target != target) {
                ++it;
                continue;
            }
            if (auto graph = graph_of_slot(target_entry.second, it->first); graph != nullptr) {
                removeGraphChild(graph.get(), it->second.view);
            }
            waitForIdle(impl->viewer.get());
            it = slots.erase(it);
            released = true;
        }
    }
    // Fullscreen-program slots (deferred lighting) live under the drawing
    // target; drop any sampling the removed target.
    for (auto& target_entry : impl->targets) {
        auto& slots = target_entry.second.program_slots;
        for (auto it = slots.begin(); it != slots.end();) {
            if (it->second.source_target != target) {
                ++it;
                continue;
            }
            if (auto graph = graph_of_slot(target_entry.second, it->first); graph != nullptr) {
                removeGraphChild(graph.get(), it->second.view);
            }
            waitForIdle(impl->viewer.get());
            it = slots.erase(it);
            released = true;
        }
    }
    if (released) {
        // The remaining command-graph child order may have changed (a sampling
        // edge disappeared, a graph was detached).
        reconcileOffscreenOrder();
        std::fprintf(stderr, "[VsgRenderer] released GPU resources for removed render target\n");
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
    if (target == nullptr || impl->viewer == nullptr || impl->window == nullptr) {
        // Nothing has been rendered off-screen in this session: the request is
        // unsupported, which is what false means (see the base contract).
        return false;
    }
    const auto entry = impl->targets.find(target);
    if (entry == impl->targets.end() || !entry->second.attachments_built) {
        return false;
    }
    auto& built = entry->second;
    if (attachment < 0 || static_cast<std::size_t>(attachment) >= built.color_images.size() ||
        built.width <= 0 || built.height <= 0) {
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

    const std::uint32_t width  = static_cast<std::uint32_t>(built.width);
    const std::uint32_t height = static_cast<std::uint32_t>(built.height);

    // The frame that wrote this target must be complete before its image is
    // copied out; this call is synchronous by contract.
    impl->viewer->deviceWaitIdle();

    auto device   = impl->window->getDevice();
    auto physical = impl->window->getPhysicalDevice();
    auto source   = built.color_images[attachment];
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
    auto memory = ::vsg::DeviceMemory::create(device, destination->getMemoryRequirements(device->deviceID),
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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

    const auto queue_family = physical->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
    auto       command_pool = ::vsg::CommandPool::create(device, queue_family);
    auto       fence        = ::vsg::Fence::create(device);
    auto       queue        = device->getQueue(queue_family);
    ::vsg::submitCommandsToQueue(command_pool, fence, 100000000000, queue,
                                [&commands](::vsg::CommandBuffer& command_buffer) { commands->record(command_buffer); });

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
    if (target == nullptr || impl->viewer == nullptr || impl->window == nullptr) {
        return false;
    }
    const auto entry = impl->targets.find(target);
    if (entry == impl->targets.end() || !entry->second.attachments_built) {
        return false;
    }
    auto& built = entry->second;
    if (built.depth_image == nullptr || built.width <= 0 || built.height <= 0) {
        if (built.depth_source != nullptr) {
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
    const VkFormat      format      = built.depth_image->format;
    const std::size_t   texel_bytes = (format == VK_FORMAT_D32_SFLOAT) ? 4u : (format == VK_FORMAT_D16_UNORM) ? 2u : 0u;
    if (texel_bytes == 0u) {
        reportFailure(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                      formatDiagnostic(u8"readDepthBuffer: format %d is packed (depth + stencil in one texel) and is "
                                       u8"not decoded; use D32_SFLOAT or D16_UNORM for depth readback",
                                       static_cast<int>(format)));
        return false;
    }

    const std::uint32_t width  = static_cast<std::uint32_t>(built.width);
    const std::uint32_t height = static_cast<std::uint32_t>(built.height);

    impl->viewer->deviceWaitIdle();

    auto device   = impl->window->getDevice();
    auto physical = impl->window->getPhysicalDevice();
    if (device == nullptr || physical == nullptr) {
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
    auto memory = ::vsg::DeviceMemory::create(device, buffer->getMemoryRequirements(device->deviceID),
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    buffer->bind(memory, 0);

    // The depth image is left in whatever layout the last pass of this target
    // ends in: a pass that promoted it leaves SHADER_READ_ONLY_OPTIMAL, an
    // ordinary one leaves the attachment layout. There is no single
    // target-level render pass any more, so the layout comes from the target's
    // recorded depth policy.
    const VkImageLayout depth_layout = built.depth_sampleable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    auto                          commands = ::vsg::Commands::create();
    commands->addChild(::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                          depth_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, built.depth_image, range)));

    VkBufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                = 0;
    region.bufferImageHeight              = 0;
    region.imageSubresource                = VkImageSubresourceLayers{ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    region.imageOffset                     = VkOffset3D{ 0, 0, 0 };
    region.imageExtent                     = VkExtent3D{ width, height, 1 };
    auto copy                              = ::vsg::CopyImageToBuffer::create();
    copy->srcImage                         = built.depth_image;
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
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, built.depth_image, range)));

    const auto queue_family = physical->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
    auto       command_pool = ::vsg::CommandPool::create(device, queue_family);
    auto       fence        = ::vsg::Fence::create(device);
    auto       queue        = device->getQueue(queue_family);
    ::vsg::submitCommandsToQueue(command_pool, fence, 100000000000, queue,
                                [&commands](::vsg::CommandBuffer& command_buffer) { commands->record(command_buffer); });

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
