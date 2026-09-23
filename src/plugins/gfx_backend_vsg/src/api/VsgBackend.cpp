#include <vine/vsg/api/VsgBackend.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vsg/app/CommandGraph.h>

#include <vine/graphics/RenderCommand.hpp>
#include <vine/vsg/api/BackendContent.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentAssembly.hpp>
#include <vine/vsg/api/ContentStore.hpp>
#include <vine/vsg/api/MaterialImages.hpp>
#include <vine/vsg/api/PassRegistry.hpp>
#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/api/SessionContent.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/api/ViewBlock.hpp>
#include <vine/vsg/api/WindowTarget.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/Observe.hpp>
#include <vine/vsg/core/VariantPool.hpp>

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
    kUnservedTarget = 0U,
    kUnservedInputs,
    kUnservedScreenDraw,
    kUnservedReleaseTarget,
    kUnservedResize,
    kUnservedColourRead,
    kUnservedDepthRead,
    kUnservedNoSession,
    kUnservedDraw,
    kUnservedCount,
};

/// @brief The names the unserved reports use, in the enum's order.
const char* const kUnservedNames[kUnservedCount]{
    "setRenderTarget() (off-screen)", "setPassInputs() (declared inputs)", "drawScreenProgram()",
    "releaseRenderTarget()", "resize() (live session)", "readColorBuffer()", "readDepthBuffer()",
    "a frame without a session", "render() (the content world is not up)",
};

/// @brief Tracks @p program into @p store, when both exist (see setDefaultContentProgram).
void trackProgram(ContentStore* store, const vine::intrusive_ptr<const vine::graphics::ShaderProgram>& program)
{
    if (store == nullptr || program == nullptr)
    {
        return;
    }
    // The SDK hands the default program as a CONST handle while the engine keeps the object mutable, and
    // the store's own handle is mutable (it owns a share of what it describes, see its note): the cast is
    // the bridge between the two views - and nothing in the store writes through it, its tables only READ
    // the object they describe.
    store->track(vine::intrusive_ptr<vine::graphics::ShaderProgram>(
        const_cast<vine::graphics::ShaderProgram*>(program.get())));
}

/// @brief The content of every pass this slice can serve, one packet per pass that has any (see swapBuffers).
///
/// The WINDOW is the only target served today: a pass whose target is an off-screen one produces no packet -
/// the executor reports the pass instead of drawing it somewhere else, and content built for it would be
/// content nothing records. A pass with no draws produces no packet either: its clear is the plan's own and
/// records without help.
std::span<const PassContent> recordContent(ContentAssembly& assembly, WindowTarget& window, api::Session& session,
                                           const core::CompiledFrame& frame,
                                           std::vector<InputImages>&  input_scratch,
                                           std::vector<PassContent>&  packets)
{
    packets.clear();
    (void)assembly.beginFrame(frame, session.timeline(), session.retirement());
    for (const core::CompiledPass& pass : frame.passes)
    {
        if (pass.draws.empty())
        {
            continue;
        }
        if (frame.targets[pass.target_index].target != nullptr)
        {
            continue;
        }
        // One entry per DECLARED input, empty when nothing can offer one: the content recorder reads one
        // entry per declaration, so a shorter span would read past its end - and "nothing offered" is the
        // fact it reports, which a missing entry would not be.
        input_scratch.assign(pass.inputs.size(), InputImages{});
        // The view block is the PASS' (the content layer's contract), built from the first drawing call's
        // camera - the engine's passes draw through one camera per scope, so the first is the pass'.
        const vine::graphics::VineViewBlock view_block =
            buildViewBlock(pass.draws[0].camera, session.frameSeconds(), window.width(), window.height());
        ::vsg::ref_ptr<::vsg::Node> content;
        (void)assembly.record(pass, window.shape().compatibility(), input_scratch,
                              std::as_bytes(std::span<const vine::graphics::VineViewBlock>(&view_block, 1U)),
                              content);
        packets.push_back(PassContent{ pass.pass, std::move(content) });
    }
    return packets;
}

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
    PassRegistry        passes;  ///< The SDK's pass objects, as the numbers the plan carries (see the header).

    // The content world: built with the session because its pieces belong to the session's device (see
    // initialize), torn down before the session because the device goes with it. The store is device-free,
    // but it is what the assembly reads its tables from - so the set lives and dies together.
    core::VariantPool                pool{};
    std::shared_ptr<BlockStorage>    storage{};
    std::shared_ptr<MaterialImages>  images{};
    std::unique_ptr<ContentStore>    store{};
    std::unique_ptr<ContentAssembly> assembly{};

    void* host_handle{nullptr};            ///< The handle the host announced (nullptr = a window of our own).
    int   announced_width{640};            ///< What the next initialize() creates its window at.
    int   announced_height{360};           ///< What the next initialize() creates its window at.
    vine::intrusive_ptr<const vine::graphics::ShaderProgram> default_program;  ///< See the header.

    std::array<core::ReportOnce, kUnservedCount> unserved_reports{};  ///< One episode per entry point.
    std::vector<core::TargetFacts>            facts;          ///< This frame's target table (borrowed rows).
    std::vector<const vine::graphics::Light*> light_scratch;  ///< Reused by setLights (no per-call allocation).
    std::vector<InputImages>                  input_scratch;  ///< Reused per content pass (see recordContent).
    std::vector<PassContent>                  packets;        ///< Reused per frame (see recordContent).
    const core::CompiledFrame*                frame{nullptr}; ///< This frame's plan, between end and swap.
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

    // The content world, on the session's device (see the header): without it the drawing half has nowhere
    // to record, and render() says so instead of collecting draws nothing can render.
    if (const ::vsg::ref_ptr<::vsg::Device> device = detail::SessionContentAccess::device(d->session))
    {
        d->storage = BlockStorage::create(device, BlockStorage::Layout{});
        d->images  = MaterialImages::create();
        if (d->storage != nullptr && d->images != nullptr)
        {
            d->store    = std::make_unique<ContentStore>();
            d->assembly = std::make_unique<ContentAssembly>(*d->store, device, d->pool, *d->storage, *d->images,
                                                            d->diagnostics);
        }
    }
    // The default program was announced before the session came up (the engine's own order), so it is
    // tracked now - the tables must answer for it before any plan names it.
    trackProgram(d->store.get(), d->default_program);
    return true;
}

void VsgBackend::shutdown()
{
    d->frame = nullptr;
    d->facts.clear();
    d->packets.clear();
    d->executor.setWindow(nullptr);
    d->executor.clearTargets();
    // The content world FIRST: its GPU objects (and the sets the assembly built) belong to the session's
    // device, which the session drops on the way down.
    d->assembly.reset();
    d->store.reset();
    d->images.reset();
    d->storage.reset();
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
    // real, the frame's content is assembled and recorded into the passes this slice can serve, the plan
    // is recorded into this frame's graph, and the graph is the session's - which is the call that submits
    // and presents, and the one whose false answer means the writes never happened.
    (void)d->executor.applyTargetPlans(*d->frame, d->facts, d->session.timeline(), d->session.retirement());

    std::span<const PassContent> packets{};
    if (WindowTarget* window = detail::SessionContentAccess::windowTarget(d->session);
        d->assembly != nullptr && window != nullptr)
    {
        packets = recordContent(*d->assembly, *window, d->session, *d->frame, d->input_scratch, d->packets);
    }

    (void)d->executor.record(*d->frame, graph, packets);
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
    // The plan needs it now - content without a program of its own is drawn with this one, and the
    // compiler resolves that before the plan exists - and the store must know it before any frame names
    // it, because the tables are what the recording reads.
    (void)d->recorder.setDefaultContentProgram(d->default_program.get());
    trackProgram(d->store.get(), d->default_program);
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
        // Declining is the state the SDK's supportsRenderTargets() note describes - the engine is told the
        // backend cannot do off-screen targets - and the announcement is DROPPED here: the plan must not
        // name a target nothing can record, or the pass would draw into the window while the host believes
        // it draws elsewhere.
        reportUnserved(kUnservedTarget);
        return;
    }
    if (d->recorder.inPass())
    {
        (void)d->recorder.setRenderTarget(nullptr);  // the default framebuffer, spelled as the plan does
    }
}

void VsgBackend::beginPass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    if (pass == nullptr)
    {
        return;  // no pass announced: whatever needed a scope is refused and reported by the recorder
    }
    const core::PassId id = d->passes.adopt(pass);
    if (d->recorder.inFrame())
    {
        // Refusing (a nested scope) is the protocol's to judge and report; this layer has no rules of its
        // own (see the header).
        (void)d->recorder.beginPass(id);
    }
    // With no frame open nothing is collected: that is the engine's pre-frame warm-up, whose pass scopes
    // are inert by contract (see the SDK's beginPass note) - the identity above is what survives it.
}

void VsgBackend::endPass()
{
    if (d->recorder.inFrame())
    {
        (void)d->recorder.endPass();  // a scope-less endPass is the protocol's to report
    }
}

void VsgBackend::setPassOrder(int order)
{
    if (d->recorder.inPass())
    {
        (void)d->recorder.setPassOrder(order);
    }
}

void VsgBackend::setViewport(int x, int y, int width, int height)
{
    if (d->recorder.inPass())
    {
        (void)d->recorder.setViewport(x, y, width, height);
    }
}

void VsgBackend::setLights(const std::vector<vine::raw_ptr<const vine::graphics::Light>>& lights)
{
    if (!d->recorder.inPass())
    {
        return;
    }
    // raw_ptr<T> IS T* (see vine/raw_ptr.hpp), so the announcement's pointers are the plan's spelling as
    // they are - only the container has to become a span, and the scratch vector keeps that from being an
    // allocation per pass per frame.
    d->light_scratch.assign(lights.begin(), lights.end());
    (void)d->recorder.setLights(d->light_scratch);
}

void VsgBackend::setPassInputs(const std::vector<vine::raw_ptr<vine::graphics::RenderTarget>>& inputs)
{
    for (const vine::raw_ptr<vine::graphics::RenderTarget>& input : inputs)
    {
        if (input != nullptr)
        {
            // A declared input that produced something is an off-screen target, and those are not served
            // yet. Announcing it would make the plan promise what the recording cannot deliver.
            reportUnserved(kUnservedInputs);
            return;
        }
    }
    // An all-null list says nothing produced anything this frame: announcing it would add "resolved to
    // nothing" entries where the plan's own default (no inputs) is already the same fact.
}

void VsgBackend::setDepthMode(vine::graphics::DepthMode mode)
{
    if (d->recorder.inPass())
    {
        (void)d->recorder.setDepthMode(mode);
    }
}

void VsgBackend::setClearPolicy(const vine::graphics::ClearPolicy& policy)
{
    if (!d->recorder.inPass())
    {
        return;
    }
    // The SDK's policy is bytes plus a depth flag; the plan's is floats plus a depth VALUE. The colour is
    // the bytes as they are (no transfer function is applied here - the convention the tests read the
    // window back with), and depth keeps the plan's own far value because the SDK has no depth value to
    // give: one there would be a second place to get the clip convention wrong (see the SDK's ClearPolicy).
    core::ClearPolicy translated;
    translated.color          = true;  // the SDK only announces a policy for a pass that clears its colour
    translated.color_value[0] = static_cast<float>(policy.color.r) / 255.0F;
    translated.color_value[1] = static_cast<float>(policy.color.g) / 255.0F;
    translated.color_value[2] = static_cast<float>(policy.color.b) / 255.0F;
    translated.color_value[3] = static_cast<float>(policy.color.a) / 255.0F;
    translated.depth          = policy.depth;
    (void)d->recorder.setClearPolicy(translated);
}

void VsgBackend::render(const std::vector<vine::graphics::RenderCommand>& commands,
                        const vine::graphics::Camera*                      camera)
{
    if (!d->recorder.inFrame())
    {
        return;  // the engine's pre-frame warm-up executes pass scopes outside any frame: nothing to collect
    }
    if (d->store == nullptr)
    {
        // The content world could not be built on this session (see initialize): collecting the draws would
        // plan a picture nothing can render, which is the silent black frame this backend refuses to be.
        reportUnserved(kUnservedDraw);
        return;
    }
    // What the commands name becomes the store's live content: the objects are borrowed for the call, and
    // the tables must answer for them when the plan (which copies their identities) is recorded - including
    // after the host has dropped its own handle (the store owns a share, see its note).
    for (const vine::graphics::RenderCommand& command : commands)
    {
        d->store->track(command.geometry);
        d->store->track(command.material);
        d->store->track(command.program);
    }
    (void)d->recorder.render(commands, camera);  // refusing (no scope, a released target) is the protocol's
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
    (void)d->passes.release(pass);
    // Nothing retained is keyed by a pass yet (see the header), so forgetting the identity is the whole of
    // the SDK's "free what the pass owns" here - and it keeps the registry from growing for a host that
    // adds and removes passes.
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
                              " is not served by this backend yet: this slice draws the window's content, "
                              "and what needs an off-screen target, a readback or a live surface resize "
                              "lands in the next slices (see .ai/design/vsg-reimplementation.md)"));
}

namespace detail
{

PassRegistry& BackendContentAccess::passes(VsgBackend& backend) noexcept
{
    return backend.d->passes;
}

ContentAssembly* BackendContentAccess::assembly(VsgBackend& backend) noexcept
{
    return backend.d->assembly.get();
}

VsgExecutor& BackendContentAccess::executor(VsgBackend& backend) noexcept
{
    return backend.d->executor;
}

}  // namespace detail

V_VSG_NS_END
