#include <vine/vsg/VsgContentSlot.hpp>

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>

#include <vine/graphics/BuiltinShaders.hpp>

#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgProgramSlot.hpp>
#include <vine/vsg/VsgLights.hpp>
#include <vine/vsg/VsgPassMaterialiser.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgRecordOrder.hpp>
#include <vine/vsg/VsgRendererState.hpp>
#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

// This unit shares the free-function layer of VsgBackendUtility.hpp / VsgUtils.hpp;
// the directive keeps its call sites unqualified.
using namespace detail;

// The content-slot helpers: each answers one question the draw path asks every frame (the
// slot viewport, the "announced lights were all dropped" episode and the env-gated TEMP
// diagnostics). They stay file-local: they take plain values, never the session, so another
// unit has nothing to call.
namespace
{

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

void updateSlotViewport(ContentSlot& content, bool presenting, const std::optional<vine::graphics::Viewport>& viewport,
                        int surf_w, int surf_h)
{
    int x = 0;
    int y = 0;
    int w = surf_w;
    int h = surf_h;
    if (!presenting && viewport && viewport->width > 0 && viewport->height > 0) {
        x = viewport->x;
        y = viewport->y;
        w = viewport->width;
        h = viewport->height;
    }
    if (w <= 0 || h <= 0) {
        return; // no surface yet (a swapchain that is still 0x0): nothing to assert
    }
    if (content.viewport_state != nullptr && content.viewport_x == x && content.viewport_y == y &&
        content.viewport_w == w && content.viewport_h == h) {
        return; // same rectangle: the recorded state is already the right one
    }
    if (content.viewport_state == nullptr) {
        content.viewport_state = ::vsg::ViewportState::create(x, y, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        content.vsg_camera->viewportState = content.viewport_state;
    }
    else {
        // In place: the view already records this object, so only its values change.
        auto& vk_viewport  = content.viewport_state->getViewport();
        vk_viewport.x      = static_cast<float>(x);
        vk_viewport.y      = static_cast<float>(y);
        vk_viewport.width  = static_cast<float>(w);
        vk_viewport.height = static_cast<float>(h);
        auto& scissor      = content.viewport_state->getScissor();
        scissor.offset     = VkOffset2D{ x, y };
        scissor.extent     = VkExtent2D{ static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
    }
    content.viewport_x = x;
    content.viewport_y = y;
    content.viewport_w = w;
    content.viewport_h = h;
}

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
                      vine::raw_ptr<const vine::graphics::Camera> camera)
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
    // Anisotropy is a DEVICE limit, so it is read from the session's device instead of assumed: a request
    // above VkPhysicalDeviceLimits::maxSamplerAnisotropy is a validation error, and the samplerAnisotropy
    // feature has to be enabled on the device for the request to be legal at all. Read here because a slot's
    // bridge owns the cache that builds the samplers.
    if (state.window != nullptr) {
        const auto physical = state.window->getPhysicalDevice();
        if (physical != nullptr) {
            content.bridge.setTextureAnisotropy(physical->getProperties().limits.maxSamplerAnisotropy);
        }
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
    // What the pass announced IS what this slot applies first (the request is the session's own, so this is
    // the only place the slot's applied value is seeded from something other than a comparison).
    content.applied    = state.request.attributes();
    content.vsg_camera = persistent.cameraBridge.create(camera);
    if (content.vsg_camera == nullptr) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error,
                           vine::graphics::DiagnosticCategory::ContentSkipped,
                           u8"camera bridge could not be created: the pass draws nothing");
        t.content_slots.erase(key);
        return;
    }
    content.root = ::vsg::Group::create();

    // Per-slot pipeline bridge. The bridge's shader set bakes the slot's depth
    // policy, and the pipeline-state registry must stay PRIVATE to the slot:
    // handing two slots one table (a session-level vsg::SharedObjects) makes
    // them share a GraphicsPipeline OBJECT, and vsg's per-view implementation
    // reuse compares only the pipeline states — never the render pass
    // (GraphicsPipeline.cpp:177) — while this backend deliberately builds a
    // distinct VkRenderPass per pass variant (clear policy / depth promotion,
    // §5.4). The second view then gets a pipeline compiled against an
    // incompatible render pass. Measured: sharing the table breaks the
    // policy-churn phase's depth invariant (scripts/vsg_selftest_evidence.sh).
    if (target == nullptr) {
        // Window slots share the renderer's (window-sized) shader sets. Which of the three a policy
        // means is one rule (see detail::shaderSetFor), not a ternary per target kind.
        content.bridge.setShaderSet(detail::shaderSetFor(content.applied.depth_mode, state.depth_on_shader_set,
                                                         state.depth_testonly_shader_set, state.depth_off_shader_set));
    }
    else {
        // Off-screen slots get a per-target shader set baked at the target's
        // size (created lazily).
        auto& set_ref = detail::shaderSetFor(content.applied.depth_mode, t.depth_on_shader_set,
                                             t.depth_testonly_shader_set, t.depth_off_shader_set);
        if (set_ref == nullptr) {
            const detail::DepthTestWrite depth = detail::depthTestWrite(content.applied.depth_mode);
            set_ref = makeContentShaderSet(persistent.default_content_program,
                                           VkExtent2D{ static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) },
                                           depth.test, depth.write,
                                           target->colorCount());
        }
        content.bridge.setShaderSet(set_ref);
    }
    // A session with NO default content program has no set to build a slot with: the slot's bridge
    // reports it
    // (see buildStateGroup) and draws nothing. Said out loud here as well, ONCE per session, at the
    // level the host asked the question at: "you named no program, so program-less content will not be
    // drawn" — not a picture it did not ask for.
    if (persistent.default_content_program == nullptr && !state.no_default_default_content_program_reported) {
        state.no_default_default_content_program_reported = true;
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ShaderFallback,
                           u8"this session has no default content program (setDefaultContentProgram(nullptr)), so content without a "
                           u8"program of its own is NOT drawn (the engine never substitutes a shading nobody named)");
    }
    // Which light source this slot feeds is not a choice: the slot's SET is always one of the
    // engine's own (built from the program the host named), and every one of them reads the slot's
    // `vine_lights` block. vsg's light-data path — light nodes under the view, collected per view
    // at record time — existed only for the vsg shading sets the engine no longer uses at all.
    content.bridge.setMaterialManager(&persistent.materialManager);
    // Upload textures through the SESSION's cache: the same texture sampled by two
    // slots would otherwise be staged (and held) twice, once per slot.
    content.bridge.setTextureCache(state.texture_cache.get());
    // Route mesh streams through the SESSION's cache as well: the same model read by two slots (or by two
    // drawables in one slot) would otherwise upload its vertices once per drawable.
    content.bridge.setMeshResourceCache(state.mesh_cache.get());
    // Route per-draw values through the SESSION's slot pool: the blocks' buffers and descriptor
    // sets are shared by every drawable of every slot, so a scene costs slots instead of one
    // buffer + one descriptor set per drawable.
    content.bridge.setDrawBlockPool(state.draw_block_pool.get());
    // Route this slot's rejections through the renderer's diagnostics (trace,
    // counters, host sink): the slot is what actually discovers them.
    installDiagnosticRoute(diagnostics, content.bridge);
    // The pass' depth policy reaches the pipeline through the bridge (it fills
    // the depth item of content that did not author one), so it must be set
    // before the slot's first sync; a later change invalidates the state.
    content.bridge.setContentDepthMode(content.applied.depth_mode);
    content.bridge.clearCache();

    // This slot's light block, written every frame from the pass' own lights and
    // bound at set 0 / binding 2 of the forward shader set: the ONE light source a content slot
    // has. Seeded here (so the descriptor exists before the first compile) and refilled per frame
    // below.
    content.lights_data = ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(VineLightsBlock)));
    content.bridge.setLightsData(content.lights_data);

    // This slot's shadow ABI, seeded before the first compile so both descriptors exist (a
    // declared-but-unwritten descriptor is an invalid set, and the content set declares the pair
    // unconditionally — see buildVineShaderSet):
    //  - the block starts DISABLED;
    //  - the map starts at the session's white fallback, and the shader never samples it while the
    //    block is disabled (the stand-in needs no depth image of its own, and it is a valid
    //    COMBINED_IMAGE_SAMPLER the whole time).
    // The sampler is NEAREST: the shader compares exact depths, so filtering across texels would
    // invent a depth nobody rasterised (see makeFullscreenProgramNode, which does the same for the
    // fullscreen path).
    content.shadow_data      = ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(vine::graphics::VineShadowBlock)));
    content.shadow_sampler   = ::vsg::Sampler::create();
    content.shadow_sampler->magFilter  = VK_FILTER_NEAREST;
    content.shadow_sampler->minFilter  = VK_FILTER_NEAREST;
    content.shadow_sampler->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    content.shadow_placeholder = state.texture_cache != nullptr ? state.texture_cache->whiteFallback()
                                                                : ::vsg::ref_ptr<::vsg::ImageInfo>();
    content.bridge.setShadowData(content.shadow_data);
    content.bridge.setShadowMap(content.shadow_placeholder, /*declared*/ false);

    // features = 0: the set declares no view-dependent binding, so ViewDependentState has nothing to
    // collect and its lightData buffer stays at the 1-vec4 minimum instead of being sized for lights
    // the slot does not put under the view.
    content.view = ::vsg::View::create(content.vsg_camera, ::vsg::ref_ptr<::vsg::Node>(),
                                      static_cast<::vsg::ViewFeatures>(0));
    content.view->addChild(content.root);

    // Position the slot's View in the target's render graph by its explicit
    // order, then compile it before it is first recorded. Content slots are
    // only created from render() calls that follow initialize(), so the
    // target's graph is always present here.
    //
    // Within a target the slot views are stacked in ASCENDING pass order —
    // the order the caller gave addPass() and the engine already runs passes
    // in (placeViewByOrder keeps content and full-screen program
    // views all sorted by it). No main/on-top semantic constrains the order —
    // a pass positioned by the user at any order draws exactly there. Ordering
    // by the explicit value also keeps the pre-frame warm-up safe: warm-up may
    // create a higher-order (on-top) slot before a lower-order (main) slot has
    // run, but the main slot is inserted ahead of it by its smaller order when
    // it is finally created.
    placeViewByOrder(state, graph, target, content.view, content.applied.order);
    content.ready = true;
    if (state.viewer != nullptr) {
        state.viewer->compile();
    }
}

void renderContentSlot(VsgRendererState& state, VsgRendererPersistent& persistent,
                       const VsgDiagnostics& diagnostics, vine::raw_ptr<const vine::graphics::Camera> camera,
                       const std::vector<vine::graphics::RenderCommand>& commands,
                       const std::vector<const vine::graphics::Light*>&   lights,
                       const std::optional<vine::graphics::Viewport>&     viewport)
{
    // The pass' SCOPE attributes are the session's request (one description of a pass, not a copy built per
    // draw call); the per-draw-call state is the arguments above.
    const vine::graphics::RenderTarget* target_key = state.request.target;
    const PassAttributes                wanted     = state.request.attributes();
    // The slot is owned by the pass that draws it: the rule lives on the request,
    // so this entry point and the screen one cannot disagree.
    const SlotKey key = state.request.slotKey();

    auto& t  = state.entryFor(state.request.target);
    auto  it = t.content_slots.find(key);
    if (it == t.content_slots.end() || !it->second.ready) {
        setupContentSlot(state, persistent, diagnostics, key, state.request.target, camera);
        it = t.content_slots.find(key);
    }
    if (it == t.content_slots.end() || !it->second.ready) {
        return; // slot could not be built (e.g. camera bridge failed)
    }
    auto& content = it->second;
    // The graph this pass records into (see passGraph): the window's swapchain
    // graph, or this pass' own off-screen graph — created on the slot's first
    // render and reused every frame after.
    const auto graph = passGraph(state, diagnostics, state.request.target, key);

    if (content.detached) {
        // The pass executes again after having been retired: re-attach its
        // retained view (its data and pipelines were kept, so no upload /
        // recompile is needed).
        placeViewByOrder(state, graph, state.request.target, content.view, content.applied.order);
        content.detached = false;
        // Re-attaching is what puts its graph back into the command graph: a
        // retired pass' graph is deliberately left out of it (see
        // reconcileOffscreenOrder).
        if (state.request.target != nullptr) {
            detail::reconcileOffscreenOrder(state);
        }
    }

    // The pass' properties are re-applied every frame, so changing them at run
    // time takes effect instead of leaving the slot with the state it was
    // first built with. ONE comparison decides whether anything has to be re-applied at all, and then each
    // attribute that changed is applied: what the slot REMEMBERS (content.applied) is the same value that
    // was compared, so storing it cannot be forgotten for an attribute someone adds later:
    //  - the depth policy is forwarded to the bridge (which rebuilds only the
    //    state wrappers, not the vertex data) and invalidates them on change;
    //  - the explicit pipeline order moves the view to its new stacking slot;
    //  - the presenting role drives the viewport each frame and re-seeds the
    //    slot's default light when it flips.
    if (content.applied != wanted) {
        if (content.applied.depth_mode != wanted.depth_mode) {
            // No device wait: the state wrappers being dropped are PARKED by the
            // bridge itself (SceneBridge::invalidateState), so the pipelines they own
            // stay alive until every slot that could have recorded them has been
            // re-recorded. The depth mode is a per-frame host decision (a UI toggling
            // depth test), so waiting here would stall the device on every frame it
            // changes — measured: 14 waits over 15 flipping frames before this.
            content.bridge.setContentDepthMode(wanted.depth_mode);
            content.bridge.invalidateState();
        }
        if (content.applied.order != wanted.order) {
            placeViewByOrder(state, graph, state.request.target, content.view, wanted.order);
        }
        content.applied = wanted; // applied, in one place, whatever changed above
    }

    // Full target extent for this slot's viewport: the live swapchain size for
    // the window target, the off-screen target's logical size otherwise.
    const int surf_w = (target_key == nullptr) ? static_cast<int>(state.window->extent2D().width) : t.width;
    const int surf_h = (target_key == nullptr) ? static_cast<int>(state.window->extent2D().height) : t.height;

    // Keep the slot's vsg camera viewport in step with its role each frame (see
    // updateSlotViewport): presenting content fills the target, other content carries
    // its pass sub-viewport.
    updateSlotViewport(content, content.applied.presenting, viewport, surf_w, surf_h);

    persistent.cameraBridge.apply(camera, content.vsg_camera);

    // This slot's lights, packed for its shader set (view space, ambient + up to three
    // directionals). Written every frame because the directions are view-space: a moving camera moves
    // them. Cheap by construction — one call and a 112-byte copy per slot per frame.
    std::size_t attached_lights = 0u;
    if (content.lights_data != nullptr && content.lights_data->dataSize() >= sizeof(VineLightsBlock)) {
        VineLightsBlock block;
        attached_lights = fillVineLightsBlock(camera, lights, block);
        std::memcpy(content.lights_data->dataPointer(), &block, sizeof(block));
        content.lights_data->dirty();
    }

    // This slot's shadow: resolved from the PASS' own declared inputs by the one rule every shadow
    // consumer uses (detail::resolveShadowInput — the fullscreen lighting pass calls it too), so the
    // forward content and the deferred lighting cannot disagree about which map they are reading.
    if (content.shadow_data != nullptr && content.shadow_data->dataSize() >= sizeof(vine::graphics::VineShadowBlock)) {
        const detail::ShadowInput shadow = detail::resolveShadowInput(state, camera, lights);
        std::memcpy(content.shadow_data->dataPointer(), &shadow.block, sizeof(shadow.block));
        content.shadow_data->dirty();
        if (content.shadow_view != shadow.map) {
            // The descriptor's image view cannot be re-pointed in place, so the new map is a new
            // descriptor: drop the cached variants and let the next build assign it. Seed-only
            // changes are rare (a pass that starts or stops declaring a shadow, a resized map).
            content.shadow_view = shadow.map;
            content.bridge.setShadowMap(shadow.map != nullptr
                                            ? ::vsg::ImageInfo::create(content.shadow_sampler, shadow.map,
                                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                            : content.shadow_placeholder,
                                        /*declared*/ shadow.map != nullptr);
            content.bridge.clearCache();
        }
    }

    if (beginLightsDroppedEpisode(lights.size(), attached_lights, content.light_fallback_reported)) {
        const std::size_t announced = lights.size();
        // One message per branch: a shared format string whose arguments are ordered for one of
        // them is how a branch ends up printing the wrong number (§54).
        diagnostics.report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ChannelIgnored,
                           attached_lights == 0u
                               ? formatDiagnostic(u8"%zu announced light(s) are unusable (disabled, or a kind the light "
                                                  u8"block does not carry); the pass is lit by the ambient fill",
                                                  announced)
                               : formatDiagnostic(u8"%zu of %zu announced light(s) are unusable (disabled, not ambient or "
                                                  u8"directional, or beyond the block's three directional slots) and are "
                                                  u8"not lit",
                                                  announced - attached_lights, announced));
    }

    // The command stream is the source of truth: reconcile the retained slot
    // root against it (in-place for moves/material edits). The legacy own-
    // window debug path skips syncing the window's presenting slot.
    if (!(state.request.target == nullptr && forceOwnWindow() && content.applied.presenting)) {
        std::vector<::vsg::ref_ptr<::vsg::Node>> created;
        content.bridge.syncRenderCommands(commands, content.root.get(), &created,
                                          &state.retained_shares);
        if (!created.empty()) {
            // Queue this slot's VIEW for an incremental (re)compile in
            // submitFrame(): traversing the View sets the correct viewID, so
            // the new/rebuild subtrees compile for the view they will be
            // recorded under (D22). One entry per view per frame, and the entry
            // carries WHERE the view is recorded (this is the queue's only
            // producer), so the compiler needs no search (see PendingCompileView).
            auto& pending = state.pending_compile_views;
            const auto known = std::find_if(pending.begin(), pending.end(), [&content](const PendingCompileView& entry) {
                return entry.view == content.view;
            });
            if (known == pending.end()) {
                pending.push_back(PendingCompileView{ content.view, state.request.target, key });
            }
        }
        // TEMP diagnostics, env-gated: how many commands this slot collected, how
        // many subtrees were built and how many pipeline variants exist (see
        // logContentSlotDiagnostics).
        logContentSlotDiagnostics(target_key, wanted.depth_mode, wanted.order, commands.size(),
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
    // by each slot's explicit order: content slots carry theirs and full-screen
    // program slots carry theirs, and any child with
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
                return kv.second.applied.order;
            }
        }
        for (const auto& kv : t.program_slots) {
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
