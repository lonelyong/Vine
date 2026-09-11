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

    // A rebuild (target resized) must first release the previous graph: it
    // may still be referenced by an in-flight command buffer, and every
    // content slot compiled against it must be dropped with it (per-view
    // pipelines bind the old render pass).
    if (t.graph != nullptr) {
        removeGraphChild(impl->command_graph.get(), t.graph);
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
        t.render_pass          = {};
        t.render_pass_load     = {};
        t.depth_ready          = false;
        t.framebuffer          = {};
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

    ::vsg::ImageViews attachments;
    attachments.reserve(static_cast<std::size_t>(color_count) + (has_depth ? 1u : 0u));
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
        attachments.push_back(t.color_views[static_cast<std::size_t>(i)]);
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
        attachments.push_back(t.depth_view);
    }
    else if (borrowed) {
        // Borrow the source's depth image/view (it renders earlier this frame):
        // the framebuffer below attaches the shared depth, loaded not cleared.
        // The borrow was validated above, so the source image exists and matches
        // this framebuffer.
        const auto src_it = impl->targets.find(depth_src);
        attachments.push_back(src_it->second.depth_view);
        // Remember WHICH source image this framebuffer borrowed: the source
        // replacing it (a rebuild) invalidates this framebuffer, and render()
        // detects that by comparing the two.
        t.depth_source_view = src_it->second.depth_view;
    }

    // The depth policy of this target's pass follows its persisted clearDepth
    // request: CLEAR (the default, depth sampled afterwards) when the engine
    // asked to clear depth; depth-LOAD (depth preserved across frames, not
    // sampled) when it asked not to. A depth-LOAD pass cannot also promote the
    // depth to a sampled texture, so such a target must not be depth-sampled
    // (the deferred G-buffer always clears depth, so it never selects LOAD).
    const bool load_depth = t.wantsDepthLoad();
    t.depth_load          = load_depth;
    if (has_color) {
        const VkFormat depth_format =
            has_depth ? (borrowed ? toDepthFormat(depth_src->depthFormat()) : toDepthFormat(target->depthFormat()))
                      : VK_FORMAT_UNDEFINED;
        if (borrowed) {
            // Composite sharing the source's depth: colour is cleared + stored
            // (sampleable for the present pass) while the depth is LOADED from
            // the source graph recorded earlier this frame. Both graphs leave
            // the depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL; the depth-share
            // barrier inserted between them orders the write -> load/test.
            t.depth_load  = false;
            t.render_pass = makeDepthLoadRenderPass(device.get(), color_formats, depth_format, /*initial_clear*/ false);
        }
        else if (load_depth) {
            // Two compatible passes over the same attachments: the first-frame
            // pass CLEARs the fresh (UNDEFINED) depth image — transitioning it
            // into DEPTH_STENCIL_ATTACHMENT_OPTIMAL and seeding its content —
            // so the steady depth-LOAD pass that follows is valid (a LOAD pass
            // cannot start from an UNDEFINED image). submitFrame records the
            // first-frame pass once, then the steady one.
            t.render_pass_load = makeDepthLoadRenderPass(device.get(), color_formats, depth_format, false);
            t.render_pass      = makeDepthLoadRenderPass(device.get(), color_formats, depth_format, true);
            t.depth_ready      = false;
        } else {
            t.render_pass = makeSampleableRenderPass(device.get(), color_formats, depth_format, target->depthPromotion());
            // A pass that promotes its depth leaves it in SHADER_READ_ONLY_OPTIMAL
            // so it can be sampled: that image can no longer be attached as
            // another target's depth (see the borrow validation above).
            t.depth_sampleable = has_depth && target->depthPromotion();
        }
    } else {
        t.render_pass = makeDepthOnlyRenderPass(device.get(), toDepthFormat(target->depthFormat()));
    }
    t.framebuffer = ::vsg::Framebuffer::create(t.render_pass, attachments, w, h, 1);

    t.graph              = ::vsg::RenderGraph::create();
    t.graph->framebuffer = t.framebuffer;
    t.graph->renderArea  = VkRect2D{
        { 0, 0 },
        { w, h }
    };
    t.graph->contents      = VK_SUBPASS_CONTENTS_INLINE;
    t.graph->viewportState = ::vsg::ViewportState::create(VkExtent2D{ w, h });
    // Clear values match the attachment order (colour attachments in order,
    // then depth). Attachment 0 is cleared to the last engine clear() colour
    // requested for this target (recorded on the target so a rebuild reapplies
    // it); extra MRT attachments clear transparent black (empty regions stay
    // black until a fragment writes them). Depth is cleared to the far plane
    // for depth-only (shadow) targets and to the value that makes the window
    // forward-path geometry test pass for colour+RT targets; a depth-LOAD pass
    // ignores its depth clear value (its depth attachment is not cleared).
    t.graph->clearValues.clear();
    for (int i = 0; i < color_count; ++i) {
        VkClearValue color_clear = {};
        if (i == 0) {
            color_clear.color = VkClearColorValue{
                { t.clear_seen ? t.clear_color.r : 0.2f,
                  t.clear_seen ? t.clear_color.g : 0.2f,
                  t.clear_seen ? t.clear_color.b : 0.2f,
                  t.clear_seen ? t.clear_color.a : 1.0f }
            };
        }
        t.graph->clearValues.push_back(color_clear);
    }
    if (has_depth) {
        // Colour targets clear depth to 0.0, matching what the window main's
        // clear() pushes — the geometry depth test that works for the window
        // forward path must see the same cleared value off-screen or every
        // fragment fails and nothing rasterises. Depth-only (shadow) targets
        // keep clearing to the far plane (1.0). The same value is recorded on
        // the target so a pass that has to clear the depth ITSELF (a mixed
        // target, see ContentSlot::clears_depth) writes exactly what the render
        // pass would have written.
        VkClearValue depth_clear = {};
        depth_clear.depthStencil = VkClearDepthStencilValue{ has_color ? 0.0f : 1.0f, 0 };
        t.depth_clear_value      = depth_clear.depthStencil.depth;
        t.graph->clearValues.push_back(depth_clear);
    }

    // A (re)built target created FRESH colour views: any OTHER target that
    // samples this one (PiP screen slots / fullscreen-program slots) still
    // holds the OLD views and would sample a stale, no-longer-drawn image
    // (its stale check only watches source size, which a same-size rebuild
    // does not change). Drop those slots so the next drawScreenTexture /
    // drawScreenProgram call reattaches against the new attachments.
    for (auto& entry : impl->targets) {
        auto& other = entry.second;
        if (entry.first == target || other.graph == nullptr) {
            continue;
        }
        // A slot's sampled target is a slot ATTRIBUTE, so consumers are found
        // by inspecting it (a slot's key is its owning pass).
        const auto forget_view = [this](const ::vsg::ref_ptr<::vsg::View>& view) {
            auto& queue = impl->pending_compile_views;
            queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
        };
        for (auto it = other.screen_slots.begin(); it != other.screen_slots.end();) {
            if (it->second.source_target == target) {
                removeGraphChild(other.graph.get(), it->second.view);
                it = other.screen_slots.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = other.program_slots.begin(); it != other.program_slots.end();) {
            if (it->second.source_target == target) {
                removeGraphChild(other.graph.get(), it->second.view);
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

    if (impl->command_graph != nullptr) {
        // Add the graph as the command graph's last child, then reorder every
        // off-screen graph into a dependency-valid sequence — each consumer is
        // recorded after the targets it samples (see reconcileOffscreenOrder()
        // for why creation order alone is not enough).
        impl->command_graph->children.push_back(t.graph);
        reconcileOffscreenOrder();
    }
    // Record the shape these attachments and this pass were built from, so
    // render() rebuilds when the host changes any of it (see Target::BuildKey).
    t.build_key = Impl::Target::BuildKey::of(*target);
    std::fprintf(stderr, "[VsgRenderer] EXPERIMENTAL off-screen target '%s' %ux%u attached\n",
                 target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str(), w, h);
    ++impl->offscreen_build_count;
    // NOTE: no compile here — the graph is empty until its first content slot
    // is added; setupContentSlot() compiles the (whole) command graph then.
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

    // Index every off-screen graph by its target, and collect the ones
    // currently in the command graph, preserving their current relative order
    // as the stable tie-break seed.
    std::map<vine::graphics::RenderTarget*, ::vsg::ref_ptr<::vsg::RenderGraph>> graph_of;
    for (const auto& entry : impl->targets) {
        if (entry.first != nullptr && entry.second.graph != nullptr) {
            graph_of.emplace(entry.first, entry.second.graph);
        }
    }
    std::vector<vine::graphics::RenderTarget*> present;
    present.reserve(graph_of.size());
    for (const auto& child : children) {
        if (child == window_graph) {
            continue;
        }
        for (const auto& entry : graph_of) {
            if (entry.second == child) {
                present.push_back(entry.first);
                break;
            }
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

    // Rewrite the command graph's children: off-screen graphs in dependency
    // order, then the window graph last. Reordering render-graph children only
    // changes per-frame record order — each graph is its own render pass, so
    // no recompilation is needed.
    children.clear();
    for (auto* t : order) {
        children.push_back(graph_of[t]);
        // After a graph whose depth another target borrows, insert that
        // borrower's depth-share barrier so its LOAD / depth test sees this
        // graph's writes (both share one depth image in the attachment layout).
        for (const auto& entry : impl->targets) {
            const auto& other = entry.second;
            if (other.depth_source == t && other.graph != nullptr && other.depth_share_barrier != nullptr) {
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
    auto ot       = impl->targets.find(target);
    if (ot != impl->targets.end()) {
        // Remove the target's off-screen graph from the command graph before
        // dropping its images / views / render pass / framebuffer / slots.
        auto& t = ot->second;
        removeGraphChild(impl->command_graph.get(), t.graph);
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
            removeGraphChild(target_entry.second.graph.get(), it->second.view);
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
            removeGraphChild(target_entry.second.graph.get(), it->second.view);
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

void VsgRenderer::placeViewByOrder(vine::graphics::RenderTarget* target,
                                   const ::vsg::ref_ptr<::vsg::View>& view,
                                   int order)
{
    auto& t = impl->entryFor(target);
    if (t.graph == nullptr || view == nullptr) {
        return;
    }
    auto& children = t.graph->children; // RenderGraph is a Group: children are ref_ptr<Node>
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
    if (entry == impl->targets.end() || entry->second.graph == nullptr) {
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
    if (entry == impl->targets.end() || entry->second.graph == nullptr) {
        return false;
    }
    auto& built = entry->second;
    if (built.depth_image == nullptr || built.render_pass == nullptr || built.width <= 0 || built.height <= 0) {
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

    // The depth image is left in whatever layout its pass ends with (attachment
    // for a depth test/write target, sampleable for a promoted one), so that is
    // what it is returned to: the next frame renders and samples it again.
    const VkImageLayout depth_layout = built.render_pass->attachments.back().finalLayout;
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
