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
        if (!dest_entry.attachments_built || dest_entry.width != dest->width() || dest_entry.height != dest->height()) {
            detail::buildOffscreenTarget(state, diagnostics, dest);
            if (!dest_entry.attachments_built) {
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

    // Destination rectangle: the ONE rule the content path uses too (detail::passDrawRect) - the rectangle
    // this pass announced, or the whole target, clamped. A program pass honours what it was given, which is
    // what a preview, an axis gizmo or a HUD in a corner all rely on.
    const vine::graphics::Viewport rect = detail::passDrawRect(viewport, surf_w, surf_h);
    const int rect_x = rect.x;
    const int rect_y = rect.y;
    const int rect_w = rect.width;
    const int rect_h = rect.height;
    if (rect_w <= 0 || rect_h <= 0) {
        return; // nothing to draw: the rule clamped this pass' rectangle away
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
    // changed (or was resized: its colour views were rebuilt), the program changed, a DIFFERENT shadow
    // map arrived, or the source's depth
    // stopped being sampleable (a pass of the source that preserves depth revokes the
    // promotion, so the depth binding has to go with it).
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
    const auto          existing         = dest_entry.program_slots.find(slot_key);
    const ProgramSlot*  previous = existing == dest_entry.program_slots.end() ? nullptr : &existing->second;
    const bool stale = previous == nullptr || !previous->ready || previous->source_target != source ||
                       previous->source_w != src.width || previous->source_h != src.height ||
                       previous->source_depth_sampleable != src.depth_sampleable ||
                       (resolved_shadow.map != nullptr && previous->shadow_view != resolved_shadow.map) ||
                       previous->program.get() != program || previous->program_revision != program_revision;
    if (stale) {
        // The previous slot goes through the ONE drop (eraseProgramSlot): its view stops being
        // recorded and its NODE is PARKED on the retire ring instead of being destroyed here. A
        // submitted command buffer may still name that node's pipeline / descriptor sets (the
        // slot was recorded in the frames before this one), and overwriting the slot — what this
        // did — destroyed them in flight. The slot reference below is taken AFTER the drop
        // because the erasure invalidates the entry the map held.
        eraseProgramSlot(state, dest_entry, dest, slot_key);
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
        // The shadow this pass' map declares: `RenderTarget::setShadowOf` names the light a target is the
        // shadow of, and the resolver finds it among the announced inputs without any inference. Its matrix
        // comes from the TARGET, not from a second derivation of the light camera: the pipeline that built
        // that camera wrote its view-projection once (RenderTarget::setProducerViewProjection), and all that
        // is missing here is the step from view space (where the shading has the fragment) into light clip.
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
        auto node = makeFullscreenProgramNode(program, src.color_views,
                                              src.depth_sampleable ? src.depth_view : ::vsg::ref_ptr<::vsg::ImageView>(),
                                              shadow, surface, rebuilt.push_data, &program_failure);
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
        // Keep the shadow the node bound: its map is the rebuild identity (see `stale` above) and
        // its block is the object the per-frame refresh rewrites (it is the same object the
        // descriptor was assigned, so mutating its bytes + dirty() is what reaches the GPU).
        rebuilt.shadow_view  = resolved_shadow.map;
        rebuilt.shadow_block = shadow.block;

        // Create + compile the fullscreen view against this target's render pass
        // (inserted provisionally at the front so the compile sees it), then move
        // it to its explicit-order position.
        if (!installProgramSlotView(state, diagnostics, overlay, rebuilt, node, rect_x, rect_y, rect_w, rect_h,
                               /*front*/ true,
                               "fullscreen program")) {
            // The compile already reported (when it was the compile): drop the
            // half-made slot so the next frame retries. This one was never recorded, and the drop
            // is the same one home as above.
            eraseProgramSlot(state, dest_entry, dest, slot_key);
            return;
        }
        ++state.program_slot_build_count;
        V_LOGI("[VsgRenderer] EXPERIMENTAL deferred fullscreen program {}x{} -> {} {},{},{}x{} attached", src.width,
               src.height, dest == nullptr ? "window" : "offscreen", rect_x, rect_y, rect_w, rect_h);
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

    // Follow the requested sub-viewport each frame.
    slot.camera->viewportState = ::vsg::ViewportState::create(rect_x, rect_y, static_cast<uint32_t>(rect_w), static_cast<uint32_t>(rect_h));
}

} // namespace detail

V_VSG_NS_END
