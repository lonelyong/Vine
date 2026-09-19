#include <vine/vsg/VsgProgramSlot.hpp>

#include <vine/vsg/VsgLights.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/lighting/Light.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/ViewportState.h>
#include <vsg/state/material.h>
#include <vsg/vk/Device.h>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/logging/Log.hpp>

#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

#include <vine/vsg/VsgUtils.hpp>
#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgContentSlot.hpp>
#include <vine/vsg/VsgPassMaterialiser.hpp>
#include <vine/vsg/VsgRecordOrder.hpp>
#include <vine/vsg/VsgTargetBookkeeping.hpp>

V_VSG_NS_BEGIN

// This translation unit is one of several that share a single free-function
// layer (VsgPipelineFactory.hpp / VsgBackendUtility.hpp); the directive keeps
// its call sites unqualified.
using namespace detail;

namespace detail
{

namespace
{

/**
 * @brief Why a retained fullscreen-program slot cannot be reused as it is.
 *
 * The rebuild predicate is asked for a yes/no answer, but what a resize has to get right is which of the
 * answers apply: a slot that rebuilds because the SOURCE's shape or policy changed must, while the same
 * source's attachments being replaced at the same shape must not (see repointProgramSlotSource). Naming
 * the reason keeps that distinction visible in the log instead of in a bisect.
 *
 * @param previous          Slot retained from an earlier frame (null when there is none yet).
 * @param source            Target this slot samples.
 * @param src               That target's entry now.
 * @param shadow_map        Shadow map this pass resolved, or null when it declared none.
 * @param program           Program this pass draws through.
 * @param program_revision  Revision of @p program now.
 * @return A short reason, or an empty string when the slot can be reused.
 */
const char* programSlotStaleReason(const ProgramSlot* previous, vine::raw_ptr<const vine::graphics::RenderTarget> source,
                                   const VsgRenderTargetEntry& src, const ::vsg::ref_ptr<::vsg::ImageView>& shadow_map,
                                   vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                   std::uint64_t program_revision)
{
    if (previous == nullptr) {
        return "no slot yet";
    }
    if (!previous->ready) {
        return "previous attempt did not finish";
    }
    if (previous->source_target != source) {
        return "another source target";
    }
    if (previous->source_depth_sampleable != src.depth_sampleable) {
        return "source depth sampleability changed";
    }
    if (shadow_map != nullptr && previous->shadow_view != shadow_map) {
        return "another shadow map";
    }
    if (previous->program.get() != program) {
        return "another program object";
    }
    if (previous->program_revision != program_revision) {
        return "the program was edited";
    }
    return "";
}

/**
 * @brief Builds and compiles an overlay View for a full-screen node.
 *
 * Wraps @p content in its own View (a dedicated camera carrying the sub-rect
 * viewport and a group holding the content) and attaches the View to @p graph:
 * appended as the last child by default (a PiP screen view drawn above the
 * content), or inserted FIRST when @p front is true (the deferred-lighting
 * main view, which later HUD content must stack above). The new View is
 * compiled before its first record — its pipeline is built against the owning
 * window render pass — and on failure the half-compiled View is detached
 * again and null is returned so the caller can drop its slot.
 *
 * @param viewer   Viewer that compiles the new View.
 * @param graph    Render graph the View is attached to (may be null).
 * @param content  Full-screen drawable to wrap.
 * @param x        Viewport origin x in device pixels.
 * @param y        Viewport origin y in device pixels.
 * @param w        Viewport width in device pixels.
 * @param h        Viewport height in device pixels.
 * @param front    When true, insert the View as the graph's first child.
 * @return The compiled View, or null when compilation failed.
 */
::vsg::ref_ptr<::vsg::View> makeCompiledProgramSlotView(
    ::vsg::Viewer& viewer,
    ::vsg::Group* graph,
    ::vsg::ref_ptr<::vsg::Node> content,
    int x,
    int y,
    int w,
    int h,
    bool front,
    bool*       compile_failed = nullptr)
{
    auto camera           = ::vsg::Camera::create();
    camera->viewportState = ::vsg::ViewportState::create(x, y, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    auto view             = ::vsg::View::create(camera);
    auto group            = ::vsg::Group::create();
    group->addChild(content);
    view->addChild(group);
    if (graph != nullptr) {
        if (front) {
            graph->children.insert(graph->children.begin(), view);
        }
        else {
            graph->addChild(view);
        }
    }
    // Compile the new View (its pipeline is built against the window render
    // pass) before it is first recorded.
    const auto compileResult = viewer.compile();
    if (!compileResult) {
        // The caller reports this (it knows the pass and the host sink): all
        // this helper does is drop the half-compiled View so it is never
        // recorded, and say that the compile was the reason.
        if (compile_failed != nullptr) {
            *compile_failed = true;
        }
        removeGraphChild(graph, view);
        return ::vsg::ref_ptr<::vsg::View>();
    }
    return view;
}

} // namespace

ProgramSlotDestination resolveProgramSlotDestination(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                                                vine::graphics::RenderTarget* source, const SlotKey& key,
                                                const char* what)
{
    ProgramSlotDestination out;
    // The destination is the SCOPE's target (setRenderTarget, nullptr = the
    // window): read, not consumed, so every draw call of the pass agrees on it.
    vine::graphics::RenderTarget* dest = state.request.target;
    // A source == destination feedback loop would sample the very attachments this
    // pass writes. Reject it with a diagnostic: a ping-pong pair of targets is the
    // standard way to build a feedback chain.
    if (dest == source) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                           formatDiagnostic(u8"%s: source == destination (feedback loop): the pass draws nothing", what));
        return out;
    }
    auto& dest_entry = state.entryFor(dest);
    if (dest != nullptr) {
        // Writing into an off-screen target: (re)build its graph to its size.
        if (dest->colorCount() <= 0 || dest->width() <= 0 || dest->height() <= 0) {
            diagnostics.report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                               formatDiagnostic(
                                   u8"%s: destination target has no usable colour attachment: the pass draws nothing", what));
            return out;
        }
        if (dest_entry.width != dest->width() || dest_entry.height != dest->height() ||
            !dest_entry.attachments_built || !dest_entry.build_key.matches(*dest)) {
            // Through the ONE decision (see syncOffscreenTarget): a destination at a new size is resized
            // in place, so the slots drawing into it — and the pipelines they compiled — stay.
            if (!detail::syncOffscreenTarget(state, diagnostics, dest)) {
                return out;
            }
        }
    }
    else if (dest_entry.graph == nullptr) {
        // The window's shared swapchain graph is created by initialize(), and both callers of this
        // function return early unless the session is initialized ⇒ this cannot fire in a live
        // session either (the other half of D57): kept as a guard on the same reasoning.
        return out; // window graph not created yet
    }
    out.target = dest;
    out.surf_w = (dest == nullptr) ? static_cast<int>(state.window->extent2D().width) : dest_entry.width;
    out.surf_h = (dest == nullptr) ? static_cast<int>(state.window->extent2D().height) : dest_entry.height;
    // The colour attachment count of the DESTINATION's render pass, which is what the draw's pipeline is
    // created against: the swapchain pass has one attachment, and an off-screen destination has as many as
    // its (just synced) entry holds -- a count that changes with the target's SHAPE, which is exactly why
    // this is read here rather than baked into the node's program.
    out.color_count = (dest == nullptr) ? 1 : static_cast<int>(dest_entry.color_views.size());

    // The pass owns its slot under this destination; if it drew elsewhere before
    // (its render target changed), drop that stale slot so it stops compositing
    // there. The graph this pass records into is the window session's shared
    // swapchain graph, or this pass' own off-screen graph (§28).
    detail::retargetPass(state, state.request.pass, dest);
    out.graph = detail::passGraph(state, diagnostics, dest, key);
    return out;
}

void placeProgramSlotView(VsgRendererState& state, const ProgramSlotDestination& dest,
                      const ::vsg::ref_ptr<::vsg::View>& view, int order)
{
    detail::placeViewByOrder(state, dest.graph, dest.target, view, order);
    if (dest.target != nullptr) {
        // An off-screen destination: the pass' graph has to be back in the command
        // graph, recorded after every target it samples. A source == destination
        // feedback loop was rejected when the destination was resolved, so this
        // consumer cannot feed its own producer.
        detail::reconcileOffscreenOrder(state);
    }
}

/** @brief Builds the View an overlay drawable records through and installs it in its slot.
 *
 * Shared by the PiP screen triangle and the fullscreen program: wrap @p content in
 * its own View (own camera + the sub-rect viewport), compile it against the
 * destination's render pass, then hand it to the slot and position it by the slot's
 * explicit order. A compile failure reports (when it was the compile) and returns
 * false, and the caller drops its slot so the next frame retries — the half-compiled
 * view is never recorded.
 *
 * @tparam Slot    Screen / program slot type (both carry camera / view / order / ready).
 * @param dest     Resolved destination of the draw.
 * @param slot     Slot to install into (its @c order positions the view).
 * @param content  The drawable the view wraps.
 * @param x        Viewport origin x in device pixels.
 * @param y        Viewport origin y in device pixels.
 * @param w        Viewport width in device pixels.
 * @param h        Viewport height in device pixels.
 * @param front    Insert the view as the graph's FIRST child until it is ordered.
 * @param what     Draw name for the compile-failure diagnostic.
 * @return true when the slot now holds a compiled, placed view.
 */
template <class Slot>
bool installProgramSlotView(VsgRendererState& state, const VsgDiagnostics& diagnostics, const ProgramSlotDestination& dest,
                        Slot& slot, const ::vsg::ref_ptr<::vsg::Node>& content, int x, int y, int w, int h,
                        bool front, const char* what)
{
    bool compile_failed = false;
    auto view = makeCompiledProgramSlotView(*state.viewer, dest.graph.get(), content, x, y, w, h, front, &compile_failed);
    if (view == nullptr) {
        if (compile_failed) {
            diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                               vine::graphics::DiagnosticCategory::CompileFailed,
                               formatDiagnostic(u8"%s view failed to compile; retrying with a full compile", what));
        }
        return false;
    }
    slot.camera = view->camera;
    slot.view   = view;
    slot.ready  = true;
    placeProgramSlotView(state, dest, view, slot.order);
    return true;
}

namespace
{

/**
 * @brief Re-points a slot's sampled-source bindings at a source whose attachments were replaced.
 *
 * A source that was resized IN PLACE (see resizeOffscreenTarget) replaced its images, so the descriptor
 * set this slot's node samples through names views that are gone — while everything else about the slot
 * (the node, its shader set, its pipeline, its view and its command buffer) is still exactly right. The
 * pipeline is the part that cannot be had again for free: vsg caches a VkPipeline on the NODE object,
 * per viewID, so keeping the node and the view is what keeps the compiled pipeline (see
 * GraphicsPipeline::compile).
 *
 * What this does, then, is the ONE thing that has to change: a replacement descriptor set with the same
 * layout, the same samplers and the same non-image descriptors (the shadow block is size-free), and the
 * source's CURRENT views in the image bindings. It is swapped into the node's state group (the set object
 * AND the BindDescriptorSet naming it), and the set it replaced is PARKED: a submitted command buffer may
 * still name its VkDescriptorSet, and it owns the old views.
 *
 * A replacement set has no Vulkan objects until a compile traversal visits it, so the session is flagged
 * (see VsgRendererState::compile_needed) and the frame runs one compile before it records.
 *
 * @param state Session the slot belongs to (its retire ring parks the replaced set).
 * @param slot  Slot whose source bindings to re-point.
 * @param src   The sampled source's entry, as it reads now.
 * @return true when the slot samples the source's current attachments; false when this set names nothing
 *         the source owns (the slot has to be rebuilt to describe it — the caller's old path).
 */
bool repointProgramSlotSource(VsgRendererState& state, ProgramSlot& slot, const VsgRenderTargetEntry& src)
{
    if (slot.source_set == nullptr || slot.node == nullptr) {
        return false;
    }
    auto state_group = slot.node.cast<::vsg::StateGroup>();
    if (state_group == nullptr) {
        return false;
    }
    const std::size_t color_count = src.color_views.size();
    ::vsg::Descriptors descriptors;
    descriptors.reserve(slot.source_set->descriptors.size());
    bool repointed = false;
    for (const auto& descriptor : slot.source_set->descriptors) {
        const auto image = descriptor.cast<::vsg::DescriptorImage>();
        if (image == nullptr) {
            descriptors.push_back(descriptor); // a buffer binding (the shadow block): it has no size, kept as it is
            continue;
        }
        const std::uint32_t  binding = image->dstBinding;
        ::vsg::ImageInfoList infos   = image->imageInfoList;
        if (binding < color_count) {
            for (auto& info : infos) {
                info = ::vsg::ImageInfo::create(info->sampler, src.color_views[binding], info->imageLayout);
            }
            repointed = true;
        }
        else if (binding == color_count && slot.binds_source_depth && src.depth_view != nullptr) {
            // The depth binding follows the ABI (binding == the colour count) and only exists while the
            // source's depth is sampleable — which the caller's staleness check keeps true across a resize.
            for (auto& info : infos) {
                info = ::vsg::ImageInfo::create(info->sampler, src.depth_view, info->imageLayout);
            }
            repointed = true;
        }
        descriptors.push_back(
            ::vsg::DescriptorImage::create(infos, binding, image->dstArrayElement, image->descriptorType));
    }
    if (!repointed) {
        return false; // this set names none of the source's attachments: rebuilding is the honest answer
    }
    auto replacement = ::vsg::DescriptorSet::create(slot.source_set->setLayout, descriptors);
    // The set reaches the record through the BindDescriptorSet that NAMES it (a state group holds state
    // COMMANDS, and a DescriptorSet is not one), and the command itself has to be REPLACED rather than
    // re-pointed: vsg's BindDescriptorSet::compile() CACHES the VkDescriptorSet handle in the command and
    // returns early once it is compiled, and record() binds that cached handle (see vsg/state/
    // BindDescriptorSet.cpp). Assigning `bind->descriptorSet` alone therefore changes nothing the frame
    // records — measured in the app as vkDestroyImageView-01026 (the replaced set kept being bound) followed
    // by a per-frame vkCmdDraw-None-08114 for the destroyed views it still named, which is a black picture.
    // A fresh command has no cached handle, so the frame's compile (state.compile_needed, below) is what
    // fills it in with the replacement.
    // No command naming this set means this node records it through something else entirely, and a rebuild
    // is the honest answer.
    bool rebound = false;
    for (auto& command : state_group->stateCommands) {
        auto bind = command.cast<::vsg::BindDescriptorSet>();
        if (bind == nullptr || bind->descriptorSet != slot.source_set) {
            continue;
        }
        auto follow = ::vsg::BindDescriptorSet::create(bind->pipelineBindPoint, bind->layout, bind->firstSet,
                                                       replacement);
        follow->dynamicOffsets = bind->dynamicOffsets;
        command                = follow;
        rebound                = true;
    }
    if (!rebound) {
        return false;
    }
    state.retireRing.park(slot.source_set);
    slot.source_set        = replacement;
    slot.source_w          = src.width;
    slot.source_h          = src.height;
    slot.source_generation = src.attachments_generation;
    state.compile_needed   = true;
    return true;
}

} // namespace

void drawScreenProgram(VsgRendererState& state, const VsgDiagnostics& diagnostics, vine::graphics::RenderTarget* source,
                       vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                       vine::raw_ptr<const vine::graphics::Camera>        camera)
{
    if (!state.initialized || state.viewer == nullptr || state.window == nullptr || source == nullptr) {
        return;
    }
    if (program == nullptr) {
        // Asked to draw through a program and given none: say so instead of drawing nothing quietly
        // (a pass that reaches here without a program is a host bug, and an empty frame with no
        // reason is exactly what this backend refuses to produce).
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                           u8"drawScreenProgram: no program was given: the pass draws nothing");
        return;
    }

    // Consume the sub-viewport queued by setViewport() (the pass's rectangle).
    const std::optional<vine::graphics::Viewport> viewport = state.request.takeViewport();

    auto src_it = state.targets.find(source);
    if (src_it == state.targets.end() || src_it->second.color_views.empty()) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                           u8"drawScreenProgram: source target has no colour attachment: the pass draws nothing");
        return;
    }
    const auto& src = src_it->second;
    if (src.width <= 0 || src.height <= 0) {
        // Unreachable while the size and the colour views are set
        // and cleared together, kept so a silent content loss is impossible (see D57 there).
        return;
    }

    // Fullscreen-program views are drawn into the CURRENT target
    // (setRenderTarget; nullptr = the window), so their slots live in that
    // target's entry: deferred / post passes can write into an off-screen target
    // as well as the window.
    //
    // The slot is owned by the pass that draws it: the rule lives on the request,
    // so this entry point and the content one cannot disagree.
    const SlotKey slot_key = state.request.slotKey();
    const ProgramSlotDestination overlay =
        resolveProgramSlotDestination(state, diagnostics, source, slot_key, "drawScreenProgram");
    if (overlay.graph == nullptr) {
        return;
    }
    vine::graphics::RenderTarget* const dest       = overlay.target;
    auto&                              dest_entry = state.entryFor(dest);
    const int                          surf_w     = overlay.surf_w;
    const int                          surf_h     = overlay.surf_h;

    // Destination rectangle: the pass' sub-viewport, else the full surface
    // (clamped into the surface - the fullscreen draw has no auto-fit).
    int rect_x = 0, rect_y = 0, rect_w = surf_w, rect_h = surf_h;
    if (viewport && viewport->width > 0 && viewport->height > 0) {
        rect_x = viewport->x;
        rect_y = viewport->y;
        rect_w = viewport->width;
        rect_h = viewport->height;
    }
    if (rect_x < 0) {
        rect_w += rect_x;
        rect_x = 0;
    }
    if (rect_y < 0) {
        rect_h += rect_y;
        rect_y = 0;
    }
    if (rect_w <= 0 || rect_h <= 0) {
        return;
    }
    if (rect_x + rect_w > surf_w) {
        rect_w = surf_w - rect_x;
    }
    if (rect_y + rect_h > surf_h) {
        rect_h = surf_h - rect_y;
    }
    if (rect_w <= 0 || rect_h <= 0) {
        return;
    }

    // The shadow this pass declared (if any), resolved ONCE PER FRAME and used twice: the map is
    // part of the rebuild identity below (a different map is a different descriptor, which cannot
    // be re-pointed in place), and the block's bytes are rewritten further down. Resolving it here
    // rather than only when the slot is rebuilt is what keeps a world-fixed shadow fixed: the block
    // says how to step from THIS pass' view space into light clip, and that step is a function of
    // the live camera, so a block frozen at slot-build time slides across the ground as the camera
    // moves (that is the demo's original "the shadow follows the camera" symptom).
    const detail::ShadowInput resolved_shadow = detail::resolveShadowInput(state, camera, state.request.lights);

    // (Re)build the retained slot when it is missing, the sampled source
    // changed, the program changed, a DIFFERENT shadow
    // map arrived, or the source's depth
    // stopped being sampleable (a pass of the source that preserves depth revokes the
    // promotion, so the depth binding has to go with it).
    //
    // A source whose attachments were replaced AT THE SAME SHAPE (a resize in place — see
    // resizeOffscreenTarget) is deliberately NOT in that list: its colour views are new objects, but the
    // binding SET is the same, so what the slot needs is a re-pointed descriptor (below) rather than a
    // rebuilt node — and keeping the node is what keeps the compiled pipeline, because vsg caches it on
    // the node object, per viewID (GraphicsPipeline::compile; measured ~26 ms per slot to make another).
    // The source's attachments_generation is what tells the two apart.
    //
    // The DESTINATION SURFACE's size is deliberately NOT part of that list, though the node is still
    // built with it (the baked default viewport, see makeFullscreenProgramNode). The node's geometry is
    // the fullscreen triangle and its rectangle is DYNAMIC state: every fragment stage samples by
    // vine_uv, which spans whatever rectangle the view is given, and the rectangle is re-set from the
    // pass' announced viewport at the end of this function (slot.camera->viewportState). A window resize
    // therefore changes nothing the node holds. Leaving it in made every fullscreen program in the
    // window rebuild whenever the window changed size — five pipeline + glslang builds, measured at
    // ~21 ms each — and since that is the frame a resize has to get out before anything at the new size
    // can be on screen, the newly exposed part of the client area stayed black for it (~130 ms on a
    // maximize). The sub-rect programs (a PiP) prove the rectangle is dynamic rather than baked: they
    // sample the whole source inside a rectangle far smaller than the surface they were built for.
    //
    // The map only forces a rebuild when there IS one: the map is a descriptor's image view, which
    // cannot be re-pointed in place, but a map that DISAPPEARS is answered in the per-frame refresh
    // below by writing a disabled block — rebuilding for it would make a program that declares the
    // shadow ABI refuse to build (and so draw nothing) over a shadow it can simply skip.
    const std::uint64_t program_revision = program->revision();
    const auto   existing = dest_entry.program_slots.find(slot_key);
    ProgramSlot* previous = existing == dest_entry.program_slots.end() ? nullptr : &existing->second;
    const char*  stale_reason = programSlotStaleReason(previous, source, src, resolved_shadow.map, program, program_revision);
    const bool   stale        = *stale_reason != '\0';
    // The source's attachments were replaced (a resize in place — see resizeOffscreenTarget). The slot
    // keeps its node, its view and its compiled pipeline, and follows by re-pointing the descriptor set
    // it samples through; when that cannot describe the source, the slot is rebuilt (the old path) rather
    // than left sampling views that are gone.
    const bool source_replaced =
        !stale && previous != nullptr && previous->source_generation != src.attachments_generation;
    const bool repointed = source_replaced && repointProgramSlotSource(state, *previous, src);
    if (stale || (source_replaced && !repointed)) {
        // The previous slot goes through the ONE drop (eraseProgramSlot): its view stops being
        // recorded and its NODE is PARKED on the retire ring instead of being destroyed here. A
        // submitted command buffer may still name that node's pipeline / descriptor sets (the
        // slot was recorded in the frames before this one), and overwriting the slot — what this
        // did — destroyed them in flight. The slot reference below is taken AFTER the drop
        // because the erasure invalidates the entry the map held.
        eraseProgramSlot(state, dest_entry, dest, slot_key);
        // Timed from the drop to the slot becoming ready (see VsgBuildProfile): the node's construction,
        // its glslang translation and its VkPipeline are one indivisible cost at this granularity, because
        // vsg creates the pipeline while compiling the view (GraphicsPipeline::compile).
        const auto slot_build_start = std::chrono::steady_clock::now();
        ProgramSlot& rebuilt = dest_entry.program_slots[slot_key];
        // Capture the pass's explicit order (announced by the engine before
        // this pass) so the fullscreen view stacks at its pipeline position
        // among the target's content slots (e.g. between an opaque depth pass
        // and a forward transparent pass) instead of always drawing first.
        rebuilt.order = state.request.order;
        rebuilt.push_data = ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(LightPushBlock)));
        const VkExtent2D surface{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) };
        ProgramNodeFailure program_failure = ProgramNodeFailure::None;
        // The host asked for the source's depth to be sampleable
        // (RenderTarget::setDepthPromotion) but a pass of the source PRESERVES
        // depth, which overrides that request (§28: LOAD and promotion are
        // mutually exclusive) — so the depth cannot be bound. Say so: a program
        // that samples it will not build, and dropping the binding silently
        // would leave the host guessing why. Reported once per slot build (the
        // slot is stable while the policy is).
        if (source->depthPromotion() && !src.depth_sampleable && src.depth_view != nullptr) {
            diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                               vine::graphics::DiagnosticCategory::ChannelIgnored,
                               formatDiagnostic(u8"drawScreenProgram: sampled target '%s' declares depth promotion, but a"
                                                u8" pass of it preserves depth, so its depth is not sampleable and is not"
                                                u8" bound (the program sees its colour attachments only)",
                                                source->name().empty() ? "(unnamed)" : source->name().as_std_str().c_str()));
        }
        // The source's depth is bound as a sampled texture ONLY when it really
        // ends in SHADER_READ_ONLY: the target's description
        // (RenderTarget::depthPromotion) is the host's request, but a pass of
        // the source that PRESERVES depth overrides it (LOAD and promotion are
        // mutually exclusive, §28), leaving the image in the attachment layout.
        // Binding it as a sampled texture then would declare a layout the image
        // is not in (a descriptor/layout mismatch validation reports every
        // frame). The actual state is what decides.
        // The shadow this pass declared (if any): the engine announced the pass' resolved inputs
        // (RenderBackend::setPassInputs), and the shadow ABI says the first one is the map. Its
        // matrix comes from the TARGET, not from a second derivation of the light camera: the
        // pipeline that built that camera wrote its view-projection once
        // (RenderTarget::setProducerViewProjection), and all that is missing here is the step from
        // view space (where the shading has the fragment) into light clip.
        FullscreenShadowInput shadow;
        if (resolved_shadow.map != nullptr) {
            shadow.map = resolved_shadow.map;
            // The block is handed over as bytes because that is what a descriptor binding takes:
            // the ABI struct's layout IS the binding's contract (asserted by test_graphics). The
            // bytes are only the initial value; the slot keeps the object and rewrites it every
            // frame (see the refresh after this block).
            auto data = ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(resolved_shadow.block)));
            std::memcpy(data->dataPointer(), &resolved_shadow.block, sizeof(resolved_shadow.block));
            data->properties.dataVariance = ::vsg::DYNAMIC_DATA;
            shadow.block                  = std::move(data);
        }
        const auto node_build_start = std::chrono::steady_clock::now();
        // The node's descriptor set is handed back so this slot can re-point it later (see
        // repointProgramSlotSource) instead of rebuilding the node for a new set of source views.
        ::vsg::ref_ptr<::vsg::DescriptorSet> node_source_set;
        auto node = makeFullscreenProgramNode(program, src.color_views,
                                              src.depth_sampleable ? src.depth_view : ::vsg::ref_ptr<::vsg::ImageView>(),
                                              shadow, surface, overlay.color_count, rebuilt.push_data, &node_source_set,
                                              &program_failure);
        state.build_profile.slots_node_ns += elapsedNs(node_build_start);
        if (node == nullptr) {
            const vine::String why =
                program_failure == ProgramNodeFailure::NoCompiler
                    ? u8"fullscreen program needs the runtime GLSL compiler, which is unavailable"
                    : program_failure == ProgramNodeFailure::NoFragmentStage
                          ? u8"fullscreen program has no fragment stage"
                          : program_failure == ProgramNodeFailure::MissingDescriptorBinding
                                ? u8"fullscreen program samples a texture this pass cannot provide (the source binds"
                                  u8" its colour attachments, plus its depth only while that one is sampleable)"
                                : u8"fullscreen program shader failed to compile";
            diagnostics.report(vine::graphics::DiagnosticSeverity::Error,
                               vine::graphics::DiagnosticCategory::CompileFailed,
                               why + vine::String(u8": the pass draws nothing"));
            // Nothing of this attempt was recorded, so the drop is the same one home (it parks the
            // slot's node — null here — and erases the slot) rather than a second way to erase.
            eraseProgramSlot(state, dest_entry, dest, slot_key);
            return;
        }
        rebuilt.source_target = source;
        rebuilt.source_w = src.width;
        rebuilt.source_h = src.height;
        rebuilt.source_depth_sampleable = src.depth_sampleable;
        // Whether the node BOUND the depth is the shader's decision, not the
        // policy's: the depth descriptor only exists when the fragment stage
        // declares the ABI binding (the colour count). A colour-only program
        // keeps drawing whatever the source's depth promotion does — including
        // the frame a pass of the source revokes it, where a slot that DOES bind
        // the depth has to be dropped (its descriptor names a layout the image
        // leaves later in that same frame, see the promotion cascade).
        rebuilt.binds_source_depth = rebuilt.source_depth_sampleable &&
                                     programSamplesDepth(program, src.color_views.size());
        // The graph the slot records into is looked up from the owning pass when the slot is
        // dropped (see eraseProgramSlot), so the slot does not remember a second copy of it.
        rebuilt.program          = vine::intrusive_ptr<const vine::graphics::ShaderProgram>(program);
        rebuilt.program_revision = program_revision;
        rebuilt.node             = node;
        // What this slot samples through, and which source attachments it was built for (see
        // repointProgramSlotSource).
        rebuilt.source_set        = node_source_set;
        rebuilt.source_generation = src.attachments_generation;
        // Keep the shadow the node bound: its map is the rebuild identity (see `stale` above) and
        // its block is the object the per-frame refresh rewrites (it is the same object the
        // descriptor was assigned, so mutating its bytes + dirty() is what reaches the GPU).
        rebuilt.shadow_view  = resolved_shadow.map;
        rebuilt.shadow_block = shadow.block;

        // Create + compile the fullscreen view against this target's render pass
        // (inserted provisionally at the front so the compile sees it), then move
        // it to its explicit-order position.
        const auto view_compile_start = std::chrono::steady_clock::now();
        const bool view_installed = installProgramSlotView(state, diagnostics, overlay, rebuilt, node, rect_x, rect_y,
                                                          rect_w, rect_h, /*front*/ true, "fullscreen program");
        state.build_profile.slots_view_ns += elapsedNs(view_compile_start);
        if (!view_installed) {
            // The compile already reported (when it was the compile): drop the
            // half-made slot so the next frame retries. This one was never recorded, and the drop
            // is the same one home as above.
            eraseProgramSlot(state, dest_entry, dest, slot_key);
            return;
        }
        ++state.program_slot_build_count;
        ++state.build_profile.slots;
        state.build_profile.slots_ns += elapsedNs(slot_build_start);
        V_LOGI("[VsgRenderer] EXPERIMENTAL deferred fullscreen program {}x{} -> {} {},{},{}x{} attached ({})", src.width,
               src.height, dest == nullptr ? "window" : "offscreen", rect_x, rect_y, rect_w, rect_h, stale_reason);
    }

    // The slot this pass owns, now that it exists in every path that reaches here.
    auto& slot = dest_entry.program_slots[slot_key];

    // Rewrite the shadow block's bytes EVERY frame, next to the view-space light block taken just
    // below and for the same reason: the block's matrix steps from this pass' view space into light
    // clip, so it is recomputed from the live camera. The slot rebuild above only covers the
    // DESCRIPTOR (which map is bound); the matrix inside the block is a per-frame value. Writing it
    // only during a rebuild froze it at the camera the slot was built with, which makes a
    // world-fixed shadow slide with the camera. The write is unconditional, including when the map
    // is gone: resolveShadowInput then hands back a DISABLED block (params.x = 0), so the stale map
    // still bound to the descriptor is never sampled.
    if (slot.shadow_block != nullptr) {
        std::memcpy(slot.shadow_block->dataPointer(), &resolved_shadow.block, sizeof(resolved_shadow.block));
        slot.shadow_block->dirty();
    }

    // Take the lights announced for this draw call (from the pass's content
    // scene) and push view-space light parameters before record. An empty list
    // seeds a small default ambient (see fillLightPushBlock) so a fullscreen
    // program pass that carries no lights still shades its albedo instead of
    // rendering black.
    std::vector<const vine::graphics::Light*> lights = state.request.takeLights();
    LightPushBlock block{};
    fillLightPushBlock(camera, lights, block);
    if (slot.push_data != nullptr && slot.push_data->dataSize() >= sizeof(block)) {
        std::memcpy(slot.push_data->dataPointer(), &block, sizeof(block));
    }

    if (slot.ready && slot.detached) {
        // Re-attach a slot retired while its pass was inactive (see
        // retireInactivePassSlots): its node and pipeline were kept.
        placeProgramSlotView(state, overlay, slot.view, slot.order);
        slot.detached = false;
    }

    // Follow the requested sub-viewport each frame — IN PLACE (see setSlotViewportRect): a fresh
    // vsg::ViewportState per frame per slot allocated for a rectangle vsg re-emits from the state it
    // already records, which is the same bargain the content path makes (see updateSlotViewport).
    if (slot.camera != nullptr) {
        detail::setSlotViewportRect(slot.camera->viewportState, rect_x, rect_y, rect_w, rect_h);
    }
}

} // namespace detail

V_VSG_NS_END
