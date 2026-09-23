#include <vine/vsg/api/VsgBackend.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/state/ImageView.h>
#include <vsg/vk/Device.h>

#include <vine/graphics/RenderCommand.hpp>
#include <vine/vsg/api/BackendContent.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentAssembly.hpp>
#include <vine/vsg/api/ContentStore.hpp>
#include <vine/vsg/api/HostReadback.hpp>
#include <vine/vsg/api/HostTargets.hpp>
#include <vine/vsg/api/MaterialImages.hpp>
#include <vine/vsg/api/PassRegistry.hpp>
#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/api/SessionContent.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/api/ViewBlock.hpp>
#include <vine/vsg/api/WindowTarget.hpp>
#include <vine/vsg/core/AllocationGate.hpp>
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

/// @brief The calls that have nowhere to go, one slot per call (see the class note).
enum Unserved : std::size_t
{
    kUnservedNoSession = 0U,
    kUnservedDraw,
    kUnservedCount,
};

/// @brief The names those reports use, in the enum's order.
const char* const kUnservedNames[kUnservedCount]{
    "a frame without a session",
    "render() (the content world is not up)",
};

/// @brief The readback episode slots, one per entry point (a held target keeps its own, see HostTargets::Entry).
enum ReadbackReport : std::size_t
{
    kReadbackColour = 0U,
    kReadbackDepth,
    kReadbackReportCount,
};

/// @brief Names @p target for a diagnostic sentence: its own name, or a stand-in when it has none.
std::string nameOf(const vine::graphics::RenderTarget* target)
{
    if (target == nullptr)
    {
        return "an unnamed target";
    }
    const vine::String& name = target->name();
    if (name.empty())
    {
        return "an unnamed target";
    }
    return "'" + std::string(reinterpret_cast<const char*>(name.data()), name.size()) + "'";
}

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

/// @brief The content of every pass the frame's targets can be served for, one packet per pass that has any
/// (see swapBuffers).
///
/// WHICH TARGET decides the shape and the extent: a pass drawing into the window is compatible with the
/// window's own shape and reconstructs from the window's extent, and a pass drawing into one of the host's
/// targets is compatible with THAT target's shape - the shape its own render pass and attachments are - and
/// uses its extent. A pass whose target this backend does not hold produces no packet: the executor reports
/// the pass instead of drawing it somewhere else, and content built for it would be content nothing records.
/// A pass with no draws produces no packet either: its clear is the plan's own and records without help.
std::span<const PassContent> recordContent(ContentAssembly& assembly, WindowTarget& window, HostTargets& targets,
                                           api::Session& session, const core::CompiledFrame& frame,
                                           std::vector<InputImages>& input_scratch,
                                           std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>>& input_views,
                                           std::vector<PassContent>& packets)
{
    packets.clear();
    (void)assembly.beginFrame(frame, session.timeline(), session.retirement());
    for (const core::CompiledPass& pass : frame.passes)
    {
        if (pass.draws.empty())
        {
            continue;
        }

        core::RenderPassCompatibility compatibility;
        std::uint32_t                width  = 0;
        std::uint32_t                height = 0;
        if (const core::CompiledTarget& compiled_target = frame.targets[pass.target_index];
            compiled_target.target == nullptr)
        {
            compatibility = window.shape().compatibility();
            width         = window.width();
            height        = window.height();
        }
        else
        {
            const HostTargets::Entry* entry = targets.find(compiled_target.target);
            if (entry == nullptr || entry->target == nullptr)
            {
                continue;
            }
            compatibility = entry->target->shape().compatibility();
            width         = entry->target->width();
            height        = entry->target->height();
        }

        // One entry per DECLARED input, in declaration order: what the input target offers a shader - each of
        // its colour attachments, plus its depth while the plan says that one is sampleable. An input this
        // backend does not hold offers nothing: the content recorder reads one entry per declaration, so a
        // shorter span would read past its end - and "nothing offered" is the fact it reports, which a missing
        // entry would not be.
        input_scratch.assign(pass.inputs.size(), InputImages{});
        input_views.assign(pass.inputs.size(), {});
        for (std::size_t index = 0; index < pass.inputs.size(); ++index)
        {
            const HostTargets::Entry* entry = targets.find(pass.inputs[index].target);
            if (entry == nullptr || entry->target == nullptr)
            {
                continue;
            }
            std::vector<::vsg::ref_ptr<::vsg::ImageView>>& views = input_views[index];
            views.clear();
            const std::uint32_t attachments = entry->target->colorAttachmentCount();
            for (std::uint32_t attachment = 0; attachment < attachments; ++attachment)
            {
                views.push_back(entry->target->colorView(attachment));
            }
            input_scratch[index].colors = views;
            if (pass.inputs[index].depth_sampleable)
            {
                input_scratch[index].depth = entry->target->depthView();
            }
        }

        // The view block is the PASS' (the content layer's contract), built from the first drawing call's
        // camera - the engine's passes draw through one camera per scope, so the first is the pass'.
        const vine::graphics::VineViewBlock view_block =
            buildViewBlock(pass.draws[0].camera, session.frameSeconds(), width, height);
        ::vsg::ref_ptr<::vsg::Node> content;
        (void)assembly.record(pass, compatibility, input_scratch,
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
    PassRegistry        passes;   ///< The SDK's pass objects, as the numbers the plan carries (see the header).
    HostTargets         targets;  ///< The host's off-screen targets (see the header and api/HostTargets).

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
    std::array<core::ReportOnce, kReadbackReportCount> readback_reports{};  ///< The unresolved-target episodes.
    std::vector<core::TargetFacts>            facts;          ///< This frame's target table (borrowed rows).
    std::vector<const vine::graphics::Light*> light_scratch;  ///< Reused by setLights (no per-call allocation).
    std::vector<InputImages>                  input_scratch;  ///< Reused per content pass (see recordContent).
    /// One colour-view list per declared input, reused per pass: the InputImages spans point into these, so
    /// they must outlive the record call and must not be reallocated while it runs (see recordContent).
    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>> input_views;
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
    // The debug-layer switch is the one the implementation this replaces read (VINE_VSG_DEBUG_LAYER, see
    // VsgRenderer): the gate scripts and the troubleshooting notes set it and expect the validation layer,
    // and a switch that silently does nothing is worse than none - the run would CLAIM to be
    // validation-clean with nothing looking. Read here, where the device is asked for.
    options.validation    = std::getenv("VINE_VSG_DEBUG_LAYER") != nullptr;
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
    // And the host's targets: their objects belong to the same device (a borrower's share of a lender's image
    // goes with its own entry - see api/HostTargets).
    d->targets.clear();
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
    // itself (see WindowTarget::facts), plus one row per host target this backend holds - built or not,
    // because the plan has to be able to say "this one has no objects yet", and because the compiler resolves
    // a pass' declared INPUTS from this same table (see HostTargets::facts).
    d->facts.clear();
    if (WindowTarget* window = detail::SessionContentAccess::windowTarget(d->session))
    {
        d->facts.push_back(window->facts());
    }
    for (const std::unique_ptr<HostTargets::Entry>& entry : d->targets.entries())
    {
        core::TargetFacts row;
        d->targets.facts(*entry, row);
        d->facts.push_back(row);
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
        packets = recordContent(*d->assembly, *window, d->targets, d->session, *d->frame, d->input_scratch,
                                d->input_views, d->packets);
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
        return;  // not a size a surface can have: nothing to announce, and nothing to follow
    }
    d->announced_width  = width;
    d->announced_height = height;
    if (d->session.initialized())
    {
        // THE SURFACE OWNS ITS SIZE (RenderBackend::resize's authority order: surface > announcement >
        // default), so a live announcement is served by FOLLOWING the surface: the session re-reads the one
        // it is on and rebuilds its swapchain when that surface changed, and what hangs off the size (the
        // render area, the window target's shape, each pass' view block) is derived from the window when the
        // next frame records - no second copy of the size to keep in step. A window this backend created for
        // itself is followed the same way; the announced numbers are what the NEXT initialize() creates that
        // window at, which is the only way this layer can apply them (it does not move a window of its own -
        // see the registered limit in the design notes).
        //
        // The announcement is what tells the follow whether an unchanged answer is a problem or the truth: a
        // size the host announced as NEW and that the platform answers with the old extent is a read that
        // arrived before the platform applied it (see followResizedSurface's second ask).
        detail::SessionContentAccess::followResizedSurface(d->session);
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
    // The off-screen half is served now: a non-null target is held (api/HostTargets), drawn into by the
    // passes that announce it, and read back through the readback entry points (which are still reported as
    // not served - see the header).
    return true;
}

void VsgBackend::setRenderTarget(vine::raw_ptr<vine::graphics::RenderTarget> target)
{
    if (target == nullptr)
    {
        if (d->recorder.inPass())
        {
            (void)d->recorder.setRenderTarget(nullptr);  // the default framebuffer, spelled as the plan does
        }
        return;
    }

    // The host's own target: its description is copied and its objects are built the first time the description
    // can make them (see HostTargets - the resize / rebuild after that is the plan's answer, applied by the
    // executor). A target this backend cannot build is REPORTED once rather than silently redirected to the
    // window: a picture drawn where the host never asked for it is worse than one that did not arrive (the
    // same rule the SDK's releaseRenderTarget note states for a target that went away mid-scope).
    const HostTargets::Ensured ensured =
        d->targets.ensure(*target, detail::SessionContentAccess::device(d->session));
    if (ensured.state == HostTargets::State::Ready)
    {
        ensured.entry->report.rearm();  // the condition ended: the same breakage reports again
    }
    else if (ensured.state != HostTargets::State::NotBuilt && d->recorder.inFrame() &&
             ensured.entry->report.shouldReport())
    {
        // NOT "NotBuilt", which is not a failure: a host configures a target before it draws into it (and the
        // executor reports the passes that needed one that was never completed). What is reported is the
        // description that cannot become objects at all - a missing lender, or a build that failed - and it is
        // reported INSIDE a frame, where it blocks the picture. OUTSIDE one the announcement is configuration:
        // the engine announces the window pass' target before the passes that write its lender (measured: the
        // demo's composite arrives before its G-buffer), and a borrow whose lender comes later in the setup is
        // order, not failure - a frame that then needs it is reported by the executor. The targets are NAMED
        // (the SDK's own label, see RenderTarget::setName): "a target could not be built" is a sentence a
        // host cannot act on, and the pipeline builder names everything it creates.
        std::string message;
        if (ensured.state == HostTargets::State::DepthSourceMissing)
        {
            // The lender is read from the target itself (shareDepth keeps it alive), so this works whether or
            // not it was ever announced - which is exactly the case being reported.
            message = nameOf(target) + " borrows its depth from " + nameOf(target->depthSource()) +
                      ", which this backend does not hold: it is not built, and the passes that draw into it "
                      "are skipped (the lender must be announced first)";
        }
        else
        {
            message = nameOf(target) +
                      " could not be built (its images, render pass or readback buffer failed to create, or "
                      "its description is not a target): the passes that draw into it are skipped";
        }
        reportDiagnostic(vine::graphics::DiagnosticSeverity::Warning,
                         vine::graphics::DiagnosticCategory::TargetBuildFailed, asString(message));
    }

    if (ensured.entry->target != nullptr)
    {
        d->executor.addTarget(target, ensured.entry->target.get());
    }
    if (d->recorder.inPass())
    {
        (void)d->recorder.setRenderTarget(target);  // the plan's identity for it: the host's own object
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
    if (!d->recorder.inPass())
    {
        return;
    }
    for (const vine::raw_ptr<vine::graphics::RenderTarget>& input : inputs)
    {
        if (input != nullptr)
        {
            // The description of every input is kept current here too: its objects were built when a pass
            // drew into it, and what has to be fresh is the facts row the plan resolves the input's shape
            // (its colour count, its depth's sampleability) from (see HostTargets::observe).
            (void)d->targets.observe(*input);
        }
    }
    // The identities, in the pass' declaration order, nulls included: "nothing produced it this frame" is
    // a fact the plan carries (see InputRef), and the recording reads one entry per declaration.
    (void)d->recorder.setPassInputs(inputs);
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
        // THE MATERIAL IS TOUCHED HERE, because this is the only place a live edit can be noticed: the engine
        // never announces one (the SDK's RenderBackend has no such entry point - see ContentStore::updateMaterial),
        // and the implementation this backend replaces refreshed each commanded material every frame for the
        // same reason. The touch compares and only a difference rebuilds the row, so a steady frame pays a
        // comparison and nothing else.
        d->store->updateMaterial(command.material.get());
    }
    (void)d->recorder.render(commands, camera);  // refusing (no scope, a released target) is the protocol's
}

void VsgBackend::drawScreenProgram(vine::graphics::RenderTarget*                     source,
                                   vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                   vine::raw_ptr<const vine::graphics::Camera>        camera)
{
    if (!d->recorder.inFrame())
    {
        return;  // the engine's warm-up executes pass scopes outside any frame: nothing to collect
    }
    if (d->store != nullptr)
    {
        // The fragment stage is content like any other: the tables must answer its two texts and their
        // declarations when the plan records (the store's own walk covers full-screen calls' programs).
        trackProgram(d->store.get(), vine::intrusive_ptr<const vine::graphics::ShaderProgram>(program));
    }
    // The source is an IDENTITY here, exactly like a content draw's program: which images it offers was
    // announced with the pass' inputs (setPassInputs), and the content layer resolves it among them.
    (void)d->recorder.drawScreenProgram(source, program, camera);
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
    if (target == nullptr)
    {
        return;
    }
    // The SDK announces that the caller may destroy it now, so everything held for it goes: the registry
    // entry (its objects die with it, unless a borrower keeps a share of a borrowed image), the executor's
    // registration (a borrowed pointer - letting it dangle would name somebody else's memory) and a pending
    // scope announcement (the recorder drops it: a pass that would have drawn into it is SKIPPED and
    // reported, never redirected to the window - see the SDK's own note).
    (void)d->targets.release(target);
    d->executor.addTarget(target, nullptr);
    (void)d->recorder.releaseRenderTarget(target);
}

bool VsgBackend::readColorBuffer(const vine::graphics::RenderTarget* target, int attachment,
                                 std::vector<std::uint8_t>& outPixels, vine::graphics::ReadbackResult* why)
{
    // A refusal says WHY on both channels: the machine answer in @p why and one sentence on the diagnostics
    // route - once per episode, because this call is synchronous and a caller polling a target that is not
    // ready yet must not be flooded. A target this backend holds keeps its own episode (its entry's); the
    // ones it cannot even resolve share one per entry point.
    const auto refuse = [&](HostReadbackRefusal refusal, core::ReportOnce& episode) {
        if (why != nullptr)
        {
            *why = readbackResultOf(refusal);
        }
        if (episode.shouldReport())
        {
            reportDiagnostic(vine::graphics::DiagnosticSeverity::Warning,
                             vine::graphics::DiagnosticCategory::ContentSkipped,
                             readbackRefusalMessage(refusal, "readColorBuffer()"));
        }
        return false;
    };

    if (target == nullptr)
    {
        return refuse(HostReadbackRefusal::NoTarget, d->readback_reports[kReadbackColour]);
    }
    HostTargets::Entry* entry = d->targets.find(target);
    if (entry == nullptr)
    {
        // Never announced, or released: the SDK's own answer is NotReady (a later frame or a re-announced
        // handle can make it readable), not Unsupported - which is the answer to "this backend never will".
        return refuse(HostReadbackRefusal::UnknownTarget, d->readback_reports[kReadbackColour]);
    }
    if (entry->target == nullptr)
    {
        return refuse(HostReadbackRefusal::NotBuilt, entry->readback_report);
    }
    if (attachment < 0)
    {
        return refuse(HostReadbackRefusal::UnknownAttachment, entry->readback_report);
    }
    const ::vsg::ref_ptr<::vsg::Device> device = detail::SessionContentAccess::device(d->session);
    if (device == nullptr)
    {
        return refuse(HostReadbackRefusal::NoDevice, entry->readback_report);
    }

    // Everything that cannot be served is answered BEFORE the device is stopped (an attachment the target
    // does not have, a format this backend does not pack, a target no frame has drawn into yet): a request
    // that has no answer costs nothing - not even the wait below.
    const HostReadbackRefusal pre =
        classifyReadback(*entry->target, core::ReadbackKind::Color, static_cast<std::uint32_t>(attachment));
    if (pre != HostReadbackRefusal::None)
    {
        return refuse(pre, entry->readback_report);
    }

    // The frame that wrote the pixels may still be in flight, so the device is stopped first - and the wait
    // is COUNTED (the counter deviceWaits() answers with): a readback is the one path that may stop the
    // device, and "it did so exactly once" stays checkable (see SessionContentAccess::waitDeviceIdle).
    detail::SessionContentAccess::waitDeviceIdle(d->session);

    const HostReadbackRefusal refusal = readColorAttachment(*entry->target, static_cast<std::uint32_t>(attachment),
                                                            device.get(), outPixels);
    if (refusal != HostReadbackRefusal::None)
    {
        return refuse(refusal, entry->readback_report);
    }
    entry->readback_report.rearm();  // a readback that works ends the episode
    if (why != nullptr)
    {
        *why = vine::graphics::ReadbackResult::Ok;
    }
    return true;
}

bool VsgBackend::readDepthBuffer(const vine::graphics::RenderTarget* target, std::vector<float>& outDepths,
                                 vine::graphics::ReadbackResult* why)
{
    const auto refuse = [&](HostReadbackRefusal refusal, core::ReportOnce& episode) {
        if (why != nullptr)
        {
            *why = readbackResultOf(refusal);
        }
        if (episode.shouldReport())
        {
            reportDiagnostic(vine::graphics::DiagnosticSeverity::Warning,
                             vine::graphics::DiagnosticCategory::ContentSkipped,
                             readbackRefusalMessage(refusal, "readDepthBuffer()"));
        }
        return false;
    };

    if (target == nullptr)
    {
        return refuse(HostReadbackRefusal::NoTarget, d->readback_reports[kReadbackDepth]);
    }
    HostTargets::Entry* entry = d->targets.find(target);
    if (entry == nullptr)
    {
        return refuse(HostReadbackRefusal::UnknownTarget, d->readback_reports[kReadbackDepth]);
    }
    if (entry->target == nullptr)
    {
        return refuse(HostReadbackRefusal::NotBuilt, entry->readback_report);
    }
    if (entry->description.depth_source != nullptr)
    {
        // The SDK's own rule: a borrowed depth is read through the SOURCE target, and the borrower answers
        // Unsupported - the image belongs to the lender, and this target never promised it.
        return refuse(HostReadbackRefusal::BorrowedDepth, entry->readback_report);
    }
    const ::vsg::ref_ptr<::vsg::Device> device = detail::SessionContentAccess::device(d->session);
    if (device == nullptr)
    {
        return refuse(HostReadbackRefusal::NoDevice, entry->readback_report);
    }

    const HostReadbackRefusal pre = classifyReadback(*entry->target, core::ReadbackKind::Depth, 0U);
    if (pre != HostReadbackRefusal::None)
    {
        return refuse(pre, entry->readback_report);
    }

    detail::SessionContentAccess::waitDeviceIdle(d->session);

    const HostReadbackRefusal refusal = readDepthAttachment(*entry->target, device.get(), outDepths);
    if (refusal != HostReadbackRefusal::None)
    {
        return refuse(refusal, entry->readback_report);
    }
    entry->readback_report.rearm();
    if (why != nullptr)
    {
        *why = vine::graphics::ReadbackResult::Ok;
    }
    return true;
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
                              " has nowhere to go: every part of the frame protocol needs a session that is "
                              "up (see .ai/design/vsg-reimplementation.md)"));
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

HostTargets& BackendContentAccess::targets(VsgBackend& backend) noexcept
{
    return backend.d->targets;
}

VsgExecutor& BackendContentAccess::executor(VsgBackend& backend) noexcept
{
    return backend.d->executor;
}

WindowTarget* BackendContentAccess::windowTarget(VsgBackend& backend) noexcept
{
    return SessionContentAccess::windowTarget(backend.d->session);
}

ContentStore* BackendContentAccess::store(VsgBackend& backend) noexcept
{
    return backend.d->store.get();
}

}  // namespace detail

V_VSG_NS_END
