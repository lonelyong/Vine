#include <vine/vsg/VsgRenderer.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
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
 * @param what     Label for the compile-failure diagnostic.
 * @return The compiled View, or null when compilation failed.
 */
::vsg::ref_ptr<::vsg::View> makeCompiledOverlayView(
    ::vsg::Viewer& viewer,
    ::vsg::Group* graph,
    ::vsg::ref_ptr<::vsg::Node> content,
    int x,
    int y,
    int w,
    int h,
    bool front,
    const char* what,
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

/**
 * @brief Computes the world -> view rotation basis for a look-at camera. *
 * @param camera Vine camera (eye / target / up).
 * @param r      Receives the view-space X axis in world coords (right).
 * @param u      Receives the view-space Y axis in world coords (up).
 * @param f      Receives the view-space -Z axis in world coords (forward).
 */
void viewRotation(const vine::graphics::Camera* camera, double r[3], double u[3], double f[3])
{
    const auto eye    = camera->eye();
    const auto center = camera->target();
    const auto up_vec = camera->up();
    double fx = center.x - eye.x;
    double fy = center.y - eye.y;
    double fz = center.z - eye.z;
    const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl > 1e-12) {
        fx /= fl;
        fy /= fl;
        fz /= fl;
    }
    else {
        fx = 0.0;
        fy = 0.0;
        fz = -1.0;
    }
    // r = normalize(f x up), u = r x f.
    double rx = fy * up_vec.z - fz * up_vec.y;
    double ry = fz * up_vec.x - fx * up_vec.z;
    double rz = fx * up_vec.y - fy * up_vec.x;
    const double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rl > 1e-12) {
        rx /= rl;
        ry /= rl;
        rz /= rl;
    }
    else {
        rx = 1.0;
        ry = 0.0;
        rz = 0.0;
    }
    const double ux = ry * fz - rz * fy;
    const double uy = rz * fx - rx * fz;
    const double uz = rx * fy - ry * fx;
    r[0] = rx;
    r[1] = ry;
    r[2] = rz;
    u[0] = ux;
    u[1] = uy;
    u[2] = uz;
    f[0] = fx;
    f[1] = fy;
    f[2] = fz;
}

/**
 * @brief Fills a fullscreen light push block for a deferred-lighting pass.
 *
 * The G-buffer stores view-space normals / positions, so directional lights
 * are pre-transformed from world to view space on the CPU (the fragment
 * shader then never needs a view matrix). Supports the first ambient plus up
 * to three directional lights (the push block is exactly 128 bytes); further
 * lights are ignored (documented S4 limitation). When the pass carries no
 * ambient light a small default ambient is seeded, mirroring how a scene pass
 * with an empty light list keeps its view's default light: without it a
 * fullscreen program pass bound to no lights would shade everything to black
 * (ambient 0 x albedo) — a silent, hard-to-diagnose blank frame.
 *
 * @param camera Camera whose view transforms the lights (may be null).
 * @param lights Scene lights to bake (borrowed).
 * @param block  Receives the packed block (zeroed first).
 */
void fillLightPushBlock(const vine::graphics::Camera*                               camera,
                        const std::vector<const vine::graphics::Light*>&            lights,
                        LightPushBlock&                                             block)
{
    block = LightPushBlock{};
    if (camera == nullptr) {
        return;
    }
    // Perspective projection parameters for view-position reconstruction from
    // the G-buffer depth (near / far / proj00 / proj11).
    if (camera->projectionType() == vine::graphics::Camera::ProjectionType::Perspective) {
        const double fov    = camera->fieldOfView() * 0.5; // degrees
        const double cot    = 1.0 / std::tan(fov * 3.14159265358979323846 / 180.0);
        const double aspect = camera->aspectRatio();
        block.projparms[0]  = static_cast<float>(camera->nearPlane());
        block.projparms[1]  = static_cast<float>(camera->farPlane());
        block.projparms[2]  = static_cast<float>(cot / aspect); // proj[0][0]
        block.projparms[3]  = static_cast<float>(cot);          // proj[1][1]
    }
    double r[3] = {}, u[3] = {}, f[3] = {};
    viewRotation(camera, r, u, f);
    int  dirlight    = 0;
    bool has_ambient = false;
    for (const auto* light : lights) {
        if (light == nullptr || !light->isEnabled()) {
            continue;
        }
        const auto c = light->color();
        switch (light->type()) {
        case vine::graphics::LightType::Ambient:
            block.ambient[0] = c.r;
            block.ambient[1] = c.g;
            block.ambient[2] = c.b;
            block.ambient[3] = light->intensity();
            has_ambient      = true;
            break;
        case vine::graphics::LightType::Directional:
            if (dirlight >= 3) {
                break; // the push block holds up to three directional lights
            }
            {
                const auto d = light->direction();
                // world -> view direction (rotation only): rows r, u, -f.
                double vx = r[0] * d.x + r[1] * d.y + r[2] * d.z;
                double vy = u[0] * d.x + u[1] * d.y + u[2] * d.z;
                double vz = -f[0] * d.x - f[1] * d.y - f[2] * d.z;
                const double vl = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (vl > 1e-9) {
                    vx /= vl;
                    vy /= vl;
                    vz /= vl;
                }
                float* dd = block.dirs[dirlight].data();
                float* cc = block.cols[dirlight].data();
                dd[0] = static_cast<float>(vx);
                dd[1] = static_cast<float>(vy);
                dd[2] = static_cast<float>(vz);
                dd[3] = 0.0f;
                cc[0] = c.r;
                cc[1] = c.g;
                cc[2] = c.b;
                cc[3] = light->intensity();
                ++dirlight;
            }
            break;
        default:
            break;
        }
    }
    if (!has_ambient) {
        // Keep an unlit fullscreen program visible: without any ambient the
        // fragment shader would multiply the albedo by zero (see the header).
        block.ambient[0] = 0.15f;
        block.ambient[1] = 0.15f;
        block.ambient[2] = 0.15f;
        block.ambient[3] = 1.0f;
    }
}

} // namespace

void VsgRenderer::drawScreenTexture(vine::graphics::RenderTarget* source, int attachment)
{
    if (!impl->initialized || impl->viewer == nullptr || impl->window == nullptr || source == nullptr) {
        return;
    }

    // Consume the sub-viewport queued by setViewport() (the ScreenPass's PiP
    // rectangle); mirrors how render() consumes one for overlays.
    const std::optional<vine::graphics::Viewport> viewport = takeRequestViewport();

    auto src_it = impl->targets.find(source);
    if (src_it == impl->targets.end() || src_it->second.color_views.empty()) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"drawScreenTexture: source target has no colour attachment: the pass draws nothing");
        return;
    }
    const auto& src = src_it->second;
    if (src.width <= 0 || src.height <= 0) {
        return;
    }
    // Select the colour attachment to sample (MRT / G-buffer targets expose
    // several sampleable textures under one target). Out-of-range indexes
    // clamp to the last attachment so a mis-set consumer still draws.
    std::size_t attachment_index = 0;
    if (attachment > 0) {
        attachment_index = static_cast<std::size_t>(attachment);
        if (attachment_index >= src.color_views.size()) {
            // Sampling a non-existent attachment is a wiring mistake: clamp so
            // the pass still draws, but say so instead of silently substituting
            // a different texture.
            reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                          vine::graphics::DiagnosticCategory::ChannelIgnored,
                          formatDiagnostic(u8"drawScreenTexture: attachment %d is out of range (%zu available); "
                                           u8"sampling the last attachment",
                                           attachment, src.color_views.size()));
            attachment_index = src.color_views.size() - 1;
        }
    }
    const auto source_view = src.color_views[attachment_index];

    // PiP (screen) views are drawn into the CURRENT target (setRenderTarget;
    // nullptr = the window), so their slots live in that target's entry: a
    // pass can composite a sampled source into an off-screen target, enabling
    // post-processing chains (A -> B -> window). The slot is owned by the pass
    // drawing it (SlotKey); the sampled source + attachment are slot
    // ATTRIBUTES re-checked every frame, so a pass that switches its input (or
    // the attachment it reads of an MRT source) is rebuilt instead of silently
    // sampling the previous texture.
    // The destination is the SCOPE's target (setRenderTarget, nullptr = the
    // window): read, not consumed, so every draw call of the pass agrees on it.
    vine::graphics::RenderTarget* dest = impl->request.target;
    // A source == destination feedback loop would sample the very attachments
    // this pass writes. Reject it with a diagnostic (a ping-pong pair of
    // targets is the standard way to build a feedback chain).
    if (dest == source) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"drawScreenTexture: source == destination (feedback loop): the pass draws nothing");
        return;
    }
    auto& dest_entry = impl->entryFor(dest);
    if (dest != nullptr) {
        // Writing into an off-screen target: (re)build its graph to its size.
        if (dest->colorCount() <= 0 || dest->width() <= 0 || dest->height() <= 0) {
            reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                          u8"drawScreenTexture: destination target has no usable colour attachment: the pass draws nothing");
            return;
        }
        if (!dest_entry.attachments_built || dest_entry.width != dest->width() || dest_entry.height != dest->height()) {
            buildOffscreenTarget(dest);
            if (!dest_entry.attachments_built) {
                return;
            }
        }
    }
    else if (dest_entry.graph == nullptr) {
        return; // window graph not created yet
    }
    const int surf_w = (dest == nullptr) ? static_cast<int>(impl->window->extent2D().width) : dest_entry.width;
    const int surf_h = (dest == nullptr) ? static_cast<int>(impl->window->extent2D().height) : dest_entry.height;

    // The pass owns its slot under this destination; if it drew elsewhere
    // before (its render target changed), drop that stale slot so it stops
    // compositing there.
    retargetPass(impl->request.pass, dest);

    // Pass-scoped identity when the engine opened a pass scope (the normal
    // path); the historical (source, attachment) identity otherwise, so a
    // direct driver that draws several PiPs in one frame stays distinct.
    const SlotKey key = (impl->request.pass != nullptr)
                            ? SlotKey::ownerPass(impl->request.pass)
                            : SlotKey::sampledTarget(source, static_cast<int>(attachment_index));

    // The graph this pass records into: the window session's shared swapchain
    // graph, or this pass' own off-screen graph (§28).
    const auto dest_graph = passGraph(dest, key);

    // Drop a stale slot when the sampled source / attachment changed, or the
    // sampled target OR the destination was resized (the sampled colour view /
    // the baked viewport was rebuilt).
    {
        const auto old = dest_entry.screen_slots.find(key);
        if (old != dest_entry.screen_slots.end() && old->second.ready &&
            (old->second.source_target != source ||
             old->second.attachment != static_cast<int>(attachment_index) ||
             old->second.source_w != src.width || old->second.source_h != src.height ||
             old->second.dest_w != surf_w || old->second.dest_h != surf_h)) {
            if (dest_graph != nullptr) {
                removeGraphChild(dest_graph.get(), old->second.view);
            }
            dest_entry.screen_slots.erase(old);
        }
    }

    auto& slot = dest_entry.screen_slots[key];

    // Destination rectangle: the pass' sub-viewport, else the full surface.
    int req_x = 0, req_y = 0, req_w = surf_w, req_h = surf_h;
    if (viewport && viewport->width > 0 && viewport->height > 0) {
        req_x = viewport->x;
        req_y = viewport->y;
        req_w = viewport->width;
        req_h = viewport->height;
    }

    int rect_x = req_x, rect_y = req_y, rect_w = req_w, rect_h = req_h;
    if (rect_x < 0 || rect_y < 0 || rect_w > surf_w || rect_h > surf_h || rect_x + rect_w > surf_w || rect_y + rect_h > surf_h) {
        // The requested rect does not fit the surface (e.g. an anchor computed
        // before the surface size was known): auto-anchor bottom-right inside
        // the surface, keeping the (16:9) size within half of it.
        int w = req_w;
        int h = req_h;
        if (w > surf_w / 2) {
            w = surf_w / 2;
            h = static_cast<int>(w * 9 / 16);
        }
        if (h > surf_h / 2) {
            h = surf_h / 2;
            w = static_cast<int>(h * 16 / 9);
        }
        const int margin = 8;
        rect_x           = surf_w - w - margin;
        rect_y           = surf_h - h - margin;
        rect_w           = w;
        rect_h           = h;
    }

    if (!slot.ready) {
        // Capture the pass's explicit order (announced by the engine before
        // this pass) so the view stacks among the target's slot views at its
        // pipeline position: a full-screen present at a low order draws
        // beneath later HUD slots, while a small PiP at a high order stays on
        // top of them (the INT_MAX default keeps a legacy-created PiP last).
        slot.order = impl->request.order;
        slot.source_target = source;
        slot.attachment    = static_cast<int>(attachment_index);
        slot.source_w    = src.width;
        slot.source_h    = src.height;
        slot.dest_w      = surf_w;
        slot.dest_h      = surf_h;
        slot.source_view = source_view;

        // Full-screen textured triangle sampling the off-screen colour
        // attachment, drawn as another View of the DESTINATION target's render
        // graph (like overlays) so the sub-viewport clips the
        // picture-in-picture rectangle.
        const VkExtent2D surface{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) };
        ProgramNodeFailure screen_failure = ProgramNodeFailure::None;
        auto content = makeScreenTextureNode(source_view, surface, &screen_failure);
        if (content == nullptr) {
            reportFailure(vine::graphics::DiagnosticSeverity::Error,
                          vine::graphics::DiagnosticCategory::CompileFailed,
                          screen_failure == ProgramNodeFailure::NoCompiler
                              ? u8"screen pass (PiP) needs the runtime GLSL compiler, which is unavailable: the pass draws nothing"
                              : u8"screen pass (PiP) shader failed to compile: the pass draws nothing");
            dest_entry.screen_slots.erase(key);
            return;
        }
        bool overlay_compile_failed = false;
        auto view = makeCompiledOverlayView(*impl->viewer, dest_graph.get(), content,
                                            rect_x, rect_y, rect_w, rect_h,
                                            /*front*/ false, "screen pass",
                                            &overlay_compile_failed);
        if (view == nullptr) {
            if (overlay_compile_failed) {
                reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                              vine::graphics::DiagnosticCategory::CompileFailed,
                              u8"screen pass (PiP) view failed to compile; retrying with a full compile");
            }
            dest_entry.screen_slots.erase(key);
            return;
        }
        slot.camera        = view->camera;
        slot.view          = view;
        slot.ready         = true;
        // Position the view by its explicit order; the compile above already
        // ran against this target's render pass, so only the record order
        // changes.
        placeViewByOrder(dest_graph, dest, view, slot.order);
        std::fprintf(stderr, "[VsgRenderer] EXPERIMENTAL screen PiP %dx%d (att %zu) -> %s %d,%d %dx%d attached\n", src.width, src.height, attachment_index, dest == nullptr ? "window" : "offscreen", rect_x, rect_y, rect_w, rect_h);
        if (dest != nullptr) {
            // A new sampling edge appeared under an off-screen destination:
            // re-order the command graph so this consumer records after every
            // target it samples (source == dest is rejected above, so this
            // cannot feed the producer back on itself).
            reconcileOffscreenOrder();
        }
    }

    if (slot.ready && slot.detached) {
        // Re-attach a slot retired while its pass was inactive (see
        // retireInactivePassSlots): its node and pipeline were kept, only the
        // view was detached from the graph.
        placeViewByOrder(dest_graph, dest, slot.view, slot.order);
        slot.detached = false;
    }

    // Follow the requested sub-viewport each frame (dynamic viewport + scissor).
    slot.camera->viewportState = ::vsg::ViewportState::create(rect_x, rect_y, static_cast<uint32_t>(rect_w), static_cast<uint32_t>(rect_h));
}

void VsgRenderer::drawScreenProgram(vine::graphics::RenderTarget*              source,
                                    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                    vine::raw_ptr<const vine::graphics::Camera>        camera)
{
    if (!impl->initialized || impl->viewer == nullptr || impl->window == nullptr || source == nullptr || program == nullptr) {
        return;
    }

    // Consume the sub-viewport queued by setViewport() (the pass's rectangle).
    const std::optional<vine::graphics::Viewport> viewport = takeRequestViewport();

    auto src_it = impl->targets.find(source);
    if (src_it == impl->targets.end() || src_it->second.color_views.empty()) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"drawScreenProgram: source target has no colour attachment: the pass draws nothing");
        return;
    }
    const auto& src = src_it->second;
    if (src.width <= 0 || src.height <= 0) {
        return;
    }

    // Fullscreen-program views are drawn into the CURRENT target
    // (setRenderTarget; nullptr = the window), so their slots live in that
    // target's entry: deferred / post passes can write into an off-screen
    // target as well as the window. A scope attribute: read, not consumed.
    vine::graphics::RenderTarget* dest = impl->request.target;
    // A source == destination feedback loop would sample the very attachments
    // this pass writes. Reject it (a ping-pong pair of targets is the standard
    // way to build a feedback chain).
    if (dest == source) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"drawScreenProgram: source == destination (feedback loop): the pass draws nothing");
        return;
    }
    auto& dest_entry = impl->entryFor(dest);
    if (dest != nullptr) {
        if (dest->colorCount() <= 0 || dest->width() <= 0 || dest->height() <= 0) {
            reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                          u8"drawScreenProgram: destination target has no usable colour attachment: the pass draws nothing");
            return;
        }
        if (!dest_entry.attachments_built || dest_entry.width != dest->width() || dest_entry.height != dest->height()) {
            buildOffscreenTarget(dest);
            if (!dest_entry.attachments_built) {
                return;
            }
        }
    }
    else if (dest_entry.graph == nullptr) {
        return; // window graph not created yet
    }
    const int surf_w = (dest == nullptr) ? static_cast<int>(impl->window->extent2D().width) : dest_entry.width;
    const int surf_h = (dest == nullptr) ? static_cast<int>(impl->window->extent2D().height) : dest_entry.height;

    // The pass owns its slot under this destination; a pass that drew
    // elsewhere before (its render target changed) drops that stale slot here.
    retargetPass(impl->request.pass, dest);
    // Pass-scoped identity when a pass scope is open (normal path), else the
    // historical per-source identity used by direct drivers.
    const SlotKey slot_key = (impl->request.pass != nullptr)
                                 ? SlotKey::ownerPass(impl->request.pass)
                                 : SlotKey::sampledTarget(source);
    // The graph this pass records into: the window session's shared swapchain
    // graph, or this pass' own off-screen graph (§28).
    const auto dest_graph = passGraph(dest, slot_key);
    auto& slot = dest_entry.program_slots[slot_key];

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

    // (Re)build the retained slot when it is missing, the sampled source
    // changed (or was resized: its colour views were rebuilt), the DESTINATION
    // was resized, or the program changed.
    const std::uint64_t program_revision = program->revision();
    const bool stale = !slot.ready || slot.source_target != source ||
                       slot.source_w != src.width || slot.source_h != src.height ||
                       slot.dest_w != surf_w || slot.dest_h != surf_h ||
                       slot.program.get() != program || slot.program_revision != program_revision;
    if (stale) {
        if (dest_graph != nullptr) {
            removeGraphChild(dest_graph.get(), slot.view);
        }
        slot = Impl::ProgramSlot{};
        // Capture the pass's explicit order (announced by the engine before
        // this pass) so the fullscreen view stacks at its pipeline position
        // among the target's content slots (e.g. between an opaque depth pass
        // and a forward transparent pass) instead of always drawing first.
        slot.order = impl->request.order;
        slot.push_data = ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(LightPushBlock)));
        const VkExtent2D surface{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) };
        ProgramNodeFailure program_failure = ProgramNodeFailure::None;
        auto node = makeFullscreenProgramNode(program, src.color_views, source->depthPromotion() ? src.depth_view : ::vsg::ref_ptr<::vsg::ImageView>(), surface, slot.push_data, &program_failure);
        if (node == nullptr) {
            const vine::String why =
                program_failure == ProgramNodeFailure::NoCompiler
                    ? u8"fullscreen program needs the runtime GLSL compiler, which is unavailable"
                    : program_failure == ProgramNodeFailure::NoFragmentStage
                          ? u8"fullscreen program has no fragment stage"
                          : u8"fullscreen program shader failed to compile";
            reportFailure(vine::graphics::DiagnosticSeverity::Error,
                          vine::graphics::DiagnosticCategory::CompileFailed,
                          why + vine::String(u8": the pass draws nothing"));
            dest_entry.program_slots.erase(slot_key);
            return;
        }
        slot.source_target = source;
        slot.source_w = src.width;
        slot.source_h = src.height;
        slot.dest_w   = surf_w;
        slot.dest_h   = surf_h;
        slot.program          = vine::intrusive_ptr<const vine::graphics::ShaderProgram>(program);
        slot.program_revision = program_revision;
        slot.node             = node;

        // Create + compile the fullscreen view against this target's render
        // pass (inserted provisionally at the front so the compile sees it),
        // then move it to its explicit-order position below.
        bool overlay_compile_failed = false;
        auto view = makeCompiledOverlayView(*impl->viewer, dest_graph.get(), node,
                                            rect_x, rect_y, rect_w, rect_h,
                                            /*front*/ true, "fullscreen program",
                                            &overlay_compile_failed);
        if (view == nullptr) {
            if (overlay_compile_failed) {
                reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                              vine::graphics::DiagnosticCategory::CompileFailed,
                              u8"fullscreen program view failed to compile; retrying with a full compile");
            }
            dest_entry.program_slots.erase(slot_key);
            return;
        }
        slot.camera        = view->camera;
        slot.view          = view;
        slot.ready         = true;
        ++impl->program_slot_build_count;
        placeViewByOrder(dest_graph, dest, view, slot.order);
        std::fprintf(stderr, "[VsgRenderer] EXPERIMENTAL deferred fullscreen program %dx%d -> %s %d,%d %dx%d attached\n", src.width, src.height, dest == nullptr ? "window" : "offscreen", rect_x, rect_y, rect_w, rect_h);
        if (dest != nullptr) {
            // New sampling edges (this program samples every colour attachment
            // of source) appeared under an off-screen destination: re-order so
            // the consumer records after its producer (source == dest is
            // rejected above, so this cannot feed back on itself).
            reconcileOffscreenOrder();
        }
    }

    // Take the lights announced for this draw call (from the pass's content
    // scene) and push view-space light parameters before record. An empty list
    // seeds a small default ambient (see fillLightPushBlock) so a fullscreen
    // program pass that carries no lights still shades its albedo instead of
    // rendering black.
    std::vector<const vine::graphics::Light*> lights = impl->request.takeLights();
    ++impl->request.draws;
    LightPushBlock block{};
    fillLightPushBlock(camera, lights, block);
    if (slot.push_data != nullptr && slot.push_data->dataSize() >= sizeof(block)) {
        std::memcpy(slot.push_data->dataPointer(), &block, sizeof(block));
    }

    if (slot.ready && slot.detached) {
        // Re-attach a slot retired while its pass was inactive (see
        // retireInactivePassSlots): its node and pipeline were kept.
        placeViewByOrder(dest_graph, dest, slot.view, slot.order);
        slot.detached = false;
    }

    // Follow the requested sub-viewport each frame.
    slot.camera->viewportState = ::vsg::ViewportState::create(rect_x, rect_y, static_cast<uint32_t>(rect_w), static_cast<uint32_t>(rect_h));
}

V_VSG_NS_END
