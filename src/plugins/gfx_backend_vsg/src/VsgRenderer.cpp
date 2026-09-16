#include <vine/vsg/VsgRenderer.hpp>

#include <vine/vsg/VsgViewCompiler.hpp>

#include <vine/vsg/VsgContentSlot.hpp>
#include <vine/vsg/VsgProgramSlot.hpp>
#include <vine/vsg/VsgReadback.hpp>

#include <vine/vsg/VsgUtils.hpp>
#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgTargetBookkeeping.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <typeinfo>

#if defined(__GNUG__)
#    include <cxxabi.h>
#endif

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/commands/Draw.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/lighting/AmbientLight.h>
#include <vsg/lighting/DirectionalLight.h>
#include <vsg/lighting/Light.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/nodes/VertexIndexDraw.h>
#include <vsg/core/Array.h>
#include <vsg/core/Exception.h>
#include <vsg/maths/vec4.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/MultisampleState.h>
#include <vsg/state/PushConstants.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/Sampler.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/ViewDependentState.h>
#include <vsg/state/ViewportState.h>
#include <vsg/state/material.h>
#include <vsg/utils/Builder.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include <vsg/utils/ShaderCompiler.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/PhysicalDevice.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>
#include <vsg/vk/ResourceRequirements.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

V_VSG_NS_BEGIN

// This translation unit is one of several that share a single free-function
// layer (VsgPipelineFactory.hpp / VsgBackendUtility.hpp); the directive keeps
// its call sites unqualified, as they read before the split.
using namespace detail;

namespace
{

/**
 * @brief Returns a diagnostic description of the exception currently being
 * handled (must be called from inside a catch handler).
 *
 * std::exception-derived exceptions report their what(); any other C++ type is
 * reported by its runtime type name (demangled on GCC/Clang), so an unexpected
 * failure never collapses to a bare "unknown exception" with no way to tell an
 * environment problem (no Vulkan ICD / display / device) from a code defect.
 *
 * @return Human-readable description of the active exception.
 */
std::string describeCurrentException()
{
    try {
        throw; // re-dispatch the exception being handled
    }
    catch (const ::vsg::Exception& e) {
        // vsg::Exception is a plain struct (message + VkResult), NOT derived
        // from std::exception — without this branch it would fall through to
        // the generic catch below and its message would be lost (the historical
        // "unknown exception").
        std::string text = "vsg::Exception: " + e.message;
        if (e.result != 0) {
            text += " (VkResult " + std::to_string(e.result) + ")";
        }
        return text;
    }
    catch (const std::exception& e) {
        return std::string("std::exception: ") + e.what();
    }
    catch (...) {
#if defined(__GNUG__)
        if (const std::type_info* type = abi::__cxa_current_exception_type()) {
            int          status    = 0;
            char*        demangled = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
            std::string  name      = (demangled != nullptr) ? demangled : type->name();
            std::free(demangled);
            return "non-std exception of type '" + name + "'";
        }
#endif
        return "non-std exception (type name unavailable)";
    }
}

/**
 * @brief vsg::Viewer whose pollEvents() does not pump the native message queue.
 *
 * vsg's Win32_Window::pollEvents() drains and dispatches the thread's Windows
 * message queue (PeekMessage/DispatchMessage). That is correct for a
 * standalone vsg application, but when vsg is embedded in a GUI toolkit such
 * as Qt — which owns the message loop — dispatching from inside a frame call
 * re-enters the toolkit: the dispatched message triggers a Qt event, which can
 * request another frame, which pumps again, recursing until the stack
 * overflows. Input is delivered by the host instead, so window polling is
 * disabled; only the buffered vsg events are dropped.
 */
class EmbeddedViewer : public ::vsg::Inherit<::vsg::Viewer, EmbeddedViewer> {
  public:
    /** @brief Discards stale events without polling any attached window. */
    bool pollEvents(bool discardPreviousEvents) override
    {
        if (discardPreviousEvents) {
            this->getEvents().clear();
        }
        return false;
    }
};

/**
 * @brief Builds the window traits for one session.
 *
 * Everything the session's VkInstance / VkDevice / swapchain needs, in one place:
 * the requested size, the validation-layer switch (VINE_VSG_DEBUG_LAYER — what makes
 * a silent pipeline / render-pass failure visible at all) and the two OPTIONAL device
 * features the SDK's render-state model maps onto. PolygonMode::Line needs
 * fillModeNonSolid and MRT pipelines with differing attachments need independentBlend;
 * both are near-universal core features, so requesting them keeps the mapped state
 * honoured instead of tripping pipeline creation on a capable device.
 *
 * When @p host_handle is non-null the window attaches to the host's native surface (a
 * Qt QWindow) rather than opening its own.
 *
 * @param host_handle Host native window handle, or null to create a vsg window.
 * @return Traits to hand to vsg::Window::create (never null).
 */
::vsg::ref_ptr<::vsg::WindowTraits> makeWindowTraits(void* host_handle)
{
    auto traits         = ::vsg::WindowTraits::create();
    traits->windowTitle = "Vine";
    traits->width       = 1280;
    traits->height      = 720;
    traits->debugLayer  = std::getenv("VINE_VSG_DEBUG_LAYER") != nullptr;
    traits->deviceFeatures                         = ::vsg::DeviceFeatures::create();
    traits->deviceFeatures->get().fillModeNonSolid = VK_TRUE;
    traits->deviceFeatures->get().independentBlend = VK_TRUE;
    // Anisotropic filtering: requested here because this list is the whole set of features the backend
    // needs, and the texture cache's samplers ask for anisotropy. Enabling a sampler feature the device was
    // never asked for is a validation error (VUID-VkSamplerCreateInfo-anisotropyEnable-01070), so leaving
    // this out does not degrade to isotropic filtering — it fails.
    traits->deviceFeatures->get().samplerAnisotropy = VK_TRUE;

    if (host_handle != nullptr) {
#ifdef _WIN32
        traits->nativeWindow = reinterpret_cast<HWND>(host_handle);
        RECT client_rect{};
        if (::GetClientRect(reinterpret_cast<HWND>(host_handle), &client_rect) &&
            client_rect.right > client_rect.left && client_rect.bottom > client_rect.top) {
            traits->width  = client_rect.right - client_rect.left;
            traits->height = client_rect.bottom - client_rect.top;
        }
#else
        // vsg's Xcb backend reads the native window as an xcb_window_t (uint32_t).
        // The host handle carries QWindow::winId() bits, so narrow it to exactly
        // that type: std::any only matches on the exact type, and storing a
        // void*/64-bit handle makes vsg throw bad_any_cast when it casts back to
        // xcb_window_t.
        traits->nativeWindow = static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(host_handle));
#endif
    }
    return traits;
}

} // namespace

VsgRenderer::VsgRenderer()
{
    // The one diagnostic route: the stderr trace plus the SDK channel. The downstream goes
    // through a member because reportDiagnostic() is protected (a lambda's closure type is not
    // a member of this class and cannot call it).
    diagnostics.setDownstream([this](const vine::graphics::RenderDiagnostic& diagnostic) {
        deliverToSdkChannel(diagnostic);
    });
}

void VsgRenderer::deliverToSdkChannel(const vine::graphics::RenderDiagnostic& diagnostic)
{
    reportDiagnostic(diagnostic.severity, diagnostic.category, diagnostic.message);
}

VsgRenderer::~VsgRenderer()
{
    shutdown();
}

bool VsgRenderer::initialize()
{
    // The unified output-target table keys the window (backbuffer) by a null
    // RenderTarget* — the same identity the graphics engine uses for the
    // on-screen target. The window entry is created below when its shared
    // swapchain render graph is assigned.
    // Defensive: tear down any still-live previous session (a caller that
    // skipped shutdown()) so this re-init starts from a clean session.
    // shutdown() nulls bound_handle, so restore the just-bound handle.
    if (state.window != nullptr) {
        void* bound = persistent.bound_handle;
        shutdown();
        persistent.bound_handle = bound;
    }
    // The stage label is printed if any step below throws, so a failing init
    // reports exactly where it died (window/device/swapchain creation, shader
    // sets, viewer compile) instead of a bare "unknown exception".
    const char* init_stage = "creating Vulkan window (instance/device/swapchain)";
    try {
    // Window. A bound host native window is attached to (e.g. a Qt QWindow)
    // instead of opening our own; the traits carry the size, the validation-layer
    // switch and the requested device features (see makeWindowTraits).
    void* host_handle = persistent.bound_handle;
    if (forceOwnWindow()) {
        // Temporary test path: create vsg's own window, ignoring the Qt-hosted
        // surface handle, to verify rendering independent of Qt compositing.
        host_handle = nullptr;
    }
    auto traits = makeWindowTraits(host_handle);
    state.window = ::vsg::Window::create(traits);
    if (state.window == nullptr) {
        V_LOGE("[VsgRenderer] Window::create FAILED (nativeWindow={}, {}x{})",
               traits->nativeWindow.has_value() ? 1 : 0, traits->width, traits->height);
        // The SDK contract for initialize() is that a false return ALSO reports the reason on the
        // diagnostics channel (RenderBackend.hpp). This was the one early failure that only wrote
        // the stderr trace, so a host watching its sink saw "failed" with no reason while the
        // exception paths below reported normally.
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::InitFailed,
                           formatDiagnostic(u8"initialize FAILED at '%s': Window::create returned no window "
                                            u8"(nativeWindow=%s, %dx%d)",
                                            init_stage, traits->nativeWindow.has_value() ? "host" : "none",
                                            traits->width, traits->height));
        shutdown();
        return false;
    }

    // The session's texture cache, created before any slot exists so every slot's bridge uploads through this
    // ONE cache (see SceneBridge::setTextureCache): a texture sampled by several slots is staged once, not
    // once per slot. Session-scoped: the images reference the session's device, so shutdown() drops them with
    // it.
    state.texture_cache = std::make_unique<vine::vsg::VsgTextureCache>();

    // The session's mesh-stream cache, created next to it for the same reason: the streams a geometry aliases
    // from a model buffer are bound through this ONE cache, so N drawables reading the same vertices/indices
    // share one bind, one device buffer and one upload instead of one each. Session-scoped: the buffers
    // belong to the session's device, so shutdown() drops them with it.
    state.mesh_cache = std::make_unique<vine::vsg::VsgMeshResourceCache>();

    // The session's per-draw uniform slots, created next to them for the same reason: our forward
    // set reads each drawable's opacity from set 1, and every drawable takes its slot from this
    // ONE pool, so the blocks live in a few mapped buffers instead of one buffer and one
    // descriptor set per drawable (see VsgDrawBlockPool). It needs the device, so it is created
    // here rather than with the other caches, and its memory goes away with the session.
    state.draw_block_pool = vine::vsg::VsgDrawBlockPool::create(state.window->getOrCreateDevice());

    // Window-target shader sets shared by its content slots (embedded SPIR-V,
    // no runtime glslang): the depth-on set keeps depth test/write on; the
    // depth-off set disables it so the slot's content always draws on top of
    // earlier content (HUD). Off-screen targets bake their own per-size sets
    // lazily.
    init_stage = "building window shader sets";
    state.depth_on_shader_set        = makeContentShaderSet(persistent.default_content_program, state.window->extent2D(), true, true);
    state.depth_testonly_shader_set  = makeContentShaderSet(persistent.default_content_program, state.window->extent2D(), true, false);
    state.depth_off_shader_set       = makeContentShaderSet(persistent.default_content_program, state.window->extent2D(), false, false);

    // The primary window layer is created lazily on the first window render
    // (the first pass that clears and draws the scene into the backbuffer).
    // The engine owns the pipeline and drives content per pass, so the
    // renderer binds neither a Vine scene nor a camera and pre-creates
    // nothing here.

    // Viewer. EmbeddedViewer disables vsg's native message pumping (Qt owns
    // the message loop here). Content slots are appended to the render graph
    // later (see setupContentSlot) as extra Views — the canonical vsg
    // multi-viewport pattern: one render pass, later Views drawn on top.
    init_stage = "creating viewer / command graph";
    state.viewer = ::vsg::ref_ptr<::vsg::Viewer>(new EmbeddedViewer());
    state.viewer->addWindow(state.window);

    // Window render graph (empty until the first content slot is created) +
    // command graph. The window target's graph IS this shared swapchain graph
    // (targets[nullptr].graph); every window content slot / PiP view is a
    // child of it.
    auto renderGraph      = ::vsg::RenderGraph::create(state.window);
    renderGraph->contents = VK_SUBPASS_CONTENTS_INLINE;
    state.entryFor(nullptr).graph = renderGraph;
    auto commandGraph     = ::vsg::CommandGraph::create(state.window);
    commandGraph->addChild(renderGraph);
    state.command_graph = commandGraph;
    state.viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ commandGraph });

    init_stage = "initial viewer compile";
    // The hints come from the one place that names them, because this call is what lets vsg CREATE the
    // session's compile manager, and renewCompileContexts() later replaces it with the same value.
    const auto compileResult = state.viewer->compile(compileManagerHints());
    if (!compileResult) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::InitFailed,
                           formatDiagnostic(u8"initialize FAILED at '%s': %s", init_stage,
                                            compileResult.message.c_str()));
        shutdown();
        return false;
    }

    state.initialized = true;
    return true;
    }
    catch (...) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::InitFailed,
                           formatDiagnostic(u8"initialize FAILED at '%s': %s", init_stage,
                                            describeCurrentException().c_str()));
        // Environment hints: an init failure here is usually a missing/invalid
        // Vulkan ICD (VK_ICD_FILENAMES), a device/driver problem or a missing
        // display — print what the backend saw so a local environment issue is
        // not mistaken for a code defect.
        V_LOGE("[VsgRenderer]   init env: VK_ICD_FILENAMES={}  VINE_VSG_DEBUG_LAYER={}  DISPLAY={}  WAYLAND_DISPLAY={}",
               std::getenv("VK_ICD_FILENAMES") ? std::getenv("VK_ICD_FILENAMES") : "(unset)",
               std::getenv("VINE_VSG_DEBUG_LAYER") ? std::getenv("VINE_VSG_DEBUG_LAYER") : "(unset)",
               std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(unset)",
               std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "(unset)");
    }
    shutdown();
    return false;
}

void VsgRenderer::shutdown()
{
    if (state.viewer != nullptr) {
        // The session is going away, so this wait cannot be avoided — and it is
        // counted (deviceWaitCount), like every other device-wide idle.
        state.retireRing.waitForIdle(state.viewer);
        // Detach the window from the viewer so its command graphs are dropped
        // before the viewer is released.
        if (state.window != nullptr) {
            state.viewer->removeWindow(state.window);
        }
        state.viewer->close();
    }
    if (state.window != nullptr) {
        // Release the native handle the platform window wraps. When the
        // reference is dropped, the Win32_Window destructor would call
        // ::DestroyWindow() (and ::UnregisterClass()) on the HOST's window —
        // here a Qt-owned HWND that Qt is itself tearing down. releaseWindow()
        // nulls the internal HWND so the destructor leaves Qt's window alone.
        state.window->releaseWindow();
    }
    // Whole-session teardown: assigning over the session state drops the window,
    // viewer, command graph, per-target render graphs, content slots and every
    // compiled pipeline that references the old vsg::Device — in one step, so
    // a newly retained vsg member cannot be forgotten here. The next
    // initialize() starts from a fresh state and allocates device ID 0, so a
    // surface-recreate re-init never trips vsg's VSG_MAX_DEVICES limit.
    state = VsgRendererState{};
    // The material manager outlives sessions (MaterialManager contract), so it
    // is cleared explicitly to drop references its cache holds to the dead
    // device; bound_handle points at a surface that is going away.
    persistent.materialManager.clear();
    persistent.bound_handle = nullptr;
}

void VsgRenderer::beginFrame()
{
    // A new frame: the passes active this frame are re-announced by beginPass()
    // as the engine runs them, so the activity set starts empty. A frame in which every pass is
    // disabled therefore announces nothing — and STILL retires the retained views of the passes
    // that did not run, because that decision is taken from the slots (who announced them this
    // frame) rather than from a flag saying the protocol was ever used.
    state.passes_active_this_frame.clear();
    // The frame's commit token, and the refusal episode it re-arms: ONE frame = ONE token, so the
    // only advance of the deferral rings (settleSubmittedFrame) has to have this frame's token.
    // Minted even when there is no session: "a frame was opened" is a fact about the caller's
    // protocol, not about the device (see FrameCommit).
    state.pending_commit                = FrameCommit::submitted();
    state.submit_without_frame_reported = false;
    state.scope_refusal_reported        = false;
    if (state.viewer == nullptr) {
        return;
    }
    state.viewer->advanceToNextFrame();
    state.viewer->handleEvents();
    // The frame's picture, built before any pass runs: every sync this frame judges by it (the
    // content slot hands it to its bridge, see SceneBridge::syncRenderCommands).
    collectFrameShares();
}

void VsgRenderer::collectFrameShares()
{
    // How many retained entries hold each object, across the material manager and every content slot
    // of every target. A cache judging by its own shares alone sees the other holders and waits for
    // them, so this count is what lets "the app let go" be observed at all — and it has to include
    // the slots that are NOT drawing this frame: their entries hold the objects too.
    //
    // O(entries now), never O(entries ever seen): each bridge counts its candidate lists (which ARE
    // its cache's key set) and the bounded program / variant caches, so the pass cannot cost more
    // than the frame it serves.
    //
    // The table is a session member and only ever filled IN PLACE (never reallocated), because one
    // frame's syncs and its end-of-frame sweeps both read it and the keys are counted per entry.
    state.retained_shares.clear();
    persistent.materialManager.collectOwnedShares(state.retained_shares);
    for (auto& target_entry : state.targets) {
        for (auto& slot_entry : target_entry.second.content_slots) {
            slot_entry.second.bridge.collectOwnedShares(state.retained_shares);
        }
    }
}

void VsgRenderer::releaseAbandonedContent()
{
    // The frame's END. The counts are collected HERE rather than carried over from the frame's
    // start: a slot dropped while the frame was open (a target rebuilt at a new size, a pass
    // retargeted, a target released) took its entries — and the shares they held — with it, so the
    // start-of-frame picture over-counts, and an over-count reports "the app let go" for an object
    // the app still holds. Fusing the collection with the sweeps keeps that from being something a
    // later edit has to remember.
    collectFrameShares();
    // Every slot, including the ones whose pass did not run this frame: their caches no sync of
    // theirs swept, so this is the pass that leaves no slot holding a geometry nothing outside the
    // caches references any more.
    for (auto& target_entry : state.targets) {
        for (auto& slot_entry : target_entry.second.content_slots) {
            slot_entry.second.bridge.releaseAbandonedGeometries(state.retained_shares);
        }
    }
    // Same moment for the materials: their entries own the Material (that is what keeps the pointer
    // key valid), so this is what stops a live scene's material churn from pinning every material it
    // has ever seen (D13). Judged by the session's counts, because a material is also held by the
    // variant template of every slot that draws it (the mutual wait the counts break, P11).
    persistent.materialManager.releaseAbandoned(state.retained_shares);
    // The session-scoped caches are swept ONCE per frame, here, and not from the per-slot sync: their
    // entries are shared by every slot, so a per-slot sweep multiplied the work by the slot count (and
    // rebuilt a share count each time) to answer a question that does not change between slots. The
    // judgement itself is unchanged — a texture (or shared mesh bind) whose last holder is gone goes —
    // and this is the ordering it needs: every slot's geometry sweep, and the frame-end one above, has
    // run, so a geometry that left the frame has already let go of the texture its entry held.
    if (state.texture_cache != nullptr) {
        state.texture_cache->releaseAbandoned();
    }
    if (state.mesh_cache != nullptr) {
        state.mesh_cache->releaseAbandoned();
    }
}

void VsgRenderer::endFrame()
{
    if (state.viewer == nullptr) {
        return;
    }
    state.viewer->update();
}

void VsgRenderer::setRenderTarget(vine::raw_ptr<vine::graphics::RenderTarget> target)
{
    // A scope attribute: the pass announced by beginPass() renders into this
    // target for every draw call of its scope (setRenderTarget comes before the
    // first one, see RenderBackend::setRenderTarget).
    state.request.target = target;
    // Announcing a target (re)arms the refusal of a dead announcement: whatever
    // was released before is none of THIS announcement's business
    // (refuseDeadTargetAnnouncement).
    state.request.target_released         = false;
    state.request.target_release_reported = false;
}

// Defined here rather than in the header for the sake of the header's reach: the state's ref-counted members
// name vsg types (VsgFwd.hpp), and a `ref_ptr` needs its pointee complete only in the translation unit that
// DESTROYS it. Every TU that includes VsgRendererState.hpp would otherwise compile vsg's app layer again.
VsgRendererState::~VsgRendererState() noexcept = default;
VsgRendererState::VsgRendererState() noexcept = default;
VsgRendererState::VsgRendererState(VsgRendererState&& other) noexcept = default;
VsgRendererState& VsgRendererState::operator=(VsgRendererState&& other) noexcept = default;

void VsgRenderer::resetPassRequest()
{
    // One assignment: a field added to VsgPassRequest can never be forgotten here,
    // which is the point of holding the whole request in one structure.
    state.request = VsgPassRequest{};
}

void VsgRenderer::setPassInputs(const std::vector<vine::raw_ptr<vine::graphics::RenderTarget>>& inputs)
{
    // Hold them for the drawing call that follows, like the lights: the slot binds them when it
    // (re)builds its retained state (see VsgContentSlot::setupContentSlot / renderContentSlot).
    //
    // assign() rather than a whole-vector copy, so the fill itself does not grow a buffer element by
    // element. It does NOT hand the buffer on to the next pass: resetPassRequest() replaces the whole
    // request (its one assignment is what keeps a field from being forgotten there), so this vector
    // starts each pass with no capacity. The steadier fix — if the per-pass allocation is ever worth
    // disturbing that guarantee — is to keep these two buffers apart from the request's scalars.
    state.request.inputs.assign(inputs.begin(), inputs.end());
}

void VsgRenderer::setLights(const std::vector<vine::raw_ptr<const vine::graphics::Light>>& lights)
{
    // Queue the lights for the next render() call (mirrors setViewport()): the
    // reserve below covers this fill only — resetPassRequest() replaces the request
    // between passes, so the buffer is not carried from one pass to the next.
    // light nodes are built when the matching view is reconciled in render().
    state.request.lights.clear();
    state.request.lights.reserve(lights.size());
    for (const auto* light : lights) {
        state.request.lights.push_back(light);
    }
}

bool VsgRenderer::supportsRenderTargets()
{
    return true;
}

void VsgRenderer::render(const std::vector<vine::graphics::RenderCommand>& commands, vine::raw_ptr<const vine::graphics::Camera> camera)
{
    // A drawing call with no pass behind it has nothing to belong to (see refuseNoPassAnnounced),
    // and a target announcement whose target was released cannot serve this draw call either
    // (the pointer lost its owner when it was released): say so once and skip the call.
    if (refuseNoPassAnnounced("render()")) {
        return;
    }
    if (refuseDeadTargetAnnouncement("render()")) {
        return;
    }
    if (!state.initialized || state.viewer == nullptr) {
        return;
    }

    // Take the PER-DRAW-CALL state: the sub-viewport queued just before this
    // draw call (overlays; the main pass never sets one and fills the surface)
    // and the lights from the content scene (empty keeps each view's default
    // light(s)). Taking clears them, so one draw call cannot inherit the other's.
    const std::optional<vine::graphics::Viewport> viewport = takeRequestViewport();
    std::vector<const vine::graphics::Light*>     lights   = state.request.takeLights();

    // The SCOPE attributes are READ, never consumed: they describe the PASS, so every draw call
    // of the same scope sees the same order / depth policy / target / presenting flag (endPass()
    // drops them with the rest of the request, and the next beginPass() starts from an empty one). What each one means is documented where it is set: setPassOrder (it is also
    // the content-slot key under this camera AND the stacking order), setDepthMode, and
    // setRenderTarget — EVERY target shares the one slot path below, and the GPU attachments are
    // ensured before the slot draws (window = the shared swapchain graph from initialize();
    // off-screen = owned attachments + graph, built / rebuilt to the target's size).
    //
    // Only the target is read into a local here (the attachment checks below need it); the order / depth
    // policy / presenting role are read by the content slot from the same request, so a pass' attributes
    // exist in ONE description instead of being copied per draw call.
    vine::graphics::RenderTarget* target_key = state.request.target;

    if (target_key != nullptr && (camera == nullptr || !target_key->isValid() || (!target_key->hasColor() && !target_key->hasDepth()))) {
        // Off-screen target unusable (no camera, invalid, or neither colour
        // nor depth attachment): nothing to draw this pass.
        return;
    }

    // A pass owns one slot per target: if this pass rendered into a DIFFERENT
    // target before (its render target changed at run time), drop that stale
    // slot so it stops drawing there (H2).
    detail::retargetPass(state, state.request.pass, target_key);

    auto& target = state.entryFor(target_key);
    // A depth borrow that is merely WAITING or whose baked source image was replaced means the
    // recorded attachments no longer match the source they have to test against (see
    // detail::borrowNeedsRebuild for both cases).
    const bool borrow_needs_rebuild = detail::borrowNeedsRebuild(state, target, target_key);
    if (target_key != nullptr &&
        (!target.attachments_built || target.width != target_key->width() ||
         target.height != target_key->height() || borrow_needs_rebuild ||
         !target.build_key.matches(*target_key))) {
        // First render into this off-screen target, or it was resized, or its
        // attachment shape changed (colour attachments, depth format, depth
        // promotion — see VsgRenderTargetEntry::BuildKey), or its depth borrow changed: build
        // (or rebuild) its attachments. Each pass then creates its own render
        // pass from its own clear request (see passGraph), so a depth-policy
        // change needs no rebuild. Any content slots compiled against the old
        // attachments are dropped by buildOffscreenTarget.
        detail::buildOffscreenTarget(state, diagnostics, target_key);
        if (!target.attachments_built) {
            return; // off-screen target could not be built
        }
    }

    // Render into the content slot this pass owns under the active target —
    // window and off-screen share the same slot machinery (C6.4). The pass' SCOPE attributes are the ones
    // queued in state.request (the slot reads them itself, so a pass' description exists once), and what
    // belongs to THIS draw call — the camera, the command stream, the announced lights and the taken
    // sub-viewport — is passed along.
    if (camera != nullptr) {
        detail::renderContentSlot(state, persistent, diagnostics, camera, commands, lights, viewport);
    }

    // Submission is deferred to swapBuffers() so one frame (main pass + all
    // overlay passes) is recorded and presented exactly once.
}

void VsgRenderer::setViewport(int x, int y, int width, int height)
{
    state.request.viewport = vine::graphics::Viewport{ x, y, width, height };
}

std::optional<vine::graphics::Viewport> VsgRenderer::takeRequestViewport()
{
    return state.request.takeViewport();
}

void VsgRenderer::setPassOrder(int order)
{
    // A scope attribute: the engine announces each pass' addPass() order before
    // it executes, so every slot that pass creates stacks at that position
    // (setupContentSlot / placeViewByOrder).
    state.request.order = order;
}

void VsgRenderer::releaseAbandonedTargets()
{
    std::vector<vine::graphics::RenderTarget*> abandoned;
    for (const auto& entry : state.targets) {
        const auto& target_entry = entry.second;
        if (entry.first != nullptr && target_entry.owner != nullptr && target_entry.owner->useCount() <= 1u) {
            abandoned.push_back(entry.first);
        }
    }
    // Collected first: releasing an entry invalidates the iteration.
    for (auto* target : abandoned) {
        releaseRenderTarget(target);
    }
}

void VsgRenderer::reportSessionDevice()
{
    if (state.device_reported || state.window == nullptr) {
        return;
    }
    state.device_reported = true;
    const ::vsg::ref_ptr<::vsg::PhysicalDevice> physical = state.window->getPhysicalDevice();
    if (physical == nullptr) {
        V_LOGW("[VsgRenderer] device: (none reported by the window)");
        return;
    }
    const VkPhysicalDeviceProperties& properties = physical->getProperties();
    V_LOGI("[VsgRenderer] device: {} (Vulkan {}.{}.{}, driver {}, type {})",
           properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
           VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
           properties.driverVersion, static_cast<int>(properties.deviceType));
}

void VsgRenderer::settleSubmittedFrame(FrameCommit commit)
{
    // 1. Switch every pass that recorded a ONE-FRAME variant back to its steady variant — the
    //    one the NEXT frame has to record (see the declaration's notes).
    for (auto& entry : state.targets) {
        for (auto& pass : entry.second.passes) {
            if (pass.second.transient && pass.second.graph != nullptr && pass.second.render_pass != nullptr) {
                pass.second.graph->renderPass = pass.second.render_pass;
                pass.second.transient         = false;
            }
        }
    }
    // 2. Release what the rings parked kRetireRingDepth frames ago: one ring per content slot
    //    (the bridge's retained nodes) and one for the renderer-owned objects. The per-draw block
    //    slots ride the same clock, on the pool's own retired queue (it outlives the bridges whose
    //    teardown dropped them -- see VsgDrawBlockPool::retire).
    for (auto& target_entry : state.targets) {
        for (auto& slot_entry : target_entry.second.content_slots) {
            slot_entry.second.bridge.advanceRetireRing(commit);
        }
    }
    if (state.draw_block_pool != nullptr) {
        state.draw_block_pool->advanceRetired(commit);
    }
    state.retireRing.advance(commit);
}

void VsgRenderer::submitFrame()
{
    // The frame protocol, in order. Each step's guarantee lives in its own comment: what is
    // dropped before the record has to be dropped before it, and what is settled after the
    // submit may only be settled after it.
    releaseAbandonedTargets();
    reportSessionDevice();

    // The frame beginFrame() opened. A submit that no frame opened is REFUSED rather than served:
    // the deferral rings advance on the committed-frame clock, so serving it would advance them for
    // a frame that was never recorded — releasing what they parked a frame too early, while a
    // command buffer the GPU may still execute names it. The refusal is an EPISODE: one report, and
    // the next beginFrame() re-arms it, so a host looping on swapBuffers() is not flooded.
    const std::optional<FrameCommit> commit = state.pending_commit;
    state.pending_commit.reset();
    if (!commit.has_value()) {
        if (!state.submit_without_frame_reported) {
            state.submit_without_frame_reported = true;
            diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                               vine::graphics::DiagnosticCategory::PassProtocolViolation,
                               u8"swapBuffers() without an open frame (no beginFrame() before it): the submit"
                               u8" was skipped instead of recording a frame twice — the deferral rings advance"
                               u8" once per COMMITTED frame, and a second advance would release GPU objects a"
                               u8" frame too early");
        }
        return;
    }

    if (!state.initialized || state.viewer == nullptr) {
        return;
    }
    // Retire the retained state of every pass that did not execute this frame (disabled, or no
    // longer registered) BEFORE submitting: such a pass must stop being drawn, and the removal
    // needs a presented frame or its stale content would stay on screen.
    retireInactivePassSlots();
    // Compile any geometry synced this frame before the record. A frame that never submits
    // keeps the queue for the next one (nothing was presented in between).
    detail::compilePendingViews(state, diagnostics);
    // The frame is submitted even when nothing was drawn: beginFrame() already ACQUIRED a
    // swapchain image, and an acquired image is only returned to the presentation engine by
    // presenting it — skipping the submission (nothing to draw / every pass disabled) leaks
    // one image per frame, which validation reports as
    // VUID-vkAcquireNextImageKHR-surface-07783, and the swapchain eventually starves.
    // Re-recording an unchanged graph is cheap (vsg records the command graph every frame).
    state.viewer->recordAndSubmit();
    state.viewer->present();
    settleSubmittedFrame(*commit);

    // Everything the app let go of, judged by counts collected for this moment (see the
    // declaration: a slot dropped during the frame must not leave the picture over-counted).
    releaseAbandonedContent();
}

void VsgRenderer::setClearPolicy(const vine::graphics::ClearPolicy& policy)
{
    // The clear belongs to the pass that announced its target (see below), so a call with no pass
    // behind it is refused like a draw call would be: pushing this clear to the WINDOW would wipe a
    // frame the caller never asked to touch.
    if (refuseNoPassAnnounced("setClearPolicy()")) {
        return;
    }
    if (refuseDeadTargetAnnouncement("setClearPolicy()")) {
        return;
    }
    // A clear marks the next render() as main (depth-on) content; a render
    // without a preceding clear is styled on-top (HUD, depth-off). The style
    // is consumed in render(). This marker is separate from the real clear
    // below (colour + optional depth on the CURRENT target), so the depth-on/
    // off mechanism no longer swallows the actual clear semantics.
    state.request.presenting  = true;
    state.request.clear_depth = policy.depth;

    // The clear applies to the CURRENT render target (set by setRenderTarget;
    // nullptr = the window): an off-screen pass's clear must reach ITS graph,
    // not the window's. The request is recorded on that target so a later
    // off-screen graph (re)build reapplies the colour and the depth policy
    // (buildOffscreenTarget), then pushed into the graph when one exists.
    vine::graphics::RenderTarget* key = state.request.target;
    auto& t = state.entryFor(key);
    const ::vsg::vec4 color{
        policy.color.r / 255.0f,
        policy.color.g / 255.0f,
        policy.color.b / 255.0f,
        policy.color.a / 255.0f
    };
    t.clear_seen              = true;
    t.clear_color             = color;
    state.request.clear_color = color;

    if (key == nullptr) {
        if (t.graph == nullptr) {
            return; // the window session has no graph yet
        }
        // Window graph: the swapchain render pass (vsg-owned) clears colour AND
        // depth at the start of every frame, so the requested colour is pushed
        // through and the depth-clear value is the main pass's (0.0). The
        // window CANNOT honour depth = false — vsg fixes the pass depth
        // load-op to CLEAR when the window is created — so a false request is
        // treated as true there (documented on RenderBackend::setClearPolicy());
        // only off-screen targets honour policy.depth through their depth-LOAD pass.
        const VkClearColorValue clear_value{
            { color.r, color.g, color.b, color.a }
        };
        t.graph->setClearValues(clear_value, VkClearDepthStencilValue{ kReverseZFarPlane, 0 });
        return;
    }

    // Off-screen: the colour is only RECORDED here. It reaches a graph when the
    // pass that asked for it next looks its own graph up (see passGraph): a pass
    // must clear to ITS OWN request, so broadcasting this colour to every pass
    // graph of the target would re-introduce exactly the "the last request wins
    // for the whole target" behaviour that §28 removes.
}

void VsgRenderer::setDepthMode(vine::graphics::DepthMode mode)
{
    // A scope attribute: the content's depth handling is explicit (Disabled /
    // TestOnly / TestAndWrite) and every draw call of the scope keeps it.
    // Independent of setClearPolicy() (a pass can test-only against depth an
    // earlier pass of the same target wrote, without clearing) and of lighting.
    state.request.depth_mode = mode;
}

void VsgRenderer::swapBuffers()
{
    // One record+submit+present per frame, after all passes were synced.
    submitFrame();
}

void VsgRenderer::setDefaultContentProgram(vine::intrusive_ptr<const vine::graphics::ShaderProgram> program)
{
    if (persistent.default_content_program == program) {
        return;
    }
    persistent.default_content_program = std::move(program);
    if (state.window == nullptr) {
        // Not initialized yet: every slot is created after this, so initialize()
        // bakes the new program and there is nothing to drop.
        return;
    }
    // A live session. The program is not a per-frame value: it selects which
    // shader set a slot draws with, and the set is built once (initialize() for
    // the window sets, the slot's own build for an off-screen target). So the
    // switch is a REBUILD of the shading side: the window sets are remade, every
    // baked slot is dropped and every target forgets its per-size sets — the
    // next frame's pass sync rebuilds them from the program now recorded. The
    // attachments, the pass graphs and the depth history stay untouched, so the
    // content is shaded differently rather than the target starting over.
    const auto extent = state.window->extent2D();
    state.depth_on_shader_set       = makeContentShaderSet(persistent.default_content_program, extent, true, true);
    state.depth_testonly_shader_set = makeContentShaderSet(persistent.default_content_program, extent, true, false);
    state.depth_off_shader_set      = makeContentShaderSet(persistent.default_content_program, extent, false, false);
    detail::resetContentShaderSlots(state);
}

void VsgRenderer::setWindowHandle(void* native_handle)
{
    persistent.bound_handle = native_handle;
}

void VsgRenderer::resize(int width, int height)
{
    // The surface owns its size: this session renders into a vsg window, so the size that counts is
    // that window's extent, and the announcement is advisory here (the engine keeps the announced
    // numbers for the passes that lay themselves out on them — RenderBackend::resize documents the
    // authority order). window->resize() re-queries the surface the host gave us (an embedded Qt
    // window, which follows the widget), and the presenting slots' viewports are re-derived from
    // the live extent below.
    (void)width;
    (void)height;
    if (state.window != nullptr) {
        state.window->resize();
    }
    // Every window presenting (full-target) content slot's camera viewport
    // follows the live window size so the render graph's render area tracks a
    // resize (renderContentSlot also re-derives each slot's viewport every
    // frame; refreshing here keeps slots correct even before their next
    // render). Other slots carry their own sub-viewport, re-set per frame by
    // their pass.
    auto& window_target = state.entryFor(nullptr);
    if (state.window == nullptr) {
        return;
    }
    const auto extent = state.window->extent2D();
    for (auto& kv : window_target.content_slots) {
        auto& slot = kv.second;
        if (slot.ready && slot.vsg_camera != nullptr && slot.applied.presenting) {
            // Through the ONE slot-viewport implementation (see detail::updateSlotViewport): it updates
            // the slot's existing ViewportState in place, so a resize does not allocate per slot either.
            detail::updateSlotViewport(slot, /*presenting*/ true, std::nullopt,
                                       static_cast<int>(extent.width), static_cast<int>(extent.height));
        }
    }
}

bool VsgRenderer::readColorBuffer(const vine::graphics::RenderTarget* target, int attachment,
                                  std::vector<std::uint8_t>& outPixels, vine::graphics::ReadbackResult* why)
{
    return detail::readColorBuffer(state, diagnostics, target, attachment, outPixels, why);
}

bool VsgRenderer::readDepthBuffer(const vine::graphics::RenderTarget* target, std::vector<float>& outDepths,
                                  vine::graphics::ReadbackResult* why)
{
    return detail::readDepthBuffer(state, diagnostics, target, outDepths, why);
}

void VsgRenderer::releaseRenderTarget(vine::graphics::RenderTarget* target)
{
    detail::releaseRenderTarget(state, diagnostics, target);
}

void VsgRenderer::drawScreenProgram(vine::graphics::RenderTarget* source,
                                    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                    vine::raw_ptr<const vine::graphics::Camera>        camera)
{
    if (refuseNoPassAnnounced("drawScreenProgram()")) {
        return;
    }
    if (refuseDeadTargetAnnouncement("drawScreenProgram()")) {
        return;
    }
    detail::drawScreenProgram(state, diagnostics, source, program, camera);
}

void* VsgRenderer::nativeHandle() const
{
    return persistent.bound_handle;
}

void VsgRenderer::setDiagnosticSink(vine::graphics::DiagnosticSink sink)
{
    RenderBackend::setDiagnosticSink(std::move(sink));
    // Re-route every retained slot bridge, and remember it for slots created
    // later (setupContentSlot installs the route). The renderer is the object a
    // host holds, so it must be the single place the sink is set.
    for (auto& target_entry : state.targets) {
        for (auto& slot_entry : target_entry.second.content_slots) {
            detail::installDiagnosticRoute(diagnostics, slot_entry.second.bridge);
        }
    }
}

std::size_t VsgRenderer::offscreenBuildCount() const noexcept
{
    return state.offscreen_build_count;
}

std::size_t VsgRenderer::programSlotBuildCount() const noexcept
{
    return state.program_slot_build_count;
}

std::size_t VsgRenderer::deviceWaitCount() const noexcept
{
    return state.retireRing.waitCount();
}

std::size_t VsgRenderer::retiredObjectCount() const noexcept
{
    return state.retireRing.releasedCount();
}

VsgRetentionStats VsgRenderer::retentionStats() const noexcept
{
    VsgRetentionStats stats;
    for (const auto& target_entry : state.targets) {
        stats.content_slots += target_entry.second.content_slots.size();
    }
    if (state.draw_block_pool != nullptr) {
        stats.slots = state.draw_block_pool->stats();
    }
    // The session's caches, which is where a scene's memory actually goes: entries are what the caches
    // bound, so a host watching for growth has to see them next to the node counts above.
    if (state.mesh_cache != nullptr) {
        stats.mesh_streams = state.mesh_cache->count();
    }
    if (state.texture_cache != nullptr) {
        stats.textures = state.texture_cache->count();
    }
    stats.slot_bytes      = stats.slots.bytes;
    stats.parked_nodes    = state.retireRing.parkedCount();
    stats.released_nodes  = state.retireRing.releasedCount();
    stats.device_waits    = state.retireRing.waitCount();
    stats.compile_contexts = state.compile_context_registrations;
    return stats;
}

std::size_t VsgRenderer::detachedSlotCount() const noexcept
{
    std::size_t count = 0;
    for (auto& entry : state.targets) {
        entry.second.forEachSlot([&](const SlotKey&, const auto& slot, auto) { count += slot.detached ? 1u : 0u; });
    }
    return count;
}

V_VSG_NS_END
