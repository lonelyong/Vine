#include <vine/vsg/VsgPassMaterialiser.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/ViewportState.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgRecordOrder.hpp>
#include <vine/vsg/VsgTargetBookkeeping.hpp>
#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

// The pass materialisation layer: turning a decided pass (its load-op variant) into the
// render pass / framebuffer / render graph it records. Each function takes the target entry
// plus the few session facts it reads; the load-op policy they apply is documented on
// planPass() in VsgPassMaterialiser.hpp.

// These units share one free-function layer (VsgBackendUtility.hpp /
// VsgPipelineFactory.hpp); the directive keeps call sites unqualified.
using namespace detail;

detail::PassAttachments detail::passAttachments(const VsgRendererState&      state,
                                                const VsgRenderTargetEntry& t,
                                                const vine::graphics::RenderTarget& target)
{
    detail::PassAttachments att;
    att.device    = state.window->getOrCreateDevice();
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

std::pair<::vsg::ref_ptr<::vsg::RenderPass>, ::vsg::ref_ptr<::vsg::Framebuffer>> detail::makePassObjects(
    const VsgRendererState& state, const VsgRenderTargetEntry& t, const detail::PassAttachments& att,
    bool pass_color_clear, bool depth_load, bool promote, VkImageLayout depth_initial)
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
    return std::pair{ render_pass, makePassFramebuffer(state, t, att, render_pass) };
}

::vsg::ref_ptr<::vsg::Framebuffer> detail::makePassFramebuffer(const VsgRendererState& state,
                                                              const VsgRenderTargetEntry& t,
                                                              const detail::PassAttachments& att,
                                                              const ::vsg::ref_ptr<::vsg::RenderPass>& render_pass)
{
    if (render_pass == nullptr) {
        return {};
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
        const auto source = state.targets.find(t.depth_source);
        attachments.push_back(att.borrowed && source != state.targets.end() ? source->second.depth_view : t.depth_view);
    }
    return ::vsg::Framebuffer::create(render_pass, attachments, static_cast<uint32_t>(t.width),
                                      static_cast<uint32_t>(t.height), 1);
}

bool detail::depthStillPromoted(const VsgRendererState& state, const VsgRenderTargetEntry& t,
                                const VsgRenderTargetEntry::PassObjects* current, int order)
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
        if (state.passes_active_this_frame.contains(other.first.owner)) {
            // It ran first this frame, so what the image holds is THAT pass' variant — but WHICH layout
            // that variant left is not asked here, and the answer given is the conservative one. That is
            // sound only because of the pairing with revokeDepthPromotion: the guard above proves
            // promotion was in force, and a pass that LOADs depth reaches the revoke cascade before
            // anything is recorded, which rebuilds this earlier pass WITHOUT promotion — making the
            // "not promoted" answer true by the time the frame is recorded. A predicate that asked
            // "did the earlier pass leave SHADER_READ_ONLY" would need that revoke decision taken BEFORE
            // the plan, which is not where it is taken (see planPass' notes): do not change one without
            // the other.
            return false; // it ran first this frame and left its own layout
        }
    }
    return true;
}

void detail::revokeDepthPromotion(VsgRendererState& state, VsgRenderTargetEntry& t,
                                  const VsgRenderTargetEntry::PassObjects* current, VkImageLayout steady_depth_initial)
{
    const detail::PassAttachments att = passAttachments(state, t, *t.owner);
    for (auto& pass : t.passes) {
        if (&pass.second == current) {
            continue;
        }
        auto [render_pass, framebuffer] =
            makePassObjects(state, t, att, pass.second.want_color_clear, pass.second.load_depth,
                            /*promote*/ false,
                            pass.second.load_depth ? steady_depth_initial : VK_IMAGE_LAYOUT_UNDEFINED);
        // The variant this pass RECORDED may still be named by an in-flight command
        // buffer, so park what is being replaced instead of stopping the device for
        // it (see VsgRetireRing::park).
        state.retireRing.park(pass.second.render_pass);
        state.retireRing.park(pass.second.render_pass_transient);
        state.retireRing.park(pass.second.framebuffer);
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

// The bodies are the moved definitions: their documentation lives on the declarations.

std::vector<VkClearValue> detail::makePassClearValues(const VsgRenderTargetEntry& t, bool has_depth,
                                                      const ::vsg::vec4& clear_color)
{
    std::vector<VkClearValue> clear_values;
    clear_values.reserve(t.color_views.size() + (has_depth ? 1u : 0u));
    for (std::size_t i = 0; i < t.color_views.size(); ++i) {
        VkClearValue value = {};
        if (i == 0u) {
            value.color = VkClearColorValue{ { clear_color.r, clear_color.g, clear_color.b, clear_color.a } };
        }
        clear_values.push_back(value); // extra MRT attachments: transparent black
    }
    if (has_depth) {
        VkClearValue value = {};
        value.depthStencil = VkClearDepthStencilValue{ t.depth_clear_value, 0 };
        clear_values.push_back(value);
    }
    return clear_values;
}

bool detail::setColorClearValue(std::vector<VkClearValue>& clear_values, bool has_color,
                                const ::vsg::vec4& clear_color)
{
    if (!has_color || clear_values.empty()) {
        return false;
    }
    clear_values[0].color = VkClearColorValue{ { clear_color.r, clear_color.g, clear_color.b, clear_color.a } };
    return true;
}

::vsg::ref_ptr<::vsg::RenderGraph> detail::makePassGraph(const VsgRenderTargetEntry& t, bool has_depth,
                                                         const ::vsg::vec4& clear_color)
{
    auto graph        = ::vsg::RenderGraph::create();
    graph->renderArea =
        VkRect2D{ { 0, 0 }, { static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) } };
    graph->contents      = VK_SUBPASS_CONTENTS_INLINE;
    // Same opt-out as the window graph (see VsgRenderer::initialize): installed, vsg's resize handler
    // scales the views' sub-viewport rectangles when the graph's extent changes, while every rectangle
    // this backend owns is re-derived from the target's current size (detail::updateSlotViewport per
    // slot, and this graph's renderArea below). The graph-level viewportState stays -- it is what
    // RenderGraph::accept pushes for commands that are not under a View -- but it is re-synced from
    // `renderArea` on every record, so it never needs a second writer either.
    graph->windowResizeHandler = {};
    graph->viewportState = ::vsg::ViewportState::create(
        VkExtent2D{ static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) });
    graph->clearValues = makePassClearValues(t, has_depth, clear_color);
    return graph;
}

::vsg::ref_ptr<::vsg::RenderGraph> detail::reuseSteadyPass(const VsgRendererState& state,
                                                           VsgRenderTargetEntry::PassObjects& objects,
                                                           bool want_color_clear, bool want_depth_clear,
                                                           bool has_color, const ::vsg::vec4& clear_color,
                                                           std::uint64_t attachments_generation)
{
    if (passVariantIsStale(objects.want_color_clear, objects.want_depth_clear, want_color_clear, want_depth_clear)) {
        return {}; // clear policy changed: the variant has to be rebuilt
    }
    // The target's attachments were replaced (a resize in place): the variant this pass recorded
    // describes images that no longer exist, and the new ones are UNDEFINED again, so which variant
    // the pass has to record (CLEAR seed / promote / plain LOAD) has to be decided again — for ONE
    // frame, which is what the plan then records (see planPassVariant).
    if (objects.attachments_generation != attachments_generation) {
        return {};
    }
    if (state.request.presenting && objects.clear_color != clear_color && objects.graph != nullptr) {
        // The colour entry only: a depth-only pass' single entry is its DEPTH value (see
        // setColorClearValue), so the helper — not this call site — decides whether there is one.
        if (setColorClearValue(objects.graph->clearValues, has_color, clear_color)) {
            objects.clear_color = clear_color;
        }
    }
    return objects.graph;
}

void detail::publishPass(VsgRenderTargetEntry& t, const SlotKey& key,
                         const VsgRenderTargetEntry::PassObjects& objects, bool has_color)
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

detail::PassPlan detail::planPass(const VsgRendererState& state, const VsgRenderTargetEntry& t, const SlotKey& key,
                                  const vine::graphics::RenderTarget& target)
{
    detail::PassPlan plan;
    // The target's attachment set (owned by buildOffscreenTarget): every pass of this target
    // attaches exactly these images, only its load-ops differ.
    plan.att       = passAttachments(state, t, target);
    plan.has_color = plan.att.has_color;
    plan.has_depth = plan.att.has_depth;

    // This pass' own clear request (the open pass scope) is what the variant's load-ops are
    // derived from — see this function's notes for the policy.
    plan.want_color_clear = state.request.presenting;
    plan.want_depth_clear = state.request.presenting && state.request.clear_depth;

    // Taken BEFORE the pass is published: the pointer addresses the target's pass table, and
    // the flags planPassVariant() reads below are the ones publishing the new objects changes.
    const auto built = t.passes.find(key);
    plan.current     = built != t.passes.end() ? &built->second : nullptr;

    const bool depth_still_promoted = depthStillPromoted(state, t, plan.current, state.request.order);
    plan.variant =
        planPassVariant(plan.has_color, plan.has_depth, plan.att.borrowed,
                        plan.has_depth && !plan.att.borrowed && target.depthPromotion(), t.any_load_pass,
                        t.depth_seeded, t.color_seeded, plan.want_color_clear, plan.want_depth_clear,
                        depth_still_promoted);
    plan.color_clear = plan.variant.color_load == VK_ATTACHMENT_LOAD_OP_CLEAR;
    plan.depth_load  = plan.has_depth && plan.variant.depth_load == VK_ATTACHMENT_LOAD_OP_LOAD;
    return plan;
}

::vsg::ref_ptr<::vsg::RenderGraph> detail::passGraph(VsgRendererState&      state, const VsgDiagnostics& diagnostics,
                                                     vine::graphics::RenderTarget* target,
                                                     const SlotKey&         key)
{
    auto& t = state.entryFor(target);
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
    // detail::planPass). Everything below APPLIES it.
    const detail::PassPlan plan = detail::planPass(state, t, key, *target);

    const auto built = t.passes.find(key);

    if (built != t.passes.end()) {
        // A pass that changed its explicit pipeline order moves its graph to the matching
        // record position (see reconcileOffscreenOrder).
        if (built->second.order != state.request.order) {
            built->second.order = state.request.order;
            detail::reconcileOffscreenOrder(state);
        }
        // Steady frame: reuse the recorded variant unless this pass changed its clear
        // policy (RenderPass::setClearEnabled / setShouldClearDepth flipped at run time).
        // Every other per-pass property (order, depth mode, lights, viewport) is re-applied
        // each frame; the load-ops have to follow suit, or a pass that starts clearing keeps
        // LOADing and its request is silently ignored.
        if (auto graph = detail::reuseSteadyPass(state, built->second, plan.want_color_clear, plan.want_depth_clear,
                                               plan.has_color, state.request.clear_color, t.attachments_generation)) {
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
        (t.clear_seen || state.request.presenting) ? t.clear_color : kDefaultClearColor;

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
        detail::revokeDepthPromotion(state, t, plan.current, plan.variant.steady_depth_initial);
        // A fullscreen program that SAMPLES this depth was built earlier in this
        // frame, while the promotion still stood, so its descriptor set names the
        // promoted layout — and the revoke (plus this pass) leaves the image in
        // the attachment layout from here on. Recording the slot would name a
        // layout the image is no longer in, once per draw, for a frame the host has
        // no way to know about, so it is dropped for this frame (see
        // dropDepthSamplingProgramSlots).
        detail::dropDepthSamplingProgramSlots(state, diagnostics, target);
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
    auto [render_pass, framebuffer] = detail::makePassObjects(state, t, plan.att, plan.color_clear, plan.depth_load,
                                                          plan.variant.promote_depth, plan.variant.depth_initial);
    ::vsg::ref_ptr<::vsg::RenderPass> render_pass_transient;
    if (plan.variant.transient) {
        render_pass_transient = render_pass;
        render_pass            = detail::makePassObjects(state, t, plan.att,
                                                       plan.variant.steady_color_load == VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                       plan.depth_load, plan.variant.promote_depth,
                                                       plan.variant.steady_depth_initial)
                                     .first;
    }

    // A rebuilt pass keeps its RenderGraph object (its content View stays
    // attached to it) and only swaps the render pass / framebuffer inside it.
    // The replaced objects may still be referenced by an in-flight command
    // buffer, so they are PARKED (released once every slot has been re-recorded,
    // see VsgRetireRing::park) instead of stopping the device mid-frame — this is
    // the policy-change path, and a host that animates a clear / depth policy
    // would otherwise stall the GPU every frame it does so. Setting them is legal
    // without recompiling the pipelines: a render pass is COMPATIBLE with another
    // when the attachments match (format / samples), and load-ops are not part of
    // that (VUID-vkCmdDraw-renderPass-02684).
    ::vsg::ref_ptr<::vsg::RenderGraph> graph =
        (built != t.passes.end()) ? built->second.graph : ::vsg::ref_ptr<::vsg::RenderGraph>();
    if (built != t.passes.end()) {
        state.retireRing.park(built->second.render_pass);
        state.retireRing.park(built->second.render_pass_transient);
        state.retireRing.park(built->second.framebuffer);
    }
    if (graph == nullptr) {
        graph = detail::makePassGraph(t, plan.has_depth, clear_color);
    }
    graph->renderPass  = plan.variant.transient ? render_pass_transient : render_pass;
    graph->framebuffer = framebuffer;
    // The graph follows the target's attachments — and the target may have been RESIZED in place since
    // this graph was made, in which case the graph (and the pass' views under it) is kept while the
    // images under it are new: the render area has to follow the size those images were made at, or
    // the record would name a rectangle the framebuffer does not have (VUID-vkCmdBeginRenderPass-
    // pRenderArea-00063). No `previous_extent` bookkeeping is needed for it: this graph has no
    // WindowResizeHandler (see makePassGraph), so nothing scales a rectangle behind this write.
    graph->renderArea = VkRect2D{ { 0, 0 },
                                  { static_cast<std::uint32_t>(t.width), static_cast<std::uint32_t>(t.height) } };
    if (plan.has_color) {
        // The colour clear value follows the pass' current request (a rebuilt pass may have just STARTED
        // clearing) — through the ONE writer of that entry, which is also what keeps a depth-only pass'
        // depth entry (the vector's only entry) out of reach. The depth entry keeps the target's own
        // depth clear value (makePassClearValues).
        setColorClearValue(graph->clearValues, plan.has_color, clear_color);
    }

    VsgRenderTargetEntry::PassObjects objects;
    objects.render_pass            = render_pass;
    objects.render_pass_transient = render_pass_transient;
    objects.framebuffer           = framebuffer;
    objects.graph                 = graph;
    objects.load_depth            = plan.depth_load;
    objects.want_color_clear      = plan.want_color_clear;
    objects.want_depth_clear      = plan.want_depth_clear;
    objects.clear_color           = clear_color;
    objects.order                 = state.request.order;
    objects.transient             = plan.variant.transient;
    objects.attachments_generation = t.attachments_generation;
    detail::publishPass(t, key, objects, plan.has_color);

    if (state.command_graph != nullptr) {
        if (built == t.passes.end()) {
            // Only a NEW pass' graph has to be added: a rebuilt pass' graph never
            // left the command graph (it is the same object).
            state.command_graph->children.push_back(graph);
        }
        // Reorder every off-screen graph into a dependency-valid sequence — each
        // consumer is recorded after the targets it samples (see
        // reconcileOffscreenOrder() for why creation order alone is not enough).
        detail::reconcileOffscreenOrder(state);
    }
    return graph;
}
void detail::dropDepthSamplingProgramSlots(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                                            vine::graphics::RenderTarget* target)
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
    for (auto& dest_entry : state.targets) {
        // Collected first: the drop below erases from this table (and it is the one home for that,
        // see eraseProgramSlot — it detaches the view, parks the node and erases the slot).
        std::vector<SlotKey> drop;
        for (const auto& slot : dest_entry.second.program_slots) {
            if (slot.second.ready && slot.second.source_target == target && slot.second.binds_source_depth) {
                drop.push_back(slot.first);
            }
        }
        for (const SlotKey& key : drop) {
            eraseProgramSlot(state, dest_entry.second, dest_entry.first, key);
            diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                               vine::graphics::DiagnosticCategory::ContentSkipped,
                               formatDiagnostic(u8"drawScreenProgram: sampled target '%s' started preserving depth, which"
                                                u8" revokes its depth promotion: the program sampling that depth is"
                                                u8" dropped for this frame (its slot is rebuilt on the owner's next"
                                                u8" draw)",
                                                target->name().empty() ? "(unnamed)" : target->name().as_std_str().c_str()));
        }
    }
}
V_VSG_NS_END
