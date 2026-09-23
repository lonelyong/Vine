#include <vine/vsg/api/VsgBackend.hpp>

#include <array>
#include <string>
#include <utility>

#include <vsg/app/CommandGraph.h>

#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/api/SessionContent.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/api/WindowTarget.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/Observe.hpp>

V_VSG_NS_BEGIN

namespace
{

/// @brief Builds a `vine::String` from an ASCII sentence (the house spelling for UTF-8 bytes).
vine::String asString(const std::string& text)
{
    return vine::String(reinterpret_cast<const char8_t*>(text.c_str()));
}

/// @brief The entry points this facade cannot serve yet, one slot per call (see the class note).
enum Unserved : std::size_t
{
    kUnservedPass = 0U,
    kUnservedOrder,
    kUnservedViewport,
    kUnservedLights,
    kUnservedInputs,
    kUnservedDepth,
    kUnservedClear,
    kUnservedDraw,
    kUnservedScreenDraw,
    kUnservedReleasePass,
    kUnservedTarget,
    kUnservedReleaseTarget,
    kUnservedResize,
    kUnservedColourRead,
    kUnservedDepthRead,
    kUnservedNoSession,
    kUnservedCount,
};

/// @brief The names the unserved reports use, in the enum's order.
const char* const kUnservedNames[kUnservedCount]{
    "beginPass()/endPass()", "setPassOrder()", "setViewport()", "setLights()", "setPassInputs()",
    "setDepthMode()", "setClearPolicy()", "render()", "drawScreenProgram()", "releasePass()",
    "setRenderTarget() (off-screen)", "releaseRenderTarget()", "resize() (live session)",
    "readColorBuffer()", "readDepthBuffer()", "a frame without a session",
};

}  // namespace

struct VsgBackend::Data
{
    core::Diagnostics diagnostics;  ///< The core's one route, forwarded to the SDK's (see the constructor).
    core::FrameArena  arena{ 64 * 1024 };
    core::Observe     observe;
    core::FrameRecorder recorder{ arena, diagnostics, observe };
    core::FrameCompiler compiler{ arena, diagnostics, observe };
    api::Session        session;
    VsgExecutor         executor{ diagnostics };

    void* host_handle{nullptr};            ///< The handle the host announced (nullptr = a window of our own).
    int   announced_width{640};            ///< What the next initialize() creates its window at.
    int   announced_height{360};           ///< What the next initialize() creates its window at.
    vine::intrusive_ptr<const vine::graphics::ShaderProgram> default_program;  ///< See the header.

    std::array<core::ReportOnce, kUnservedCount> unserved_reports{};  ///< One episode per entry point.
    std::vector<core::TargetFacts>               facts;              ///< This frame's target table (borrowed rows).
    const core::CompiledFrame*                   frame{nullptr};     ///< This frame's plan, between end and swap.
};

VsgBackend::VsgBackend() : d(std::make_unique<Data>())
{
    // The core's one diagnostic route feeds the SDK's: the session and the pieces report through it, and a
    // host that installed a sink - or counts - sees those reports without this layer re-reporting anything.
    d->diagnostics.setSink([this](const vine::graphics::RenderDiagnostic& diagnostic) {
        reportDiagnostic(diagnostic.severity, diagnostic.category, diagnostic.message);
    });
}

VsgBackend::~VsgBackend() = default;

bool VsgBackend::initialize()
{
    // What the host announced, as the session's options: the size the window is created at (the surface
    // belongs to this backend when it owns the window, so the announcement is applied HERE - see the SDK's
    // resize() note) and the handle it may have adopted (nullptr = a window of our own).
    api::SessionOptions options;
    options.width         = d->announced_width;
    options.height        = d->announced_height;
    options.native_handle = d->host_handle;

    if (!d->session.initialize(options, d->diagnostics))
    {
        // The session reported why (InitFailed), and the sink forwarded it: false plus a reason is the
        // SDK's contract for a failed initialize(), and a half-built session is this layer's to clean up
        // (the session does its own, see its note).
        return false;
    }

    // The pieces the frame drive needs, registered where they are created: the window is the default
    // framebuffer's target, and the executor is the one that records into it.
    if (WindowTarget* window = detail::SessionContentAccess::windowTarget(d->session))
    {
        d->executor.setWindow(window);
    }
    d->frame = nullptr;
    return true;
}

void VsgBackend::shutdown()
{
    d->frame = nullptr;
    d->facts.clear();
    d->executor.setWindow(nullptr);
    d->executor.clearTargets();
    d->session.shutdown();
}

void VsgBackend::beginFrame()
{
    if (!d->session.initialized())
    {
        reportUnserved(kUnservedNoSession);
        return;
    }
    // The session mints the frame's token and advances the viewer's clock; the recorder opens the plan for
    // the same token, so the description and the session's frame cannot disagree about which frame it is.
    const core::FrameToken token = d->session.beginFrame();
    (void)d->recorder.beginFrame(token);  // a protocol violation is the recorder's to report (see its note)
}

void VsgBackend::endFrame()
{
    if (!d->session.initialized())
    {
        return;  // beginFrame() already said so, once
    }
    (void)d->recorder.endFrame();
    (void)d->recorder.swapBuffers();  // closes the frame's books: endFrame() alone only leaves the pass scope

    // The facts the plan is compiled against: the window as the default framebuffer, answered by the target
    // itself (see WindowTarget::facts) - off-screen targets join this table in the slice that serves them.
    d->facts.clear();
    if (WindowTarget* window = detail::SessionContentAccess::windowTarget(d->session))
    {
        d->facts.push_back(window->facts());
    }
    d->frame = &d->compiler.compile(d->recorder.description(), core::FrameFacts{ d->facts });
}

void VsgBackend::swapBuffers()
{
    if (d->frame == nullptr || !d->session.initialized())
    {
        return;  // no frame was closed: beginFrame/endFrame said what was missing
    }

    if (d->frame->passes.empty())
    {
        // NOTHING TO RECORD, and the frame is still submitted and presented - the session's own rule (an
        // acquired image has to be handed back), and the graph it submits is the one the session created
        // with its window's render graph in it. That graph is not decoration: it is what moves the acquired
        // image out of UNDEFINED, and a present of an image still in UNDEFINED is a VUID (measured: an EMPTY
        // command graph presents an image nobody rendered into). The plan with passes below builds its own
        // graph instead, because the executor places each pass graph in it (the window's included, see
        // recordWindow).
        (void)d->executor.commit(*d->frame, d->session);
        d->frame = nullptr;
        return;
    }

    const ::vsg::ref_ptr<::vsg::CommandGraph> graph = detail::SessionContentAccess::makeFrameGraph(d->session);
    if (graph == nullptr)
    {
        d->frame = nullptr;
        return;
    }

    // The frame drive's own order, once (see VsgExecutor): what the plan answered for its targets becomes
    // real, the plan is recorded into this frame's graph, and the graph is the session's - which is the
    // call that submits and presents, and the one whose false answer means the writes never happened.
    (void)d->executor.applyTargetPlans(*d->frame, d->facts, d->session.timeline(), d->session.retirement());
    (void)d->executor.record(*d->frame, graph);
    if (detail::SessionContentAccess::assignFrameGraphs(d->session, ::vsg::CommandGraphs{ graph }))
    {
        (void)d->executor.commit(*d->frame, d->session);  // it reports its own failure and marks what it wrote
    }
    d->frame = nullptr;
}

void VsgBackend::setWindowHandle(void* native_handle)
{
    // A session decision: the next initialize() adopts the handle (or moves an established session onto it -
    // see core::planSessionMove, which the session applies).
    d->host_handle = native_handle;
}

void* VsgBackend::nativeHandle() const
{
    // The SDK's question is "which surface am I on": a host surface was adopted, a window of our own is not
    // the host's to name - and before initialize() there is nothing to name.
    return d->session.initialized() ? d->host_handle : nullptr;
}

void VsgBackend::resize(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;  // not a size a surface can have: nothing to announce, and nothing to apply
    }
    d->announced_width  = width;
    d->announced_height = height;
    if (d->session.initialized())
    {
        // This backend OWNS its window (when it created one), so applying the announcement to a LIVE surface
        // is this layer's job - and it is not served yet. Saying so once beats an announcement that looks
        // applied while the surface keeps its old size (the next initialize() creates the window at it).
        reportUnserved(kUnservedResize);
    }
}

void VsgBackend::setDefaultContentProgram(vine::intrusive_ptr<const vine::graphics::ShaderProgram> program)
{
    d->default_program = std::move(program);
    // A live switch re-bakes CONTENT shading only (the SDK's contract) - and that baking lands with the
    // content slices, which are also the ones that read this program.
}

bool VsgBackend::supportsRenderTargets()
{
    // The off-screen half is a later slice, and declining HERE is how the engine knows before it stages
    // off-screen work (see the SDK's note): accepting targets and drawing them nowhere would be a wrong
    // picture instead of a slow one.
    return false;
}

void VsgBackend::setRenderTarget(vine::raw_ptr<vine::graphics::RenderTarget> target)
{
    if (target != nullptr)
    {
        reportUnserved(kUnservedTarget);
    }
    // nullptr is the default framebuffer: the window the frame presents, which the drive already uses.
}

void VsgBackend::beginPass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    (void)pass;
    reportUnserved(kUnservedPass);
}

void VsgBackend::endPass()
{
    reportUnserved(kUnservedPass);
}

void VsgBackend::setPassOrder(int order)
{
    (void)order;
    reportUnserved(kUnservedOrder);
}

void VsgBackend::setViewport(int x, int y, int width, int height)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    reportUnserved(kUnservedViewport);
}

void VsgBackend::setLights(const std::vector<vine::raw_ptr<const vine::graphics::Light>>& lights)
{
    (void)lights;
    reportUnserved(kUnservedLights);
}

void VsgBackend::setPassInputs(const std::vector<vine::raw_ptr<vine::graphics::RenderTarget>>& inputs)
{
    (void)inputs;
    reportUnserved(kUnservedInputs);
}

void VsgBackend::setDepthMode(vine::graphics::DepthMode mode)
{
    (void)mode;
    reportUnserved(kUnservedDepth);
}

void VsgBackend::setClearPolicy(const vine::graphics::ClearPolicy& policy)
{
    (void)policy;
    reportUnserved(kUnservedClear);
}

void VsgBackend::render(const std::vector<vine::graphics::RenderCommand>& commands,
                        const vine::graphics::Camera*                      camera)
{
    (void)commands;
    (void)camera;
    reportUnserved(kUnservedDraw);
}

void VsgBackend::drawScreenProgram(vine::graphics::RenderTarget*                     source,
                                   vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                   vine::raw_ptr<const vine::graphics::Camera>        camera)
{
    (void)source;
    (void)program;
    (void)camera;
    reportUnserved(kUnservedScreenDraw);
}

void VsgBackend::releasePass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    (void)pass;
    reportUnserved(kUnservedReleasePass);
}

void VsgBackend::releaseRenderTarget(vine::graphics::RenderTarget* target)
{
    (void)target;
    reportUnserved(kUnservedReleaseTarget);
}

bool VsgBackend::readColorBuffer(const vine::graphics::RenderTarget* target, int attachment,
                                 std::vector<std::uint8_t>& outPixels, vine::graphics::ReadbackResult* why)
{
    (void)target;
    (void)attachment;
    (void)outPixels;
    if (why != nullptr)
    {
        *why = vine::graphics::ReadbackResult::Unsupported;
    }
    reportUnserved(kUnservedColourRead);
    return false;
}

bool VsgBackend::readDepthBuffer(const vine::graphics::RenderTarget* target, std::vector<float>& outDepths,
                                 vine::graphics::ReadbackResult* why)
{
    (void)target;
    (void)outDepths;
    if (why != nullptr)
    {
        *why = vine::graphics::ReadbackResult::Unsupported;
    }
    reportUnserved(kUnservedDepthRead);
    return false;
}

bool VsgBackend::initialized() const noexcept
{
    return d->session.initialized();
}

std::uint64_t VsgBackend::framesPresented() const noexcept
{
    return d->session.framesPresented();
}

std::size_t VsgBackend::deviceWaits() const noexcept
{
    return d->session.deviceWaits();
}

void VsgBackend::reportUnserved(std::size_t slot) noexcept
{
    if (slot >= kUnservedCount || !d->unserved_reports[slot].shouldReport())
    {
        return;
    }
    reportDiagnostic(vine::graphics::DiagnosticSeverity::Warning,
                     vine::graphics::DiagnosticCategory::UnsupportedRequest,
                     asString(std::string(kUnservedNames[slot]) +
                              " is not served by this backend yet: it drives empty frames today, and the "
                              "SDK-facing drawing path lands in the next slices (see "
                              ".ai/design/vsg-reimplementation.md)"));
}

V_VSG_NS_END
