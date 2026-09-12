#include <vine/vsg/VsgRenderer.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <vsg/app/View.h>
#include <vsg/lighting/Light.h>
#include <vsg/state/ViewportState.h>
#include <vsg/state/material.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/RenderPass.h>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

#include <vine/vsg/VsgUtils.hpp>
#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>

V_VSG_NS_BEGIN

// This translation unit is one of several that share a single free-function
// layer (VsgPipelineFactory.hpp / VsgBackendUtility.hpp); the directive keeps
// its call sites unqualified.
using namespace detail;

namespace
{

/** @brief Keeps a content slot's camera viewport in step with its role.
 *
 * Presenting (full-target) content always fills the whole target; other content
 * carries its pass sub-viewport when one was queued, and the full target when none
 * was (an unset or empty sub-viewport means "the whole target"). This runs every
 * frame because the slot is created lazily (on its first render) and the target may
 * have been resized before that.
 *
 * @param camera     Slot camera whose baked viewport is updated.
 * @param presenting Whether the slot fills the target (vs. composites into it).
 * @param viewport   Sub-viewport the pass announced, if any.
 * @param surf_w     Target surface width in device pixels.
 * @param surf_h     Target surface height in device pixels.
 */
void updateSlotViewport(::vsg::Camera& camera, bool presenting, const std::optional<vine::graphics::Viewport>& viewport,
                        int surf_w, int surf_h)
{
    if (!presenting && viewport && viewport->width > 0 && viewport->height > 0) {
        camera.viewportState = ::vsg::ViewportState::create(
            viewport->x, viewport->y, static_cast<uint32_t>(viewport->width), static_cast<uint32_t>(viewport->height));
        return;
    }
    camera.viewportState =
        ::vsg::ViewportState::create(VkExtent2D{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) });
}

/** @brief Seeds a slot's default light after its presenting role flipped.
 *
 * The seed keeps a slot whose scene carries no usable lights readable: the window's
 * presenting slot gets vsg's default headlight, everything else an ambient fill (a
 * directional headlight would shade an axis gizmo dark from diagonal views). A scene
 * that provides lights replaces the seed every frame (see setGroupLights).
 *
 * @param light_group    Group holding the slot's lights (cleared first).
 * @param want_headlight Whether the slot should be seeded with the headlight.
 * @param presenting     Slot's presenting role (picks the ambient name).
 */
void seedSlotLight(::vsg::Group& light_group, bool want_headlight, bool presenting)
{
    light_group.children.clear();
    if (want_headlight) {
        light_group.addChild(::vsg::createHeadlight());
    }
    else {
        light_group.addChild(makeAmbientLight(presenting ? "offscreen_ambient" : "content_ambient"));
    }
}

/** @brief Whether an all-lights-dropped episode has to be reported right now.
 *
 * "Every announced light was disabled or of an unsupported kind" is a property of
 * the scene, not of a frame: it is reported once per episode and re-armed as soon as
 * any usable light shows up again (or the pass announces none at all, which is the
 * normal "keep the slot's light" request).
 *
 * @param announced Number of lights the pass announced this frame.
 * @param attached  Number of lights actually attached.
 * @param reported  The slot's episode flag (raised when true is returned).
 * @return true when the caller must report the episode.
 */
bool beginLightsDroppedEpisode(std::size_t announced, std::size_t attached, bool& reported)
{
    if (attached != 0u || announced == 0u) {
        reported = false; // usable lights (or none announced): re-arm the report
        return false;
    }
    if (reported) {
        return false; // already reported for this episode
    }
    reported = true;
    return true;
}

/** @brief Logs a content slot's per-frame counts when VINE_VSG_DIAG_MRT is set.
 *
 * TEMP diagnostics (see the MRT notes): tell "no geometry was collected" apart from
 * "geometry was collected but not rasterised", and confirm pipeline sharing
 * (variants << commands when states repeat). Env-gated so it costs nothing normally.
 *
 * @param target        Target the slot draws into (nullptr = the window).
 * @param depth_mode    The pass' depth policy (logged as its enum value).
 * @param order         The pass' explicit pipeline order.
 * @param commands      Commands collected for this slot this frame.
 * @param created       Subtrees the bridge built for this slot this frame.
 * @param root_children Children under the slot's retained root.
 * @param variants      Distinct pipeline variants the bridge registered.
 */
void logContentSlotDiagnostics(const vine::graphics::RenderTarget* target, vine::graphics::DepthMode depth_mode,
                               int order, std::size_t commands, std::size_t created, std::size_t root_children,
                               std::size_t variants)
{
    if (std::getenv("VINE_VSG_DIAG_MRT") == nullptr) {
        return;
    }
    std::fprintf(stderr, "[MRT-DIAG] target=%s depth_mode=%d order=%d commands=%zu created=%zu rootChildren=%zu variants=%zu\n",
                 target == nullptr ? "window" : (target->name().empty() ? "offscreen" : target->name().stdstr().c_str()),
                 static_cast<int>(depth_mode), order, commands, created, root_children, variants);
}

} // namespace

void VsgRenderer::beginPass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    if (impl.pass_open) {
        // The engine runs one pass at a time; a nested beginPass means the
        // previous scope was never ended, so its announced state would silently
        // apply to the new pass. Report it and start clean.
        reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                      vine::graphics::DiagnosticCategory::PassProtocolViolation,
                      u8"beginPass() while a pass scope is open: the open pass' request was dropped");
    }
    // A pass opens a CLEAN request: no pass may inherit what an earlier one
    // announced. The direct-driver path (no beginPass) keeps its queue instead.
    resetPassRequest();
    impl.request.pass = pass;
    impl.pass_open    = true;
    if (pass != nullptr) {
        // The pass owns its retained slot and counts as active this frame: a
        // pass that is not announced again next frame is retired (see
        // retireInactivePassSlots), which is what makes disabling it take effect.
        impl.passes_active_this_frame.insert(pass);
        impl.pass_protocol_used = true;
    }
}

bool VsgRenderer::isPassScopeOpen() const
{
    return impl.pass_open;
}

void VsgRenderer::endPass()
{
    if (!impl.pass_open) {
        // Reported because it means the pass protocol is out of step: the
        // state announced since the last endPass (or beginPass) had already
        // been dropped, so whatever the caller expected to apply did not.
        reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                      vine::graphics::DiagnosticCategory::PassProtocolViolation,
                      u8"endPass() without an open pass scope: the announced request was already dropped");
    }
    // Close the scope: everything the pass announced is dropped here (including
    // scope attributes no draw call consumed), so nothing can apply to the next
    // pass.
    impl.pass_open = false;
    resetPassRequest();
}

void VsgRenderer::Impl::unhookTargetPasses(Target& t)
{
    for (auto& pass : t.passes) {
        removeGraphChild(command_graph.get(), pass.second.graph);
    }
    // Destructive teardown keeps the counted device wait (§3): the bridge caches
    // dropped below release the shared object registry, whose pipelines / samplers
    // the retained nodes do not necessarily keep alive as the only owner. Parking
    // the views instead was measured to trip vkDestroyPipeline-00765 /
    // vkDestroySampler-01082, so this is the wait, not a park.
    waitForIdle();
    for (auto& slot_entry : t.content_slots) {
        slot_entry.second.bridge.clearCache();
        // A dropped slot must not stay queued for the frame's incremental compile:
        // its view no longer belongs to any target.
        const auto& view  = slot_entry.second.view;
        auto&       queue = pending_compile_views;
        queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
    }
}

void VsgRenderer::Impl::detachSlotView(Target&                            owner,
                                      vine::graphics::RenderTarget*      owner_key,
                                      const SlotKey&                     key,
                                      const ::vsg::ref_ptr<::vsg::View>& view)
{
    if (auto graph = owner.slotGraph(owner, owner_key, key); graph != nullptr) {
        removeGraphChild(graph.get(), view);
    }
    // A dropped view must not stay queued for the frame's incremental compile —
    // only content slots queue their views, so this is a no-op for the other
    // kinds (which compile the moment they are built).
    auto& queue = pending_compile_views;
    queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
}

void VsgRenderer::erasePassSlotsFromTarget(vine::graphics::RenderTarget* target,
                                           const vine::graphics::RenderPass* pass)
{
    if (pass == nullptr) {
        return;
    }
    const auto target_entry = impl.targets.find(target);
    if (target_entry == impl.targets.end()) {
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
    impl.waitForIdle();

    t.visitSlot(key, [&](auto& slot, Impl::Target::SlotKind kind) {
        impl.detachSlotView(t, target, key, slot.view);
        // Only a content slot owns a bridge (its caches go with the slot).
        if constexpr (requires { slot.bridge; }) {
            slot.bridge.clearCache();
        }
        t.eraseSlot(kind, key);
    });
}

void VsgRenderer::retargetPass(const vine::graphics::RenderPass* pass,
                               vine::graphics::RenderTarget*     target)
{
    if (pass == nullptr) {
        return;
    }
    // A pass keeps exactly one retained slot per target. When it draws into a
    // different target than before, the slot it left behind would otherwise
    // keep drawing its content there forever.
    for (auto& entry : impl.targets) {
        if (entry.first == target) {
            continue;
        }
        erasePassSlotsFromTarget(entry.first, pass);
    }
}

void VsgRenderer::retireInactivePassSlots()
{
    if (!impl.pass_protocol_used) {
        return; // direct driver (legacy keys): nothing is pass-owned
    }
    // A slot needs retiring when its pass did not execute this frame and its
    // view is still attached. Already-retired slots are skipped, so a pass that
    // stays disabled costs nothing per frame (no scan hit, no detach, no
    // repeated diagnostic).
    const auto needs_retire = [this](const SlotKey& key, bool detached) {
        return !detached && key.owner != nullptr &&
               impl.passes_active_this_frame.count(key.owner) == 0;
    };
    // NO device wait here: this path DETACHES a view from its graph and keeps the
    // slot (the view, its node and the compiled pipelines stay referenced by the
    // slot), so nothing is destroyed and nothing a pending command buffer names
    // can go away. Disabling a pass is a per-frame host decision of an editor, and
    // stopping the device for it would be a stall for no lifetime reason.
    bool any = false;
    for (auto& entry : impl.targets) {
        auto& t = entry.second;
        t.forEachSlot([&](const SlotKey& key, auto& slot, auto) {
            if (!needs_retire(key, slot.detached)) {
                return;
            }
            impl.detachSlotView(t, entry.first, key, slot.view);
            slot.detached = true;
            any          = true;
        });
    }
    if (!any) {
        return; // nothing changed: no re-order, no log line
    }
    // Dropping a view can remove a command-graph dependency edge.
    reconcileOffscreenOrder();
    V_LOGI("[VsgRenderer] retired (detached) the retained view of pass(es) not active this frame");
}

void VsgRenderer::releasePass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    const vine::graphics::RenderPass* removed = pass;
    for (auto& entry : impl.targets) {
        erasePassSlotsFromTarget(entry.first, removed);
    }
    impl.passes_active_this_frame.erase(removed);
    if (impl.request.pass == removed) {
        impl.request.pass = nullptr;
    }
    // Dropping a sampling slot can change the off-screen record order.
    reconcileOffscreenOrder();
}

void VsgRenderer::setupContentSlot(const SlotKey& key, vine::graphics::RenderTarget* target, vine::raw_ptr<const vine::graphics::Camera> camera, int order, vine::graphics::DepthMode depth_mode, bool presenting)
{
    // Content slots are retained Views under the TARGET's render graph — the
    // window target (target == nullptr) and every off-screen target share
    // this one mechanism. Each pass is its own View + bridge, so several
    // passes sharing one camera and order still stack as separate content.
    auto& t          = impl.entryFor(target);
    auto& content    = t.content_slots[key];
    if (content.ready) {
        return;
    }
    // The graph this pass records into: the window session's single swapchain
    // graph, or an off-screen graph created for THIS pass from its own clear
    // request (see passGraph). A pass that cannot get one — an off-screen
    // target that failed to build — drops the half-made slot, reported because
    // the pass then draws nothing and the host may have no other way to learn
    // the target is unusable.
    const auto graph = passGraph(target, key);
    if (graph == nullptr) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error,
                      vine::graphics::DiagnosticCategory::TargetBuildFailed,
                      u8"no render graph for the pass' target: the pass draws nothing");
        t.content_slots.erase(key);
        return;
    }
    content.order      = order;
    content.depth_mode = depth_mode;
    content.presenting = presenting;
    content.vsg_camera = persistent.cameraBridge.create(camera);
    if (content.vsg_camera == nullptr) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error,
                      vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"camera bridge could not be created: the pass draws nothing");
        t.content_slots.erase(key);
        return;
    }
    content.root = ::vsg::Group::create();

    // Per-slot pipeline bridge. vsg compiles pipelines per viewID, so every
    // content slot keeps its own SceneBridge (sharing already-compiled
    // pipelines across views crashes GraphicsPipeline::vk()); the bridge's
    // shader set bakes the slot's depth policy.
    if (target == nullptr) {
        // Window slots share the renderer's (window-sized) shader sets.
        content.bridge.setShaderSet(depth_mode == vine::graphics::DepthMode::TestAndWrite ? impl.depth_on_shader_set
                                    : depth_mode == vine::graphics::DepthMode::TestOnly ? impl.depth_testonly_shader_set
                                                                                         : impl.depth_off_shader_set);
    }
    else {
        // Off-screen slots get a per-target shader set baked at the target's
        // size (created lazily).
        auto& set_ref = depth_mode == vine::graphics::DepthMode::TestAndWrite ? t.depth_on_shader_set
                        : depth_mode == vine::graphics::DepthMode::TestOnly ? t.depth_testonly_shader_set
                                                                            : t.depth_off_shader_set;
        if (set_ref == nullptr) {
            const bool depth_test  = depth_mode != vine::graphics::DepthMode::Disabled;
            const bool depth_write = depth_mode == vine::graphics::DepthMode::TestAndWrite;
            set_ref = buildShaderSet(persistent.shader_preset,
                                     VkExtent2D{ static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) },
                                     depth_test, depth_write,
                                     target->colorCount());
        }
        content.bridge.setShaderSet(set_ref);
    }
    content.bridge.setMaterialManager(&persistent.materialManager);
    // Route this slot's rejections through the renderer's diagnostics (trace,
    // counters, host sink): the slot is what actually discovers them.
    installDiagnosticRoute(content.bridge);
    // The pass' depth policy reaches the pipeline through the bridge (it fills
    // the depth item of content that did not author one), so it must be set
    // before the slot's first sync; a later change invalidates the state.
    content.bridge.setContentDepthMode(depth_mode);
    content.bridge.clearCache();

    // Seed the slot's default light before the first compile. A slot whose
    // scene carries no lights (checked per frame) keeps this seed: the
    // window's presenting (full-target) slot gets vsg's default headlight,
    // everything else an ambient fill — ambient keeps HUD / off-screen content
    // readable from any angle (a directional headlight would shade the axis
    // gizmo dark from diagonal views). When the content scene provides lights
    // they replace this seed each frame, so the light source always reflects
    // the scene, never the slot's depth style.
    content.light_group = ::vsg::Group::create();
    content.headlight_seed = (presenting && target == nullptr);
    if (content.headlight_seed) {
        content.light_group->addChild(::vsg::createHeadlight());
    }
    else {
        content.light_group->addChild(makeAmbientLight(presenting ? "offscreen_ambient" : "content_ambient"));
    }

    content.view = ::vsg::View::create(content.vsg_camera);
    content.view->addChild(content.light_group);
    content.view->addChild(content.root);

    // Position the slot's View in the target's render graph by its explicit
    // order, then compile it before it is first recorded. Content slots are
    // only created from render() calls that follow initialize(), so the
    // target's graph is always present here.
    //
    // Within a target the slot views are stacked in ASCENDING pass order —
    // the order the caller gave addPass() and the engine already runs passes
    // in (placeViewByOrder keeps content, fullscreen-program and PiP / present
    // views all sorted by it). No main/on-top semantic constrains the order —
    // a pass positioned by the user at any order draws exactly there. Ordering
    // by the explicit value also keeps the pre-frame warm-up safe: warm-up may
    // create a higher-order (on-top) slot before a lower-order (main) slot has
    // run, but the main slot is inserted ahead of it by its smaller order when
    // it is finally created.
    placeViewByOrder(graph, target, content.view, content.order);
    content.ready = true;
    if (impl.viewer != nullptr) {
        impl.viewer->compile();
    }
}

void VsgRenderer::renderContentSlot(const ContentSlotRequest& request)
{
    if (request.commands == nullptr || request.lights == nullptr) {
        return; // no command stream / light list to draw from
    }
    // The slot is owned by the pass that draws it (pass scope) or, for a
    // direct driver, by the historical (camera, order) pair.
    const SlotKey key = (impl.request.pass != nullptr)
                            ? SlotKey::ownerPass(impl.request.pass)
                            : SlotKey::cameraOrder(request.camera, request.order);

    auto& t  = impl.entryFor(request.target);
    auto  it = t.content_slots.find(key);
    if (it == t.content_slots.end() || !it->second.ready) {
        setupContentSlot(key, request.target, request.camera, request.order, request.depth_mode, request.presenting);
        it = t.content_slots.find(key);
    }
    if (it == t.content_slots.end() || !it->second.ready) {
        return; // slot could not be built (e.g. camera bridge failed)
    }
    auto& content = it->second;
    // The graph this pass records into (see passGraph): the window's swapchain
    // graph, or this pass' own off-screen graph — created on the slot's first
    // render and reused every frame after.
    const auto graph = passGraph(request.target, key);

    if (content.detached) {
        // The pass executes again after having been retired: re-attach its
        // retained view (its data and pipelines were kept, so no upload /
        // recompile is needed).
        placeViewByOrder(graph, request.target, content.view, content.order);
        content.detached = false;
        // Re-attaching is what puts its graph back into the command graph: a
        // retired pass' graph is deliberately left out of it (see
        // reconcileOffscreenOrder).
        if (request.target != nullptr) {
            reconcileOffscreenOrder();
        }
    }

    // The pass' properties are re-applied every frame, so changing them at run
    // time takes effect instead of leaving the slot with the state it was
    // first built with:
    //  - the depth policy is forwarded to the bridge (which rebuilds only the
    //    state wrappers, not the vertex data) and invalidates them on change;
    //  - the explicit pipeline order moves the view to its new stacking slot;
    //  - the presenting role drives the viewport each frame and re-seeds the
    //    slot's default light when it flips.
    if (content.depth_mode != request.depth_mode) {
        // No device wait: the state wrappers being dropped are PARKED by the
        // bridge itself (SceneBridge::invalidateState), so the pipelines they own
        // stay alive until every slot that could have recorded them has been
        // re-recorded. The depth mode is a per-frame host decision (a UI toggling
        // depth test), so waiting here would stall the device on every frame it
        // changes — measured: 14 waits over 15 flipping frames before this.
        content.depth_mode = request.depth_mode;
        content.bridge.setContentDepthMode(request.depth_mode);
        content.bridge.invalidateState();
    }
    if (content.order != request.order) {
        content.order = request.order;
        placeViewByOrder(graph, request.target, content.view, request.order);
    }
    if (content.presenting != request.presenting) {
        content.presenting       = request.presenting;
        const bool want_headlight = (request.presenting && request.target == nullptr);
        if (content.headlight_seed != want_headlight) {
            content.headlight_seed = want_headlight;
            seedSlotLight(*content.light_group, want_headlight, request.presenting);
        }
    }

    // Full target extent for this slot's viewport: the live swapchain size for
    // the window target, the off-screen target's logical size otherwise.
    const int surf_w = (request.target == nullptr) ? static_cast<int>(impl.window->extent2D().width) : t.width;
    const int surf_h = (request.target == nullptr) ? static_cast<int>(impl.window->extent2D().height) : t.height;

    // Keep the slot's vsg camera viewport in step with its role each frame (see
    // updateSlotViewport): presenting content fills the target, other content carries
    // its pass sub-viewport.
    updateSlotViewport(*content.vsg_camera, content.presenting, request.viewport, surf_w, surf_h);

    persistent.cameraBridge.apply(request.camera, content.vsg_camera);

    // Lights come from the pass' content scene each frame (the scene is the source of
    // truth); setGroupLights leaves the slot's seeded default light in place unless at
    // least one announced light is usable (see beginLightsDroppedEpisode).
    const std::size_t attached_lights = setGroupLights(content.light_group.get(), *request.lights);
    if (beginLightsDroppedEpisode(request.lights->size(), attached_lights, content.light_fallback_reported)) {
        reportFailure(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ChannelIgnored,
                      formatDiagnostic(u8"%zu announced light(s) are all disabled or of an unsupported "
                                       u8"kind; the pass keeps its default light",
                                       request.lights->size()));
    }

    // The command stream is the source of truth: reconcile the retained slot
    // root against it (in-place for moves/material edits). The legacy own-
    // window debug path skips syncing the window's presenting slot.
    if (!(request.target == nullptr && forceOwnWindow() && content.presenting)) {
        std::vector<::vsg::ref_ptr<::vsg::Node>> created;
        content.bridge.syncRenderCommands(*request.commands, content.root.get(), &created);
        if (!created.empty()) {
            // Queue this slot's VIEW for an incremental (re)compile in
            // submitFrame(): traversing the View sets the correct viewID, so
            // the new/rebuild subtrees compile for the view they will be
            // recorded under (D22). One entry per view per frame.
            auto& pending = impl.pending_compile_views;
            if (std::find(pending.begin(), pending.end(), content.view) == pending.end()) {
                pending.push_back(content.view);
            }
        }
        // TEMP diagnostics, env-gated: how many commands this slot collected, how
        // many subtrees were built and how many pipeline variants exist (see
        // logContentSlotDiagnostics).
        logContentSlotDiagnostics(request.target, request.depth_mode, request.order, request.commands->size(),
                                  created.size(), content.root->children.size(),
                                  content.bridge.pipelineVariantCount());
    }
}

V_VSG_NS_END
