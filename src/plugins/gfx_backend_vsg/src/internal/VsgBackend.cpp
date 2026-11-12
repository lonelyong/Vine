#include <vine/vsg/internal/VsgBackend.hpp>

#include <algorithm>
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
#include <vsg/state/Sampler.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/PhysicalDevice.h>

#include <vine/graphics/Diagnostics.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/backend/AllocationGate.hpp>
#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/FrameRecorder.hpp>
#include <vine/graphics/backend/Observe.hpp>
#include <vine/graphics/backend/VariantPool.hpp>
#include <vine/vsg/internal/BackendContent.hpp>
#include <vine/vsg/internal/BlockStorage.hpp>
#include <vine/vsg/internal/ContentAssembly.hpp>
#include <vine/vsg/internal/ContentStore.hpp>
#include <vine/vsg/internal/Device.hpp>
#include <vine/vsg/internal/HostReadback.hpp>
#include <vine/vsg/internal/HostTargets.hpp>
#include <vine/vsg/internal/MaterialImages.hpp>
#include <vine/vsg/internal/OffscreenSession.hpp>
#include <vine/vsg/internal/PassRegistry.hpp>
#include <vine/vsg/internal/Session.hpp>
#include <vine/vsg/internal/SessionContent.hpp>
#include <vine/vsg/internal/ViewBlock.hpp>
#include <vine/vsg/internal/VsgExecutor.hpp>
#include <vine/vsg/internal/WindowTarget.hpp>
#include <vine/vsg/support/VsgSceneRules.hpp>
#include <vine/vsg/support/VsgUtils.hpp>

VN_VSG_NS_BEGIN

namespace
{

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
std::string nameOf(const vn::graphics::RenderTarget* target)
{
    if (target == nullptr) {
        return "an unnamed target";
    }
    const vn::String& name = target->name();
    if (name.empty()) {
        return "an unnamed target";
    }
    return "'" + std::string(reinterpret_cast<const char*>(name.data()), name.size()) + "'";
}

/// @brief Tracks @p program into @p store, when both exist (see setDefaultContentProgram).
void trackProgram(ContentStore* store, const vn::intrusive_ptr<const vn::graphics::ShaderProgram>& program)
{
    if (store == nullptr || program == nullptr) {
        return;
    }
    // The SDK hands the default program as a CONST handle while the engine keeps the object mutable, and
    // the store's own handle is mutable (it owns a share of what it describes, see its note): the cast is
    // the bridge between the two views - and nothing in the store writes through it, its tables only READ
    // the object they describe.
    store->track(vn::intrusive_ptr<vn::graphics::ShaderProgram>(const_cast<vn::graphics::ShaderProgram*>(program.get())));
}

/// @brief The spelling of "this frame belongs to no window": see Data::driving.
constexpr std::size_t kNoWindow = static_cast<std::size_t>(-1);

/**
 * @brief Gets the queue family @p device records on: the first one that can do graphics, or -1 when it has none.
 *
 * Read from the DEVICE's own queues rather than from the physical device, and that is the difference that matters:
 * a device built for a window has already chosen the family it serves a surface on, and an off-screen session that
 * asked the physical device for "a graphics family" could get one the device was never created with (command pools
 * and submits then refuse it). The queues a device has are the families it can use, by definition.
 *
 * @param device The device to ask.
 * @return The queue family index, or -1 when the device exposes no graphics queue at all.
 */
int graphicsQueueFamily(const ::vsg::Device& device)
{
    for (const auto& queue : device.getQueues()) {
        if ((queue->queueFlags() & VK_QUEUE_GRAPHICS_BIT) == VK_QUEUE_GRAPHICS_BIT) {
            return static_cast<int>(queue->queueFamilyIndex());
        }
    }
    return -1;
}

/// @brief Whether @p pass draws a full-screen call whose SOURCE is @p target.
///
/// The full-screen ABI binds binding N as the source's attachment N (see BuiltinShaders::screenCopyProgram), so
/// the source's offer is the whole target even when the pass declared it as one ProducedImage. This answers
/// "does this input have to carry that compatibility view", and it is asked here because the plan resolves the
/// images while the calls name the source.
bool isScreenSource(const core::CompiledPass& pass, const void* target) noexcept
{
    if (target == nullptr) {
        return false;
    }
    for (const core::CompiledDraw& draw : pass.draws) {
        if (draw.kind == core::DrawKind::Screen && draw.source == target) {
            return true;
        }
    }
    return false;
}

/// @brief The content of every pass the frame's targets can be served for, one packet per pass that has any
/// (see present).
///
/// WHICH TARGET decides the shape and the extent: a pass drawing into the window is compatible with the
/// window's own shape and reconstructs from the window's extent, and a pass drawing into one of the host's
/// targets is compatible with THAT target's shape - the shape its own render pass and attachments are - and
/// uses its extent. A pass whose target this backend does not hold produces no packet: the executor reports
/// the pass instead of drawing it somewhere else, and content built for it would be content nothing records.
/// A pass with no draws produces no packet either: its clear is the plan's own and records without help.
///
/// A FRAME MAY HAVE NO WINDOW AT ALL (the off-screen form, see executeFrame): then @p window is null and a pass
/// that names no target of its own has nowhere to draw - it produces no packet, and the plan has already left it
/// out (it was compiled against facts with no default-framebuffer row). Passes that name their own targets are
/// recorded exactly as they are on the windowed path.
///
/// PREPARE_ONLY is the WARM-UP half of the same walk (see RenderFrameExecutor::prepareFrame): the per-pass shape and
/// image offers are resolved exactly as a frame resolves them - that is what makes a prepared identity the one the
/// frame asks for - and then the pass is PREPARED instead of recorded, so no packet is produced and no block is
/// written. The two verbs therefore cannot disagree about which passes exist or what they need.
std::span<const PassContent> recordContent(ContentAssembly&                                            assembly,
                                           WindowTarget*                                               window,
                                           HostTargets&                                                targets,
                                           MaterialImages&                                             images,
                                           vn::graphics::Diagnostics&                                  diagnostics,
                                           core::FrameTimeline&                                        timeline,
                                           core::RetirementQueue&                                      retirement,
                                           float                                                       frame_seconds,
                                           const core::CompiledFrame&                                  frame,
                                           std::vector<InputImages>&                                   input_scratch,
                                           std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>>& input_views,
                                           std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>>& input_screen_views,
                                           std::vector<std::vector<::vsg::ref_ptr<::vsg::Sampler>>>&   input_color_samplers,
                                           std::vector<PassContent>&                                   packets,
                                           bool                                                        prepare_only)
{
    packets.clear();
    // THE SAME OPENING FOR BOTH VERBS (see api/ContentAssembly): a recording frame opens the block storage's frame,
    // a preparing one does not - the ring's rotation belongs to the frame (P11's "value side only").
    if (prepare_only) {
        (void)assembly.prepare(frame, timeline, retirement);
    }
    else {
        (void)assembly.beginFrame(frame, timeline, retirement);
    }
    for (const core::CompiledPass& pass : frame.passes) {
        if (pass.draws.empty()) {
            continue;
        }

        core::RenderPassCompatibility compatibility;
        std::uint32_t                 width  = 0;
        std::uint32_t                 height = 0;
        if (const core::CompiledTarget& compiled_target = frame.targets[pass.target_index]; compiled_target.target == nullptr) {
            if (window == nullptr) {
                continue; // no window: a pass that names no target of its own has nowhere to draw
            }
            compatibility = window->shape().compatibility();
            width         = window->width();
            height        = window->height();
        }
        else {
            const HostTargets::Entry* entry = targets.find(compiled_target.target);
            if (entry == nullptr || entry->target == nullptr) {
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
        input_screen_views.assign(pass.inputs.size(), {});
        input_color_samplers.assign(pass.inputs.size(), {});
        for (std::size_t index = 0; index < pass.inputs.size(); ++index) {
            InputImages&                                   offer = input_scratch[index];
            std::vector<::vsg::ref_ptr<::vsg::ImageView>>& views = input_views[index];
            views.clear();

            if (pass.inputs[index].kind == vn::graphics::ImageSource::Kind::ImportedTexture) {
                // An imported texture is one sampled image: the pass' program reads it at the input set's
                // sampler (the input walk's by-name rule), not at the cache's material sampler. The view is
                // copied into the per-pass vector so the span stays valid until assembly.record returns.
                const auto* texture = static_cast<const vn::graphics::Texture*>(pass.inputs[index].texture);
                if (texture == nullptr) {
                    continue; // an empty declaration produces nothing to bind
                }
                vn::vsg::detail::TextureReject reject = vn::vsg::detail::TextureReject::Ok;
                // The state belongs to the INPUT: the same texture declared with two states uploads once and
                // yields two samplers (see MaterialImages). No state = the backend's input sampler, the
                // behavior every imported input had before state existed.
                const SamplerImage sampled =
                    pass.inputs[index].sampler.has_value() ? images.acquire(texture, *pass.inputs[index].sampler, reject) : images.acquire(texture, reject);
                if (reject != vn::vsg::detail::TextureReject::Ok || sampled.view == nullptr) {
                    diagnostics.report(vn::graphics::DiagnosticSeverity::Warning,
                                       vn::graphics::DiagnosticCategory::ContentSkipped,
                                       vn::String(u8"pass input texture could not be sampled; the input binds the white "
                                                  u8"stand-in: ") +
                                           vn::vsg::detail::textureRejectMessage(reject, *texture));
                }
                if (sampled.view != nullptr) {
                    views.push_back(sampled.view);
                }
                offer.colors = views;
                if (sampled.sampler != nullptr) {
                    std::vector<::vsg::ref_ptr<::vsg::Sampler>>& samplers = input_color_samplers[index];
                    samplers.clear();
                    samplers.push_back(sampled.sampler);
                    offer.color_samplers = samplers;
                }
                continue;
            }

            const HostTargets::Entry* entry = targets.find(pass.inputs[index].target);
            if (entry == nullptr || entry->target == nullptr) {
                continue;
            }
            const core::CompiledInput& input       = pass.inputs[index];
            const std::uint32_t        attachments = entry->target->colorAttachmentCount();

            if (input.kind == vn::graphics::ImageSource::Kind::ProducedImage) {
                // ONE image, the attachment the plan narrowed the input to. Offering the whole target here is
                // what used to happen, and it made a content program's binding 0 answer with attachment 0 even
                // when the pass declared attachment 1 (the declaration was a selector nothing read).
                if (input.depth) {
                    if (input.depth_sampleable) {
                        offer.depth = entry->target->depthView();
                    }
                }
                else if (input.attachment >= 0 && static_cast<std::uint32_t>(input.attachment) < attachments) {
                    views.push_back(entry->target->colorView(static_cast<std::uint32_t>(input.attachment)));
                    offer.colors = views;
                }
            }
            else {
                // The coarse declaration: every colour attachment, plus the depth while the plan says it is
                // sampleable.
                for (std::uint32_t attachment = 0; attachment < attachments; ++attachment) {
                    views.push_back(entry->target->colorView(attachment));
                }
                offer.colors = views;
                if (input.depth_sampleable) {
                    offer.depth = entry->target->depthView();
                }
            }

            // The full-screen ABI's compatibility path: binding N reads the source's attachment N, so a call
            // whose source is this input reads the WHOLE target - even when the pass declared the source as one
            // ProducedImage (ScreenPass's addInput(ImageRef) is that shape, and the engine's G-buffer previews
            // are built on it). The content path above keeps the narrowed offer.
            if (isScreenSource(pass, input.target)) {
                std::vector<::vsg::ref_ptr<::vsg::ImageView>>& screen_views = input_screen_views[index];
                screen_views.clear();
                for (std::uint32_t attachment = 0; attachment < attachments; ++attachment) {
                    screen_views.push_back(entry->target->colorView(attachment));
                }
                offer.screen_colors = screen_views;
            }
        }

        // The view block is the PASS' (the content layer's contract), built from the first drawing call's
        // camera - the engine's passes draw through one camera per scope, so the first is the pass'.
        if (prepare_only) {
            // PREPARING: everything the pass needs and nothing the FRAME owns - no view block, no draw/lights/shadow
            // block, no command graph (see api/ContentAssembly::prepare). A pass this backend could not serve was
            // skipped above, exactly as the recorded frame skips it, so both verbs answer the same set of passes.
            (void)assembly.prepare(pass, compatibility, input_scratch);
            continue;
        }
        const vn::graphics::VineViewBlock view_block = buildViewBlock(pass.draws[0].camera, frame_seconds, width, height);
        ::vsg::ref_ptr<::vsg::Node>       content;
        (void)assembly.record(pass, compatibility, input_scratch, std::as_bytes(std::span<const vn::graphics::VineViewBlock>(&view_block, 1U)), content);
        packets.push_back(PassContent{ pass.pass, std::move(content) });
    }
    return packets;
}

} // namespace

namespace detail
{

/**
 * @brief The target object of ONE window a device serves (every window but the first, see VsgBackend).
 *
 * WHAT IT IS: the SDK's `WindowPresentTarget` for a window that is not the one the backend itself answers for. It
 * holds no state - the window's session, its swapchain and its size all belong to the device - and what it forwards
 * to is the device's own per-window entry points, so the rule "which window is this" is answered in ONE place.
 */
class WindowFacade : public vn::graphics::WindowPresentTarget {
  public:
    /** @brief Creates the target of window @p index on @p device. */
    WindowFacade(VsgBackend& device, std::size_t index)
      : device_(&device)
      , index_(index)
    {}

    void setWindowHandle(void* native_handle) override
    {
        device_->setWindowHandleAt(index_, native_handle);
    }

    [[nodiscard]] void* nativeHandle() const override
    {
        return device_->windowHandleAt(index_);
    }

    void resize(int width, int height) override
    {
        device_->resizeWindow(index_, width, height);
    }

    vn::graphics::PresentOutcome present() override
    {
        return device_->presentWindow(index_);
    }

  private:
    VsgBackend* const device_;
    const std::size_t index_;
};

} // namespace detail

struct VsgBackend::Data {
    /// How this device reaches the display, as the creator stated it (see DeviceDesc): `None` means it was created
    /// WITHOUT a window, and that is what capabilities().presentation and createSurfaceTarget answer from.
    vn::graphics::DeviceDesc desc;

    /// The highest number of presented frames this device has ever seen (see frameReport): the host-visible
    /// watermark must not go backwards when a window leaves (its session stops counting).
    std::uint64_t presented_watermark{ 0 };

    /// This layer's inner route: its sink forwards into the SDK's ACTIVE route (see the constructor), which
    /// is the engine's when an engine drives this backend - that is what makes the host read one number.
    vn::graphics::Diagnostics diagnostics;
    core::FrameArena          arena{ 64 * 1024 };
    core::Observe             observe;
    core::FrameRecorder       recorder{ arena, diagnostics, observe };
    core::FrameCompiler       compiler{ arena, diagnostics, observe };
    VsgExecutor               executor{ diagnostics };
    PassRegistry              passes;  // The SDK's pass objects, as the numbers the plan carries (see the header).
    HostTargets               targets; // The host's off-screen targets (see the header and api/HostTargets).

    // The content world: built with the session because its pieces belong to the session's device (see
    // initialize), torn down before the session because the device goes with it. The store is device-free,
    // but it is what the assembly reads its tables from - so the set lives and dies together.
    core::VariantPool             pool{};
    std::shared_ptr<BlockStorage> storage{};
    /// The in-flight count the next beginFrame() justifies the layout against; 0 = ask the session (the
    /// production read). TEST SEAM (see BackendContentAccess::assumeInFlightSlots): the framework these
    /// builds run against reports exactly the assumed count, so the deeper-count re-layout has no natural
    /// way to be observed here.
    std::uint32_t stated_in_flight{ 0 };
    /// Storages a growth could not park yet: KEPT until the session ends, never freed early (the same
    /// fallback ContentAssembly::beginFrame takes when the parking window is not open yet).
    std::vector<std::shared_ptr<BlockStorage>> kept_storage{};
    std::shared_ptr<MaterialImages>            images{};
    std::unique_ptr<ContentStore>              store{};
    std::unique_ptr<ContentAssembly>           assembly{};

    void*                                                host_handle{ nullptr };  // The handle the host announced (nullptr = a window of our own).
    int                                                  announced_width{ 640 };  // What the next initialize() creates its windows at.
    int                                                  announced_height{ 360 }; // What the next initialize() creates its windows at.
    vn::intrusive_ptr<const vn::graphics::ShaderProgram> default_program;         // See the header.

    /**
     * @brief One window this device serves: its own session, and the target object the host holds.
     *
     * WHAT A WINDOW OWNS: the surface, the swapchain and the default-framebuffer target under them (all inside its
     * session). What it does NOT own is anything a second window would have to duplicate - the device, its compiled
     * programs and the buffers they read live on the backend (see core::DeviceClock as well).
     */
    struct WindowSlot {
        void*                         host_handle{ nullptr };
        std::unique_ptr<api::Session> session;
        /// The object the SDK sees. Set for EVERY window, the first included: the device hands out what presents and
        /// is not one of them (see the header), and one identity per window is what keeps "which window is this frame
        /// for" answerable by a pointer compare.
        std::unique_ptr<vn::graphics::WindowPresentTarget> target;
    };

    std::vector<std::unique_ptr<WindowSlot>> windows; // In the order the host asked (0 = the device's own).    /// The session a frame that names NO window
                                                      // runs through (see executeFrame): built on the first such frame,
    /// torn down with the device. It is a SUBMIT carrier, not a window - it has no surface, acquires nothing and
    /// presents nothing.
    std::unique_ptr<api::OffscreenSession> offscreen;
    /// Which session the OPEN frame belongs to: an index into @ref windows, or kNoWindow when the frame is off-screen
    /// (executeFrame names it; presentWindow and the content seam read it back).
    std::size_t driving{ kNoWindow };
    /// THE DEVICE'S completion clock and park queue, shared by every window (see core::DeviceClock): whether a buffer
    /// or a pipeline may be destroyed is a question about the whole device, because the windows draw with the same
    /// objects. A session driven on its own keeps its own pair (see api::SessionOptions).
    core::DeviceClock     clock;
    core::RetirementQueue retirement{ 0 };
    /// The device itself, held here rather than only inside the windows: it outlives every window (the content world
    /// is built on it), and a window that goes away must not take it down. A device created WITHOUT a window (see
    /// DeviceDesc::None) has no window's session to own its instance, so the instance is held beside it.
    ::vsg::ref_ptr<::vsg::Device>   device;
    ::vsg::ref_ptr<::vsg::Instance> instance;

    std::array<vn::graphics::ReportOnce, kUnservedCount>       unserved_reports{};      // One episode per entry point.
    std::array<vn::graphics::ReportOnce, kReadbackReportCount> readback_reports{};      // The unresolved-target episodes.
    vn::graphics::ReportOnce                                   occupied_report{};       // The "a window already served was asked again" episode.
    vn::graphics::ReportOnce                                   foreign_target_report{}; // The "a frame named someone else's target" episode.
    /// What the frame's steps have let go; zero while the host holds everything.
    std::size_t                             released_objects{ 0 };
    std::size_t                             released_textures{ 0 };
    std::size_t                             released_streams{ 0 };
    std::vector<core::TargetFacts>          facts;         // This frame's target table (borrowed rows).
    std::vector<vn::graphics::LightValue> light_scratch; // Reused by announceLights (no per-call allocation).
    std::vector<InputImages>                input_scratch; // Reused per content pass (see recordContent).
    /// One colour-view list per declared input, reused per pass: the InputImages spans point into these, so
    /// they must outlive the record call and must not be reallocated while it runs (see recordContent).
    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>> input_views;
    /// The same, for the WHOLE source target of a full-screen call (see InputImages::screen_colors).
    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>> input_screen_views;
    /// One sampler list per declared input (see InputImages::color_samplers); imported inputs fill one entry.
    std::vector<std::vector<::vsg::ref_ptr<::vsg::Sampler>>> input_color_samplers;
    std::vector<PassContent>                                 packets;          // Reused per frame (see recordContent).
    const core::CompiledFrame*                               frame{ nullptr }; // This frame's plan, between end and swap.
};

VsgBackend::VsgBackend(const vn::graphics::DeviceDesc& desc)
  : d(std::make_unique<Data>())
{
    d->desc = desc;
    // The session and the pieces report through this layer's route, whose sink forwards them into the SDK's
    // ACTIVE route (RenderBackend::setDiagnosticsRoute - the engine's, when an engine drives this backend).
    // That is what puts those sentences in the host's sink and in the engine's single count, without this
    // layer re-reporting anything.
    d->diagnostics.setSink(
        [this](const vn::graphics::RenderDiagnostic& diagnostic) { reportDiagnostic(diagnostic.severity, diagnostic.category, diagnostic.message); });
}

VsgBackend::~VsgBackend()
{
    // THE CREATOR'S TEARDOWN, not the member order's: the sessions borrow the device's clock, its retirement queue
    // and the device itself, and Data's member order destroys those before the sessions that still name them
    // (measured under ASan: OffscreenSession::shutdown -> DeviceClock::removeWindow on a freed clock). shutdown()
    // orders the teardown and is idempotent, so a backend destroyed without an explicit shutdown() is still safe.
    shutdown();
}

bool VsgBackend::initialize()
{
    // The host's announcements and the debug switch are the DEVICE's: every window is created with the size the
    // surface last announced, and with the same layers.
    const bool validation = std::getenv("VN_VSG_DEBUG_LAYER") != nullptr;

    if (d->desc.presentation == vn::graphics::DevicePresentation::None) {
        // A DEVICE THAT PRESENTS NOTHING IS CREATED WITHOUT A WINDOW: the windowless seam (createDevice) is the one
        // door that needs no surface (see Device.hpp), so nothing on this path can bring a window into existence -
        // which is what makes windowCount() == 0 the answer for the whole lifetime of such a device.
        if (d->device == nullptr) {
            const DeviceResult created = createDevice(DeviceOptions{ .validation = validation });
            if (!created.ok || created.device == nullptr) {
                d->diagnostics.report(vn::graphics::DiagnosticSeverity::Error, vn::graphics::DiagnosticCategory::InitFailed, created.error);
                return false;
            }
            d->instance = created.instance;
            d->device   = created.device;
        }
    }
    else {
        // A HANDLE IS A CREATION INPUT (see DeviceDesc): the host's window is what the device is made for, so the
        // first window exists before the loop below brings it up. A Handle with nothing to attach to is wiring this
        // layer cannot repair, and it is said instead of silently becoming a window of our own.
        if (d->desc.presentation == vn::graphics::DevicePresentation::Handle && d->windows.empty()) {
            if (d->desc.native_handle == nullptr) {
                d->diagnostics.report(vn::graphics::DiagnosticSeverity::Error,
                                      vn::graphics::DiagnosticCategory::InitFailed,
                                      vn::String::fromUtf8("this device was created with a window handle as its "
                                                           "presentation input, but none was given"));
                return false;
            }
            ensureWindowAt(0);
            d->windows[0]->host_handle = d->desc.native_handle;
        }

        // A host that never asked for a window still gets one - the device's own (see createSurfaceTarget): the frame
        // path has to have something to present through when a host drives the aggregate directly.
        if (d->windows.empty()) {
            ensureWindowAt(0);
        }

        for (std::size_t index = 0; index < d->windows.size(); ++index) {
            Data::WindowSlot& window = *d->windows[index];

            api::SessionOptions options;
            options.width         = d->announced_width;
            options.height        = d->announced_height;
            options.validation    = validation;
            options.native_handle = window.host_handle;
            // ONE DEVICE, SEVERAL WINDOWS: the first session creates the device, every other one ADOPTS it, and every
            // window reports its completion evidence into the DEVICE's clock and parks into the DEVICE's queue -
            // because what that evidence decides (may this buffer or pipeline go) is a question about the whole
            // device.
            options.device        = d->device.get();
            options.clock         = &d->clock;
            options.retirement    = &d->retirement;

            // A KEEP IS NOT A REPLACEMENT: this window is on the surface it is already on, so its device, its
            // swapchain and every compiled pipeline survive, and the content world below stays with them (the
            // session's own decision table is what answers this, see Session::wouldKeep).
            if (window.session->wouldKeep(options)) {
                continue;
            }
            if (!window.session->initialize(options, d->diagnostics)) {
                // The session reported why (InitFailed), and the sink forwarded it: false plus a reason is the SDK's
                // contract for a failed initialize(), and a half-built session is this layer's to clean up (the
                // session does its own, see its note).
                return false;
            }
            if (d->device == nullptr) {
                // The first window created the device; every later one adopted the same one (see the options above).
                d->device = detail::SessionContentAccess::device(*window.session);
            }
        }
    }

    if (d->device == nullptr) {
        return false; // no window came up and none was created, so there is no device to draw with
    }

    // The sample-count limit is the DEVICE's, read once here rather than at every target build: a target's
    // description is refused against it (see HostTargets::State::SamplesUnsupported), and the same number is
    // what capabilities() reports - one fact, one source.
    if (const ::vsg::PhysicalDevice* physical = d->device->getPhysicalDevice(); physical != nullptr) {
        const VkPhysicalDeviceLimits& limits = physical->getProperties().limits;
        d->targets.setMaxSamples(std::min(limits.framebufferColorSampleCounts, limits.framebufferDepthSampleCounts));
    }

    // THE CONTENT WORLD BELONGS TO THE DEVICE, and is built ONCE: a window that came up, was re-attached or joined
    // later does not invalidate what was uploaded or compiled on the same device.
    d->frame = nullptr;
    if (d->storage != nullptr) {
        return true;
    }

    d->storage = BlockStorage::create(d->device, BlockStorage::Layout{});
    d->images  = MaterialImages::create();
    if (d->images != nullptr) {
        // THE CACHE IS TOLD WHAT THE DEVICE OFFERS, once, here - where the device is. The limit is a
        // device property, and a cache that is never told samples with anisotropy enabled at 1x, which is
        // the same picture as not asking (see MaterialImages::makeSampler): the entry point existed and
        // nothing called it, so mip-chained textures were filtered isotropically however good the device
        // was. Read from the physical device the session built, and clamped by anisotropyFor - asking for
        // more than VkPhysicalDeviceLimits::maxSamplerAnisotropy is a validation error.
        if (const ::vsg::PhysicalDevice* physical = d->device->getPhysicalDevice()) {
            d->images->setMaxAnisotropy(physical->getProperties().limits.maxSamplerAnisotropy);
        }
    }
    if (d->storage != nullptr && d->images != nullptr) {
        d->store    = std::make_unique<ContentStore>();
        d->assembly = std::make_unique<ContentAssembly>(*d->store, d->device, d->pool, *d->storage, *d->images, d->diagnostics);
    }
    // The default program was announced before the device came up (the engine's own order), so it is tracked now -
    // the tables must answer for it before any plan names it.
    trackProgram(d->store.get(), d->default_program);
    return true;
}

void VsgBackend::ensureWindowAt(std::size_t index)
{
    while (d->windows.size() <= index) {
        auto window     = std::make_unique<Data::WindowSlot>();
        window->session = std::make_unique<api::Session>();
        // EVERY window gets its own target object, including the first: the device holds and hands out what presents
        // (see the header), and it is not one of them - one shape for N windows, and "which window is this frame
        // for" stays a pointer compare.
        window->target  = std::make_unique<detail::WindowFacade>(*this, d->windows.size());
        d->windows.push_back(std::move(window));
    }
}

api::Session& VsgBackend::sessionAt(std::size_t index)
{
    ensureWindowAt(index);
    return *d->windows[index]->session;
}

api::OffscreenSession* VsgBackend::ensureOffscreenSession()
{
    if (d->device == nullptr) {
        return nullptr; // no device came up: there is nothing to submit an off-screen frame on
    }
    if (d->offscreen == nullptr) {
        d->offscreen = std::make_unique<api::OffscreenSession>();
    }
    if (d->offscreen->initialized()) {
        return d->offscreen.get();
    }

    // The device was created by a window's session (a Vulkan device is built with the surface it presents to), or by
    // the windowless seam when this device presents nothing at all. Either way the family is read from the DEVICE's
    // own queues - the families it was created with - and never inferred from the physical device (see
    // graphicsQueueFamily).
    api::OffscreenSessionOptions options;
    options.device       = d->device.get();
    options.queue_family = graphicsQueueFamily(*d->device);
    // The DEVICE's clock and park queue, like every window: an off-screen frame's GPU work has to count towards the
    // watermark every shared resource is dated against (see core::DeviceClock).
    options.clock        = &d->clock;
    options.retirement   = &d->retirement;
    if (!d->offscreen->initialize(options, d->diagnostics)) {
        return nullptr; // it reported why (InitFailed, forwarded to the SDK's route)
    }
    return d->offscreen.get();
}

void VsgBackend::releaseContentWorld() noexcept
{
    // THE DESTRUCTIVE PATH RUNS UNDER COUNTED DEVICE IDLES, one per window (the resource rule the design writes
    // down: destruction has two legal paths, and the whole-session replacement is the one that idles). What is
    // about to be dropped holds the PIPELINES the frames just submitted still name, and a pipeline destroyed while
    // a queue executes it is a validation error rather than a picture: measured 2026-09-25, tearing the world down
    // right after a frame that had switched to a newly compiled program or topology produced four
    // VUID-vkDestroyPipeline-pipeline-00765 messages (see the per-frame cost recipe in test_vsg/VsgBackendTest,
    // which is the case that creates a pipeline after frames have started). The waits are COUNTED because "the frame
    // path never idles the device" has to stay checkable - see Session::deviceWaits and RetentionStats::device_waits.
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        detail::SessionContentAccess::waitDeviceIdle(*window->session);
    }
    // A WINDOWLESS DEVICE HAS NO WINDOW TO IDLE ABOVE: the off-screen session is a frame source too, and its
    // submitted work is device-wide, so the same destructive rule applies to it. On a device with a window the
    // idle above already covers this work; waiting again is harmless and keeps the rule one sentence.
    if (d->offscreen != nullptr) {
        d->offscreen->waitDeviceIdle();
    }

    d->frame = nullptr;
    d->facts.clear();
    d->packets.clear();
    d->executor.setWindow(nullptr);
    d->executor.clearTargets();
    // The content world FIRST: its GPU objects (and the sets the assembly built) belong to the device, which the
    // windows drop on the way down.
    d->assembly.reset();
    d->store.reset();
    d->images.reset();
    d->storage.reset();
    // The replacements growth could not park yet go with the device that owns them (the shape needs no
    // reset: a fresh storage states its own, see BlockStorage::layout).
    d->kept_storage.clear();
    // And the host's targets: their objects belong to the same device (a borrower's share of a lender's image
    // goes with its own entry - see api/HostTargets).
    d->targets.clear();
}

void VsgBackend::shutdown()
{
    // The device's objects FIRST (see releaseContentWorld), then the sessions, then the device itself: what is
    // destroyed must not be named by anything still alive. The off-screen session is one of those sessions - it
    // borrows the device and reports into the device's clock - so it goes before the clock and the device do.
    releaseContentWorld();
    d->offscreen.reset();
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        window->session->shutdown();
    }
    d->device     = {};
    d->instance   = {};
    // The evidence starts over with the next device: nothing parked survives it, and the windows that come back are
    // registered again by their own initialize().
    d->clock      = core::DeviceClock{};
    d->retirement = core::RetirementQueue{ 0 };
    d->driving    = kNoWindow;
}

vn::graphics::RenderFrameExecutor& VsgBackend::executor()
{
    // The frame half of this device is its own (see RenderDevice::executor): the aggregate answers both faces.
    return *this;
}

vn::raw_ptr<vn::graphics::WindowPresentTarget> VsgBackend::createSurfaceTarget(void* native_handle, vn::graphics::SurfaceTargetResult* why)
{
    // A DEVICE CREATED TO PRESENT NOTHING has no window half at all: the machine answer is NoPresentation, and it is
    // SILENT - a display-less device is a first-class state rather than a failure (see RenderDevice::createSurfaceTarget).
    if (d->desc.presentation == vn::graphics::DevicePresentation::None) {
        if (why != nullptr) {
            *why = vn::graphics::SurfaceTargetResult::NoPresentation;
        }
        return nullptr;
    }

    // ONE LIVE SWAPCHAIN PER WINDOW (see the header). The same window again - including a second window of the
    // device's own, which has no other reason to exist - is the "occupied" episode, said once, because a host that
    // asked has to hear it rather than discover a picture that never appears.
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        if (window->host_handle == native_handle) {
            if (why != nullptr) {
                *why = vn::graphics::SurfaceTargetResult::WindowOccupied;
            }
            if (d->occupied_report.shouldReport()) {
                reportDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                                 vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                 vn::String::fromUtf8("a window this device already serves was asked for again: it was "
                                                      "left alone (one live swapchain per window - see "
                                                      "createSurfaceTarget, and .ai/design/graphics-window-device-"
                                                      "viewer.md)"));
            }
            return nullptr;
        }
    }

    ensureWindowAt(d->windows.size());
    d->windows.back()->host_handle = native_handle;
    if (why != nullptr) {
        *why = vn::graphics::SurfaceTargetResult::Ok;
    }
    // THE DEVICE IS NOT A WINDOW (see the header): every ask - the first included - is answered by that window's own
    // target object, which forwards back here by index.
    return d->windows.back()->target.get();
}

std::size_t VsgBackend::windowCount() const noexcept
{
    return d->windows.size();
}

void VsgBackend::openFrameBooks()
{
    // The per-frame storage is brought up to what this frame needs HERE - between frames, where no recording
    // is in progress and the previous frame's bytes can be left behind safely: a storage a frame outgrew is
    // replaced (see the growth note), and so is one the session has LEARNED to be too shallow for the
    // in-flight count (see the in-flight floor note). Both are answered by one replacement.
    growBlockStorageIfNeeded();

    // The frame's session mints the token and advances its own frame stamp; the recorder opens the plan for the same
    // token, so the description and the session's frame cannot disagree about which frame it is. A frame with no
    // window is the OFF-SCREEN form: the off-screen session opens it, and acquires nothing (see api::OffscreenSession).
    core::FrameToken token{};
    if (d->driving < d->windows.size()) {
        token = sessionAt(d->driving).beginFrame();
    }
    else if (d->offscreen != nullptr) {
        token = d->offscreen->beginFrame();
    }
    (void)d->recorder.beginFrame(token); // it answers whether the frame's books could be opened (see its note)
}

void VsgBackend::collectFrame(const vn::graphics::FrameDescription& frame)
{
    // THE VALUE, translated into this layer's own collection, in the order the value states. What the engine
    // compiled arrives as data (see FrameDescription), so the frame's own shape is what drives this loop - and
    // every call below is this backend's own piece of work, which is why the picture (and the plan) come out the
    // same whether the frame came from the engine or from a host driving one pass (RenderPass::execute).
    for (const vn::graphics::FrameDescription::Pass& entry : frame.passes) {
        if (entry.pass == nullptr) {
            continue; // a frame's entry always names a pass; a null one is nothing to run
        }
        openPassScope(entry.pass);
        announcePassOrder(entry.order);
        announcePassInputs(entry.inputs);
        if (entry.draw.has_value()) {
            const vn::graphics::PassDraw& description = *entry.draw;
            announceTarget(description.target);
            if (description.viewport.has_value()) {
                announceViewport(*description.viewport);
            }
            if (description.clear.has_value()) {
                announceClear(*description.clear);
            }
            announcePassState(description.state);
            if (description.depth_range.has_value()) {
                announceDepthRange(*description.depth_range);
            }
            for (const vn::graphics::DrawDescription& draw : description.draws) {
                if (draw.light_values.has_value()) {
                    announceLights(*draw.light_values);
                }
                if (!draw.camera.present) {
                    continue; // a drawing call with no view is no call at all (see DrawDescription)
                }
                if (draw.kind == vn::graphics::DrawDescription::Kind::Screen) {
                    collectScreenDraw(draw.source, draw.program.get(), draw.program_description, draw.camera);
                }
                else if (draw.commands != nullptr) {
                    // The override is a PASS fact (see PassDraw::program_override), so every drawing call of the
                    // pass is drawn with it; the command list itself is never rewritten. Its TEXT travels with it
                    // (PassDraw::program_override_description), which is what the recorder applies.
                    collectContentDraw(*draw.commands, draw.camera, description.program_override_description);
                }
            }
        }
        closePassScope();
    }
}

void VsgBackend::collectTargetFacts()
{
    // The facts the plan is compiled against: the window as the default framebuffer, answered by the target
    // itself (see WindowTarget::facts), plus one row per host target this backend holds - built or not,
    // because the plan has to be able to say "this one has no objects yet", and because the compiler resolves
    // a pass' declared INPUTS from this same table (see HostTargets::facts).
    //
    // A FRAME WITH NO WINDOW has no default-framebuffer row at all: that row is what the frame's passes naming no
    // target resolve to, and compiling without it is how "this pass has nowhere to draw" becomes a reported skip
    // instead of a picture in a window nobody named.
    d->facts.clear();
    if (d->driving < d->windows.size()) {
        if (WindowTarget* window = detail::SessionContentAccess::windowTarget(sessionAt(d->driving))) {
            d->facts.push_back(window->facts());
        }
    }
    for (const std::unique_ptr<HostTargets::Entry>& entry : d->targets.entries()) {
        core::TargetFacts row;
        d->targets.facts(*entry, row);
        d->facts.push_back(row);
    }
}

void VsgBackend::closeFrameBooks()
{
    (void)d->recorder.endFrame();
    (void)d->recorder.swapBuffers(); // closes the frame's books: endFrame() alone only leaves the pass scope
    collectTargetFacts();
    d->frame = &d->compiler.compile(d->recorder.description(), core::FrameFacts{ d->facts });
}

vn::graphics::PresentOutcome VsgBackend::executeFrame(const vn::graphics::FrameDescription& frame, vn::graphics::PresentTarget* present_to)
{
    // WHAT THE LAST FRAME'S UPLOADS NO LONGER NEED, BEFORE THIS ONE RECORDS. A staging buffer is the source of one
    // vkCmdCopyBuffer; the moment that copy's fence signals, the bytes are dead weight - and vsg keeps them for the life
    // of the session (measured: 1.2 GB after a 704 MB model, one block per frame slot because uploads rotate through
    // them). Running the sweep here is the whole trigger: a host asks for nothing, and a copy that has not finished yet is
    // simply picked up by a later frame.
    if (d->assembly != nullptr) {
        (void)releaseFinishedUploadStaging();
    }

    // THE FRAME'S TARGET NAMES ITS WINDOW - AND NAMING NONE IS ITS OWN ANSWER. A device hands out one target per
    // window (see createSurfaceTarget), so a non-null target must be one this device handed out; anything else is
    // wiring it could not have produced, so nothing is recorded and the reason is said once per episode instead of
    // being drawn into someone else's window. nullptr is a frame with NO WINDOW: the off-screen form, which runs
    // through the device's off-screen session, records into the targets its passes name, submits, and presents
    // nothing (see `.ai/design/graphics-architecture.md` §6).
    std::size_t index = kNoWindow;
    if (present_to != nullptr) {
        for (std::size_t candidate = 0; candidate < d->windows.size(); ++candidate) {
            if (d->windows[candidate]->target.get() == present_to) {
                index = candidate;
                break;
            }
        }
        if (index >= d->windows.size()) {
            if (d->foreign_target_report.shouldReport()) {
                reportDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                                 vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                 vn::String::fromUtf8("a frame named a surface target this device did not hand out: "
                                                      "nothing was recorded (a device serves the windows it created, "
                                                      "see createSurfaceTarget)"));
            }
            return vn::graphics::PresentOutcome::Lost; // a surface was named and nothing could reach it
        }
    }

    api::OffscreenSession* offscreen = nullptr;
    if (present_to == nullptr) {
        offscreen = ensureOffscreenSession();
        if (offscreen == nullptr) {
            reportUnserved(kUnservedNoSession);
            // Nothing runs: the device the frame would be submitted on is not up, and nothing was going to be
            // presented either way.
            return vn::graphics::PresentOutcome::Skipped;
        }
        // NO WINDOW IN THE FRAME, SO NONE IN THE EXECUTOR: a pass that names no target has nowhere to draw, and
        // the plan it is compiled against says so (see closeFrameBooks).
        d->driving = kNoWindow;
        d->executor.setWindow(nullptr);
    }
    else {
        // The window this frame belongs to: the executor's default-framebuffer target is ITS, and presentWindow()
        // reads the same index back when the frame's tail arrives.
        d->driving            = index;
        api::Session& session = sessionAt(index);
        if (!session.initialized()) {
            reportUnserved(kUnservedNoSession);
            // Nothing runs and nothing presents: the session that would record it is not up, so the picture did not
            // reach the surface the frame named.
            return vn::graphics::PresentOutcome::Lost;
        }
        d->executor.setWindow(detail::SessionContentAccess::windowTarget(session));
    }

    // THE VALUE, run by the layer that owns the identities, in the order the value states (see collectFrame - the
    // same walk prepareFrame makes, so a prepared frame and this one cannot disagree about what the frame is).
    openFrameBooks();
    collectFrame(frame);
    closeFrameBooks();

    // THE FRAME'S TAIL, and it is this call's last step whichever way the frame is presented. WITH a window, the
    // executor presents through the target the frame named - PresentTarget::present is the mechanism, the executor is
    // the one making the call - and in this backend that call is what commits the frame and shows it. WITHOUT a
    // window there is nothing to present, so the off-screen session commits it inline.
    if (offscreen != nullptr) {
        return commitFrameThrough(nullptr, offscreen);
    }
    return present_to != nullptr ? present_to->present() : vn::graphics::PresentOutcome::Skipped;
}

vn::graphics::PrepareReport VsgBackend::prepareFrame(const vn::graphics::FrameDescription& frame, vn::graphics::PresentTarget* present_to)
{
    // NOTHING TO PREPARE WITHOUT THE CONTENT WORLD: it is what compiles a half, a set or a stream at all, and it
    // exists only while a device does (see initialize). The answer says so rather than reporting a zero it did not
    // earn.
    if (d->assembly == nullptr || d->store == nullptr || d->images == nullptr) {
        return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::NotInitialized };
    }
    // A FRAME IS OPEN ON THIS BACKEND (executeFrame between its books and its present): its plan and its recorded
    // packets belong to it, and collecting a second frame over them would corrupt both. Refused, never queued.
    if (d->frame != nullptr) {
        return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::Refused };
    }

    // WHICH SURFACE THE FRAME NAMES: the same resolution executeFrame makes (a target this device handed out, or the
    // off-screen form), minus everything that would touch it. Nothing is acquired, resized or presented - the shape
    // is read from the target's own facts (a const answer), which is exactly what P11 asks for: an image acquired
    // here and never presented would starve the next real frame.
    std::size_t index = kNoWindow;
    if (present_to != nullptr) {
        for (std::size_t candidate = 0; candidate < d->windows.size(); ++candidate) {
            if (d->windows[candidate]->target.get() == present_to) {
                index = candidate;
                break;
            }
        }
        if (index >= d->windows.size()) {
            // The same episode the frame path reports (one sentence, one fact): a surface this device did not hand
            // out is wiring it could not have produced, whether it arrives to be prepared or to be run.
            if (d->foreign_target_report.shouldReport()) {
                reportDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                                 vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                 vn::String::fromUtf8("a frame named a surface target this device did not hand out: "
                                                      "nothing was prepared (a device serves the windows it created, "
                                                      "see createSurfaceTarget)"));
            }
            return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::Refused };
        }
        if (!sessionAt(index).initialized()) {
            return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::Refused };
        }
    }
    d->driving = index;

    // THE PER-FRAME STORAGE IS BROUGHT UP TO WHAT THIS FRAME NEEDS, exactly as a frame's opening would - and this is
    // the place for it, because prepare is defined to run between frames. A prepared frame whose first real frame
    // would grow the storage would rebuild every declared set it just prepared (the sets bind that buffer), which is
    // the one way a warm-up could make the frame it warmed cost more rather than less.
    growBlockStorageIfNeeded();
    growMaterialArenaIfNeeded();

    // THE PLAN, COMPILED FROM THE SAME VALUE AND THE SAME FACTS a frame compiles it from - the collection (see
    // collectFrame) and the target-fact table are the frame's own steps, so the identities prepared here are the
    // ones the frame will ask for. The recorder needs a TOKEN and this is not a frame, so it gets the "not a frame"
    // spelling (0): nothing downstream of prepare reads it - the content tables date their rows by the session's
    // clock, not by the plan's number.
    if (!d->recorder.beginFrame(core::FrameToken{})) {
        d->driving = kNoWindow;
        return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::Refused };
    }
    collectFrame(frame);
    closeFrameBooks();
    const core::CompiledFrame& plan = *d->frame;
    d->frame                        = nullptr; // PREPARE LEAVES NO FRAME TO PRESENT: there is nothing recorded

    // THE VALUE SIDE, AND ONLY IT. The same walk a frame takes (`recordContent`) resolves each pass' shape and image
    // offers and then PREPARES it instead of recording it (see api/ContentPass::prepare): the compiled halves, the
    // declared sets, the sampled-input sets, the stream uploads and the pipeline identities are what it builds, and
    // what it produces is no command graph at all. Running the frame's own walk is what makes "what prepare built"
    // and "what the frame builds" ONE implementation rather than two spellings that can drift (`G1`; `T32` pins the
    // identity half, `T33` the whole). Nothing here acquires, submits, presents or rotates the block ring.
    const std::uint64_t halves_before  = d->assembly->halves().builds();
    const std::uint64_t sets_before    = d->assembly->sets().builds();
    const std::uint64_t uploads_before = d->assembly->uploads().uploads();
    const std::uint64_t pool_before    = d->pool.created();

    WindowTarget* const window_target = index < d->windows.size() ? detail::SessionContentAccess::windowTarget(sessionAt(index)) : nullptr;
    (void)recordContent(*d->assembly,
                        window_target,
                        d->targets,
                        *d->images,
                        d->diagnostics,
                        d->clock.timeline(),
                        d->retirement,
                        // No frame clock is read: a preparing pass builds no view block (the frame's own clock,
                        // sampled when it opens, is what its view block carries).
                        0.0F,
                        plan,
                        d->input_scratch,
                        d->input_views,
                        d->input_screen_views,
                        d->input_color_samplers,
                        d->packets,
                        /*prepare_only*/ true);

    vn::graphics::PrepareReport report;
    report.outcome = vn::graphics::PrepareReport::Outcome::Prepared;
    for (const core::CompiledPass& pass : plan.passes) {
        if (pass.draws.empty()) {
            continue; // a pass with nothing to draw has nothing to prepare (its clear is the plan's own)
        }
        ++report.passes;
        report.draws += static_cast<std::uint32_t>(pass.draws.size());
    }
    // THE SHAPE THE FRAME ASKED FOR, not what this backend managed to walk (see PrepareReport): a pass the walk above
    // skipped because its target has no objects yet is still counted here, and the build deltas below are what say
    // how much of the request actually became objects. Two numbers, two questions - and neither pretends to be the
    // other.
    // WHAT THIS CALL BUILT, not what is resident: a second prepare of an unchanged frame answers zeros, which is the
    // property the call exists for.
    report.halves    = d->assembly->halves().builds() - halves_before;
    report.sets      = d->assembly->sets().builds() - sets_before;
    report.uploads   = d->assembly->uploads().uploads() - uploads_before;
    report.pipelines = d->pool.created() - pool_before;
    return report;
}

vn::graphics::PresentOutcome VsgBackend::presentWindow(std::size_t index)
{
    if (index >= d->windows.size()) {
        return vn::graphics::PresentOutcome::Skipped;
    }
    api::Session& session = sessionAt(index);
    if (d->frame == nullptr || !session.initialized()) {
        // No frame is open (executeFrame closed none, and says why), or the session is down: there is a surface and
        // nothing to hand it, which is a loss rather than "present nothing" - the host re-attaches on it.
        return vn::graphics::PresentOutcome::Lost;
    }
    return commitFrameThrough(&session, nullptr);
}

vn::graphics::PresentOutcome VsgBackend::commitFrameThrough(api::Session* window, api::OffscreenSession* offscreen)
{
    if (d->frame == nullptr || (window == nullptr && offscreen == nullptr)) {
        return vn::graphics::PresentOutcome::Skipped;
    }
    // A WINDOW PRESENTS, AN OFF-SCREEN SESSION DOES NOT: both commit, and the answer says which happened, so the
    // executor can report "the picture reached a window" apart from "it went nowhere by design".
    const auto settled = [window](bool committed) {
        if (!committed) {
            return vn::graphics::PresentOutcome::Lost;
        }
        return window != nullptr ? vn::graphics::PresentOutcome::Presented : vn::graphics::PresentOutcome::Skipped;
    };

    // The frame's own clock and park queue: the window's, or the off-screen session's - both are the DEVICE's when
    // the backend gave them one (see core::DeviceClock).
    core::FrameTimeline&   timeline      = window != nullptr ? window->timeline() : offscreen->timeline();
    core::RetirementQueue& retirement    = window != nullptr ? window->retirement() : offscreen->retirement();
    const float            frame_seconds = window != nullptr ? window->frameSeconds() : offscreen->frameSeconds();
    WindowTarget*          window_target = window != nullptr ? detail::SessionContentAccess::windowTarget(*window) : nullptr;

    if (d->frame->passes.empty()) {
        // NOTHING TO RECORD, and the frame is still submitted - the session's own rule (an acquired image has to be
        // handed back), and the graph it submits is the one the session created with its window's render graph in it.
        // That graph is not decoration: it is what moves the acquired image out of UNDEFINED, and a present of an
        // image still in UNDEFINED is a VUID (measured: an EMPTY command graph presents an image nobody rendered
        // into). The plan with passes below builds its own graph instead, because the executor places each pass graph
        // in it (the window's included, see recordWindow).
        //
        // An OFF-SCREEN session's bootstrap graph is empty and valid: there is no image to move out of UNDEFINED,
        // because nothing was acquired and nothing will be presented.
        //
        // The sweep runs on this path too: a frame that draws nothing is still a frame in which the host may have
        // dropped something, and the sweep's place is the frame's - not "the frames that drew". The content side is
        // opened first for the same reason: the table walk is what PUBLISHES the declaration indexes the residency
        // sweeps ask (see ContentAssembly::beginFrame), so a host that stops drawing must still lose its uploads.
        if (d->assembly != nullptr) {
            (void)d->assembly->beginFrame(*d->frame, timeline, retirement);
        }
        sweepAbandonedContent();
        const bool committed = window != nullptr ? d->executor.commit(*d->frame, *window) : d->executor.commit(*d->frame, *offscreen);
        d->frame             = nullptr;
        return settled(committed);
    }

    const ::vsg::ref_ptr<::vsg::CommandGraph> graph = window != nullptr ? detail::SessionContentAccess::makeFrameGraph(*window) : offscreen->makeFrameGraph();
    if (graph == nullptr) {
        // The frame's graph could not be built, so none of its picture was ever going to reach the surface.
        d->frame = nullptr;
        return vn::graphics::PresentOutcome::Lost;
    }

    // The frame drive's own order, once (see VsgExecutor): what the plan answered for its targets becomes real, the
    // frame's content is assembled and recorded into the passes this slice can serve, the plan is recorded into this
    // frame's graph, and the graph is the session's - which is the call that submits (and, for a window, presents),
    // and the one whose false answer means the writes never happened.
    (void)d->executor.applyTargetPlans(*d->frame, d->facts, timeline, retirement);
    // THE MATERIAL ARENA IS SIZED BEFORE ANYTHING IS RECORDED (see the declaration): every live row keeps a
    // slot until the host releases it, so the frame that would have run out is given a bigger one first.
    growMaterialArenaIfNeeded();
    std::span<const PassContent> packets{};
    if (d->assembly != nullptr && d->images != nullptr && (window_target != nullptr || window == nullptr)) {
        packets = recordContent(*d->assembly,
                                window_target,
                                d->targets,
                                *d->images,
                                d->diagnostics,
                                timeline,
                                retirement,
                                frame_seconds,
                                *d->frame,
                                d->input_scratch,
                                d->input_views,
                                d->input_screen_views,
                                d->input_color_samplers,
                                d->packets,
                                /*prepare_only*/ false);
    }
    (void)d->executor.record(*d->frame, graph, packets);
    // WHAT THE HOST LET GO IS LET GO HERE: the frame's content has been recorded, and the commit below is
    // what advances the retirement queue - so this is the last moment a park can be dated against this frame
    // (see the frame drive's ordering note, and the ordering rule core::RetirementQueue states).
    sweepAbandonedContent();
    const bool assigned  = window != nullptr ? detail::SessionContentAccess::assignFrameGraphs(*window, ::vsg::CommandGraphs{ graph })
                                             : offscreen->assignFrameGraphs(::vsg::CommandGraphs{ graph });
    bool       committed = false;
    if (assigned) {
        // The executor reports its own failure and marks what it wrote; the answer is whether the picture got out.
        committed = window != nullptr ? d->executor.commit(*d->frame, *window) : d->executor.commit(*d->frame, *offscreen);
    }
    d->frame = nullptr;
    return settled(committed);
}

bool VsgBackend::releaseFinishedUploadStaging()
{
    // EVERY LIVE SESSION, because the staging belongs to the VIEWER that staged it (one transfer task each). They share
    // the device, and nothing here touches it: the proof is each block's own fence (see detail::StagingTask).
    bool released = false;
    for (const auto& window : d->windows) {
        if (window == nullptr || window->session == nullptr) {
            continue;
        }
        released = window->session->releaseUploadStaging() || released;
    }
    if (d->offscreen != nullptr) {
        released = d->offscreen->releaseUploadStaging() || released;
    }
    return released;
}

std::size_t VsgBackend::uploadStagingBytes() const noexcept
{
    ::VkDeviceSize total = 0;
    for (const auto& window : d->windows) {
        if (window == nullptr || window->session == nullptr) {
            continue;
        }
        total += window->session->stagedBytes();
    }
    if (d->offscreen != nullptr) {
        total += d->offscreen->stagedBytes().value_or(0);
    }
    return static_cast<std::size_t>(total);
}

void VsgBackend::setWindowHandleAt(std::size_t index, void* native_handle)
{
    // A session decision for THAT window: the next initialize() adopts the handle (or moves an established session
    // onto it - see core::planSessionMove, which the session applies).
    ensureWindowAt(index);
    d->windows[index]->host_handle = native_handle;
}

void* VsgBackend::windowHandleAt(std::size_t index) const noexcept
{
    // The SDK's question is "which surface am I on": a host surface was adopted, a window of our own is not
    // the host's to name - and before initialize() there is nothing to name.
    if (index >= d->windows.size()) {
        return nullptr;
    }
    return d->windows[index]->session->initialized() ? d->windows[index]->host_handle : nullptr;
}

void VsgBackend::resizeWindow(std::size_t index, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return; // not a size a surface can have: nothing to announce, and nothing to follow
    }
    d->announced_width  = width;
    d->announced_height = height;
    if (index >= d->windows.size()) {
        return; // a window that does not exist yet: the announced size is what it will be created at
    }
    api::Session& session = sessionAt(index);
    if (session.initialized()) {
        // THE SURFACE OWNS ITS SIZE (PresentTarget::resize's authority order: surface > announcement >
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
        detail::SessionContentAccess::followResizedSurface(session);
    }
}

void VsgBackend::setDefaultContentProgram(vn::intrusive_ptr<const vn::graphics::ShaderProgram> program)
{
    d->default_program = std::move(program);
    // The plan needs it now - content without a program of its own is drawn with this one, and the
    // compiler resolves that before the plan exists - and the store must know it before any frame names
    // it, because the tables are what the recording reads. The TEXT is taken here, on the content side (this call is
    // a host API), and both halves travel: the object for the store's ownership/abandon rule, the value for its rows.
    const vn::graphics::ShaderProgramDescriptionPtr text =
        d->default_program != nullptr ? d->default_program->description() : vn::graphics::ShaderProgramDescriptionPtr{};
    (void)d->recorder.setDefaultContentProgram(text.get());
    trackProgram(d->store.get(), d->default_program);
    if (d->store != nullptr) {
        d->store->track(text);
    }
}

vn::graphics::Capabilities VsgBackend::capabilities() const noexcept
{
    // Both halves are served - EXCEPT THE WINDOW ONE when the device was created to present nothing (see
    // DeviceDesc): frames are presented through a target this device handed out, a non-null target is held
    // (api/HostTargets), drawn into by the passes that announce it, and read back through the readback entry points -
    // and a `None` device says in the same value that it has no window half at all.
    vn::graphics::Capabilities answer{
        .render_targets = true,
        .presentation   = d->desc.presentation != vn::graphics::DevicePresentation::None,
    };

    // THE FEATURES ARE THE DEVICE'S ANSWERS, read from the physical device the session built rather than assumed
    // from the floor: two of the three are requirements this backend cannot run without (see DeviceRequirements),
    // which is exactly why a live session reporting them is a fact and not a promise, while GPU timings is NOT a
    // floor - a device that cannot write timestamps runs whole frames and reports its pass times as unavailable.
    if (const ::vsg::PhysicalDevice* physical = d->device != nullptr ? d->device->getPhysicalDevice() : nullptr) {
        const VkPhysicalDeviceProperties& properties = physical->getProperties();
        // The LIMITS travel beside the feature bits: "anisotropy exists" and "how much of it" are different
        // answers, and the number is clamped the one way the sampler path clamps it (anisotropyFor).
        answer.max_anisotropy                        = vn::vsg::detail::anisotropyFor(properties.limits.maxSamplerAnisotropy);
        answer.max_color_attachments                 = properties.limits.maxColorAttachments;
        // ONE target has ONE sample count, and its colour and depth attachments must both support it, so the
        // limit reported is what BOTH can do (a device whose depth tops out lower is the binding side).
        answer.max_samples                           = std::min(properties.limits.framebufferColorSampleCounts, properties.limits.framebufferDepthSampleCounts);
        // How WIDE a rasterised LINE may be is the device's answer, not the feature bit's: `wideLines` says
        // "widths above 1.0 are allowed", and this is how far the implementation goes (some stop at 1.0 even
        // with the feature on). The recorder clamps to it (see ContentPass::Scope::max_line_width).
        answer.max_line_width                        = properties.limits.lineWidthRange[1];
        if (properties.limits.timestampComputeAndGraphics != VK_FALSE) {
            answer.features |= vn::graphics::Capabilities::bit(vn::graphics::Capabilities::Feature::GpuTimings);
        }
        if (physical->getFeatures().fillModeNonSolid == VK_TRUE) {
            answer.features |= vn::graphics::Capabilities::bit(vn::graphics::Capabilities::Feature::Wireframe);
        }
        if (physical->getFeatures().samplerAnisotropy == VK_TRUE) {
            answer.features |= vn::graphics::Capabilities::bit(vn::graphics::Capabilities::Feature::AnisotropicFiltering);
        }
    }
    return answer;
}

vn::graphics::FrameReport VsgBackend::frameReport() const noexcept
{
    // ONE ANSWER from the two places that know (see FrameReport): the backend's own instrumentation
    // (frame/Observe.hpp) answers the frame that just ended and what is retained, and the DEVICE's frame clock
    // answers where the frames are - SUBMITTED is what reached the queue, COMPLETED is what every window has proof
    // of (see core::DeviceClock). Both watermarks are the device's own, so a host reads the same numbers the
    // retirement window is dated against instead of guessing a lag.
    const core::FrameTimeline& timeline = d->clock.timeline();

    // PRESENTED is the window half's own count: every frame a window's session committed was handed to the platform's
    // present call (see api::Session::framesPresented - the off-screen carrier presents nothing and answers 0). The
    // sum is kept MONOTONIC: a window that leaves stops being counted, and a host-visible watermark must not go
    // backwards (see FrameProgress).
    std::uint64_t presented = 0;
    for (const auto& window : d->windows) {
        if (window->session != nullptr) {
            presented += window->session->framesPresented();
        }
    }
    d->presented_watermark = std::max(d->presented_watermark, presented);

    vn::graphics::FrameReport report;
    report.counters  = d->observe.counters();
    report.retention = d->observe.retention();
    report.progress =
        vn::graphics::FrameProgress{ .submitted = timeline.submittedFrame(), .completed = timeline.completedFrame(), .presented = d->presented_watermark };

    // WHAT IS RESIDENT, PER KIND: the accounting half of GPU residency (see ResidencyLedger). The kinds this
    // device retains are composed HERE, where all three tables are reachable - a table answers only for itself,
    // and the ledger is the one place a host has to learn. Absent numbers stay absent: the stream store does not
    // count bytes and the image cache keeps no build history, so those rows say nothing about them instead of
    // reporting a zero nobody measured.
    if (d->assembly != nullptr && d->images != nullptr) {
        vn::graphics::ResidencyLedger ledger;
        const StreamUploads&          uploads = d->assembly->uploads();
        ledger.note(vn::graphics::ResidencyRow{ .kind    = vn::graphics::ResidencyKind::Streams,
                                                .live    = static_cast<std::uint32_t>(uploads.live()),
                                                .built   = uploads.uploads(),
                                                .retired = uploads.retired(),
                                                .refused = uploads.refusals() });
        ledger.note(vn::graphics::ResidencyRow{ .kind  = vn::graphics::ResidencyKind::Images,
                                                .live  = static_cast<std::uint32_t>(d->images->count()),
                                                .bytes = d->images->bytes() });
        if (d->storage != nullptr) {
            ledger.note(d->storage->materialResidency());
        }
        report.retention->residency = std::move(ledger);
    }
    return report;
}

void VsgBackend::setRenderTarget(vn::raw_ptr<vn::graphics::RenderTarget> target)
{
    if (target == nullptr) {
        return; // the default framebuffer needs no announcement: a frame that names no target draws into it
    }

    // The host's own target, configured before any frame draws into it: held, and built as far as its description
    // allows (see HostTargets - the resize / rebuild after that is the plan's answer, applied by the executor).
    // OUTSIDE a frame this is CONFIGURATION and says nothing: a borrow whose lender is announced later in that
    // setup is order, not failure (the engine's own pipeline does exactly that - measured on the demo, the
    // composite arrives before its G-buffer). A frame that then draws into a target no objects were made for is
    // reported by the pass that names it (see announceTarget), where a picture depends on it.
    (void)adoptHostTarget(target, /*picture_depends*/ false);
}

void VsgBackend::announceTarget(vn::raw_ptr<vn::graphics::RenderTarget> target)
{
    if (target == nullptr) {
        if (d->recorder.inPass()) {
            (void)d->recorder.setRenderTarget(nullptr); // the default framebuffer, spelled as the plan does
        }
        return;
    }

    // A pass of THIS frame draws into it, so a description that cannot become objects at all blocks the picture
    // and is reported once (see adoptHostTarget). The target is held either way, whether or not it could be
    // built yet.
    (void)adoptHostTarget(target, d->recorder.inFrame());
    if (d->recorder.inPass()) {
        (void)d->recorder.setRenderTarget(target); // the plan's identity for it: the host's own object
    }
}

bool VsgBackend::adoptHostTarget(vn::raw_ptr<vn::graphics::RenderTarget> target, bool picture_depends)
{
    const HostTargets::Ensured ensured = d->targets.ensure(*target, d->device);
    if (ensured.state == HostTargets::State::Ready) {
        ensured.entry->report.rearm(); // the condition ended: the same breakage reports again
    }
    else if (ensured.state != HostTargets::State::NotBuilt && picture_depends && ensured.entry->report.shouldReport()) {
        reportUnbuildableTarget(
            target,
            ensured.state == HostTargets::State::DepthSourceMissing
                ? UnbuildableReason::DepthSourceMissing
                : (ensured.state == HostTargets::State::SamplesUnsupported ? UnbuildableReason::SamplesUnsupported : UnbuildableReason::BuildFailed),
            d->targets.maxSamples());
    }

    if (ensured.entry->target != nullptr) {
        d->executor.addTarget(target, ensured.entry->target.get(), target->name().as_std_str().c_str());
    }
    return ensured.entry->target != nullptr;
}

void VsgBackend::reportUnbuildableTarget(vn::raw_ptr<vn::graphics::RenderTarget> target, UnbuildableReason reason, std::uint32_t max_samples)
{
    // What is reported is the description that cannot become objects at all - a missing lender, or a build that
    // failed - and never "NotBuilt", which is not a failure: a host configures a target before it draws into it
    // (and the executor reports the passes that needed one that was never completed). The targets are NAMED (the
    // SDK's own label, see RenderTarget::setName): "a target could not be built" is a sentence a host cannot act
    // on, and the pipeline builder names everything it creates.
    std::string message;
    if (reason == UnbuildableReason::SamplesUnsupported) {
        // The two reasons a sample count is refused, said as the difference they are: a count the device does
        // not have, and a multisampled target asking for what a multisampled depth cannot do (be shared with
        // another pass, or be sampled by a shader). Both are the DESCRIPTION's, so the host can fix it.
        const std::uint32_t samples  = target->samples();
        const bool          too_many = samples > max_samples;
        message                      = nameOf(target) + " asks for " + std::to_string(samples) + " samples per pixel, ";
        message += too_many ? "above the device's limit of " + std::to_string(max_samples) +
                                  ": the passes that draw into it are skipped (see Capabilities::max_samples)"
                            : "and borrows its depth from another target: a borrowed depth is that target's resolved "
                              "single-sample picture, and a framebuffer's attachments share one sample count - announce the "
                              "borrower at one sample, or let it own its depth";
    }
    else if (reason == UnbuildableReason::DepthSourceMissing) {
        // The lender is read from the target itself (shareDepth keeps it alive), so this works whether or
        // not it was ever announced - which is exactly the case being reported.
        message = nameOf(target) + " borrows its depth from " + nameOf(target->depthSource()) +
                  ", which this backend does not hold: it is not built, and the passes that draw into it "
                  "are skipped (the lender must be announced first)";
    }
    else {
        message = nameOf(target) + " could not be built (its images, render pass or readback buffer failed to create, or "
                                   "its description is not a target): the passes that draw into it are skipped";
    }
    reportDiagnostic(vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::TargetBuildFailed, vn::String::fromUtf8(message));
}

void VsgBackend::openPassScope(vn::raw_ptr<const vn::graphics::RenderPass> pass)
{
    if (pass == nullptr) {
        return; // no pass announced: whatever needed a scope is refused and reported by the recorder
    }
    const core::PassId id = d->passes.adopt(pass);
    if (d->recorder.inFrame()) {
        (void)d->recorder.beginPass(id);
    }
}

void VsgBackend::closePassScope()
{
    if (d->recorder.inFrame()) {
        (void)d->recorder.endPass();
    }
}

void VsgBackend::announcePassOrder(int order)
{
    if (d->recorder.inPass()) {
        (void)d->recorder.setPassOrder(order);
    }
}

void VsgBackend::announceViewport(const vn::graphics::Viewport& viewport)
{
    if (d->recorder.inPass()) {
        (void)d->recorder.setViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    }
}

void VsgBackend::announceLights(std::span<const vn::graphics::LightValue> lights)
{
    if (!d->recorder.inPass()) {
        return;
    }
    // The values are already a copy taken on the content side (see Light::value): only the container has to become a
    // span, and the scratch vector keeps that from being an allocation per pass per frame.
    d->light_scratch.assign(lights.begin(), lights.end());
    (void)d->recorder.setLights(std::span<const vn::graphics::LightValue>(d->light_scratch));
}

void VsgBackend::announcePassInputs(const std::vector<vn::graphics::ImageSource>& inputs)
{
    if (!d->recorder.inPass()) {
        return;
    }
    for (const vn::graphics::ImageSource& input : inputs) {
        if (input.isProduced() && input.target != nullptr) {
            // The description of every produced input is kept current here too: its objects were built when a
            // pass drew into it, and what has to be fresh is the facts row the plan resolves the input's shape
            // (its colour count, its depth's sampleability) from (see HostTargets::observe).
            (void)d->targets.observe(*input.target);
        }
    }
    // The identities, in the pass' declaration order, empties included: "nothing produced it this frame" is
    // a fact the plan carries (see InputRef), and the recording reads one entry per declaration.
    (void)d->recorder.setPassInputs(inputs);
}

void VsgBackend::announcePassState(const vn::graphics::PassRenderState& state)
{
    if (d->recorder.inPass()) {
        (void)d->recorder.setPassState(state);
    }
}

void VsgBackend::announceDepthRange(const vn::graphics::DepthRange& range)
{
    if (d->recorder.inPass()) {
        (void)d->recorder.setDepthRange(range);
    }
}

void VsgBackend::announceClear(const vn::graphics::ClearPolicy& policy)
{
    if (!d->recorder.inPass()) {
        return;
    }
    // The SDK's policy is linear floats plus a depth flag; the plan's is floats plus a depth VALUE. The colour is
    // passed THROUGH, with no transfer function: the SDK's contract is that a colour reaching the engine is already
    // linear, so the only encoding left is the target's own (an sRGB surface encodes on write inside the GPU, and a
    // linear off-screen target stores the value as it stands - see the SDK's ClearPolicy). Depth keeps the plan's
    // own far value because the SDK has no depth value to give: one there would be a second place to get the clip
    // convention wrong.
    //
    // WHY TWO SHAPES at all: the backend half speaks DEVICE values (a packed float4 per attachment, the thing a clear
    // plan is compared and submitted with), while the SDK speaks colours with alpha semantics. This function is the
    // one place the two meet, and since the SDK side became linear the translation is a straight copy - the reason it
    // still exists is that neither half should have to know the other's shape.
    core::ClearValues translated;
    translated.color          = true; // the SDK only announces a policy for a pass that clears its colour
    translated.color_value[0] = policy.color.r;
    translated.color_value[1] = policy.color.g;
    translated.color_value[2] = policy.color.b;
    translated.color_value[3] = policy.color.a;
    // The per-attachment colours travel exactly as stated; an attachment nobody stated keeps transparent black
    // (see the SDK's ClearPolicy and core::planClearValues).
    translated.extra_color_values.reserve(policy.extra_colors.size());
    for (const vn::Colorf& colour : policy.extra_colors) {
        translated.extra_color_values.push_back({ colour.r, colour.g, colour.b, colour.a });
    }
    translated.depth = policy.depth;
    (void)d->recorder.setClearPolicy(translated);
}

void VsgBackend::collectContentDraw(const std::vector<vn::graphics::RenderCommand>& commands,
                                    const vn::graphics::CameraSnapshot&             camera,
                                    const vn::graphics::ShaderProgramDescriptionPtr& program_override)
{
    if (!d->recorder.inFrame()) {
        return; // nothing is collected outside a frame
    }
    if (d->store == nullptr) {
        // The content world could not be built on this session (see initialize): collecting the draws would
        // plan a picture nothing can render, which is the silent black frame this backend refuses to be.
        reportUnserved(kUnservedDraw);
        return;
    }
    // What the commands name becomes the store's live content: the IDENTITY and the DESCRIPTION are the values a frame
    // carries (`RenderCommand::node` / `description`), and the tables must answer for them when the plan is recorded -
    // including after the host has dropped its own handle (the store owns a share, see its note). A command built by
    // hand carries no node and is named by the geometry's own id, which is the identity such a caller means.
    for (const vn::graphics::RenderCommand& command : commands) {
        const vn::graphics::NodeId identity =
            !command.node.isNull() ? command.node : (command.geometry != nullptr ? command.geometry->id() : vn::graphics::NodeId{});
        d->store->track(identity, command.geometryDescription());
        d->store->track(command.material);
        // The material's VALUES travel the same way the geometry's do: a command built from a row carries the values
        // that row was published with, so the material table is described from a value and never from the live
        // object - which is what lets the content side edit a material while another thread records (design
        // graphics-threading.md C18).
        d->store->track(command.material.get(), command.materialDescription());
        d->store->track(command.program);
        // The program's TEXT travels the same way the material's values do: a command built from a row carries the
        // text that row was published with, so the program table is described from a value and never from the live
        // object - which is what lets the content side edit a program while another thread records.
        d->store->track(command.programDescription());
    }
    // The picture's shape is the CAMERA's (see Camera), and the recorder reads it from the same camera when it
    // builds the view block - one number, read by culling and projection alike, so they cannot drift.
    if (program_override != nullptr) {
        // The override's own text is content like any other: the store must answer for it when the plan records,
        // even though no command names it.
        d->store->track(program_override);
    }
    // The pass' override is resolved when the draw is recorded (see FrameRecorder::render): the command list stays the
    // shared one the engine collected, and nothing here rewrites it.
    (void)d->recorder.render(commands, camera, program_override.get()); // refusing (a released target) is the recorder's to report
}

void VsgBackend::collectScreenDraw(vn::graphics::RenderTarget*                    source,
                                   vn::raw_ptr<const vn::graphics::ShaderProgram> program,
                                   const vn::graphics::ShaderProgramDescriptionPtr& program_description,
                                   const vn::graphics::CameraSnapshot&            camera)
{
    if (!d->recorder.inFrame()) {
        return; // nothing is collected outside a frame
    }
    // The VALUE the pass carried is what the tables and the recorder read; a hand-built description that named only
    // the object is converted here, on the content side (the same door every hand-built command uses).
    const vn::graphics::ShaderProgramDescriptionPtr text =
        program_description != nullptr ? program_description
                                       : (program != nullptr ? program->description() : vn::graphics::ShaderProgramDescriptionPtr{});
    if (d->store != nullptr) {
        // The fragment stage is content like any other: the tables must answer its two texts and their
        // declarations when the plan records (the store's own walk covers full-screen calls' programs).
        trackProgram(d->store.get(), vn::intrusive_ptr<const vn::graphics::ShaderProgram>(program));
        d->store->track(text);
    }
    // The source is an IDENTITY here, exactly like a content draw's program: which images it offers was
    // announced with the pass' inputs (the value's resolved `inputs`), and the content layer resolves it among them.
    (void)d->recorder.drawScreenProgram(source, text.get(), camera);
}

void VsgBackend::releasePass(vn::raw_ptr<const vn::graphics::RenderPass> pass)
{
    (void)d->passes.release(pass);
    // Nothing retained is keyed by a pass yet (see the header), so forgetting the identity is the whole of
    // the SDK's "free what the pass owns" here - and it keeps the registry from growing for a host that
    // adds and removes passes.
}

void VsgBackend::releaseRenderTarget(vn::graphics::RenderTarget* target)
{
    if (target == nullptr) {
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

bool VsgBackend::releaseContent(vn::raw_ptr<const vn::Object> content)
{
    if (content == nullptr || !initialized() || d->store == nullptr || d->images == nullptr) {
        return false;
    }

    // ONE TYPE DISPATCH: the SDK names the entry with Object because the four content types have no common
    // narrower base (see graphics-content-lifecycle.md). The order is exact: no two of these types
    // are related, and a Texture2D / CubeMap answers through Texture::desc().
    if (const auto* geometry = vn::obj_cast<const vn::graphics::Geometry>(content)) {
        // The store releases the shape the frames described for it (see its own overload): releasing the geometry
        // releases the description's rows, and a shape nobody described has none.
        return d->store->releaseGeometry(geometry);
    }
    if (const auto* program = vn::obj_cast<const vn::graphics::ShaderProgram>(content)) {
        return d->store->releaseProgram(program);
    }
    if (const auto* material = vn::obj_cast<const vn::graphics::MaterialBase>(content)) {
        // A MATERIAL IS DISPATCHED AT ITS BASE, NOT AT `Material`: an asset and the instance a drawable overrides
        // over it are two concrete types under one shading model (see MaterialBase's note - `RenderCommand::
        // material` is a MaterialBasePtr, and nothing downstream branches on which kind it got). Casting to the
        // asset missed every instance, i.e. exactly the per-object materials a host is most likely to release.
        // The ARENA SLOT is NOT freed here: a recorded plan may still read the row's block, so the slot follows the
        // same rows as the facts and the block storage returns it once no row names the identity (see
        // MaterialArena::retireUnpinned).
        return d->store->releaseMaterial(material);
    }
    if (const auto* texture = vn::obj_cast<const vn::graphics::Texture>(content)) {
        return d->images->releaseTexture(texture);
    }
    return false;
}

bool VsgBackend::releaseContent(const vn::Buffer<float>& buffer)
{
    // The element type is the caller's way of saying WHICH stream; the residency keys on the buffer's ADDRESS (see
    // StreamUploads / StreamKey), so the two overloads share one body.
    return releaseStreamBuffer(&buffer);
}

bool VsgBackend::releaseContent(const vn::Buffer<std::uint32_t>& buffer)
{
    return releaseStreamBuffer(&buffer);
}

bool VsgBackend::releaseStreamBuffer(const void* buffer)
{
    if (buffer == nullptr || !initialized() || d->assembly == nullptr) {
        return false;
    }
    // THE DEVICE'S clock and queue, not a window's: a stream is read by every window's frames, so "may this bind be
    // freed" is a question about the whole device (see core::DeviceClock). The release is PARKED, never destroyed -
    // a submitted command buffer may still name the bind (see ResourceTable / StreamUploads).
    (void)d->assembly->uploads().releaseBuffer(buffer, d->clock.timeline(), d->retirement);
    return true;
}

bool VsgBackend::readColorBuffer(const vn::graphics::RenderTarget* target,
                                 int                               attachment,
                                 std::vector<std::uint8_t>&        outPixels,
                                 vn::graphics::ReadbackResult*     why)
{
    // A refusal says WHY on both channels: the machine answer in @p why and one sentence on the diagnostics
    // route - once per episode, because this call is synchronous and a caller polling a target that is not
    // ready yet must not be flooded. A target this backend holds keeps its own episode (its entry's); the
    // ones it cannot even resolve share one per entry point.
    const auto refuse = [&](HostReadbackRefusal refusal, vn::graphics::ReportOnce& episode) {
        if (why != nullptr) {
            *why = readbackResultOf(refusal);
        }
        if (episode.shouldReport()) {
            reportDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                             vn::graphics::DiagnosticCategory::ContentSkipped,
                             readbackRefusalMessage(refusal, "readColorBuffer()"));
        }
        return false;
    };

    if (target == nullptr) {
        return refuse(HostReadbackRefusal::NoTarget, d->readback_reports[kReadbackColour]);
    }
    HostTargets::Entry* entry = d->targets.find(target);
    if (entry == nullptr) {
        // Never announced, or released: the SDK's own answer is NotReady (a later frame or a re-announced
        // handle can make it readable), not Unsupported - which is the answer to "this backend never will".
        return refuse(HostReadbackRefusal::UnknownTarget, d->readback_reports[kReadbackColour]);
    }
    if (entry->target == nullptr) {
        return refuse(HostReadbackRefusal::NotBuilt, entry->readback_report);
    }
    if (attachment < 0) {
        return refuse(HostReadbackRefusal::UnknownAttachment, entry->readback_report);
    }
    const ::vsg::ref_ptr<::vsg::Device> device = d->device;
    if (device == nullptr) {
        return refuse(HostReadbackRefusal::NoDevice, entry->readback_report);
    }

    // Everything that cannot be served is answered BEFORE the device is stopped (an attachment the target
    // does not have, a format this backend does not pack, a target no frame has drawn into yet): a request
    // that has no answer costs nothing - not even the wait below.
    const HostReadbackRefusal pre = classifyReadback(*entry->target, core::ReadbackKind::Color, static_cast<std::uint32_t>(attachment));
    if (pre != HostReadbackRefusal::None) {
        return refuse(pre, entry->readback_report);
    }

    // The frame that wrote the pixels may still be in flight in ANY window (the pixels are a device object), so
    // every window is stopped first - and each wait is COUNTED (the counter deviceWaits() answers with): a readback
    // is the one path that may stop the device, and "it did so once per window" stays checkable (see
    // SessionContentAccess::waitDeviceIdle).
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        detail::SessionContentAccess::waitDeviceIdle(*window->session);
    }

    const HostReadbackRefusal refusal = readColorAttachment(*entry->target, static_cast<std::uint32_t>(attachment), device.get(), outPixels);
    if (refusal != HostReadbackRefusal::None) {
        return refuse(refusal, entry->readback_report);
    }
    entry->readback_report.rearm(); // a readback that works ends the episode
    if (why != nullptr) {
        *why = vn::graphics::ReadbackResult::Ok;
    }
    return true;
}

bool VsgBackend::readDepthBuffer(const vn::graphics::RenderTarget* target, std::vector<float>& outDepths, vn::graphics::ReadbackResult* why)
{
    const auto refuse = [&](HostReadbackRefusal refusal, vn::graphics::ReportOnce& episode) {
        if (why != nullptr) {
            *why = readbackResultOf(refusal);
        }
        if (episode.shouldReport()) {
            reportDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                             vn::graphics::DiagnosticCategory::ContentSkipped,
                             readbackRefusalMessage(refusal, "readDepthBuffer()"));
        }
        return false;
    };

    if (target == nullptr) {
        return refuse(HostReadbackRefusal::NoTarget, d->readback_reports[kReadbackDepth]);
    }
    HostTargets::Entry* entry = d->targets.find(target);
    if (entry == nullptr) {
        return refuse(HostReadbackRefusal::UnknownTarget, d->readback_reports[kReadbackDepth]);
    }
    if (entry->target == nullptr) {
        return refuse(HostReadbackRefusal::NotBuilt, entry->readback_report);
    }
    if (entry->description.depth_source != nullptr) {
        // The SDK's own rule: a borrowed depth is read through the SOURCE target, and the borrower answers
        // Unsupported - the image belongs to the lender, and this target never promised it.
        return refuse(HostReadbackRefusal::BorrowedDepth, entry->readback_report);
    }
    const ::vsg::ref_ptr<::vsg::Device> device = d->device;
    if (device == nullptr) {
        return refuse(HostReadbackRefusal::NoDevice, entry->readback_report);
    }

    const HostReadbackRefusal pre = classifyReadback(*entry->target, core::ReadbackKind::Depth, 0U);
    if (pre != HostReadbackRefusal::None) {
        return refuse(pre, entry->readback_report);
    }

    // Every window is stopped: the depth image is a device object, and the frame that wrote it may be in flight in
    // any of them (see readColorBuffer's note).
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        detail::SessionContentAccess::waitDeviceIdle(*window->session);
    }

    const HostReadbackRefusal refusal = readDepthAttachment(*entry->target, device.get(), outDepths);
    if (refusal != HostReadbackRefusal::None) {
        return refuse(refusal, entry->readback_report);
    }
    entry->readback_report.rearm();
    if (why != nullptr) {
        *why = vn::graphics::ReadbackResult::Ok;
    }
    return true;
}

bool VsgBackend::initialized() const noexcept
{
    // A device created to present nothing has no session to ask: it is up when its device is (and that is the only
    // thing a window would have told us here).
    if (d->desc.presentation == vn::graphics::DevicePresentation::None) {
        return d->device != nullptr;
    }
    // Up when ANY window is: a device with several windows is up as soon as one of them can draw, and a window that
    // failed to come up does not take the others down with it (its own initialize() said why).
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        if (window->session->initialized()) {
            return true;
        }
    }
    return false;
}

std::uint64_t VsgBackend::framesPresented() const noexcept
{
    std::uint64_t presented = 0;
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        presented += window->session->framesPresented();
    }
    return presented;
}

std::size_t VsgBackend::deviceWaits() const noexcept
{
    // The DEVICE's queue is where every window's destructive path is counted (see core::DeviceClock): one number,
    // because "the frame path never idles the device" is one fact about one device.
    return d->retirement.deviceWaits();
}

std::size_t VsgBackend::releasedContentObjects() const noexcept
{
    return d->released_objects;
}

std::size_t VsgBackend::releasedTextures() const noexcept
{
    return d->released_textures;
}

std::size_t VsgBackend::releasedStreams() const noexcept
{
    return d->released_streams;
}

void VsgBackend::sweepAbandonedContent()
{
    if (d->store == nullptr || d->images == nullptr) {
        // No content world on this session (see initialize): there is nothing retained to let go of either.
        return;
    }
    // WHAT THIS DRIVES IS THE STORE'S HALF, AND THAT IS WHY IT IS HERE. The streams and the images ask their own
    // residency question inside ContentAssembly::beginFrame, because the answer is a property of THAT frame's tables
    // (the declaration indexes the store publishes). The store's own entries are the opposite kind of step: they must
    // not be marked before the frame's content has been recorded, and they must not be dropped after the commit has
    // advanced the retirement queue - so they get one call between the two (see the frame drive).
    d->released_objects += d->store->releaseAbandoned();

    // THE MATERIAL ARENA'S SLOTS ARE NOT THIS CALL'S BUSINESS ANY MORE: a slot follows the same rows one kind down,
    // and the block storage returns it at the frame's opening from the published declaration index (see
    // MaterialArena::retireUnpinned / ContentAssembly::beginFrame) - so the store never names a GPU object, and this
    // sweep has no id list to hand on.

    // THE IMAGE CACHE HAS NO CAPACITY TO RUN OUT OF (see MaterialImages): an entry leaves when its texture is
    // released (the sweep above) or named explicitly, never to make room. So there is no "over the bound" sentence
    // here - the host decides what to release, and count()/bytes() are what it reads to decide.

    // The shared streams follow the rows the sweep just parked (see api/StreamUploads): the frame's
    // beginFrame() retires every entry no live geometry declares any more, and this keeps the counter the
    // report reads in step with that table.
    if (d->assembly != nullptr) {
        d->released_streams = static_cast<std::size_t>(d->assembly->uploads().retired());
    }
    d->released_textures = d->images->released();
}

void VsgBackend::growMaterialArenaIfNeeded()
{
    if (d->storage == nullptr || d->store == nullptr) {
        return; // no content world on this session: nothing notes materials
    }

    const BlockStorage::Layout before  = d->storage->layout();
    const BlockStorage::Growth pending = d->storage->growthNeeded();
    // Every material row the store answers for is a slot the arena keeps, plus whatever a refused note asked
    // for (the fallback path). One number, from the layer that knows the live set.
    const std::uint32_t live           = static_cast<std::uint32_t>(d->store->materialCount());
    const std::uint32_t need           = std::max(live, pending.materials);
    if (need <= before.materials.capacity) {
        return;
    }

    BlockStorage::Growth request{};
    request.materials                         = need;
    const BlockStorage::Layout    wanted      = BlockStorage::grownLayout(before, request);
    std::shared_ptr<BlockStorage> replacement = BlockStorage::create(d->device, wanted);
    if (replacement == nullptr || !adoptBlockStorage(std::move(replacement))) {
        return; // the old storage keeps serving and counting; a refusal is reported by the draw that hits it
    }

    // The same shape of sentence as the block budgets' growth: Info, because the request was served by a bigger
    // buffer rather than by dropping content.
    reportDiagnostic(vn::graphics::DiagnosticSeverity::Info,
                     vn::graphics::DiagnosticCategory::ContentSkipped,
                     formatDiagnostic(u8"the live material set outgrew the arena, so the block storage was replaced "
                                      u8"(a new buffer; the old one is parked): materials %u -> %u",
                                      before.materials.capacity,
                                      wanted.materials.capacity));
}

void VsgBackend::growBlockStorageIfNeeded()
{
    if (d->storage == nullptr) {
        return; // no content world on this session (see initialize): nothing writes blocks
    }

    // TWO REQUESTS, ONE REPLACEMENT. A frame that ran out of budget asks for deeper rings (see the growth
    // note), and the count the session LEARNED asks for rings at least one slab deeper than it (see
    // BlockStorage::layoutForInFlight) - which the storage is not, when the framework turns out to keep more
    // frames in flight than the code assumed. Answering them one at a time would drop the second request:
    // a replacement starts with no growth request of its own (see BlockStorage::growthNeeded).
    const std::uint32_t        learned = d->stated_in_flight != 0U ? d->stated_in_flight : d->clock.widestSlots();
    const BlockStorage::Layout before  = d->storage->layout();
    const BlockStorage::Growth need    = d->storage->growthNeeded();

    const bool slabs_short = learned != 0U && !BlockStorage::hasSlabsForInFlight(before, learned);
    if (!slabs_short && !need.needed()) {
        return;
    }

    BlockStorage::Layout wanted = before;
    if (slabs_short) {
        wanted = BlockStorage::layoutForInFlight(wanted, learned);
    }
    if (need.needed()) {
        wanted = BlockStorage::grownLayout(wanted, need);
    }

    std::shared_ptr<BlockStorage> replacement = BlockStorage::create(d->device, wanted);
    if (replacement == nullptr || !adoptBlockStorage(std::move(replacement))) {
        return; // the old storage keeps serving (and counting); its refusals were already reported
    }

    // WHY THE BYTES MOVED IS SAID OUT LOUD: for a grown budget, the per-pass warnings were the frames that
    // lost content and this is the fact that ends them; for the learned count this diagnostic is the ONLY
    // record - the frames it protects were about to shade with another frame's slab, silently. Info, because
    // BOTH requests were served - by a bigger buffer rather than by dropping less.
    std::string reason;
    if (slabs_short && need.needed()) {
        reason = "the session learned its in-flight count (" + std::to_string(learned) +
                 ") and the frame's block budget was exhausted, so the block storage was replaced (a new "
                 "buffer; the old one is parked):";
    }
    else if (slabs_short) {
        reason = "the session learned its in-flight count (" + std::to_string(learned) +
                 "), so the per-frame block storage was re-laid out (a new buffer; the old one is parked):";
    }
    else {
        reason = "the frame's block budget was exhausted, so the block storage grew (a new buffer; the old "
                 "one is parked):";
    }
    const auto append = [&reason](const char* what, std::uint32_t from, std::uint32_t to) {
        if (from != to) {
            reason += std::string(" ") + what + " " + std::to_string(from) + " -> " + std::to_string(to) + ";";
        }
    };
    if (slabs_short) {
        append("slabs", before.views.slabs, wanted.views.slabs);
    }
    append("views", before.views.blocks_per_frame, wanted.views.blocks_per_frame);
    append("draws", before.draws.blocks_per_frame, wanted.draws.blocks_per_frame);
    append("lights", before.lights.blocks_per_frame, wanted.lights.blocks_per_frame);
    append("shadows", before.shadows.blocks_per_frame, wanted.shadows.blocks_per_frame);
    reportDiagnostic(vn::graphics::DiagnosticSeverity::Info, vn::graphics::DiagnosticCategory::ContentSkipped, vn::String::fromUtf8(reason));
}

bool VsgBackend::adoptBlockStorage(std::shared_ptr<BlockStorage> storage)
{
    if (storage == nullptr) {
        return false;
    }
    // The assembly first: it is what hands the storage to a record, and its cached sets bind the buffer
    // (the replaced sets are parked through the session's own window - see ContentSets::repoint).
    if (d->assembly != nullptr) {
        d->assembly->repoint(*storage, d->clock.timeline(), d->retirement);
    }
    std::shared_ptr<BlockStorage> previous = std::move(d->storage);
    d->storage                             = std::move(storage);
    if (previous != nullptr) {
        // The old bytes may still be named by a submitted command buffer, so they leave through the window
        // every replaced object gets - and when there is no window to park through yet, they are KEPT
        // (memory spent, never freed early: the same fallback ContentAssembly::beginFrame takes).
        //
        // ONE PARK, ONE DEVICE: the queue is the device's and its watermark is what EVERY window has proven (see
        // core::DeviceClock), so this is the same act whether one window or four draw with the storage.
        if (!d->retirement.retire(d->clock.timeline(), [previous]() {})) {
            d->kept_storage.push_back(std::move(previous));
        }
    }
    return true;
}

void VsgBackend::reportUnserved(std::size_t slot) noexcept
{
    if (slot >= kUnservedCount || !d->unserved_reports[slot].shouldReport()) {
        return;
    }
    reportDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                     vn::graphics::DiagnosticCategory::UnsupportedRequest,
                     vn::String::fromUtf8(std::string(kUnservedNames[slot]) + " has nowhere to go: every part of the frame protocol needs a session that is "
                                                                              "up (see .ai/design/vsg-backend.md)"));
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

BlockStorage* BackendContentAccess::storage(VsgBackend& backend) noexcept
{
    return backend.d->storage.get();
}

bool BackendContentAccess::useBlockStorage(VsgBackend& backend, const BlockStorage::Layout& layout)
{
    if (backend.d->device == nullptr || backend.d->assembly == nullptr) {
        return false;
    }
    std::shared_ptr<BlockStorage> replacement = BlockStorage::create(backend.d->device, layout);
    return backend.adoptBlockStorage(std::move(replacement));
}

void BackendContentAccess::assumeInFlightSlots(VsgBackend& backend, std::uint32_t frames_in_flight) noexcept
{
    backend.d->stated_in_flight = frames_in_flight;
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
    // The window the last frame was for (the tests that use this seam drive one window, and executeFrame records
    // which one that was - see Data::driving). A frame that named NO window leaves none: the off-screen form has no
    // default framebuffer to answer with.
    if (backend.d->driving >= backend.d->windows.size()) {
        return nullptr;
    }
    return SessionContentAccess::windowTarget(*backend.d->windows[backend.d->driving]->session);
}

vn::raw_ptr<vn::graphics::WindowPresentTarget> BackendContentAccess::surfaceTarget(VsgBackend& backend, std::size_t index) noexcept
{
    if (index >= backend.d->windows.size()) {
        return nullptr;
    }
    return backend.d->windows[index]->target.get();
}

MaterialImages* BackendContentAccess::images(VsgBackend& backend) noexcept
{
    return backend.d->images.get();
}

ContentStore* BackendContentAccess::store(VsgBackend& backend) noexcept
{
    return backend.d->store.get();
}

core::VariantPool& BackendContentAccess::pool(VsgBackend& backend) noexcept
{
    return backend.d->pool;
}

} // namespace detail

VN_VSG_NS_END
