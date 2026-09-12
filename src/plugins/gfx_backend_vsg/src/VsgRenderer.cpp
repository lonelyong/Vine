#include <vine/vsg/VsgRenderer.hpp>

#include <vine/vsg/VsgViewCompiler.hpp>

#include <vine/vsg/VsgContentSlot.hpp>
#include <vine/vsg/VsgOverlay.hpp>
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
#include <vsg/app/CompileManager.h>
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
        shutdown();
        return false;
    }

    // Window-target shader sets shared by its content slots (embedded SPIR-V,
    // no runtime glslang): the depth-on set keeps depth test/write on; the
    // depth-off set disables it so the slot's content always draws on top of
    // earlier content (HUD). Off-screen targets bake their own per-size sets
    // lazily.
    init_stage = "building window shader sets";
    state.depth_on_shader_set        = buildShaderSet(persistent.shader_preset, state.window->extent2D(), true, true);
    state.depth_testonly_shader_set  = buildShaderSet(persistent.shader_preset, state.window->extent2D(), true, false);
    state.depth_off_shader_set       = buildShaderSet(persistent.shader_preset, state.window->extent2D(), false, false);

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
    const auto compileResult = state.viewer->compile();
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
    // as the engine runs them, so the activity set starts empty. The
    // protocol-used marker is deliberately STICKY (not cleared here): once the
    // backend has been driven through pass scopes, a frame in which every pass
    // is disabled announces nothing and must still retire the retained views.
    state.passes_active_this_frame.clear();
    if (state.viewer == nullptr) {
        return;
    }
    state.viewer->advanceToNextFrame();
    state.viewer->handleEvents();
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
}

void VsgRenderer::resetPassRequest()
{
    // One assignment: a field added to VsgPassRequest can never be forgotten here,
    // which is the point of holding the whole request in one structure.
    state.request = VsgPassRequest{};
}

void VsgRenderer::setLights(const std::vector<vine::raw_ptr<const vine::graphics::Light>>& lights)
{
    // Queue the lights for the next render() call (mirrors setViewport()): the
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
    if (!state.initialized || state.viewer == nullptr) {
        return;
    }

    // Take the PER-DRAW-CALL state: the sub-viewport queued just before this
    // draw call (overlays; the main pass never sets one and fills the surface)
    // and the lights from the content scene (empty keeps each view's default
    // light(s)). Taking clears them, so one draw call cannot inherit the other's.
    const std::optional<vine::graphics::Viewport> viewport = takeRequestViewport();
    std::vector<const vine::graphics::Light*>     lights   = state.request.takeLights();
    ++state.request.draws;

    // The SCOPE attributes are READ, never consumed: they describe the PASS, so every draw call
    // of the same scope sees the same order / depth policy / target / presenting flag (endPass()
    // drops them with the rest of the request, and a direct driver overwrites them with its next
    // set* call). What each one means is documented where it is set: setPassOrder (it is also
    // the content-slot key under this camera AND the stacking order), setDepthMode, and
    // setRenderTarget — EVERY target shares the one slot path below, and the GPU attachments are
    // ensured before the slot draws (window = the shared swapchain graph from initialize();
    // off-screen = owned attachments + graph, built / rebuilt to the target's size).
    const int                       pass_order = state.request.order;
    const vine::graphics::DepthMode depth_mode = state.request.depth_mode;
    const bool                      presenting = state.request.presenting;
    vine::graphics::RenderTarget*   target_key = state.request.target;

    if (target_key != nullptr && (camera == nullptr || !target_key->valid() || (!target_key->hasColor() && !target_key->hasDepth()))) {
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
    // window and off-screen share the same slot machinery (C6.4). The slot's
    // depth style and presenting role are carried per call and re-applied when
    // they changed; its stacking position follows the pass' explicit order.
    if (camera != nullptr) {
        VsgContentSlotRequest request;
        request.target      = target_key;
        request.camera      = camera;
        request.commands    = &commands;
        request.lights      = &lights;
        request.depth_mode  = depth_mode;
        request.presenting  = presenting;
        request.clear_depth = state.request.clear_depth;
        request.order       = pass_order;
        request.viewport    = viewport;
        detail::renderContentSlot(state, persistent, diagnostics, request);
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

void VsgRenderer::settleSubmittedFrame()
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
    //    (the bridge's retained nodes) and one for the renderer-owned objects.
    for (auto& target_entry : state.targets) {
        for (auto& slot_entry : target_entry.second.content_slots) {
            slot_entry.second.bridge.advanceRetireRing();
        }
    }
    state.retireRing.advance();
}

void VsgRenderer::submitFrame()
{
    // The frame protocol, in order. Each step's guarantee lives in its own comment: what is
    // dropped before the record has to be dropped before it, and what is settled after the
    // submit may only be settled after it.
    releaseAbandonedTargets();
    reportSessionDevice();

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
    settleSubmittedFrame();

    // Same point in the frame: release the material resources of materials the app has dropped.
    // Their entries own the Material (that is what keeps the pointer key valid), so this is what
    // stops a live scene's material churn from pinning every material it has ever seen (D13).
    persistent.materialManager.releaseAbandoned();
}

void VsgRenderer::clear(const vine::Color& backgroundColor, bool clearDepth)
{
    // A clear marks the next render() as main (depth-on) content; a render
    // without a preceding clear is styled on-top (HUD, depth-off). The style
    // is consumed in render(). This marker is separate from the real clear
    // below (colour + optional depth on the CURRENT target), so the depth-on/
    // off mechanism no longer swallows the actual clear semantics.
    state.request.presenting  = true;
    state.request.clear_depth = clearDepth;

    // The clear applies to the CURRENT render target (set by setRenderTarget;
    // nullptr = the window): an off-screen pass's clear must reach ITS graph,
    // not the window's. The request is recorded on that target so a later
    // off-screen graph (re)build reapplies the colour and the depth policy
    // (buildOffscreenTarget), then pushed into the graph when one exists.
    vine::graphics::RenderTarget* key = state.request.target;
    auto& t = state.entryFor(key);
    const ::vsg::vec4 color{
        backgroundColor.r / 255.0f,
        backgroundColor.g / 255.0f,
        backgroundColor.b / 255.0f,
        backgroundColor.a / 255.0f
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
        // window CANNOT honour clearDepth=false — vsg fixes the pass depth
        // load-op to CLEAR when the window is created — so a false request is
        // treated as true there (documented on RenderBackend::clear()); only
        // off-screen targets honour clearDepth through their depth-LOAD pass.
        const VkClearColorValue clear_value{
            { color.r, color.g, color.b, color.a }
        };
        t.graph->setClearValues(clear_value, VkClearDepthStencilValue{ 0.0f, 0 });
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
    // Independent of clear() (a pass can test-only against depth an earlier
    // pass of the same target wrote, without clearing) and of lighting.
    state.request.depth_mode = mode;
}

void VsgRenderer::swapBuffers()
{
    // One record+submit+present per frame, after all passes were synced.
    submitFrame();
}

vine::raw_ptr<vine::graphics::MaterialManager> VsgRenderer::materialManager()
{
    return &persistent.materialManager;
}

void VsgRenderer::setShaderPreset(vine::graphics::ShaderPreset preset)
{
    persistent.shader_preset = preset;
}

void VsgRenderer::setWindowHandle(void* native_handle)
{
    persistent.bound_handle = native_handle;
}

void VsgRenderer::resize(int width, int height)
{
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
        if (slot.ready && slot.vsg_camera != nullptr && slot.presenting) {
            slot.vsg_camera->viewportState = ::vsg::ViewportState::create(extent);
        }
    }
}

bool VsgRenderer::readColorBuffer(vine::graphics::RenderTarget* target, int attachment,
                                  std::vector<std::uint8_t>& outPixels)
{
    return detail::readColorBuffer(state, diagnostics, target, attachment, outPixels);
}

bool VsgRenderer::readDepthBuffer(vine::graphics::RenderTarget* target, std::vector<float>& outDepths)
{
    return detail::readDepthBuffer(state, diagnostics, target, outDepths);
}

void VsgRenderer::releaseRenderTarget(vine::graphics::RenderTarget* target)
{
    detail::releaseRenderTarget(state, diagnostics, target);
}

void VsgRenderer::releaseWindowLayer(vine::raw_ptr<const vine::graphics::Camera> camera, int order)
{
    detail::releaseWindowLayer(state, camera, order);
}

void VsgRenderer::drawScreenTexture(vine::graphics::RenderTarget* source, int attachment)
{
    detail::drawScreenTexture(state, diagnostics, source, attachment);
}

void VsgRenderer::drawScreenProgram(vine::graphics::RenderTarget* source,
                                    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                    vine::raw_ptr<const vine::graphics::Camera>        camera)
{
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

void VsgRenderer::frame()
{
    if (!state.initialized || state.viewer == nullptr) {
        return;
    }
    // VSG frame order: advance -> handleEvents -> update -> record -> present.
    // No Vine content is bound to the renderer: the engine drives content per
    // pass, so this convenience hook only presents whatever the passes synced
    // (submitFrame() skips when nothing was rendered this frame).
    beginFrame();
    endFrame();
    swapBuffers();
}

::vsg::ref_ptr<::vsg::Viewer> VsgRenderer::viewer() const
{
    return state.viewer;
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
    return state.retireRing.waits;
}

std::size_t VsgRenderer::retiredObjectCount() const noexcept
{
    return state.retireRing.released;
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
