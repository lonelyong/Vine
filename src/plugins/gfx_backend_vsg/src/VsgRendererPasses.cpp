#include <vine/vsg/VsgRenderer.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
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

#include "VsgUtils.hpp"
#include "VsgRendererImpl.hpp"
#include "VsgBackendUtility.hpp"
#include "VsgPipelineFactory.hpp"

V_VSG_NS_BEGIN

// This translation unit is one of several that share a single free-function
// layer (VsgPipelineFactory.hpp / VsgBackendUtility.hpp); the directive keeps
// its call sites unqualified.
using namespace detail;

void VsgRenderer::beginPass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    if (impl->pass_open) {
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
    impl->request.pass = pass;
    impl->pass_open    = true;
    if (pass != nullptr) {
        // The pass owns its retained slot and counts as active this frame: a
        // pass that is not announced again next frame is retired (see
        // retireInactivePassSlots), which is what makes disabling it take effect.
        impl->passes_active_this_frame.insert(pass);
        impl->pass_protocol_used = true;
    }
}

bool VsgRenderer::isPassScopeOpen() const
{
    return impl->pass_open;
}

void VsgRenderer::endPass()
{
    if (!impl->pass_open) {
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
    impl->pass_open = false;
    resetPassRequest();
}

void VsgRenderer::erasePassSlotsFromTarget(vine::graphics::RenderTarget* target,
                                           const vine::graphics::RenderPass* pass)
{
    if (pass == nullptr) {
        return;
    }
    const auto target_entry = impl->targets.find(target);
    if (target_entry == impl->targets.end()) {
        return;
    }
    auto&      t   = target_entry->second;
    const SlotKey key = SlotKey::ownerPass(pass);
    // Nothing to do for a pass this target holds no slot for: avoid a device
    // wait on the common path (a pass that moved targets usually owns a slot
    // in only one of them).
    if (t.content_slots.find(key) == t.content_slots.end() &&
        t.screen_slots.find(key) == t.screen_slots.end() &&
        t.program_slots.find(key) == t.program_slots.end()) {
        return;
    }
    // Wait BEFORE detaching / dropping anything: the views, pipelines and
    // samplers about to be destroyed may still be referenced by a submitted
    // command buffer (destroying them first trips VUID-vkDestroyPipeline /
    // vkDestroySampler).
    waitForIdle(impl->viewer.get());
    // A dropped view must not stay queued for the frame's incremental compile.
    const auto forget_view = [this](const ::vsg::ref_ptr<::vsg::View>& view) {
        auto& queue = impl->pending_compile_views;
        queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
    };

    if (const auto it = t.content_slots.find(key); it != t.content_slots.end()) {
        removeGraphChild(t.graph.get(), it->second.view);
        forget_view(it->second.view);
        it->second.bridge.clearCache();
        t.content_slots.erase(it);
    }
    if (const auto it = t.screen_slots.find(key); it != t.screen_slots.end()) {
        removeGraphChild(t.graph.get(), it->second.view);
        t.screen_slots.erase(it);
    }
    if (const auto it = t.program_slots.find(key); it != t.program_slots.end()) {
        removeGraphChild(t.graph.get(), it->second.view);
        t.program_slots.erase(it);
    }
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
    for (auto& entry : impl->targets) {
        if (entry.first == target) {
            continue;
        }
        erasePassSlotsFromTarget(entry.first, pass);
    }
}

void VsgRenderer::retireInactivePassSlots()
{
    if (!impl->pass_protocol_used) {
        return; // direct driver (legacy keys): nothing is pass-owned
    }
    // A slot needs retiring when its pass did not execute this frame and its
    // view is still attached. Already-retired slots are skipped, so a pass that
    // stays disabled costs nothing per frame (no scan hit, no device wait, no
    // repeated diagnostic).
    const auto needs_retire = [this](const SlotKey& key, bool detached) {
        return !detached && key.owner != nullptr &&
               impl->passes_active_this_frame.count(key.owner) == 0;
    };
    bool any = false;    for (const auto& entry : impl->targets) {
        const auto& t = entry.second;
        for (const auto& kv : t.content_slots) {
            any = any || needs_retire(kv.first, kv.second.detached);
        }
        for (const auto& kv : t.screen_slots) {
            any = any || needs_retire(kv.first, kv.second.detached);
        }
        for (const auto& kv : t.program_slots) {
            any = any || needs_retire(kv.first, kv.second.detached);
        }
        if (any) {
            break;
        }
    }
    if (!any) {
        return;
    }
    // Wait BEFORE detaching: the pipelines the detached views hold must not be
    // destroyed while a submitted command buffer may still reference them. The
    // slots themselves are KEPT (only the view is detached) so re-enabling a
    // pass re-attaches instead of re-uploading its mesh and recompiling.
    waitForIdle(impl->viewer.get());

    for (auto& entry : impl->targets) {
        auto& t = entry.second;
        for (auto& kv : t.content_slots) {
            if (!needs_retire(kv.first, kv.second.detached)) {
                continue;
            }
            removeGraphChild(t.graph.get(), kv.second.view);
            kv.second.detached = true;
        }
        for (auto& kv : t.screen_slots) {
            if (!needs_retire(kv.first, kv.second.detached)) {
                continue;
            }
            removeGraphChild(t.graph.get(), kv.second.view);
            kv.second.detached = true;
        }
        for (auto& kv : t.program_slots) {
            if (!needs_retire(kv.first, kv.second.detached)) {
                continue;
            }
            removeGraphChild(t.graph.get(), kv.second.view);
            kv.second.detached = true;
        }
    }
    // Dropping a view can remove a command-graph dependency edge.
    reconcileOffscreenOrder();
    std::fprintf(stderr, "[VsgRenderer] retired (detached) the retained view of pass(es) not active this frame\n");
}

void VsgRenderer::releasePass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    const vine::graphics::RenderPass* removed = pass;
    for (auto& entry : impl->targets) {
        erasePassSlotsFromTarget(entry.first, removed);
    }
    impl->passes_active_this_frame.erase(removed);
    if (impl->request.pass == removed) {
        impl->request.pass = nullptr;
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
    auto& t          = impl->entryFor(target);
    auto& content    = t.content_slots[key];
    if (content.ready) {
        return;
    }
    if (t.graph == nullptr) {
        // No graph yet (e.g. an off-screen target that failed to build): drop
        // the half-made slot. Reported because the pass then draws nothing,
        // and the host may have no other way to learn the target is unusable.
        reportFailure(vine::graphics::DiagnosticSeverity::Error,
                      vine::graphics::DiagnosticCategory::TargetBuildFailed,
                      u8"no render graph for the pass' target: the pass draws nothing");
        t.content_slots.erase(key);
        return;
    }
    content.order      = order;
    content.depth_mode = depth_mode;
    content.presenting = presenting;
    content.vsg_camera = persistent->cameraBridge.create(camera);
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
        content.bridge.setShaderSet(depth_mode == vine::graphics::DepthMode::TestAndWrite ? impl->depth_on_shader_set
                                    : depth_mode == vine::graphics::DepthMode::TestOnly ? impl->depth_testonly_shader_set
                                                                                         : impl->depth_off_shader_set);
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
            set_ref = buildShaderSet(persistent->shader_preset,
                                     VkExtent2D{ static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) },
                                     depth_test, depth_write,
                                     target->colorCount());
        }
        content.bridge.setShaderSet(set_ref);
    }
    content.bridge.setMaterialManager(&persistent->materialManager);
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
    placeViewByOrder(target, content.view, content.order);
    content.ready = true;
    if (impl->viewer != nullptr) {
        impl->viewer->compile();
    }
}

void VsgRenderer::renderContentSlot(const ContentSlotRequest& request)
{
    if (request.commands == nullptr || request.lights == nullptr) {
        return; // no command stream / light list to draw from
    }
    // The slot is owned by the pass that draws it (pass scope) or, for a
    // direct driver, by the historical (camera, order) pair.
    const SlotKey key = (impl->request.pass != nullptr)
                            ? SlotKey::ownerPass(impl->request.pass)
                            : SlotKey::cameraOrder(request.camera, request.order);

    auto& t  = impl->entryFor(request.target);
    auto  it = t.content_slots.find(key);
    if (it == t.content_slots.end() || !it->second.ready) {
        setupContentSlot(key, request.target, request.camera, request.order, request.depth_mode, request.presenting);
        it = t.content_slots.find(key);
    }
    if (it == t.content_slots.end() || !it->second.ready) {
        return; // slot could not be built (e.g. camera bridge failed)
    }
    auto& content = it->second;

    if (content.detached) {
        // The pass executes again after having been retired: re-attach its
        // retained view (its data and pipelines were kept, so no upload /
        // recompile is needed).
        placeViewByOrder(request.target, content.view, content.order);
        content.detached = false;
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
        // Wait before the state rebuild: the pipelines / descriptor sets the
        // bridge is about to drop may still be referenced by a submitted
        // command buffer.
        waitForIdle(impl->viewer.get());
        content.depth_mode = request.depth_mode;
        content.bridge.setContentDepthMode(request.depth_mode);
        content.bridge.invalidateState();
    }
    if (content.order != request.order) {
        content.order = request.order;
        placeViewByOrder(request.target, content.view, request.order);
    }
    if (content.presenting != request.presenting) {
        content.presenting       = request.presenting;
        const bool want_headlight = (request.presenting && request.target == nullptr);
        if (content.headlight_seed != want_headlight) {
            content.headlight_seed = want_headlight;
            content.light_group->children.clear();
            if (want_headlight) {
                content.light_group->addChild(::vsg::createHeadlight());
            }
            else {
                content.light_group->addChild(makeAmbientLight(request.presenting ? "offscreen_ambient" : "content_ambient"));
            }
        }
    }

    // Full target extent for this slot's viewport: the live swapchain size for
    // the window target, the off-screen target's logical size otherwise.
    const int surf_w = (request.target == nullptr) ? static_cast<int>(impl->window->extent2D().width) : t.width;
    const int surf_h = (request.target == nullptr) ? static_cast<int>(impl->window->extent2D().height) : t.height;

    // Keep the slot's vsg camera viewport in step with its role each frame:
    // presenting (full-target) content always fills the whole target; other
    // content carries its pass sub-viewport when one was queued (an unset or
    // empty sub-viewport means the full target). The slot is created lazily on
    // its first render, so this also covers the first frame and any resize that
    // happened before the slot existed.
    if (content.presenting) {
        content.vsg_camera->viewportState =
            ::vsg::ViewportState::create(VkExtent2D{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) });
    }
    else if (request.viewport && request.viewport->width > 0 && request.viewport->height > 0) {
        content.vsg_camera->viewportState =
            ::vsg::ViewportState::create(request.viewport->x, request.viewport->y,
                                         static_cast<uint32_t>(request.viewport->width),
                                         static_cast<uint32_t>(request.viewport->height));
    }
    else {
        content.vsg_camera->viewportState =
            ::vsg::ViewportState::create(VkExtent2D{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) });
    }

    persistent->cameraBridge.apply(request.camera, content.vsg_camera);

    // Lights come from the pass' content scene each frame (the scene is the
    // source of truth); an empty list keeps the slot's seeded default light.
    // The light source is never chosen by the slot's depth style.
    setGroupLights(content.light_group.get(), *request.lights);

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
            auto& pending = impl->pending_compile_views;
            if (std::find(pending.begin(), pending.end(), content.view) == pending.end()) {
                pending.push_back(content.view);
            }
        }
        // TEMP diagnostics (VINE_VSG_DIAG_MRT): report how many commands were
        // collected for this slot, how many geometry subtrees were built and
        // how many distinct pipeline variants the bridge registered, to tell
        // "no geometry collected" from "geometry not rasterised" and to
        // confirm pipeline sharing (variants << commands when states repeat).
        if (std::getenv("VINE_VSG_DIAG_MRT") != nullptr) {
            std::fprintf(stderr, "[MRT-DIAG] target=%s depth_mode=%d order=%d commands=%zu created=%zu rootChildren=%zu variants=%zu\n",
                         request.target == nullptr ? "window"
                                                   : (request.target->name().empty() ? "offscreen" : request.target->name().stdstr().c_str()),
                         static_cast<int>(request.depth_mode),
                         request.order,
                         request.commands->size(),
                         created.size(),
                         content.root->children.size(),
                         content.bridge.pipelineVariantCount());
        }
    }
}

V_VSG_NS_END
