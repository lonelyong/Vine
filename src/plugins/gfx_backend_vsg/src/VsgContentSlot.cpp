#include <vine/vsg/VsgContentSlot.hpp>

#include <cstdio>
#include <optional>
#include <string>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/lighting/Light.h>

#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgContentSlot.hpp>
#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgPassMaterialiser.hpp>
#include <vine/vsg/VsgRecordOrder.hpp>
#include <vine/vsg/VsgRendererState.hpp>
#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

// This unit shares the free-function layer of VsgBackendUtility.hpp / VsgUtils.hpp;
// the directive keeps its call sites unqualified.
using namespace detail;

// The content-slot helpers: each answers one question the draw path asks every frame (the
// slot viewport, the seeded light, the "announced lights were all dropped" episode and the
// env-gated TEMP diagnostics). They stay file-local: they take plain values, never the
// session, so another unit has nothing to call.
namespace
{

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

namespace detail
{

bool beginLightsDroppedEpisode(std::size_t announced, std::size_t attached, bool& reported)
{
    if (announced == 0u || attached >= announced) {
        reported = false; // nothing announced, or every announced light is lit: re-arm the report
        return false;
    }
    if (reported) {
        return false; // already reported for this episode
    }
    reported = true;
    return true;
}

void setupContentSlot(VsgRendererState& state, VsgRendererPersistent& persistent,
                      const VsgDiagnostics& diagnostics, const SlotKey& key,
                      vine::graphics::RenderTarget*               target,
                      vine::raw_ptr<const vine::graphics::Camera> camera,
                      int                                         order,
                      vine::graphics::DepthMode                   depth_mode,
                      bool                                        presenting)
{
    // Content slots are retained Views under the TARGET's render graph — the
    // window target (target == nullptr) and every off-screen target share
    // this one mechanism. Each pass is its own View + bridge, so several
    // passes sharing one camera and order still stack as separate content.
    auto& t          = state.entryFor(target);
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
    const auto graph = passGraph(state, diagnostics, target, key);
    if (graph == nullptr) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error,
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
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error,
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
        content.bridge.setShaderSet(depth_mode == vine::graphics::DepthMode::TestAndWrite ? state.depth_on_shader_set
                                    : depth_mode == vine::graphics::DepthMode::TestOnly ? state.depth_testonly_shader_set
                                                                                         : state.depth_off_shader_set);
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
    installDiagnosticRoute(diagnostics, content.bridge);
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
    placeViewByOrder(state, graph, target, content.view, content.order);
    content.ready = true;
    if (state.viewer != nullptr) {
        state.viewer->compile();
    }
}

void renderContentSlot(VsgRendererState& state, VsgRendererPersistent& persistent,
                       const VsgDiagnostics& diagnostics, const VsgContentSlotRequest& request)
{
    if (request.commands == nullptr || request.lights == nullptr) {
        return; // no command stream / light list to draw from
    }
    // The slot is owned by the pass that draws it (pass scope) or, for a
    // direct driver, by the historical (camera, order) pair.
    const SlotKey key = (state.request.pass != nullptr)
                            ? SlotKey::ownerPass(state.request.pass)
                            : SlotKey::cameraOrder(request.camera, request.order);

    auto& t  = state.entryFor(request.target);
    auto  it = t.content_slots.find(key);
    if (it == t.content_slots.end() || !it->second.ready) {
        setupContentSlot(state, persistent, diagnostics, key, request.target, request.camera,
                        request.order, request.depth_mode, request.presenting);
        it = t.content_slots.find(key);
    }
    if (it == t.content_slots.end() || !it->second.ready) {
        return; // slot could not be built (e.g. camera bridge failed)
    }
    auto& content = it->second;
    // The graph this pass records into (see passGraph): the window's swapchain
    // graph, or this pass' own off-screen graph — created on the slot's first
    // render and reused every frame after.
    const auto graph = passGraph(state, diagnostics, request.target, key);

    if (content.detached) {
        // The pass executes again after having been retired: re-attach its
        // retained view (its data and pipelines were kept, so no upload /
        // recompile is needed).
        placeViewByOrder(state, graph, request.target, content.view, content.order);
        content.detached = false;
        // Re-attaching is what puts its graph back into the command graph: a
        // retired pass' graph is deliberately left out of it (see
        // reconcileOffscreenOrder).
        if (request.target != nullptr) {
            detail::reconcileOffscreenOrder(state);
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
        placeViewByOrder(state, graph, request.target, content.view, request.order);
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
    const int surf_w = (request.target == nullptr) ? static_cast<int>(state.window->extent2D().width) : t.width;
    const int surf_h = (request.target == nullptr) ? static_cast<int>(state.window->extent2D().height) : t.height;

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
        const std::size_t announced = request.lights->size();
        // One message per branch: a shared format string whose arguments are ordered for one of
        // them is how a branch ends up printing the wrong number (§54).
        diagnostics.report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ChannelIgnored,
                           attached_lights == 0u
                               ? formatDiagnostic(u8"%zu announced light(s) are all disabled or of an unsupported "
                                                  u8"kind; the pass keeps its default light",
                                                  announced)
                               : formatDiagnostic(u8"%zu of %zu announced light(s) are disabled or of an unsupported "
                                                  u8"kind and are not lit",
                                                  announced - attached_lights, announced));
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
            auto& pending = state.pending_compile_views;
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
void placeViewByOrder(VsgRendererState& state, ::vsg::ref_ptr<::vsg::RenderGraph> graph,
                       vine::graphics::RenderTarget* target,
                       const ::vsg::ref_ptr<::vsg::View>& view, int order)
{
    auto& t = state.entryFor(target);
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
} // namespace detail

V_VSG_NS_END
