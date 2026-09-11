#include <vine/vsg/VsgRenderer.hpp>

#include "VsgUtils.hpp"
#include "VsgRendererImpl.hpp"
#include "VsgBackendUtility.hpp"
#include "VsgPipelineFactory.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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

} // namespace

VsgRenderer::VsgRenderer()
  : impl(new Impl()),
    persistent(new Persistent())
{
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
    if (impl->window != nullptr) {
        void* bound = persistent->bound_handle;
        shutdown();
        persistent->bound_handle = bound;
    }
    // The stage label is printed if any step below throws, so a failing init
    // reports exactly where it died (window/device/swapchain creation, shader
    // sets, viewer compile) instead of a bare "unknown exception".
    const char* init_stage = "creating Vulkan window (instance/device/swapchain)";
    try {
    // Window. When a host native window is bound, attach to its surface (e.g.
    // a Qt QWindow) instead of creating a separate window.
    auto traits         = ::vsg::WindowTraits::create();
    traits->windowTitle = "Vine";
    traits->width       = 1280;
    traits->height      = 720;
    // Debug switch VINE_VSG_DEBUG_LAYER turns on the Vulkan validation layer so
    // silent pipeline/render-pass failures (no validation in normal runs)
    // surface as messages.
    traits->debugLayer = std::getenv("VINE_VSG_DEBUG_LAYER") != nullptr;
    // The SDK render-state model maps to pipeline features that are optional in
    // Vulkan: PolygonMode::Line needs fillModeNonSolid, and MRT pipelines with
    // differing attachments need independentBlend. Both are near-universal core
    // features; request them so pipelines honour the mapped state instead of
    // tripping validation / pipeline creation on capable devices.
    traits->deviceFeatures = ::vsg::DeviceFeatures::create();
    traits->deviceFeatures->get().fillModeNonSolid = VK_TRUE;
    traits->deviceFeatures->get().independentBlend = VK_TRUE;

    void* host_handle = persistent->bound_handle;
    if (forceOwnWindow()) {
        // Temporary test path: create vsg's own window, ignoring the Qt-hosted
        // surface handle, to verify rendering independent of Qt compositing.
        host_handle = nullptr;
    }
    if (host_handle != nullptr) {
#ifdef _WIN32
        traits->nativeWindow = reinterpret_cast<HWND>(host_handle);
        RECT client_rect{};
        if (::GetClientRect(reinterpret_cast<HWND>(host_handle), &client_rect) && client_rect.right > client_rect.left && client_rect.bottom > client_rect.top)
        {
            traits->width  = client_rect.right - client_rect.left;
            traits->height = client_rect.bottom - client_rect.top;
        }
#else
        // vsg's Xcb backend reads the native window as an xcb_window_t
        // (uint32_t). The host handle carries QWindow::winId() bits, so
        // narrow it to exactly that type; std::any only matches on the
        // exact type, and storing a void*/64-bit handle makes vsg throw
        // bad_any_cast when it casts back to xcb_window_t.
        traits->nativeWindow = static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(host_handle));
#endif
    }
    impl->window = ::vsg::Window::create(traits);
    if (impl->window == nullptr) {
        std::fprintf(stderr,
                     "[VsgRenderer] Window::create FAILED (nativeWindow=%d, %ux%u)\n",
                     traits->nativeWindow.has_value() ? 1 : 0,
                     traits->width,
                     traits->height);
        shutdown();
        return false;
    }

    // Window-target shader sets shared by its content slots (embedded SPIR-V,
    // no runtime glslang): the depth-on set keeps depth test/write on; the
    // depth-off set disables it so the slot's content always draws on top of
    // earlier content (HUD). Off-screen targets bake their own per-size sets
    // lazily.
    init_stage = "building window shader sets";
    impl->depth_on_shader_set        = buildShaderSet(persistent->shader_preset, impl->window->extent2D(), true, true);
    impl->depth_testonly_shader_set  = buildShaderSet(persistent->shader_preset, impl->window->extent2D(), true, false);
    impl->depth_off_shader_set       = buildShaderSet(persistent->shader_preset, impl->window->extent2D(), false, false);

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
    impl->viewer = ::vsg::ref_ptr<::vsg::Viewer>(new EmbeddedViewer());
    impl->viewer->addWindow(impl->window);

    // Window render graph (empty until the first content slot is created) +
    // command graph. The window target's graph IS this shared swapchain graph
    // (targets[nullptr].graph); every window content slot / PiP view is a
    // child of it.
    auto renderGraph      = ::vsg::RenderGraph::create(impl->window);
    renderGraph->contents = VK_SUBPASS_CONTENTS_INLINE;
    impl->entryFor(nullptr).graph = renderGraph;
    auto commandGraph     = ::vsg::CommandGraph::create(impl->window);
    commandGraph->addChild(renderGraph);
    impl->command_graph = commandGraph;
    impl->viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ commandGraph });

    init_stage = "initial viewer compile";
    const auto compileResult = impl->viewer->compile();
    if (!compileResult) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::InitFailed,
                      formatDiagnostic(u8"initialize FAILED at '%s': %s", init_stage,
                                       compileResult.message.c_str()));
        shutdown();
        return false;
    }

    impl->initialized = true;
    return true;
    }
    catch (...) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::InitFailed,
                      formatDiagnostic(u8"initialize FAILED at '%s': %s", init_stage,
                                       describeCurrentException().c_str()));
        // Environment hints: an init failure here is usually a missing/invalid
        // Vulkan ICD (VK_ICD_FILENAMES), a device/driver problem or a missing
        // display — print what the backend saw so a local environment issue is
        // not mistaken for a code defect.
        std::fprintf(stderr,
                     "[VsgRenderer]   init env: VK_ICD_FILENAMES=%s  VINE_VSG_DEBUG_LAYER=%s  DISPLAY=%s  WAYLAND_DISPLAY=%s\n",
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
    if (impl->viewer != nullptr) {
        impl->viewer->deviceWaitIdle();
        // Detach the window from the viewer so its command graphs are dropped
        // before the viewer is released.
        if (impl->window != nullptr) {
            impl->viewer->removeWindow(impl->window);
        }
        impl->viewer->close();
    }
    if (impl->window != nullptr) {
        // Release the native handle the platform window wraps. When the
        // reference is dropped, the Win32_Window destructor would call
        // ::DestroyWindow() (and ::UnregisterClass()) on the HOST's window —
        // here a Qt-owned HWND that Qt is itself tearing down. releaseWindow()
        // nulls the internal HWND so the destructor leaves Qt's window alone.
        impl->window->releaseWindow();
    }
    // Whole-session teardown: replacing the (session) Impl drops the window,
    // viewer, command graph, per-target render graphs, content slots and every
    // compiled pipeline that references the old vsg::Device — in one step, so
    // a newly retained vsg member cannot be forgotten here. The next
    // initialize() starts from a fresh Impl and allocates device ID 0, so a
    // surface-recreate re-init never trips vsg's VSG_MAX_DEVICES limit.
    impl = std::make_unique<Impl>();
    // The material manager outlives sessions (MaterialManager contract), so it
    // is cleared explicitly to drop references its cache holds to the dead
    // device; bound_handle points at a surface that is going away.
    persistent->materialManager.clear();
    persistent->bound_handle = nullptr;
}

void VsgRenderer::beginFrame()
{
    // A new frame: the passes active this frame are re-announced by beginPass()
    // as the engine runs them, so the activity set starts empty. The
    // protocol-used marker is deliberately STICKY (not cleared here): once the
    // backend has been driven through pass scopes, a frame in which every pass
    // is disabled announces nothing and must still retire the retained views.
    impl->passes_active_this_frame.clear();
    if (impl->viewer == nullptr) {
        return;
    }
    impl->viewer->advanceToNextFrame();
    impl->viewer->handleEvents();
}

void VsgRenderer::endFrame()
{
    if (impl->viewer == nullptr) {
        return;
    }
    impl->viewer->update();
}

void VsgRenderer::setRenderTarget(vine::raw_ptr<vine::graphics::RenderTarget> target)
{
    // A scope attribute: the pass announced by beginPass() renders into this
    // target for every draw call of its scope (setRenderTarget comes before the
    // first one, see RenderBackend::setRenderTarget).
    impl->request.target = target;
}

void VsgRenderer::resetPassRequest()
{
    // One assignment: a field added to PassRequest can never be forgotten here,
    // which is the point of holding the whole request in one structure.
    impl->request = Impl::PassRequest{};
}

void VsgRenderer::setLights(const std::vector<vine::raw_ptr<const vine::graphics::Light>>& lights)
{
    // Queue the lights for the next render() call (mirrors setViewport()): the
    // light nodes are built when the matching view is reconciled in render().
    impl->request.lights.clear();
    impl->request.lights.reserve(lights.size());
    for (const auto* light : lights) {
        impl->request.lights.push_back(light);
    }
}

bool VsgRenderer::supportsRenderTargets()
{
    return true;
}

void VsgRenderer::render(const std::vector<vine::graphics::RenderCommand>& commands, vine::raw_ptr<const vine::graphics::Camera> camera)
{
    if (!impl->initialized || impl->viewer == nullptr) {
        return;
    }

    // Take the PER-DRAW-CALL state: the sub-viewport queued just before this
    // draw call (overlays; the main pass never sets one and fills the surface)
    // and the lights from the content scene (empty keeps each view's default
    // light(s)). Taking clears them, so one draw call cannot inherit the other's.
    const std::optional<vine::graphics::Viewport> viewport = takeRequestViewport();
    std::vector<const vine::graphics::Light*>     lights   = impl->request.takeLights();
    ++impl->request.draws;

    // The SCOPE attributes below are read, never consumed: they describe the
    // pass, so every draw call of the same scope sees the same order / depth
    // policy / target / presenting flag. endPass() drops them with the rest of
    // the request (a direct driver overwrites them with its next set* call).
    //
    //  * order: the engine announces each pass' addPass() order before it runs.
    //    It is the content-slot key under this camera AND the stacking order
    //    (ascending) — setupContentSlot keeps each target's slot views sorted by
    //    it, so stacking follows the user-set pipeline order whatever the
    //    creation order.
    //  * depth: setDepthMode() (from RenderPass::depthMode) decides Disabled /
    //    TestOnly / TestAndWrite. clear() only records that this pass fills the
    //    target (the "presenting" pass, used to seed the default light); it does
    //    not imply a depth mode.
    //  * target: setRenderTarget (nullptr = the window). Every target shares ONE
    //    content-slot path (renderContentSlot): only the GPU attachment kind
    //    differs, and it is ensured here before the slot draws (window = the
    //    shared swapchain graph from initialize(); off-screen = owned
    //    attachments + graph, built / rebuilt to the target's size).
    const int                       pass_order = impl->request.order;
    const vine::graphics::DepthMode depth_mode = impl->request.depth_mode;
    const bool                      presenting = impl->request.presenting;
    vine::graphics::RenderTarget*   target_key = impl->request.target;

    if (target_key != nullptr && (camera == nullptr || !target_key->valid() || (!target_key->hasColor() && !target_key->hasDepth()))) {
        // Off-screen target unusable (no camera, invalid, or neither colour
        // nor depth attachment): nothing to draw this pass.
        return;
    }

    // A pass owns one slot per target: if this pass rendered into a DIFFERENT
    // target before (its render target changed at run time), drop that stale
    // slot so it stops drawing there (H2).
    retargetPass(impl->request.pass, target_key);

    auto& target = impl->entryFor(target_key);
    // A depth-LOAD policy (clearDepth=false) needs a render pass whose depth
    // attachment is not cleared; when the target's persisted clear policy
    // differs from its current pass, the graph is rebuilt so depth is either
    // genuinely preserved (LOAD) or cleared (CLEAR) — buildOffscreenTarget
    // bakes the policy and colour into the pass. A target that BORROWS another
    // target's depth never selects the depth-LOAD pass (its depth policy comes
    // from the owner), so the rebuild predicate must not expect one there (H4).
    // A target whose passes disagree (mixed) LOADs too: the passes that asked
    // for a clear clear it themselves (ContentSlot::clears_depth).
    const bool want_load_depth = target.wantsDepthLoad();
    // A depth borrow that could not be honoured yet (the source had no depth
    // image when this target was built) is retried as soon as the source has
    // one: the baked borrow differs from the requested one. A source that is
    // permanently unusable is remembered as such (unusable_depth_source), so
    // this retries only while the borrow is merely WAITING — a disabled or
    // never-built producer costs one map lookup per frame, not a rebuild loop.
    vine::graphics::RenderTarget* wanted_source =
        target_key != nullptr ? target_key->depthSource() : nullptr;
    bool borrow_pending = false;
    if (wanted_source != nullptr && target.depth_source != wanted_source &&
        target.unusable_depth_source != wanted_source) {
        const auto src_it   = impl->targets.find(wanted_source);
        borrow_pending = src_it != impl->targets.end() && src_it->second.depth_view != nullptr;
    }
    // A borrow that WAS honoured holds on to the source's depth IMAGE (its
    // framebuffer attachment is that view). A source that is rebuilt — a size
    // change, or the depth-policy change that this same predicate watches for
    // its own targets — replaces its depth image, and the borrower's framebuffer
    // would keep testing the replaced one, which nobody writes any more: the
    // borrowed depth silently freezes (and the old image stays alive). Compare
    // the source's current image with the one this target was baked with and
    // rebuild, which re-runs the borrow validation against the new image.
    bool borrow_stale = false;
    if (target.depth_source != nullptr && target.unusable_depth_source != target.depth_source) {
        const auto src_it = impl->targets.find(target.depth_source);
        borrow_stale = src_it == impl->targets.end() || src_it->second.depth_view != target.depth_source_view;
    }
    if (target_key != nullptr &&
        (target.graph == nullptr || target.width != target_key->width() ||
         target.height != target_key->height() || target.depth_load != want_load_depth || borrow_pending ||
         borrow_stale || !target.build_key.matches(*target_key))) {
        // First render into this off-screen target, or it was resized, or its
        // depth-clear policy changed, or its attachment / pass shape did
        // (colour attachments, depth format, depth promotion — see
        // Target::BuildKey): build (or rebuild) its attachments +
        // render graph. Any content slots compiled against an older graph are
        // dropped by buildOffscreenTarget.
        buildOffscreenTarget(target_key);
        if (target.graph == nullptr) {
            return; // off-screen target could not be built
        }
    }

    // Render into the content slot this pass owns under the active target —
    // window and off-screen share the same slot machinery (C6.4). The slot's
    // depth style and presenting role are carried per call and re-applied when
    // they changed; its stacking position follows the pass' explicit order.
    if (camera != nullptr) {
        ContentSlotRequest request;
        request.target      = target_key;
        request.camera      = camera;
        request.commands    = &commands;
        request.lights      = &lights;
        request.depth_mode  = depth_mode;
        request.presenting  = presenting;
        request.clear_depth = impl->request.clear_depth;
        request.order       = pass_order;
        request.viewport    = viewport;
        renderContentSlot(request);
    }

    // Submission is deferred to swapBuffers() so one frame (main pass + all
    // overlay passes) is recorded and presented exactly once.
}

void VsgRenderer::setViewport(int x, int y, int width, int height)
{
    impl->request.viewport = vine::graphics::Viewport{ x, y, width, height };
}

std::optional<vine::graphics::Viewport> VsgRenderer::takeRequestViewport()
{
    return impl->request.takeViewport();
}

void VsgRenderer::setPassOrder(int order)
{
    // A scope attribute: the engine announces each pass' addPass() order before
    // it executes, so every slot that pass creates stacks at that position
    // (setupContentSlot / placeViewByOrder).
    impl->request.order = order;
}

bool VsgRenderer::incrementalCompileViews()
{
    auto compileManager = impl->viewer->compileManager;
    if (compileManager == nullptr) {
        return false;
    }

    for (const auto& view : impl->pending_compile_views) {
        if (view == nullptr) {
            return false;
        }

        // Locate the owning target (window target keyed by nullptr) and the
        // retained content slot the view belongs to, so the compile context
        // can carry that target's render pass (window swapchain vs off-screen
        // framebuffer — a graphics pipeline cannot be created without one).
        Impl::Target*     owner    = nullptr;
        Impl::ContentSlot* slot    = nullptr;
        bool              is_window = false;
        for (auto& [target_key, target] : impl->targets) {
            for (auto& [slot_key, candidate] : target.content_slots) {
                if (candidate.ready && candidate.view == view) {
                    owner     = &target;
                    slot      = &candidate;
                    is_window = (target_key == nullptr);
                    break;
                }
            }
            if (slot != nullptr) {
                break;
            }
        }
        if (slot == nullptr) {
            // A pending view that is not a content slot (e.g. a PiP or
            // program slot compiled by another path): let the caller fall
            // back to the full compile.
            return false;
        }

        // Register the slot's (render pass + view) context once. The pool's
        // pooled traversal was built by CompileManager::create(viewer, hints)
        // when the window graph was still empty, so without this the pool has
        // no context that matches this view and compile() would compile
        // nothing.
        if (!slot->compile_context_registered) {
            ::vsg::CollectResourceRequirements collect;
            view->accept(collect);
            const auto& requirements = collect.requirements;
            try {
                if (is_window) {
                    if (impl->window == nullptr) {
                        return false;
                    }
                    compileManager->add(*impl->window, view, requirements);
                }
                else {
                    if (owner->framebuffer == nullptr || owner->framebuffer->getDevice() == nullptr) {
                        return false;
                    }
                    compileManager->add(*owner->framebuffer, view, requirements);
                }
            }
            catch (...) {
                return false;
            }
            slot->compile_context_registered = true;
        }

        // Compile ONLY this view: restrict the compile to the context whose
        // pre-assigned view matches, so the new/rebuild subtree is compiled
        // for the viewID it will be recorded under and no other slot is
        // touched.
        ::vsg::CompileResult result;
        try {
            ::vsg::View* const target_view = view.get();
            result = compileManager->compile(
                view, [target_view](::vsg::Context& context) { return context.view == target_view; });
        }
        catch (...) {
            return false;
        }
        if (!result) {
            return false;
        }
        // Feed dynamic data / slot / bin updates from the incremental compile
        // into the record tasks (per-frame dynamic buffers such as the DYNAMIC
        // opacity colour arrays rely on this).
        ::vsg::updateViewer(*impl->viewer, result);
    }

    return true;
}

void VsgRenderer::submitFrame()
{
    // Off-screen targets the host dropped without announcing it: the table owns
    // them (Target::owner), so once the host's last reference is gone nothing can
    // ever look the entry up again — the same rule the geometry and material
    // caches follow. Release them properly here instead of keeping their
    // attachments, render graph and compiled pipelines alive for the session.
    // Engine targets are normally released by releaseRenderTarget(); this is the
    // safety net for a host that destroys the RenderTarget itself.
    std::vector<vine::graphics::RenderTarget*> abandoned_targets;
    for (const auto& entry : impl->targets) {
        const auto& target_entry = entry.second;
        if (entry.first != nullptr && target_entry.owner != nullptr && target_entry.owner->useCount() <= 1u) {
            abandoned_targets.push_back(entry.first);
        }
    }
    for (auto* target : abandoned_targets) {
        releaseRenderTarget(target);
    }

    // Which device this session actually runs on, on the record: "the gate
    // passed" is only meaningful together with the driver it passed on, and a
    // software rasteriser and a real GPU exercise different paths. Reported on
    // the first submit, because that is where the window's device and swapchain
    // exist (vsg creates them lazily, on first use).
    if (!impl->device_reported && impl->window != nullptr) {
        impl->device_reported = true;
        const ::vsg::ref_ptr<::vsg::PhysicalDevice> physical = impl->window->getPhysicalDevice();
        if (physical != nullptr) {
            const VkPhysicalDeviceProperties& properties = physical->getProperties();
            std::fprintf(stderr, "[VsgRenderer] device: %s (Vulkan %u.%u.%u, driver %u, type %d)\n",
                         properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
                         VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
                         properties.driverVersion, static_cast<int>(properties.deviceType));
        }
        else {
            std::fprintf(stderr, "[VsgRenderer] device: (none reported by the window)\n");
        }
    }

    if (!impl->initialized || impl->viewer == nullptr) {
        return;
    }
    // Retire the retained state of every pass that did not execute this frame
    // (disabled, or no longer registered) BEFORE submitting: such a pass must
    // stop being drawn, and the removal itself needs a presented frame or the
    // stale content would stay on screen.
    retireInactivePassSlots();
    // The frame is submitted even when nothing was drawn. beginFrame() already
    // ACQUIRED a swapchain image for it, and an acquired image is only returned
    // to the presentation engine by presenting it: skipping the submission
    // (nothing to draw / every pass disabled) leaks one image per frame, which
    // the validation layer reports as
    // VUID-vkAcquireNextImageKHR-surface-07783 and which eventually starves the
    // swapchain. Re-recording an unchanged graph is cheap (vsg records the
    // command graph every frame anyway).
    // Compile any geometry synced this frame before the record. If this frame
    // never submits, the queue survives to the next submit (nothing was
    // presented in between).
    if (!impl->pending_compile_views.empty()) {
        // D22 incremental compile: ON by default. vsg's compileManager.compile
        // path is NOT wired for this renderer out of the box — the manager's
        // pooled traversal is built once at Viewer::compile() time, and in
        // Vine that first compile runs on an EMPTY window graph (content-slot
        // views are appended lazily later), so the pool holds no contexts and
        // compileManager->compile(view) silently compiles nothing ("successful"
        // but with unbuilt pipelines), which crashes at record
        // (GraphicsPipeline::vk on an empty _implementation). incrementalCompileViews()
        // registers each queued view's context into the pool itself and
        // compiles only that view, so it is safe to run every frame that some
        // slot gained geometry. Setting VINE_VSG_DISABLE_INCREMENTAL_COMPILE
        // forces the full-graph compile (stable, vsg skips already-compiled
        // objects) as an A/B escape hatch; any incremental failure also falls
        // back to the full compile automatically.
        bool compiled = false;
        if (std::getenv("VINE_VSG_DISABLE_INCREMENTAL_COMPILE") == nullptr &&
            impl->viewer->compileManager != nullptr) {
            compiled = incrementalCompileViews();
        }
        if (!compiled) {
            const auto compileResult = impl->viewer->compile();
            if (!compileResult) {
                reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::CompileFailed,
                              formatDiagnostic(u8"frame compile failed (%s): newly added content is not drawn this frame",
                                               compileResult.message.c_str()));
            }
        }
        impl->pending_compile_views.clear();
    }
    // Depth-LOAD (clearDepth=false) off-screen targets alternate between two
    // compatible render passes: the first frame after a (re)build uses the
    // depth-CLEAR pass, which initialises the fresh depth image's layout and
    // content; every later frame uses the depth-LOAD pass, which preserves it.
    // Swapping the graph's render pass is legal because the two passes differ
    // only in the depth load-op (framebuffer / pipeline compatible).
    for (auto& entry : impl->targets) {
        auto& t = entry.second;
        if (entry.first == nullptr || t.graph == nullptr || !t.depth_load ||
            t.render_pass_load == nullptr) {
            continue;
        }
        t.graph->renderPass = t.depth_ready ? t.render_pass_load : t.render_pass;
        t.depth_ready       = true;
    }
    impl->viewer->recordAndSubmit();
    impl->viewer->present();

    // One frame has been submitted: release the retained nodes that were
    // parked kRetireRingDepth frames ago, when every command-buffer slot that
    // could still reference them has been re-recorded (see
    // SceneBridge::retireNode). Done after the submit so the parked objects
    // stay alive for the whole frame that dropped them.
    for (auto& target_entry : impl->targets) {
        for (auto& slot_entry : target_entry.second.content_slots) {
            slot_entry.second.bridge.advanceRetireRing();
        }
    }

    // Same point in the frame: release the material resources of materials the
    // app has dropped. Their entries own the Material (that is what keeps the
    // pointer key valid), so this is what stops a live scene's material churn
    // from pinning every material it has ever seen (D13).
    persistent->materialManager.releaseAbandoned();
}

void VsgRenderer::clear(const vine::Color& backgroundColor, bool clearDepth)
{
    // A clear marks the next render() as main (depth-on) content; a render
    // without a preceding clear is styled on-top (HUD, depth-off). The style
    // is consumed in render(). This marker is separate from the real clear
    // below (colour + optional depth on the CURRENT target), so the depth-on/
    // off mechanism no longer swallows the actual clear semantics.
    impl->request.presenting  = true;
    impl->request.clear_depth = clearDepth;

    // The clear applies to the CURRENT render target (set by setRenderTarget;
    // nullptr = the window): an off-screen pass's clear must reach ITS graph,
    // not the window's. The request is recorded on that target so a later
    // off-screen graph (re)build reapplies the colour and the depth policy
    // (buildOffscreenTarget), then pushed into the graph when one exists.
    vine::graphics::RenderTarget* key = impl->request.target;
    auto& t = impl->entryFor(key);
    const ::vsg::vec4 color{
        backgroundColor.r / 255.0f,
        backgroundColor.g / 255.0f,
        backgroundColor.b / 255.0f,
        backgroundColor.a / 255.0f
    };
    const bool seen_before = t.clear_seen;
    t.clear_seen  = true;
    t.clear_color = color;
    // Two passes of one target that disagree about clearing depth cannot both
    // be served by that target's single render pass (it bakes ONE depth
    // load-op): remember the clash so the target switches to the depth-LOAD
    // pass and the passes that asked for a clear issue it themselves, before
    // their own draws (see Target::wantsDepthLoad /
    // ContentSlot::clears_depth). Without this the LAST request silently won
    // for the whole target — a later pass's "preserve depth" then also
    // suppressed the earlier pass's clear, and the depth of the previous frame
    // stayed behind content that should have been redrawn from scratch.
    if (seen_before && t.clear_depth != clearDepth) {
        t.depth_policy_mixed = true;
    }
    t.clear_depth = clearDepth;

    if (t.graph == nullptr) {
        // No graph yet (e.g. the first pass into an off-screen target): render()
        // builds one from the recorded request via buildOffscreenTarget.
        return;
    }

    if (key == nullptr) {
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

    // Off-screen graph: update the colour entries in place (the pass was built
    // with the right depth load-op — CLEAR or LOAD — from t.clear_depth).
    // Attachment 0 gets the requested colour; extra MRT attachments stay
    // transparent black so untouched G-buffer regions remain empty.
    const int color_count = key->colorCount();
    const std::size_t n   = std::min<std::size_t>(t.graph->clearValues.size(),
                                                  static_cast<std::size_t>(color_count));
    for (std::size_t i = 0; i < n; ++i) {
        t.graph->clearValues[i].color = VkClearColorValue{
            { i == 0u ? color.r : 0.0f,
              i == 0u ? color.g : 0.0f,
              i == 0u ? color.b : 0.0f,
              i == 0u ? color.a : 0.0f }
        };
    }
}

void VsgRenderer::setDepthMode(vine::graphics::DepthMode mode)
{
    // A scope attribute: the content's depth handling is explicit (Disabled /
    // TestOnly / TestAndWrite) and every draw call of the scope keeps it.
    // Independent of clear() (a pass can test-only against depth an earlier
    // pass of the same target wrote, without clearing) and of lighting.
    impl->request.depth_mode = mode;
}

void VsgRenderer::swapBuffers()
{
    // One record+submit+present per frame, after all passes were synced.
    submitFrame();
}

vine::raw_ptr<vine::graphics::MaterialManager> VsgRenderer::materialManager()
{
    return &persistent->materialManager;
}

void VsgRenderer::setShaderPreset(vine::graphics::ShaderPreset preset)
{
    persistent->shader_preset = preset;
}

void VsgRenderer::setWindowHandle(void* native_handle)
{
    persistent->bound_handle = native_handle;
}

void VsgRenderer::resize(int width, int height)
{
    (void)width;
    (void)height;
    if (impl->window != nullptr) {
        impl->window->resize();
    }
    // Every window presenting (full-target) content slot's camera viewport
    // follows the live window size so the render graph's render area tracks a
    // resize (renderContentSlot also re-derives each slot's viewport every
    // frame; refreshing here keeps slots correct even before their next
    // render). Other slots carry their own sub-viewport, re-set per frame by
    // their pass.
    auto& window_target = impl->entryFor(nullptr);
    if (impl->window == nullptr) {
        return;
    }
    const auto extent = impl->window->extent2D();
    for (auto& kv : window_target.content_slots) {
        auto& slot = kv.second;
        if (slot.ready && slot.vsg_camera != nullptr && slot.presenting) {
            slot.vsg_camera->viewportState = ::vsg::ViewportState::create(extent);
        }
    }
}

void* VsgRenderer::nativeHandle() const
{
    return persistent->bound_handle;
}

void VsgRenderer::installDiagnosticRoute(SceneBridge& bridge)
{
    // The bridge reports a diagnostic; the renderer turns it into the single
    // route (stderr trace + backend counters + host sink). Routing it through
    // the renderer instead of handing the host sink straight to the bridge is
    // what keeps diagnosticCount() honest: a bridge report is a backend report.
    bridge.setDiagnosticSink([this](const vine::graphics::RenderDiagnostic& diagnostic) {
        reportFailure(diagnostic.severity, diagnostic.category, diagnostic.message);
    });
}

void VsgRenderer::setDiagnosticSink(vine::graphics::DiagnosticSink sink)
{
    RenderBackend::setDiagnosticSink(std::move(sink));
    // Re-route every retained slot bridge, and remember it for slots created
    // later (setupContentSlot installs the route). The renderer is the object a
    // host holds, so it must be the single place the sink is set.
    for (auto& target_entry : impl->targets) {
        for (auto& slot_entry : target_entry.second.content_slots) {
            installDiagnosticRoute(slot_entry.second.bridge);
        }
    }
}

void VsgRenderer::reportFailure(vine::graphics::DiagnosticSeverity severity,
                                vine::graphics::DiagnosticCategory category,
                                const vine::String&             message)
{
    const char* level = severity == vine::graphics::DiagnosticSeverity::Error    ? "error"
                        : severity == vine::graphics::DiagnosticSeverity::Warning ? "warning"
                                                                                  : "info";
    std::fprintf(stderr, "[VsgRenderer] %s: %s\n", level, message.stdstr().c_str());
    reportDiagnostic(severity, category, message);
}

void VsgRenderer::frame()
{
    if (!impl->initialized || impl->viewer == nullptr) {
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
    return impl->viewer;
}

std::size_t VsgRenderer::offscreenBuildCount() const noexcept
{
    return impl->offscreen_build_count;
}

std::size_t VsgRenderer::programSlotBuildCount() const noexcept
{
    return impl->program_slot_build_count;
}

std::size_t VsgRenderer::detachedSlotCount() const noexcept
{
    std::size_t count = 0;
    for (const auto& entry : impl->targets) {
        for (const auto& kv : entry.second.content_slots) {
            count += kv.second.detached ? 1u : 0u;
        }
        for (const auto& kv : entry.second.screen_slots) {
            count += kv.second.detached ? 1u : 0u;
        }
        for (const auto& kv : entry.second.program_slots) {
            count += kv.second.detached ? 1u : 0u;
        }
    }
    return count;
}

V_VSG_NS_END
