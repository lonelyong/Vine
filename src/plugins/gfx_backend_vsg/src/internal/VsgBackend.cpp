#include <vine/backend_vsg/internal/VsgBackend.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
#include <vine/backend_vsg/internal/BackendContent.hpp>
#include <vine/backend_vsg/internal/BlockStorage.hpp>
#include <vine/backend_vsg/internal/ContentAssembly.hpp>
#include <vine/backend_vsg/internal/ContentStore.hpp>
#include <vine/backend_vsg/internal/Device.hpp>
#include <vine/graphics/backend/FrameGate.hpp>
#include <vine/backend_vsg/internal/FrameScratch.hpp>
#include <vine/backend_vsg/internal/HostReadback.hpp>
#include <vine/backend_vsg/internal/HostTargets.hpp>
#include <vine/backend_vsg/internal/MaterialImages.hpp>
#include <vine/backend_vsg/internal/OffscreenSession.hpp>
#include <vine/backend_vsg/internal/PassRegistry.hpp>
#include <vine/backend_vsg/internal/WindowSession.hpp>
#include <vine/backend_vsg/internal/SessionContent.hpp>
#include <vine/backend_vsg/internal/TransferStaging.hpp>
#include <vine/backend_vsg/internal/ViewBlock.hpp>
#include <vine/backend_vsg/internal/VsgExecutor.hpp>
#include <vine/backend_vsg/internal/VsgDriver.hpp>
#include <vine/backend_vsg/internal/WindowTarget.hpp>
#include <vine/backend_vsg/support/VsgSceneRules.hpp>
#include <vine/backend_vsg/support/VsgUtils.hpp>

VN_BACKEND_VSG_NS_BEGIN

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

/// @brief The spelling of "this frame belongs to no window": see VsgExecutor::kNoWindow.
constexpr std::size_t kNoWindow = VsgExecutor::kNoWindow;

/**
 * @brief The frame THIS THREAD is driving, while it drives one: what a window target's present() reads back.
 *
 * WHY A THREAD-LOCAL AND NOT A DEVICE FIELD. The present of a frame arrives as `PresentTarget::present()` on the
 * window object the frame named - a call with no frame parameter - and it is made from INSIDE the frame body, on the
 * frame's own thread. Two drivers' frames may run at once (see backend::FrameGate), so a device-wide "the frame in flight"
 * would hand one frame's plan to another's present. The frame's own state lives in the driving executor (see
 * VsgExecutor::FrameDrive); this is the one pointer that lets the callback find it again, and it is scoped to the
 * frame body exactly like the vk backend's frame-body depth.
 */
struct FrameInFlight {
    VsgExecutor*           executor{ nullptr };  // The driver whose frame is open on this thread.
    FrameScratch*          scratch{ nullptr };   // The driving session's scratch (its plan is the frame's).
    std::size_t            driving{ kNoWindow }; // The window index the frame drives, or kNoWindow.
    api::WindowSession*    window{ nullptr };    // The window's session, or null for the off-screen form.
    api::OffscreenSession* offscreen{ nullptr }; // The off-screen session, or null for the window form.
};

/// The frame of THIS thread (see FrameInFlight): a file-scope object that is state, so it keeps the s_ prefix.
thread_local FrameInFlight* s_frame_in_flight = nullptr;

/// @brief Points the thread-local at @p frame while it lives, and puts the previous one back (nesting safe).
struct FrameInFlightScope {
    explicit FrameInFlightScope(FrameInFlight* frame) noexcept
      : previous(take(frame))
    {
    }

    ~FrameInFlightScope()
    {
        s_frame_in_flight = previous;
    }

    FrameInFlightScope(const FrameInFlightScope&)            = delete;
    FrameInFlightScope& operator=(const FrameInFlightScope&) = delete;

    /// @brief Installs @p frame and answers what was installed before it.
    static FrameInFlight* take(FrameInFlight* frame) noexcept
    {
        FrameInFlight* const before = s_frame_in_flight;
        s_frame_in_flight           = frame;
        return before;
    }

    FrameInFlight* previous{ nullptr };
};

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
/// One pass' resolved shape and image offers: what it draws into, and what its declared inputs offer.
///
/// @see offerInputs for what "served" means, and recordContent's file note for why the two verbs share it.
struct PassOffers {
    bool                          served{ false }; // false when this backend holds nothing for the pass' target.
    core::RenderPassCompatibility compatibility{}; // The target's shape (the pipeline key's half).
    std::uint32_t                 width{ 0 };      // The target's extent (the view block's contract).
    std::uint32_t                 height{ 0 };
};

/// @brief Resolves one pass' target shape and its declared inputs' image offers.
///
/// WHICH TARGET decides the shape and the extent: a pass drawing into the window is compatible with the window's own
/// shape and reconstructs from the window's extent, and a pass drawing into one of the host's targets is compatible
/// with THAT target's shape. A pass whose target this backend does not hold answers `served == false`: the executor
/// reports the pass instead of drawing it somewhere else. A frame with NO WINDOW (the off-screen form) has no
/// default-framebuffer target, so a pass naming none has nowhere to draw and answers the same.
///
/// THE INPUTS ARE ONE ENTRY PER DECLARATION, in declaration order: what the input target offers a shader - each of
/// its colour attachments, plus its depth while the plan says that one is sampleable. An input this backend does not
/// hold offers nothing: the content recorder reads one entry per declaration, so a shorter span would read past its
/// end - and "nothing offered" is the fact it reports, which a missing entry would not be.
///
/// ONE WALK FOR BOTH VERBS (see api/ContentPlan): the shape and offers a PREPARE resolves and the ones a frame
/// resolves are the same questions, so they share this function - "what prepare built" and "what the frame asks
/// for" cannot drift (`G1`; `T32`).
[[nodiscard]] PassOffers offerInputs(const core::CompiledPass&                                   pass,
                                     const core::CompiledFrame&                                  frame,
                                     WindowTarget*                                               window,
                                     HostTargets&                                                targets,
                                     MaterialImages&                                             images,
                                     vn::graphics::Diagnostics&                                  diagnostics,
                                     std::vector<InputImages>&                                   input_scratch,
                                     std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>>& input_views,
                                     std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>>& input_screen_views,
                                     std::vector<std::vector<::vsg::ref_ptr<::vsg::Sampler>>>&   input_color_samplers)
{
    PassOffers offers;
    if (const core::CompiledTarget& compiled_target = frame.targets[pass.target_index]; compiled_target.target == nullptr) {
        if (window == nullptr) {
            return offers; // no window: a pass that names no target of its own has nowhere to draw
        }
        offers.compatibility = window->shape().compatibility();
        offers.width         = window->width();
        offers.height        = window->height();
    }
    else {
        const HostTargets::Entry* entry = targets.find(compiled_target.target);
        if (entry == nullptr || entry->target == nullptr) {
            return offers;
        }
        offers.compatibility = entry->target->shape().compatibility();
        offers.width         = entry->target->width();
        offers.height        = entry->target->height();
    }
    offers.served = true;

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
            vn::backend_vsg::detail::TextureReject reject = vn::backend_vsg::detail::TextureReject::Ok;
            // The state belongs to the INPUT: the same texture declared with two states uploads once and
            // yields two samplers (see MaterialImages). No state = the backend's input sampler, the
            // behavior every imported input had before state existed.
            const SamplerImage sampled =
                pass.inputs[index].sampler.has_value() ? images.acquire(texture, *pass.inputs[index].sampler, reject) : images.acquire(texture, reject);
            if (reject != vn::backend_vsg::detail::TextureReject::Ok || sampled.view == nullptr) {
                diagnostics.report(vn::graphics::DiagnosticSeverity::Warning,
                                   vn::graphics::DiagnosticCategory::ContentSkipped,
                                   vn::String(u8"pass input texture could not be sampled; the input binds the white "
                                              u8"stand-in: ") +
                                       vn::backend_vsg::detail::textureRejectMessage(reject, *texture));
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
    return offers;
}

/// @brief THE FRAME'S DEVICE HALF (see api/ContentPlan): opens the content frame and resolves every pass into @p plan.
///
/// The caller surrounds this call with the device's content lock: it touches the tables, the compiled halves, the
/// declared sets, the stream uploads, the material images, the block storage's ring and the targets - one
/// acquisition for the whole frame, which is the trade `D2` accepts. What it produces is values; the recording that
/// follows reads them and takes no lock (`C3` / `S8c-2`).
///
/// A pass the backend holds no target for produces no slot in the plan - exactly the pass the recorded frame skips,
/// and the executor reports it.
void resolveContent(ContentAssembly&                                            assembly,
                    WindowTarget*                                               window,
                    HostTargets&                                                targets,
                    MaterialImages&                                             images,
                    vn::graphics::Diagnostics&                                  diagnostics,
                    core::FrameTimeline&                                        timeline,
                    core::RetirementQueue&                                      retirement,
                    float                                                       frame_seconds,
                    const core::CompiledFrame&                                  frame,
                    ContentPlan&                                                plan,
                    std::vector<InputImages>&                                   input_scratch,
                    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>>& input_views,
                    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>>& input_screen_views,
                    std::vector<std::vector<::vsg::ref_ptr<::vsg::Sampler>>>&   input_color_samplers)
{
    plan.beginFrame(frame.passes.size());
    // THE FRAME'S TABLES AND ITS SWEEPS (see api/ContentAssembly): the facts, the streams, the images, the arena's
    // returned slots, the halves and sets whose content left, the sampled-input sets - and the block storage's ring,
    // whose rotation is the frame's first act.
    (void)assembly.beginFrame(frame, timeline, retirement);

    for (const core::CompiledPass& pass : frame.passes) {
        if (pass.draws.empty()) {
            continue; // a pass with no draws: its clear is the plan's own and records without help
        }
        const PassOffers offers = offerInputs(pass, frame, window, targets, images, diagnostics, input_scratch, input_views, input_screen_views, input_color_samplers);
        if (!offers.served) {
            continue;
        }
        // The view block is the PASS' (the content layer's contract), built from the first drawing call's camera -
        // the engine's passes draw through one camera per scope, so the first is the pass'.
        const vn::graphics::VineViewBlock view_block = buildViewBlock(pass.draws[0].camera, frame_seconds, offers.width, offers.height);
        (void)assembly.resolve(plan, pass, offers.compatibility, input_scratch, std::as_bytes(std::span<const vn::graphics::VineViewBlock>(&view_block, 1U)));
    }
}

/// @brief THE WARM-UP'S WALK (see RenderFrameExecutor::prepareFrame): the same offers a frame resolves, prepared.
///
/// The per-pass shape and image offers are resolved exactly as a frame resolves them - that is what makes a prepared
/// identity the one the frame asks for - and then the pass is PREPARED instead of resolved into values, so nothing is
/// kept and no block is written (the ring's rotation belongs to the frame, see api/ContentAssembly::prepare).
bool prepareContent(ContentAssembly&                                            assembly,
                    WindowTarget*                                               window,
                    HostTargets&                                                targets,
                    MaterialImages&                                             images,
                    vn::graphics::Diagnostics&                                  diagnostics,
                    core::FrameTimeline&                                        timeline,
                    core::RetirementQueue&                                      retirement,
                    const core::CompiledFrame&                                  frame,
                    std::vector<InputImages>&                                   input_scratch,
                    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>>& input_views,
                    std::vector<std::vector<::vsg::ref_ptr<::vsg::ImageView>>>& input_screen_views,
                    std::vector<std::vector<::vsg::ref_ptr<::vsg::Sampler>>>&   input_color_samplers)
{
    (void)assembly.prepare(frame, timeline, retirement);
    bool complete = true;
    for (const core::CompiledPass& pass : frame.passes) {
        if (pass.draws.empty()) {
            continue;
        }
        const PassOffers offers = offerInputs(pass, frame, window, targets, images, diagnostics, input_scratch, input_views, input_screen_views, input_color_samplers);
        if (!offers.served) {
            continue; // exactly the pass a recorded frame skips, so both verbs answer the same set
        }
        complete = assembly.prepare(pass, offers.compatibility, input_scratch) && complete;
    }
    return complete;
}

/**
 * @brief Builds a diagnostic about the BACKEND itself: severity, category, stable code and the sentence.
 *
 * @details
 * - The code is stated INSIDE the report call that consumes this value, which is the shape
 *   `check_diagnostic_formats.py` scans for: a report a host matches on says which cause it is, at the call.
 *
 * @param severity How bad it is.
 * @param category The coarse class.
 * @param code     The stable identifier of the cause (see scripts/diagnostic_codes.txt).
 * @param message  The human sentence (with the numbers in it).
 * @return The diagnostic, ready to hand to reportDiagnostic.
 */
[[nodiscard]] vn::graphics::RenderDiagnostic backendDiagnostic(const vn::graphics::DiagnosticSeverity severity,
                                                              const vn::graphics::DiagnosticCategory category,
                                                              const char8_t*                         code,
                                                              vn::String                             message)
{
    vn::graphics::RenderDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.category = category;
    diagnostic.code     = vn::String(code);
    diagnostic.message  = std::move(message);
    return diagnostic;
}

/**
 * @brief Builds the diagnostic for a REFUSED readback: the cause's stable code beside the sentence that explains it.
 *
 * @details
 * - ONE SWITCH FOR THE WHOLE ENUM, and no `default`: a refusal added to `HostReadbackRefusal` cannot compile without a
 *   `vsg.readback.*` code here, which is what keeps the vocabulary complete as the enum grows.
 * - THE TWO ENTRY POINTS SHARE IT (colour and depth): the cause belongs to the request, not to the attachment, and the
 *   sentence already names which cast was asked for.
 * - `None` is mapped too, and it means "this backend reached the report with no refusal": a defect here, said as one.
 *
 * @param refusal The classified refusal.
 * @param what    The entry point's name, as the sentence states it.
 * @return The diagnostic, ready to hand to reportDiagnostic.
 */
[[nodiscard]] vn::graphics::RenderDiagnostic readbackRefusalDiagnostic(const HostReadbackRefusal refusal, const char* what)
{
    const char8_t* code = u8"vsg.readback.refused";
    switch (refusal) {
    case HostReadbackRefusal::None:              code = u8"vsg.readback.refused"; break;
    case HostReadbackRefusal::NoTarget:          code = u8"vsg.readback.no_target"; break;
    case HostReadbackRefusal::UnknownTarget:     code = u8"vsg.readback.unknown_target"; break;
    case HostReadbackRefusal::NotBuilt:          code = u8"vsg.readback.not_built"; break;
    case HostReadbackRefusal::NotRecorded:       code = u8"vsg.readback.not_recorded"; break;
    case HostReadbackRefusal::UnknownAttachment: code = u8"vsg.readback.unknown_attachment"; break;
    case HostReadbackRefusal::UnreadableFormat:  code = u8"vsg.readback.unreadable_format"; break;
    case HostReadbackRefusal::BorrowedDepth:     code = u8"vsg.readback.borrowed_depth"; break;
    case HostReadbackRefusal::NoDevice:          code = u8"vsg.readback.no_device"; break;
    case HostReadbackRefusal::InsideFrame:       code = u8"vsg.readback.in_frame"; break;
    case HostReadbackRefusal::TransferFailed:    code = u8"vsg.readback.transfer_failed"; break;
    }
    return backendDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                             vn::graphics::DiagnosticCategory::ContentSkipped,
                             code,
                             readbackRefusalMessage(refusal, what));
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
    core::Observe             observe;
    /// THE TARGETS A PLAN RESOLVES BY IDENTITY, one table for every driver (see VsgTargetRegistry): a target is the
    /// device's to hold, and two drivers must resolve the same identities.
    VsgTargetRegistry registry;
    /// THE DEVICE'S OWN DRIVER's frame state (the aggregate answers `executor()`); every driver handed out by
    /// createSession() owns one of its own and SHARES @ref registry.
    VsgExecutor       executor{ diagnostics, registry };
    /// The drivers createSession() handed out (the device owns them; releaseSession destroys one).
    std::vector<std::unique_ptr<VsgDriver>> drivers;
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

    /**
     * @brief One window this device serves: its own session, and the target object the host holds.
     *
     * WHAT A WINDOW OWNS: the surface, the swapchain and the default-framebuffer target under them (all inside its
     * session). What it does NOT own is anything a second window would have to duplicate - the device, its compiled
     * programs and the buffers they read live on the backend (see core::DeviceClock as well).
     */
    struct WindowSlot {
        void*                         host_handle{ nullptr };
        std::unique_ptr<api::WindowSession> session;
        /// The object the SDK sees. Set for EVERY window, the first included: the device hands out what presents and
        /// is not one of them (see the header), and one identity per window is what keeps "which window is this frame
        /// for" answerable by a pointer compare.
        std::unique_ptr<vn::graphics::WindowPresentTarget> target;
        /// THE WINDOW'S OWN FRAME CLAIM: one frame at a time drives one swapchain, so a second frame naming this
        /// window (another driver's, or one prepared while this one records) waits for the first to end. The claim
        /// is what used to be the device gate - and it is where it belongs, because a swapchain is the window's.
        std::mutex frame;
    };

    std::vector<std::unique_ptr<WindowSlot>> windows; // In the order the host asked (0 = the device's own).    /// The session a frame that names NO window
                                                      // runs through (see executeFrame): built on the first such frame,
    /// torn down with the device. It is a SUBMIT carrier, not a window - it has no surface, acquires nothing and
    /// presents nothing.
    std::unique_ptr<api::OffscreenSession> offscreen;
    /// THE OFF-SCREEN SESSION'S FRAME CLAIM: the same rule the windows' slots state, for the one session a frame
    /// naming no target runs through (frames wait for each other there; the session is the device's one).
    std::mutex offscreen_frame{};
    /// THE DEVICE'S TWO-SIDED GATE (see backend::FrameGate): frame bodies SHARE it - two drivers' frames overlap -
    /// and host verbs (initialize / shutdown / readback / the frame report / the window set) take it EXCLUSIVE. A verb
    /// called from inside a body's thread gets an empty lease and answers a refusal instead of waiting for itself.
    vn::graphics::backend::FrameGate gate{};
    /// THE DEVICE'S CONTENT LOCK: the tables a frame reads - the store, halves, sets, uploads, images, targets, the
    /// pass registry, the variant pool, the block storage and the clock/park queue - are touched by ONE CONTENT PHASE
    /// at a time. It is taken once by the frame's HEAD (see openFrameHead) and once by the phase that assembles,
    /// records and commits (see commitFrameThrough); the collection and plan compilation in between run on the
    /// driver's own scratch and take no device lock (`S8c-1`; a lock-free record walk is `S8c-2`).
    std::mutex content{};
    ///
    /// WHAT THE FRAME PHASES SPENT ON `content` (see FrameLocking): the head, the commit and a warm-up take the lock
    /// through `lockContentForPhase`, which counts the acquisition and whether it had to block. Host verbs take
    /// `content` directly, because their use between frames belongs to no frame. ATOMIC because the report is read by
    /// a host thread while another driver's frame runs.
    std::atomic<std::uint64_t> content_lock_acquisitions{ 0 };
    std::atomic<std::uint64_t> content_lock_waits{ 0 };

    /**
     * @brief Takes the device's content lock for a FRAME PHASE, counting the acquisition and any wait.
     *
     * @details
     * - THE TRY IS WHAT MAKES "the frame waited" measurable: a failed try is counted, and the block follows - the same
     *   measurement the vk backend's cache lock takes, so the two reports compare.
     * - Re-entering it from inside a phase (a sink calling a verb that takes the lock) is a deadlock, which is why the
     *   content verbs ask `gate.inSharedEntry()` first (see reportReentry).
     *
     * @return The hold; it releases the content lock when it goes out of scope.
     */
    [[nodiscard]] std::unique_lock<std::mutex> lockContentForPhase()
    {
        content_lock_acquisitions.fetch_add(1U, std::memory_order_relaxed);
        std::unique_lock<std::mutex> held(content, std::defer_lock);
        if (!held.try_lock()) {
            content_lock_waits.fetch_add(1U, std::memory_order_relaxed);
            held.lock();
        }
        return held;
    }

    /** @brief Gets the content lock's cumulative numbers (see FrameLocking). */
    [[nodiscard]] vn::graphics::FrameLocking contentLockNumbers() const noexcept
    {
        return vn::graphics::FrameLocking{ content_lock_acquisitions.load(std::memory_order_relaxed),
                                           content_lock_waits.load(std::memory_order_relaxed) };
    }
    /// THE ONE DIAGNOSTIC ROUTE, ONE SENTENCE AT A TIME: every frame body and every host verb reports through it,
    /// and a host's sink is not required to be re-entrant. RECURSIVE because the sink itself may call back into the
    /// backend (a host verb from a diagnostics sink is exactly the reentry case `reportReentry` answers): that call
    /// reports on the same thread, and a plain mutex would turn the case into a deadlock instead of a refusal.
    std::recursive_mutex diagnostics_route{};
    /// THE EPISODES FRAME BODIES SHARE (`ReportOnce` is a plain flag): two drivers' frames may reach the same
    /// sentence at once (a foreign target, a frame without a session, a reentry), so asking "have I said this?" and
    /// saying it happen under this lock - never the content lock, because a report can be raised while a content
    /// phase is held (the frame head reports an unbuildable target from under it). RECURSIVE for the same reason
    /// the route is: a sink may reach another episode on its own thread.
    std::recursive_mutex episodes{};
    /// THE DEVICE'S completion clock and park queue, shared by every window (see core::DeviceClock): whether a buffer
    /// or a pipeline may be destroyed is a question about the whole device, because the windows draw with the same
    /// objects. A session driven on its own keeps its own pair (see api::WindowSessionOptions).
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

    /// The episode of "a frame was started from the thread that already drives one" (see executeFrame).
    vn::graphics::ReportOnce frame_reentry_report{};
    /// The episode of "a host verb was called from inside a frame body" (see reportReentry).
    vn::graphics::ReportOnce host_reentry_report{};
    /// TEST SEAM (see BackendContentAccess::setRecordSeam): a hook the RECORDING phase calls once per frame, with the
    /// device's content lock NOT held - the one place a test can hold a frame open while another driver's whole frame
    /// runs, which is how the overlap witness states "the recording takes no device lock". Set between frames; a
    /// frame body reads it once (see commitFrameThrough).
    std::function<void()> record_seam{};
    /// What the frame's steps have let go; zero while the host holds everything.
    std::size_t                             released_objects{ 0 };
    std::size_t                             released_textures{ 0 };
    std::size_t                             released_streams{ 0 };

    [[nodiscard]] VsgExecutor::UploadCounters readUploadCounters() const noexcept
    {
        VsgExecutor::UploadCounters counters;
        if (assembly != nullptr) {
            const StreamUploads& uploads = assembly->uploads();
            counters.streams           = uploads.uploads();
            counters.stream_bytes      = uploads.uploadedBytes();
            counters.stream_releases   = uploads.retired();
        }
        if (images != nullptr) {
            counters.textures         = images->built();
            counters.texture_bytes    = images->uploadedBytes();
            counters.texture_releases = images->released();
        }
        return counters;
    }

    /// The last EXECUTED frame's own upload/release numbers; absent until a frame has run (an unmeasured frame must
    /// not read like an empty one). The LAST frame to end, whichever driver ran it: the report describes one frame.
    std::optional<vn::graphics::FrameUploads> last_uploads{};
    /// What that frame spent on the content lock (see FrameLocking); absent until one ran, for the same reason.
    std::optional<vn::graphics::FrameLocking> last_locking{};
    /// The counters of the last EXECUTED frame (see FrameRecorder::beginFrame: they describe ONE frame, so the device
    /// keeps the last one it saw rather than adding them up).
    vn::graphics::FrameCounters last_counters{};
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
        [this](const vn::graphics::RenderDiagnostic& diagnostic) {
            // THE WHOLE VALUE: a code, a subject and a limit are what a host matches on, and rebuilding the diagnostic
            // from three of its fields would drop exactly those (the SDK's report is one value, see RenderDiagnostic).
            // ONE SENTENCE AT A TIME: frames on two threads report through this route, and the host's sink is not
            // required to be re-entrant (the same rule the vk backend's report sink takes).
            std::lock_guard<std::recursive_mutex> guard(d->diagnostics_route);
            reportDiagnostic(diagnostic);
        });
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
    // A HOST VERB: it creates or replaces the device, its windows and the content world, so it owns the gate - no
    // frame body runs beside it. Called from inside a frame body (a sink or a hook) it answers false instead of
    // waiting for itself (see backend::FrameGate).
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        return false;
    }

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

            api::WindowSessionOptions options;
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
            // THE INSTRUMENTATION IS THE SESSION'S, not the device's (S8c): the recorder RESETS the counters when a
            // frame opens (see FrameRecorder::beginFrame), so two drivers counting into one object would overwrite
            // each other's answer - and `frameReport()` quotes the last frame to END, which the commit copies up.
            // Null asks the session for its own (the fallback it already keeps).

            // A KEEP IS NOT A REPLACEMENT: this window is on the surface it is already on, so its device, its
            // swapchain and every compiled pipeline survive, and the content world below stays with them (the
            // session's own decision table is what answers this, see WindowSession::wouldKeep).
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
    // what capabilities() reports - one fact, one source. The device answers with a BIT SET, while the limit is a
    // COUNT, so the intersection is collapsed to the largest count it offers (a host that read the bits would be
    // told "15" by a device that offers 1/2/4/8 - see ADR-2026-11-14-sample-count-refused-not-lowered).
    if (const ::vsg::PhysicalDevice* physical = d->device->getPhysicalDevice(); physical != nullptr) {
        const VkPhysicalDeviceLimits& limits = physical->getProperties().limits;
        d->targets.setMaxSamples(vn::backend_vsg::largestSampleCount(
            std::min(limits.framebufferColorSampleCounts, limits.framebufferDepthSampleCounts)));
    }

    // THE CONTENT WORLD BELONGS TO THE DEVICE, and is built ONCE: a window that came up, was re-attached or joined
    // later does not invalidate what was uploaded or compiled on the same device.
    resetFrameScratch();
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
    // THE DEFAULT PROGRAM was announced before the device came up (the engine's own order), so it is tracked now -
    // the tables must answer for it before any plan names it. Each DRIVER announced its own (C8), so each is tracked.
    trackProgram(d->store.get(), d->executor.defaultProgram());
    if (d->store != nullptr) {
        d->store->track(d->executor.defaultProgramText());
    }
    for (const std::unique_ptr<VsgDriver>& driver : d->drivers) {
        trackProgram(d->store.get(), driver->executor().defaultProgram());
        if (d->store != nullptr) {
            d->store->track(driver->executor().defaultProgramText());
        }
    }
    return true;
}

void VsgBackend::ensureWindowAt(std::size_t index)
{
    while (d->windows.size() <= index) {
        auto window     = std::make_unique<Data::WindowSlot>();
        window->session = std::make_unique<api::WindowSession>();
        // EVERY window gets its own target object, including the first: the device holds and hands out what presents
        // (see the header), and it is not one of them - one shape for N windows, and "which window is this frame
        // for" stays a pointer compare.
        window->target  = std::make_unique<detail::WindowFacade>(*this, d->windows.size());
        d->windows.push_back(std::move(window));
    }
}

api::WindowSession& VsgBackend::sessionAt(std::size_t index)
{
    ensureWindowAt(index);
    return *d->windows[index]->session;
}

api::OffscreenSession* VsgBackend::ensureOffscreenSession()
{
    if (d->device == nullptr) {
        return nullptr; // no device came up: there is nothing to submit an off-screen frame on
    }
    // THE ONE LAZY PIECE EVERY WINDOWLESS FRAME NEEDS, AND TWO DRIVERS MAY ASK AT ONCE (S8c): the session is created
    // under the content lock, which is the lock a frame's head holds - so one thread builds it and the other finds it.
    // Without this, the loser of the race would overwrite a half-built viewer (measured: a segfault in a two-thread
    // case before this lock existed).
    std::lock_guard<std::mutex> guard(d->content);
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
    // The instrumentation is the SESSION's (see initialize's note): two drivers' frames each count into their own.
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
    // path never idles the device" has to stay checkable - see WindowSession::deviceWaits and RetentionStats::device_waits.
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        detail::SessionContentAccess::waitDeviceIdle(*window->session);
    }
    // A WINDOWLESS DEVICE HAS NO WINDOW TO IDLE ABOVE: the off-screen session is a frame source too, and its
    // submitted work is device-wide, so the same destructive rule applies to it. On a device with a window the
    // idle above already covers this work; waiting again is harmless and keeps the rule one sentence.
    if (d->offscreen != nullptr) {
        d->offscreen->waitDeviceIdle();
    }

    resetFrameScratch();
    d->executor.setWindow(nullptr);
    for (const std::unique_ptr<VsgDriver>& driver : d->drivers) {
        driver->executor().setWindow(nullptr);
    }
    d->executor.clearTargets(); // the registry is shared: one clear empties it for every driver
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
    // A HOST VERB, and the most destructive one: it waits for every frame body to end (see backend::FrameGate), so
    // "the last executeFrame returned" and "no frame is running" are the same fact before anything is destroyed.
    // Called from inside a frame body it does nothing: the device keeps what it has, which is the only safe answer.
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        return;
    }

    // THE GPU IS FINISHED BEFORE ANYTHING IT NAMES IS DESTROYED. The sessions PIPELINE frames (see
    // core::FrameTimeline and the slot rings), so "the last executeFrame returned" does not mean "the queue is
    // empty" - and this backend keeps no fence wait of its own outside the sessions. Destroying a material image, a
    // command graph or a buffer a submission still reads is a use-after-free inside the driver's worker thread, which
    // is exactly what a memory checker measured (see .ai/bugs/vsg-teardown-frees-in-flight-resources.md): the block
    // freed by MaterialImages::~Data here was read by lavapipe's thread afterwards. Every session is idled first, in
    // the device's own vocabulary (Viewer::deviceWaitIdle), and the wait is counted like every other device wait.
    if (d->offscreen != nullptr) {
        d->offscreen->waitDeviceIdle();
    }
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        if (window != nullptr && window->session != nullptr) {
            detail::SessionContentAccess::waitDeviceIdle(*window->session);
        }
    }

    // The device's objects NEXT (see releaseContentWorld), then the sessions, then the device itself: what is
    // destroyed must not be named by anything still alive. The off-screen session is one of those sessions - it
    // borrows the device and reports into the device's clock - so it goes before the clock and the device do.
    releaseContentWorld();
    d->drivers.clear(); // the handed-out drivers own no GPU objects; their device is about to go
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
    // THE DEVICE'S OWN DRIVER STARTS WITHOUT A WINDOW: the next device's frames name their own, and nothing may
    // read the old one's answer (handed-out drivers are destroyed just above).
    d->executor.drive().driving = VsgExecutor::kNoWindow;
}

vn::graphics::RenderFrameExecutor& VsgBackend::executor()
{
    // The frame half of this device is its own (see RenderDevice::executor): the aggregate answers both faces.
    return *this;
}

vn::graphics::RenderFrameExecutor* VsgBackend::createSession()
{
    // A DRIVER IS A FRAME'S OWN STATE (see VsgDriver): the device's own driver is the aggregate; one handed out here
    // owns its executor and SHARES the device's content and target table (S8b). The device owns what it hands out.
    //
    // A HOST VERB: it appends to the device's driver list, which a running frame of another thread reads through the
    // gate - so it owns the gate, and a call from inside a frame body answers null instead of waiting for itself
    // (creating drivers from a sink is wiring this layer cannot serve).
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        return nullptr;
    }
    auto driver = std::make_unique<VsgDriver>(*this, d->diagnostics, d->registry);
    auto* raw   = driver.get();
    d->drivers.push_back(std::move(driver));
    return raw;
}

void VsgBackend::releaseSession(vn::graphics::RenderFrameExecutor& session)
{
    // Releasing the device's own executor is a NO-OP (the SDK's rule), so a host may release what it borrowed
    // unconditionally. One of ours is destroyed here - the device owns it, the caller never does.
    //
    // A HOST VERB: the gate makes "the driver is gone" and "no frame of it is running" one fact. From inside a frame
    // body it does nothing (the caller is misusing the borrow rule, and destroying a driver a frame is running on
    // would be a use-after-free).
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        return;
    }
    std::erase_if(d->drivers, [&session](const std::unique_ptr<VsgDriver>& driver) { return driver.get() == &session; });
}

vn::raw_ptr<vn::graphics::WindowPresentTarget> VsgBackend::createSurfaceTarget(void* native_handle, vn::graphics::SurfaceTargetResult* why)
{
    // A HOST VERB: it appends to the device's window list, which every frame body reads - so it owns the gate. From
    // inside a frame body it answers UnsupportedSurface rather than waiting for itself (see backend::FrameGate).
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        if (why != nullptr) {
            *why = vn::graphics::SurfaceTargetResult::UnsupportedSurface;
        }
        return nullptr;
    }

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
                reportDiagnostic(backendDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                                                   vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                                   u8"vsg.window.occupied",
                                                   vn::String::fromUtf8("a window this device already serves was asked for again: it was "
                                                                        "left alone (one live swapchain per window - see "
                                                                        "createSurfaceTarget, and .ai/design/graphics-window-device-"
                                                                        "viewer.md)")));
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
    // The window set is host-announced (createSurfaceTarget owns the gate), but the answer is a device-wide fact a
    // frame body's thread may also ask for - so it is read under the content lock, which every frame's head takes.
    std::lock_guard<std::mutex> guard(d->content);
    return d->windows.size();
}

void VsgBackend::openFrameHead(VsgExecutor& executor, FrameScratch& scratch, const vn::graphics::FrameDescription& frame, bool executing)
{
    // ONE ACQUISITION OF THE DEVICE'S CONTENT LOCK, and the whole reason this step exists (`G10`): the walk below
    // resolves every DEVICE fact the frame will read - the pass numbers, the targets it draws into, the facts it
    // samples, the content it names and the storage it writes into - so the collection and plan compilation that
    // follow touch the session's own memory and nothing else. A per-lookup lock in the record walk would pay a mutex
    // ~2000 times a frame (the trade `D2` rejects); this pays it twice.
    //
    // THE BASELINE IS TAKEN BEFORE THIS PHASE TAKES THE LOCK (see FrameReport::locking): the delta the frame publishes
    // is what its own phases spent, so the head's own acquisition has to be inside it.
    executor.drive().locking_before = d->contentLockNumbers();
    const std::unique_lock<std::mutex> guard = d->lockContentForPhase();

    // WHAT THE LAST FRAME'S UPLOADS NO LONGER NEED, BEFORE THIS ONE RECORDS. A staging buffer is the source of one
    // vkCmdCopyBuffer; the moment that copy's fence signals, the bytes are dead weight - and vsg keeps them for the life
    // of the session (measured: 1.2 GB after a 704 MB model, one block per frame slot because uploads rotate through
    // them). Running the sweep here is the whole trigger: a host asks for nothing, and a copy that has not finished yet is
    // simply picked up by a later frame.
    if (d->assembly != nullptr) {
        (void)releaseFinishedUploadStaging();
    }

    // THE STORAGE IS BROUGHT UP TO WHAT THIS FRAME NEEDS HERE - between frames, where no recording is in progress and
    // the previous frame's bytes can be left behind safely: a storage a frame outgrew is replaced (see the growth
    // note), and so is one the session has LEARNED to be too shallow for the in-flight count (see the in-flight floor
    // note). Both are answered by one replacement.
    growBlockStorageIfNeeded();

    // AN EXECUTED FRAME REPLACES THE DEVICE'S UPLOAD ANSWER HERE, BEFORE ANY WORK: a frame that fails before its tail
    // reports ZERO, and an earlier frame's numbers must not survive into it (see FrameReport::uploads). A warm-up is
    // not a frame and leaves the answer describing the last EXECUTED one.
    if (executing) {
        d->last_uploads = vn::graphics::FrameUploads{};
        // The same rule for the lock numbers: absent is better than a previous frame's answer (see FrameLocking).
        d->last_locking.reset();
    }

    // THE FRAME'S PASS NUMBERS, ITS TARGETS AND ITS CONTENT: everything below names DEVICE tables, so it happens once,
    // here, and the scratch carries the answers into the collection (see FrameScratch::pass_ids and the note on
    // PassRegistry: a pass must answer the same number for its whole life, whatever frame first names it).
    scratch.pass_ids.clear();
    scratch.pass_ids.reserve(frame.passes.size());
    for (const vn::graphics::FrameDescription::Pass& entry : frame.passes) {
        if (entry.pass == nullptr) {
            scratch.pass_ids.push_back(0); // a frame's entry always names a pass; a null one is nothing to run
            continue;
        }
        scratch.pass_ids.push_back(d->passes.adopt(entry.pass));
        if (!entry.draw.has_value()) {
            continue;
        }
        const vn::graphics::PassDraw& description = *entry.draw;
        if (description.target != nullptr) {
            // A pass of THIS frame draws into it: a description that cannot become objects at all blocks the picture
            // and is reported once (see adoptHostTarget).
            (void)adoptHostTarget(description.target, /*picture_depends*/ true);
        }
        for (const vn::graphics::ImageSource& input : entry.inputs) {
            if (input.isProduced() && input.target != nullptr) {
                // The description of every produced input is kept current here: its objects were built when a pass
                // drew into it, and what has to be fresh is the facts row the plan resolves the input's shape from
                // (see HostTargets::observe).
                (void)d->targets.observe(*input.target);
            }
        }
        for (const vn::graphics::DrawDescription& draw : description.draws) {
            if (!draw.camera.present) {
                continue; // a drawing call with no view is no call at all (see DrawDescription)
            }
            if (draw.kind == vn::graphics::DrawDescription::Kind::Screen) {
                // The fragment stage is content like any other: the tables must answer its two texts and their
                // declarations when the plan records (the store's own walk covers full-screen calls' programs).
                const vn::graphics::ShaderProgramDescriptionPtr text =
                    draw.program_description != nullptr ? draw.program_description
                                                        : (draw.program != nullptr ? draw.program->description() : vn::graphics::ShaderProgramDescriptionPtr{});
                trackProgram(d->store.get(), vn::intrusive_ptr<const vn::graphics::ShaderProgram>(draw.program));
                if (d->store != nullptr) {
                    d->store->track(text);
                }
                continue;
            }
            if (draw.commands == nullptr) {
                continue;
            }
            if (d->store == nullptr) {
                // Collecting the draws would plan a picture nothing can render, which is the silent black frame this
                // backend refuses to be (see collectContentDraw's old note): said once, and nothing is tracked.
                reportUnserved(kUnservedDraw);
                continue;
            }
            // What the commands name becomes the store's live content: the IDENTITY and the DESCRIPTION are the values
            // a frame carries (`RenderCommand::node` / `description`), and the tables must answer for them when the
            // plan is recorded - including after the host has dropped its own handle (the store owns a share).
            for (const vn::graphics::RenderCommand& command : *draw.commands) {
                const vn::graphics::NodeId identity =
                    !command.node.isNull() ? command.node : (command.geometry != nullptr ? command.geometry->id() : vn::graphics::NodeId{});
                d->store->track(identity, command.geometryDescription());
                d->store->track(command.material);
                d->store->track(command.material.get(), command.materialDescription());
                d->store->track(command.program);
                d->store->track(command.programDescription());
            }
            if (description.program_override_description != nullptr) {
                // The override's own text is content like any other: the store must answer for it when the plan records,
                // even though no command names it.
                d->store->track(description.program_override_description);
            }
        }
    }

    // THE TARGET FACTS the plan is compiled against, read here because they are the device's targets (see
    // collectTargetFacts): the collection compiles against this snapshot while another driver's head may be
    // announcing a target.
    collectTargetFacts(scratch, executor.drive().driving);

    // THE FRAME'S UPLOAD BASELINE, after the staging sweep and before the books open: the delta the frame reports is
    // taken across the whole frame (see commitFrameThrough).
    executor.drive().uploads_before = d->readUploadCounters();
}

void VsgBackend::openFrameBooks(FrameScratch& scratch, std::size_t driving)
{
    // The frame's session mints the token and advances its own frame stamp; the recorder opens the plan for the same
    // token, so the description and the session's frame cannot disagree about which frame it is. A frame with no
    // window is the OFF-SCREEN form: the off-screen session opens it, and acquires nothing (see api::OffscreenSession).
    //
    // NO DEVICE LOCK: the token, the acquisition and the plan are the SESSION's (see the header's frame phases).
    core::FrameToken token{};
    if (driving < d->windows.size()) {
        token = sessionAt(driving).beginFrame();
    }
    else if (d->offscreen != nullptr) {
        token = d->offscreen->beginFrame();
    }
    (void)scratch.recorder.beginFrame(token); // it answers whether the frame's books could be opened (see its note)
}

void VsgBackend::collectFrame(FrameScratch& scratch, const vn::graphics::FrameDescription& frame)
{
    // THE VALUE, translated into this layer's own collection, in the order the value states. What the engine
    // compiled arrives as data (see FrameDescription), so the frame's own shape is what drives this loop - and
    // every call below is this layer's own piece of work, which is why the picture (and the plan) come out the
    // same whether the frame came from the engine or from a host driving one pass (RenderPass::execute).
    //
    // IT READS THE HEAD'S SNAPSHOT (see openFrameHead): the pass numbers come from the scratch, and nothing below
    // touches the device's pass registry, its targets or its content store - which is what lets two drivers' frame
    // bodies run at once (see backend::FrameGate).
    for (std::size_t index = 0; index < frame.passes.size(); ++index) {
        const vn::graphics::FrameDescription::Pass& entry = frame.passes[index];
        if (entry.pass == nullptr || index >= scratch.pass_ids.size()) {
            continue; // a frame's entry always names a pass; a null one is nothing to run
        }
        openPassScope(scratch, entry.pass, scratch.pass_ids[index]);
        announcePassOrder(scratch, entry.order);
        announcePassInputs(scratch, entry.inputs);
        if (entry.draw.has_value()) {
            const vn::graphics::PassDraw& description = *entry.draw;
            announceTarget(scratch, description.target);
            if (description.viewport.has_value()) {
                announceViewport(scratch, *description.viewport);
            }
            if (description.clear.has_value()) {
                announceClear(scratch, *description.clear);
            }
            announcePassState(scratch, description.state);
            if (description.depth_range.has_value()) {
                announceDepthRange(scratch, *description.depth_range);
            }
            for (const vn::graphics::DrawDescription& draw : description.draws) {
                if (draw.light_values.has_value()) {
                    announceLights(scratch, *draw.light_values);
                }
                if (!draw.camera.present) {
                    continue; // a drawing call with no view is no call at all (see DrawDescription)
                }
                if (draw.kind == vn::graphics::DrawDescription::Kind::Screen) {
                    collectScreenDraw(scratch, draw.source, draw.program.get(), draw.program_description, draw.camera);
                }
                else if (draw.commands != nullptr) {
                    // The override is a PASS fact (see PassDraw::program_override), so every drawing call of the
                    // pass is drawn with it; the command list itself is never rewritten. Its TEXT travels with it
                    // (PassDraw::program_override_description), which is what the recorder applies.
                    collectContentDraw(scratch, *draw.commands, draw.camera, description.program_override_description);
                }
            }
        }
        closePassScope(scratch);
    }
}

void VsgBackend::collectTargetFacts(FrameScratch& scratch, std::size_t driving)
{
    // The facts the plan is compiled against: the window as the default framebuffer, answered by the target
    // itself (see WindowTarget::facts), plus one row per host target this backend holds - built or not,
    // because the plan has to be able to say "this one has no objects yet", and because the compiler resolves
    // a pass' declared INPUTS from this same table (see HostTargets::facts).
    //
    // A FRAME WITH NO WINDOW has no default-framebuffer row at all: that row is what the frame's passes naming no
    // target resolve to, and compiling without it is how "this pass has nowhere to draw" becomes a reported skip
    // instead of a picture in a window nobody named.
    //
    // CALLED FROM THE FRAME'S HEAD (under the content lock): the rows are a COPY of the device's targets, and reading
    // them while another frame changes them is exactly the race the head exists to remove.
    scratch.facts.clear();
    if (driving < d->windows.size()) {
        if (WindowTarget* window = detail::SessionContentAccess::windowTarget(sessionAt(driving))) {
            scratch.facts.push_back(window->facts());
        }
    }
    for (const std::unique_ptr<HostTargets::Entry>& entry : d->targets.entries()) {
        core::TargetFacts row;
        d->targets.facts(*entry, row);
        scratch.facts.push_back(row);
    }
}

void VsgBackend::closeFrameBooks(FrameScratch& scratch)
{
    (void)scratch.recorder.endFrame();
    (void)scratch.recorder.swapBuffers(); // closes the frame's books: endFrame() alone only leaves the pass scope
    scratch.frame = &scratch.compiler.compile(scratch.recorder.description(), core::FrameFacts{ scratch.facts });
}

vn::graphics::PresentOutcome VsgBackend::executeFrame(const vn::graphics::FrameDescription& frame, vn::graphics::PresentTarget* present_to)
{
    // THE AGGREGATE IS THE DEVICE'S OWN DRIVER (see RenderDevice::executor): its frames run through its executor, the
    // same body every handed-out driver runs through its own (see executeFrameFor).
    return executeFrameFor(d->executor, frame, present_to);
}

vn::graphics::PresentOutcome VsgBackend::executeFrameFor(VsgExecutor& executor,
                                                      const vn::graphics::FrameDescription& frame,
                                                      vn::graphics::PresentTarget*          present_to)
{
    // TWO DRIVERS' FRAME BODIES MAY RUN AT ONCE (see backend::FrameGate): this body takes the gate's SHARED side. What
    // still excludes itself is the SESSION the frame runs in (a window owns one swapchain and one open frame - the
    // claim below) and the DEVICE'S CONTENT (one locked phase at a time, see commitFrameThrough). A frame started from
    // the thread that already drives one is REFUSED, not queued: waiting there would be the thread waiting for itself.
    const vn::graphics::backend::FrameGate::Shared body = d->gate.enterShared();
    if (!body) {
        std::lock_guard<std::recursive_mutex> guard(d->episodes);
        if (d->frame_reentry_report.shouldReport()) {
            reportDiagnostic(backendDiagnostic(vn::graphics::DiagnosticSeverity::Error,
                                               vn::graphics::DiagnosticCategory::PassProtocolViolation,
                                               u8"vsg.session.frame_reentry",
                                               u8"vsg: a frame was started from the thread that is already driving one "
                                               u8"(a hook or a sink drove another engine); this frame was refused - a "
                                               u8"thread drives one frame at a time"));
        }
        return vn::graphics::PresentOutcome::Refused;
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
            std::lock_guard<std::recursive_mutex> guard(d->episodes);
            if (d->foreign_target_report.shouldReport()) {
                reportDiagnostic(backendDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                                                   vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                                   u8"vsg.window.foreign_target",
                                                   vn::String::fromUtf8("a frame named a surface target this device did not hand out: "
                                                                        "nothing was recorded (a device serves the windows it created, "
                                                                        "see createSurfaceTarget)")));
            }
            return vn::graphics::PresentOutcome::Lost; // a surface was named and nothing could reach it
        }
    }

    // THE FRAME'S OWN STATE IS THE DRIVER'S (see VsgExecutor::FrameDrive): the scratch it collects into and the
    // session it drives are one frame's, so two drivers cannot hand each other a plan or a token. Its SESSION is
    // claimed for the WHOLE body below: one window owns one swapchain and one open frame, so a second frame naming it
    // waits for the first to end - which is exactly what the device gate used to do for every frame, now scoped to
    // the one resource that needs it (`G5`: the exclusion belongs to the swapchain, not to the device).
    VsgExecutor::FrameDrive&     drive = executor.drive();
    api::OffscreenSession*       offscreen = nullptr;
    std::unique_lock<std::mutex> session_claim{};
    if (present_to == nullptr) {
        offscreen = ensureOffscreenSession();
        if (offscreen == nullptr) {
            reportUnserved(kUnservedNoSession);
            // Nothing runs: the device the frame would be submitted on is not up, and nothing was going to be
            // presented either way.
            return vn::graphics::PresentOutcome::Skipped;
        }
        // NO WINDOW IN THE FRAME, SO NONE IN THE EXECUTOR: a pass that names no target has nowhere to draw, and
        // the plan it is compiled against says so (see closeFrameBooks). One off-screen session serves the device's
        // windowless frames, so two of them WAIT for each other (a supported pattern: the second frame is not
        // refused, it is queued behind the first).
        drive.driving = VsgExecutor::kNoWindow;
        executor.setWindow(nullptr);
        session_claim = std::unique_lock<std::mutex>(d->offscreen_frame);
    }
    else {
        // The window this frame belongs to: the executor's default-framebuffer target is ITS, and the frame's tail
        // finds the same drive state back through the thread-local (see presentWindow).
        drive.driving         = index;
        api::WindowSession& session = sessionAt(index);
        if (!session.initialized()) {
            reportUnserved(kUnservedNoSession);
            // Nothing runs and nothing presents: the session that would record it is not up, so the picture did not
            // reach the surface the frame named.
            return vn::graphics::PresentOutcome::Lost;
        }
        executor.setWindow(detail::SessionContentAccess::windowTarget(session));
        session_claim = std::unique_lock<std::mutex>(d->windows[index]->frame);
    }

    // THE FRAME'S SCRATCH IS THE DRIVING SESSION'S (see FrameScratch, `.ai/design/graphics-sessions.md` F9): a plan is
    // one frame's, and the session that drives it owns the arena the plan lives in. It is set on the DRIVER and cleared
    // on EVERY exit below - a later verb must not find a stale plan, and another driver must never see it.
    FrameScratch* const scratch = offscreen != nullptr ? offscreen->frameScratch() : sessionAt(index).frameScratch();
    if (scratch == nullptr) {
        reportUnserved(kUnservedNoSession);
        return vn::graphics::PresentOutcome::Skipped;
    }
    // THE DRIVER SETTING REACHES THE SESSION THAT DRIVES (see setDefaultContentProgram): a scratch may belong to a
    // session that came up after the setting was announced, so the value is handed over here - one pointer copy.
    (void)scratch->recorder.setDefaultContentProgram(executor.defaultProgramText().get());
    drive.scratch = scratch;

    // THE FRAME IN FLIGHT ON THIS THREAD, for the window target's present() (see FrameInFlight): the callback has no
    // frame parameter, and a device-wide answer would hand one frame's plan to another driver's present.
    FrameInFlight in_flight{ &executor, scratch, drive.driving, offscreen == nullptr ? &sessionAt(index) : nullptr, offscreen };
    const FrameInFlightScope in_flight_scope{ &in_flight };
    struct DriveCloser {
        VsgExecutor::FrameDrive& drive;
        ~DriveCloser()
        {
            // THE SCRATCH BORROW ENDS WITH THE FRAME (it points into the session's plan); the window index STAYS -
            // it is this driver's "the window its last frame was for", which the test seam and the diagnostics read
            // back (see BackendContentAccess::windowTarget).
            drive.scratch = nullptr;
        }
    } drive_closer{ drive };

    // THE FRAME'S HEAD - everything it reads from the DEVICE, under one acquisition of the content lock (see
    // openFrameHead).
    openFrameHead(executor, *scratch, frame, /*executing*/ true);

    // THE VALUE, on the SESSION'S OWN MEMORY: the collection and the plan compilation take no device lock, which is
    // where two drivers' frames genuinely overlap today (see the header's frame phases).
    openFrameBooks(*scratch, drive.driving);
    collectFrame(*scratch, frame);
    closeFrameBooks(*scratch);

    // THE FRAME'S TAIL, and it is this call's last step whichever way the frame is presented. WITH a window, the
    // executor presents through the target the frame named - PresentTarget::present is the mechanism, the executor is
    // the one making the call - and in this backend that call is what commits the frame and shows it. WITHOUT a
    // window there is nothing to present, so the off-screen session commits it inline. Either way the commit takes the
    // content lock once and reports the frame's own upload numbers (see commitFrameThrough).
    return offscreen != nullptr ? commitFrameThrough(executor, nullptr, offscreen)
                                : (present_to != nullptr ? present_to->present() : vn::graphics::PresentOutcome::Skipped);
}

vn::graphics::PrepareReport VsgBackend::prepareFrame(const vn::graphics::FrameDescription& frame, vn::graphics::PresentTarget* present_to)
{
    // THE AGGREGATE IS THE DEVICE'S OWN DRIVER: the same warm-up body every handed-out driver runs (see prepareFrameFor).
    return prepareFrameFor(d->executor, frame, present_to);
}

vn::graphics::PrepareReport VsgBackend::prepareFrameFor(VsgExecutor& executor,
                                                     const vn::graphics::FrameDescription& frame,
                                                     vn::graphics::PresentTarget*          present_to)
{
    // THE SAME GATE AS executeFrame, SHARED: a warm-up walks the content path and may record a transfer batch, so it
    // shares the machinery a frame drives - and two drivers' warm-ups may run at once, exactly like their frames.
    // `Refused` carries no reason - the report says which refusal it was.
    const vn::graphics::backend::FrameGate::Shared body = d->gate.enterShared();
    if (!body) {
        std::lock_guard<std::recursive_mutex> guard(d->episodes);
        if (d->frame_reentry_report.shouldReport()) {
            reportDiagnostic(backendDiagnostic(vn::graphics::DiagnosticSeverity::Error,
                                               vn::graphics::DiagnosticCategory::PassProtocolViolation,
                                               u8"vsg.session.frame_reentry",
                                               u8"vsg: a warm-up was started from the thread that is already driving a "
                                               u8"frame; it was refused - a thread drives one frame at a time"));
        }
        return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::Refused };
    }

    // NOTHING TO PREPARE WITHOUT THE CONTENT WORLD: it is what compiles a half, a set or a stream at all, and it
    // exists only while a device does (see initialize). The answer says so rather than reporting a zero it did not
    // earn.
    if (d->assembly == nullptr || d->store == nullptr || d->images == nullptr) {
        return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::NotReady,
                                            vn::graphics::EngineNotReady::NotInitialized };
    }
    // A FRAME IS OPEN ON THIS DRIVER (executeFrame between its books and its present): its plan and its recorded
    // packets belong to it, and collecting a second frame over them would corrupt both. Refused, never queued - and
    // asked of the DRIVER's own frame state, because another driver's open frame is its own state (S8c).
    if (executor.drive().scratch != nullptr && executor.drive().scratch->frame != nullptr) {
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
            std::lock_guard<std::recursive_mutex> guard(d->episodes);
            if (d->foreign_target_report.shouldReport()) {
                reportDiagnostic(backendDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                                                   vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                                   u8"vsg.window.foreign_target",
                                                   vn::String::fromUtf8("a frame named a surface target this device did not hand out: "
                                                                        "nothing was prepared (a device serves the windows it created, "
                                                                        "see createSurfaceTarget)")));
            }
            return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::Refused };
        }
        if (!sessionAt(index).initialized()) {
            return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::Refused };
        }
    }

    // THE SESSION THE FRAME WILL RUN IN, resolved exactly as executeFrame resolves it: a frame that names no surface
    // runs in the device's OFF-SCREEN session. Bringing that session up here is not opening a frame - it creates the
    // viewer and its transfer task, acquires nothing and presents nothing - and the warm-up needs it, because that
    // session's transfer task is where a prepared texture's copies go (see the flush below).
    api::OffscreenSession* const offscreen = index < d->windows.size() ? nullptr : ensureOffscreenSession();

    // THE SAME PER-SESSION CLAIM A FRAME TAKES (see executeFrameFor): a warm-up drives the session it prepares, and
    // two things cannot open one swapchain at once - so a warm-up on a session another frame is driving waits for it,
    // exactly as the frame path does.
    std::unique_lock<std::mutex> session_claim = index < d->windows.size()
                                                     ? std::unique_lock<std::mutex>(d->windows[index]->frame)
                                                     : std::unique_lock<std::mutex>(d->offscreen_frame);

    VsgExecutor::FrameDrive& drive = executor.drive();
    drive.driving                  = index;

    // THE WARM-UP COLLECTS INTO THE SESSION IT WILL RUN IN (see FrameScratch): the same scratch executeFrame would
    // use, so what it prepares is what the frame finds. Cleared on every exit below.
    FrameScratch* const scratch = offscreen != nullptr ? offscreen->frameScratch() : sessionAt(index).frameScratch();
    if (scratch == nullptr) {
        drive.driving = VsgExecutor::kNoWindow;
        return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::Refused };
    }
    // THE DRIVER SETTING REACHES THE SESSION THAT DRIVES, exactly as in executeFrame.
    (void)scratch->recorder.setDefaultContentProgram(executor.defaultProgramText().get());
    drive.scratch = scratch;
    FrameInFlight in_flight{ &executor, scratch, drive.driving, offscreen == nullptr ? &sessionAt(index) : nullptr, offscreen };
    const FrameInFlightScope in_flight_scope{ &in_flight };
    struct DriveCloser {
        VsgExecutor::FrameDrive& drive;
        ~DriveCloser() { drive.scratch = nullptr; } // the index stays: see executeFrame's closer
    } drive_closer{ drive };

    // THE SESSION IS TOLD A PREPARE WENT THROUGH IT (see OffscreenSession::notePreparedWork): a prepare creates
    // pipelines and flushes uploads without opening a frame, and the teardown's device idle follows that fact rather
    // than a frame counter.
    if (offscreen != nullptr) {
        offscreen->notePreparedWork();
    }

    // THE FRAME'S HEAD (the storage growth included, see openFrameHead) - and NOT as an executed frame: a warm-up
    // leaves the device's upload answer describing the last frame that really ran.
    openFrameHead(executor, *scratch, frame, /*executing*/ false);

    // THE PLAN, COMPILED FROM THE SAME VALUE AND THE SAME FACTS a frame compiles it from - the collection (see
    // collectFrame) and the target-fact table are the frame's own steps, so the identities prepared here are the
    // ones the frame will ask for. The recorder needs a TOKEN and this is not a frame, so it gets the "not a frame"
    // spelling (0): nothing downstream of prepare reads it - the content tables date their rows by the session's
    // clock, not by the plan's number.
    //
    // THE COUNTERS ARE THE SESSION'S OWN (see FrameScratch), so a warm-up does not disturb the device's answer at all:
    // it counts into the driving session's instrumentation, and `frameReport()` quotes the last EXECUTED frame.
    if (!scratch->recorder.beginFrame(core::FrameToken{})) {
        return vn::graphics::PrepareReport{ vn::graphics::PrepareReport::Outcome::Refused };
    }
    collectFrame(*scratch, frame);
    closeFrameBooks(*scratch);
    const core::CompiledFrame& plan = *scratch->frame;
    scratch->frame                  = nullptr; // PREPARE LEAVES NO FRAME TO PRESENT: there is nothing recorded

    // THE VALUE SIDE, AND ONLY IT - ONE CONTENT PHASE (see the header's frame phases): the same walk a frame's head
    // takes (`resolveContent` resolves each pass' shape and image offers) is run here with `prepareContent`, which
    // PREPARES what the frame would resolve (see api/ContentPass::prepare): the compiled halves, the declared sets,
    // the sampled-input sets, the stream uploads and the pipeline identities are what it builds, and what it produces
    // is no value at all. Running the frame's own walk is what makes "what prepare built" and "what the frame builds"
    // ONE implementation rather than two spellings that can drift (`G1`; `T32` pins the identity half, `T33` the
    // whole). Nothing here acquires, submits, presents or rotates the block ring.
    std::uint64_t built_halves    = 0;
    std::uint64_t built_sets      = 0;
    std::uint64_t built_uploads   = 0;
    std::uint64_t built_pipelines = 0;
    {
        // THE DEVICE'S CONTENT LOCK, held for this phase and no longer (see Data::content): what the walk touches -
        // the tables, the halves, the declared sets, the image cache, the variant pool and the block storage's
        // material slots - is the device's, and another driver's frame owns the same tables.
        const std::unique_lock<std::mutex> guard = d->lockContentForPhase();

        // THE MATERIAL ARENA IS SIZED BEFORE ANYTHING IS PREPARED, exactly as a frame's content phase does it: every
        // live row keeps a slot until the host releases it, so the preparation never loses a draw to a full arena.
        growMaterialArenaIfNeeded();

        const std::uint64_t halves_before   = d->assembly->halves().builds();
        const std::uint64_t sets_before     = d->assembly->sets().builds();
        const std::uint64_t uploads_before  = d->assembly->uploads().uploads();
        const std::uint64_t textures_before = d->images->built();
        const std::uint64_t pool_before     = d->pool.created();

        WindowTarget* const window_target = index < d->windows.size() ? detail::SessionContentAccess::windowTarget(sessionAt(index)) : nullptr;
        (void)prepareContent(*d->assembly,
                             window_target,
                             d->targets,
                             *d->images,
                             d->diagnostics,
                             d->clock.timeline(),
                             d->retirement,
                             plan,
                             scratch->input_scratch,
                             scratch->input_views,
                             scratch->input_screen_views,
                             scratch->input_color_samplers);

        // THE TEXTURES THIS CALL STAGED ARE COPIED NOW, through the frame's own transfer step - not a second copy path
        // (ADR-2026-11-14; backlog T39). `MaterialImages::acquire` only stages bytes into the image; the viewer's
        // transfer task is what copies them, and a warm-up never reaches a frame that would run it. Handing the images
        // to that task and waiting for its batch is what makes "prepared" mean the same thing for textures as for
        // streams. The bytes go as soon as the copies have run, so a descriptor set compiled later cannot copy them a
        // second time.
        ::vsg::ref_ptr<::vsg::TransferTask> transfer_task;
        if (index < d->windows.size()) {
            transfer_task = detail::SessionContentAccess::transferTask(sessionAt(index));
        }
        else if (offscreen != nullptr) {
            transfer_task = offscreen->transferTask();
        }
        if (transfer_task != nullptr && transfer_task->device != nullptr && d->images->queueUploads(*transfer_task->device, *transfer_task) > 0U) {
            if (detail::flushUploads(*transfer_task)) {
                (void)d->images->releaseUploadedStaging();
            }
        }

        // WHAT THIS CALL BUILT, read while the tables are still this phase's: a counter read after the lock is released
        // could be another driver's delta.
        built_halves    = d->assembly->halves().builds() - halves_before;
        built_sets      = d->assembly->sets().builds() - sets_before;
        // BOTH KINDS OF UPLOAD (see PrepareReport): the streams whose binds were built, and the texture images this call
        // staged (and, above, copied) - the same two kinds FrameUploads reports, so a host comparing the two is
        // comparing one vocabulary.
        built_uploads   = (d->assembly->uploads().uploads() - uploads_before) + (d->images->built() - textures_before);
        built_pipelines = d->pool.created() - pool_before;
    }

    vn::graphics::PrepareReport report;
    report.outcome = vn::graphics::PrepareReport::Outcome::Prepared;
    // THE SHAPE OF THE FRAME ASKED FOR, computed by the SDK (see FrameShape): the same function the direct backend
    // calls, over the same value, so the two reports agree by construction - and the rule is the one a recorded frame's
    // `FrameCounters` follows (a pass that draws something or announces a clear; a clear-only pass is a real pass).
    const vn::graphics::FrameShape shape = vn::graphics::frameShapeOf(frame);
    report.passes = shape.passes;
    report.draws  = shape.draws;
    // THE SHAPE THE FRAME ASKED FOR, not what this backend managed to walk (see PrepareReport): a pass the walk above
    // skipped because its target has no objects yet is still counted here, and the build deltas below are what say
    // how much of the request actually became objects. Two numbers, two questions - and neither pretends to be the
    // other.
    // WHAT THIS CALL BUILT, not what is resident: a second prepare of an unchanged frame answers zeros, which is the
    // property the call exists for.
    report.halves    = built_halves;
    report.sets      = built_sets;
    report.uploads   = built_uploads;
    report.pipelines = built_pipelines;
    return report;
}

vn::graphics::PresentOutcome VsgBackend::presentWindow(std::size_t index)
{
    // THE FRAME THIS THREAD IS DRIVING (see FrameInFlight): the target object's present() is called from inside the
    // frame body, on the frame's own thread, and the frame's state is the DRIVER's - a device-wide copy would hand
    // one driver's plan to another driver's present.
    FrameInFlight* const in_flight = s_frame_in_flight;
    if (in_flight == nullptr || in_flight->window == nullptr || in_flight->driving != index || in_flight->scratch == nullptr ||
        in_flight->scratch->frame == nullptr) {
        // No frame is open on this thread for that window (executeFrame closed none, or the host presents outside a
        // frame): there is a surface and nothing to hand it, which is a loss rather than "present nothing" - the
        // host re-attaches on it.
        return vn::graphics::PresentOutcome::Lost;
    }
    if (!in_flight->window->initialized()) {
        return vn::graphics::PresentOutcome::Lost;
    }
    return commitFrameThrough(*in_flight->executor, in_flight->window, nullptr);
}

void VsgBackend::resetFrameScratch() noexcept
{
    // A PLAN IS ONE FRAME'S AND THE SESSIONS HOLD THEM (see FrameScratch): a teardown or a re-initialize drops what
    // they collected, so no later present can commit a plan whose device-side objects are being replaced.
    const auto reset = [](FrameScratch* scratch) {
        if (scratch != nullptr) {
            scratch->frame = nullptr;
            scratch->facts.clear();
            scratch->pass_ids.clear();
            scratch->packets.clear();
            // THE CONTENT PLAN GOES WITH THEM: its values name device objects, and a plan that outlived a teardown
            // would keep a replaced storage's binds alive (memory spent, nothing dangling - but no reason to keep it).
            scratch->content.clear();
        }
    };
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        if (window->session != nullptr) {
            reset(window->session->frameScratch());
        }
    }
    if (d->offscreen != nullptr) {
        reset(d->offscreen->frameScratch());
    }
    // AND EVERY DRIVER'S SCRATCH BORROW, which points into one of those scratches (see FrameDrive): a drive that
    // outlived its plan would be a dangling scratch, not a stale one. The WINDOW INDEX stays: the window set
    // survives a re-initialize, and "the window the last frame was for" is read back after one (see
    // BackendContentAccess::windowTarget).
    const auto clear_drive = [](VsgExecutor& executor) { executor.drive().scratch = nullptr; };
    clear_drive(d->executor);
    for (const std::unique_ptr<VsgDriver>& driver : d->drivers) {
        clear_drive(driver->executor());
    }
}

vn::graphics::PresentOutcome VsgBackend::commitFrameThrough(VsgExecutor& executor, api::WindowSession* window, api::OffscreenSession* offscreen)
{
    if (window == nullptr && offscreen == nullptr) {
        return vn::graphics::PresentOutcome::Skipped;
    }
    // THE PLAN IS THE DRIVING SESSION'S (see FrameScratch): the one it collected into, never a copy on the device.
    FrameScratch* const frame_scratch = window != nullptr ? window->frameScratch() : offscreen->frameScratch();
    if (frame_scratch == nullptr || frame_scratch->frame == nullptr) {
        return vn::graphics::PresentOutcome::Skipped;
    }

    // THE FRAME'S CONTENT HAS THREE PHASES, AND THE DEVICE'S LOCK IS TAKEN TWICE (see Data::content; `C3` /
    // `S8c-2`):
    //   1. THE HEAD resolves every device fact the frame will read into the session's own plan - the tables, the
    //      compiled halves, the declared sets, the stream uploads, the material arena, the blocks and the pipeline
    //      identities - under one acquisition, because another driver's head touches the same tables;
    //   2. THE RECORDING turns those values into vsg nodes and takes NO DEVICE LOCK, which is where two drivers'
    //      content phases genuinely overlap (a held frame stops neither the other's head nor its commit);
    //   3. THE COMMIT places the recorded nodes in the frame's graph, sweeps what the host let go and submits,
    //      under one acquisition, because the targets, the store and the session's commit are the device's.
    // A host verb that only touches those tables (releaseContent, setRenderTarget, the default program) takes the
    // same lock, which is why it can run between the phases (R6's shape) but never during one.

    // WHAT THE FRAME ACTUALLY BUILT, as one delta (see FrameReport::uploads): counts and bytes the recording below
    // copies, and the entries the residency sweep lets go of. Read inside this phase (the counters are the tables'),
    // and published on EVERY exit - a frame that failed after recording still uploaded what it uploaded.
    const auto finish = [this, frame_scratch, &executor](vn::graphics::PresentOutcome outcome) {
        const VsgExecutor::UploadCounters after = d->readUploadCounters();
        vn::graphics::FrameUploads      moved;
        moved.stream_uploads   = after.streams - executor.drive().uploads_before.streams;
        moved.stream_bytes     = after.stream_bytes - executor.drive().uploads_before.stream_bytes;
        moved.stream_releases  = after.stream_releases - executor.drive().uploads_before.stream_releases;
        moved.texture_uploads  = after.textures - executor.drive().uploads_before.textures;
        moved.texture_bytes    = after.texture_bytes - executor.drive().uploads_before.texture_bytes;
        moved.texture_releases = after.texture_releases - executor.drive().uploads_before.texture_releases;
        executor.drive().last_uploads = moved;
        d->last_uploads               = moved;
        // WHAT THE FRAME SPENT ON THE CONTENT LOCK (see FrameLocking): its own phases' acquisitions, and the ones that
        // had to block. Read here, at the frame's tail, so the answer describes this frame and no other - and read
        // from atomics, because this lambda runs with the content lock held on some paths.
        const vn::graphics::FrameLocking lock_now = d->contentLockNumbers();
        d->last_locking = vn::graphics::FrameLocking{
            lock_now.acquisitions - executor.drive().locking_before.acquisitions,
            lock_now.waits - executor.drive().locking_before.waits,
        };
        // THE DEVICE'S FRAME ANSWER: the counters of the frame that just ended, copied from the SESSION that ran it
        // (each session owns its instrumentation, see initialize). A report taken later describes this frame.
        d->last_counters = frame_scratch->observe.counters();
        return outcome;
    };

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

    if (frame_scratch->frame->passes.empty()) {
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
        //
        // ONE PHASE: a frame with no content has no recording to overlap, so its whole device half is one acquisition
        // (the same opening the head does, and no plan to resolve).
        const std::unique_lock<std::mutex> guard = d->lockContentForPhase();
        if (d->assembly != nullptr) {
            (void)d->assembly->beginFrame(*frame_scratch->frame, timeline, retirement);
        }
        sweepAbandonedContent();
        const bool committed = window != nullptr ? executor.commit(*frame_scratch->frame, *window)
                                                 : executor.commit(*frame_scratch->frame, *offscreen);
        frame_scratch->frame = nullptr;
        return finish(settled(committed));
    }

    const ::vsg::ref_ptr<::vsg::CommandGraph> graph = window != nullptr ? detail::SessionContentAccess::makeFrameGraph(*window) : offscreen->makeFrameGraph();
    if (graph == nullptr) {
        // The frame's graph could not be built, so none of its picture was ever going to reach the surface.
        const std::unique_lock<std::mutex> guard = d->lockContentForPhase();
        frame_scratch->frame = nullptr;
        return finish(vn::graphics::PresentOutcome::Lost);
    }

    // PHASE 1 - THE HEAD: the frame drive's own order, once (see VsgExecutor): what the plan answered for its
    // targets becomes real and the frame's content is resolved into the session's plan, all of it under the device's
    // content lock. A host verb may run before or after this phase, never inside it.
    {
        const std::unique_lock<std::mutex> guard = d->lockContentForPhase();
        (void)executor.applyTargetPlans(*frame_scratch->frame, frame_scratch->facts, timeline, retirement);
        // THE MATERIAL ARENA IS SIZED BEFORE ANYTHING IS RESOLVED (see the declaration): every live row keeps a
        // slot until the host releases it, so the frame that would have run out is given a bigger one first.
        growMaterialArenaIfNeeded();
        // THE PLAN IS ONE FRAME'S: a frame that resolves nothing (a resumed session with no content world, or a
        // window whose session is not up) must not record the plan the LAST frame left.
        frame_scratch->content.clear();
        if (d->assembly != nullptr && d->images != nullptr && (window_target != nullptr || window == nullptr)) {
            resolveContent(*d->assembly,
                           window_target,
                           d->targets,
                           *d->images,
                           d->diagnostics,
                           timeline,
                           retirement,
                           frame_seconds,
                           *frame_scratch->frame,
                           frame_scratch->content,
                           frame_scratch->input_scratch,
                           frame_scratch->input_views,
                           frame_scratch->input_screen_views,
                           frame_scratch->input_color_samplers);
        }
    }

    // PHASE 2 - THE RECORDING, AND IT TAKES NO DEVICE LOCK (`C3` / `S8c-2`): every call in the plan is a value the
    // head resolved, so turning them into vsg nodes reads no table and waits for no other driver. This is the window
    // the overlap witness holds a frame open in (see BackendContentAccess::setRecordSeam).
    if (d->record_seam) {
        d->record_seam();
    }
    frame_scratch->packets.clear();
    frame_scratch->packets.reserve(frame_scratch->content.passes().size());
    for (const ContentPlan::Pass& pass : frame_scratch->content.passes()) {
        ::vsg::ref_ptr<::vsg::Node> content;
        (void)ContentPlan::record(pass, content);
        frame_scratch->packets.push_back(PassContent{ pass.pass, std::move(content) });
    }

    // PHASE 3 - THE COMMIT: the plan is recorded into this frame's graph, the host's abandoned content is swept and
    // the graph is the session's - which is the call that submits (and, for a window, presents), and the one whose
    // false answer means the writes never happened. All of it under the device's content lock: the targets, the
    // store and the session's commit are the device's.
    {
        const std::unique_lock<std::mutex> guard = d->lockContentForPhase();
        (void)executor.record(*frame_scratch->frame, graph, frame_scratch->packets);
        // WHAT THE HOST LET GO IS LET GO HERE: the frame's content has been recorded, and the commit below is
        // what advances the retirement queue - so this is the last moment a park can be dated against this frame
        // (see the frame drive's ordering note, and the ordering rule core::RetirementQueue states).
        sweepAbandonedContent();
        const bool assigned  = window != nullptr ? detail::SessionContentAccess::assignFrameGraphs(*window, ::vsg::CommandGraphs{ graph })
                                                 : offscreen->assignFrameGraphs(::vsg::CommandGraphs{ graph });
        bool       committed = false;
        if (assigned) {
            // The executor reports its own failure and marks what it wrote; the answer is whether the picture got out.
            committed = window != nullptr ? executor.commit(*frame_scratch->frame, *window)
                                          : executor.commit(*frame_scratch->frame, *offscreen);
        }
        frame_scratch->frame = nullptr;
        return finish(settled(committed));
    }
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
    // THE STAGING IS WRITTEN BY FRAME PHASES (an upload stages, a frame's head/commit frees it), so the sum is read
    // under the content lock - the same lock those phases hold (see openFrameHead and commitFrameThrough).
    std::lock_guard<std::mutex> guard(d->content);
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
    // A HOST VERB: the handle list is read by initialize() and by nothing a frame body touches, but the window set is
    // the device's - so it owns the gate, and a call from a sink does nothing instead of waiting for itself.
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        return;
    }
    // A session decision for THAT window: the next initialize() adopts the handle (or moves an established session
    // onto it - see core::planSessionMove, which the session applies).
    ensureWindowAt(index);
    d->windows[index]->host_handle = native_handle;
}

void* VsgBackend::windowHandleAt(std::size_t index) const noexcept
{
    // The SDK's question is "which surface am I on": a host surface was adopted, a window of our own is not
    // the host's to name - and before initialize() there is nothing to name. The window set is the device's, and a
    // frame body of another thread may be running, so the lock is the content lock's (the same one a frame head takes).
    std::lock_guard<std::mutex> guard(d->content);
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
    // A HOST VERB: following a surface rebuilds its swapchain, which must not overlap its own frame - so this owns the
    // gate (see backend::FrameGate) and, below, the WINDOW's claim. From inside a frame body it does nothing.
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        return;
    }
    d->announced_width  = width;
    d->announced_height = height;
    if (index >= d->windows.size()) {
        return; // a window that does not exist yet: the announced size is what it will be created at
    }
    // THE WINDOW'S OWN FRAME (see WindowSlot::frame): a swapchain rebuild under a running frame is what the claim
    // excludes, and the gate's exclusive side already excludes every body - this is the same fact stated once more,
    // for the frame that is prepared/executed on this window by THIS thread (the host leased, so none is).
    std::lock_guard<std::mutex> claim(d->windows[index]->frame);
    api::WindowSession& session = sessionAt(index);
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
    // THE AGGREGATE IS THE DEVICE'S OWN DRIVER: its announcement sets ITS driver's value (see adoptDefaultProgram).
    adoptDefaultProgram(d->executor, std::move(program));
}

void VsgBackend::adoptDefaultProgram(VsgExecutor& executor, vn::intrusive_ptr<const vn::graphics::ShaderProgram> program)
{
    // A DRIVER SETTING (`C8`): the value belongs to the driver that announced it, and every frame that driver runs
    // hands it to the driving session's recorder (see executeFrameFor). Two engines therefore shade program-less
    // content differently instead of overwriting each other.
    //
    // IT IS ALSO A CONTENT VERB (the store must answer for the program before any frame names it), so it takes the
    // content lock - and refuses a call from inside a frame body (see reportReentry).
    if (d->gate.inSharedEntry()) {
        reportReentry("setDefaultContentProgram()");
        return;
    }
    std::lock_guard<std::mutex> guard(d->content);
    executor.setDefaultContentProgram(std::move(program));
    // THE STORE MUST KNOW IT BEFORE ANY FRAME NAMES IT (see setDefaultContentProgram): the object for the store's own
    // ownership/abandon rule, the text for its rows - the same two halves every announcement tracks.
    trackProgram(d->store.get(), executor.defaultProgram());
    if (d->store != nullptr) {
        d->store->track(executor.defaultProgramText());
    }
}

vn::graphics::Capabilities VsgBackend::capabilities() const noexcept
{
    // THE DEVICE'S ANSWERS, one sentence: the device pointer and the physical device are written by initialize()
    // (a host verb), but a frame body of another thread may be running, so the read takes the content lock.
    std::lock_guard<std::mutex> guard(d->content);
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
        answer.max_anisotropy                        = vn::backend_vsg::detail::anisotropyFor(properties.limits.maxSamplerAnisotropy);
        answer.max_color_attachments                 = properties.limits.maxColorAttachments;
        // ONE target has ONE sample count, and its colour and depth attachments must both support it, so the
        // limit reported is what BOTH can do (a device whose depth tops out lower is the binding side). The device
        // states a BIT SET and the contract states a COUNT, so the set is collapsed to its largest count here - the
        // very number setMaxSamples received, which is what makes "the limit" one fact rather than two.
        answer.max_samples                           = vn::backend_vsg::largestSampleCount(
            std::min(properties.limits.framebufferColorSampleCounts, properties.limits.framebufferDepthSampleCounts));
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
    // A HOST VERB: the answer composes device-wide state a frame body writes (the counters, the last frame's
    // uploads, the retention tables and the presented watermark), so it owns the gate - and called from inside a frame
    // body it answers an EMPTY report rather than waiting for itself (see backend::FrameGate; the caller is a sink
    // that is misusing an observable, and a wrong number is worse than none).
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        return vn::graphics::FrameReport{};
    }
    // ONE ANSWER from the two places that know (see FrameReport): the backend's own instrumentation
    // (frame/Observe.hpp) answers the frame that just ended and what is retained, and the DEVICE's frame clock
    // answers where the frames are - SUBMITTED is what reached the queue, COMPLETED is what every window has proof
    // of (see core::DeviceClock). Both watermarks are the device's own, so a host reads the same numbers the
    // retirement window is dated against instead of guessing a lag.
    const core::FrameTimeline& timeline = d->clock.timeline();

    // PRESENTED is the window half's own count: every frame a window's session committed was handed to the platform's
    // present call (see api::WindowSession::framesPresented - the off-screen carrier presents nothing and answers 0). The
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
    report.counters  = d->last_counters; // The frame that just ENDED (each session owns its counters, see initialize).
    report.retention = d->observe.retention();
    report.uploads   = d->last_uploads; // This frame's own delta (see FrameUploads); absent until one ran.
    report.locking   = d->last_locking; // What it spent on the content lock (see FrameLocking); absent until one ran.
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
    // A CONTENT VERB: it changes the device's target table, so it takes the content lock - and NOT the device gate,
    // because it must stay usable while a frame is between its phases (the shape R6 gives releaseContent). Called from
    // inside a frame body (a sink), it changes nothing and says why once: the frame that is running already snapshotted
    // the targets its head resolved.
    if (d->gate.inSharedEntry()) {
        reportReentry("setRenderTarget()");
        return;
    }
    std::lock_guard<std::mutex> guard(d->content);

    // The host's own target, configured before any frame draws into it: held, and built as far as its description
    // allows (see HostTargets - the resize / rebuild after that is the plan's answer, applied by the executor).
    // OUTSIDE a frame this is CONFIGURATION and says nothing: a borrow whose lender is announced later in that
    // setup is order, not failure (the engine's own pipeline does exactly that - measured on the demo, the
    // composite arrives before its G-buffer). A frame that then draws into a target no objects were made for is
    // reported by the pass that names it (see announceTarget), where a picture depends on it.
    (void)adoptHostTarget(target, /*picture_depends*/ false);
}

void VsgBackend::announceTarget(FrameScratch& scratch, vn::raw_ptr<vn::graphics::RenderTarget> target)
{
    if (!scratch.recorder.inPass()) {
        return;
    }
    // THE RECORDER'S IDENTITY FOR IT, and nothing else: the target was held and built by the frame's head (see
    // openFrameHead), so the collection walk touches no device table (see collectFrame).
    (void)scratch.recorder.setRenderTarget(target); // null = the default framebuffer, spelled as the plan does
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
    std::string    message;
    const char8_t* code = u8"vsg.target.build_failed"; // the branch below replaces it when the reason is a narrower one
    if (reason == UnbuildableReason::SamplesUnsupported) {
        // The reasons a sample count is refused, said as the difference they are: a count above what the device
        // offers, a number that is not a sample count at all, and a multisampled target asking for what a
        // multisampled depth cannot do (be shared with another pass). The first two are the DESCRIPTION's, so the
        // host can fix it - and both name the limit it should read, because `Capabilities::max_samples` plus "the
        // counts are powers of two" is the whole set the device offers.
        const std::uint32_t samples       = target->samples() == 0U ? 1U : target->samples();
        const bool          too_many      = samples > max_samples;
        const bool          above_a_count = (samples & (samples - 1U)) != 0U;
        message                           = nameOf(target) + " asks for " + std::to_string(samples) + " samples per pixel, ";
        if (too_many) {
            code = u8"vsg.target.samples_above_limit";
            message += "above what this device offers (up to " + std::to_string(max_samples) +
                       "): the passes that draw into it are skipped (see Capabilities::max_samples)";
        }
        else if (above_a_count) {
            code = u8"vsg.target.samples_not_a_count";
            message += "that is not a sample count the device offers (the counts are powers of two, up to " +
                       std::to_string(max_samples) + "): the passes that draw into it are skipped (see Capabilities::max_samples)";
        }
        else {
            code = u8"vsg.target.borrow_multisampled";
            message += "and borrows its depth from another target: a borrowed depth is that target's resolved "
                       "single-sample picture, and a framebuffer's attachments share one sample count - announce the "
                       "borrower at one sample, or let it own its depth";
        }
    }
    else if (reason == UnbuildableReason::DepthSourceMissing) {
        code = u8"vsg.target.borrow_source_missing";
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
    const vn::graphics::RenderDiagnostic diagnostic = backendDiagnostic(vn::graphics::DiagnosticSeverity::Warning,
                                                                        vn::graphics::DiagnosticCategory::TargetBuildFailed,
                                                                        code,
                                                                        vn::String::fromUtf8(message));
    reportDiagnostic(diagnostic);
}

void VsgBackend::openPassScope(FrameScratch& scratch, vn::raw_ptr<const vn::graphics::RenderPass> pass, std::uint32_t id)
{
    if (pass == nullptr) {
        return; // no pass announced: whatever needed a scope is refused and reported by the recorder
    }
    // THE NUMBER IS THE HEAD'S (see openFrameHead and Scratch::pass_ids): the identity must not depend on when the
    // first frame happens to arrive, and the collection must not touch the device's pass registry.
    if (scratch.recorder.inFrame()) {
        (void)scratch.recorder.beginPass(id);
    }
}

void VsgBackend::closePassScope(FrameScratch& scratch)
{
    if (scratch.recorder.inFrame()) {
        (void)scratch.recorder.endPass();
    }
}

void VsgBackend::announcePassOrder(FrameScratch& scratch, int order)
{
    if (scratch.recorder.inPass()) {
        (void)scratch.recorder.setPassOrder(order);
    }
}

void VsgBackend::announceViewport(FrameScratch& scratch, const vn::graphics::Viewport& viewport)
{
    if (scratch.recorder.inPass()) {
        (void)scratch.recorder.setViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    }
}

void VsgBackend::announceLights(FrameScratch& scratch, std::span<const vn::graphics::LightValue> lights)
{
    if (!scratch.recorder.inPass()) {
        return;
    }
    // The values are already a copy taken on the content side (see Light::value): only the container has to become a
    // span, and the scratch vector keeps that from being an allocation per pass per frame.
    scratch.light_scratch.assign(lights.begin(), lights.end());
    (void)scratch.recorder.setLights(std::span<const vn::graphics::LightValue>(scratch.light_scratch));
}

void VsgBackend::announcePassInputs(FrameScratch& scratch, const std::vector<vn::graphics::ImageSource>& inputs)
{
    if (!scratch.recorder.inPass()) {
        return;
    }
    // THE IDENTITIES, in the pass' declaration order, empties included: "nothing produced it this frame" is a fact the
    // plan carries (see InputRef), and the recording reads one entry per declaration. The targets' facts were observed
    // by the frame's head (see openFrameHead), so this walk touches no device table.
    (void)scratch.recorder.setPassInputs(inputs);
}

void VsgBackend::announcePassState(FrameScratch& scratch, const vn::graphics::PassRenderState& state)
{
    if (scratch.recorder.inPass()) {
        (void)scratch.recorder.setPassState(state);
    }
}

void VsgBackend::announceDepthRange(FrameScratch& scratch, const vn::graphics::DepthRange& range)
{
    if (scratch.recorder.inPass()) {
        (void)scratch.recorder.setDepthRange(range);
    }
}

void VsgBackend::announceClear(FrameScratch& scratch, const vn::graphics::ClearPolicy& policy)
{
    if (!scratch.recorder.inPass()) {
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
    (void)scratch.recorder.setClearPolicy(translated);
}

void VsgBackend::collectContentDraw(FrameScratch&                                   scratch,
                                    const std::vector<vn::graphics::RenderCommand>& commands,
                                    const vn::graphics::CameraSnapshot&             camera,
                                    const vn::graphics::ShaderProgramDescriptionPtr& program_override)
{
    if (!scratch.recorder.inFrame()) {
        return; // nothing is collected outside a frame
    }
    // THE PICTURE'S SHAPE IS THE CAMERA's (see Camera), and the recorder reads it from the same camera when it builds
    // the view block - one number, read by culling and projection alike, so they cannot drift. WHAT THE COMMANDS NAME
    // was tracked into the content store by the frame's head (see openFrameHead), so this walk only tells the recorder
    // what the frame draws - which is why it takes no device lock.
    (void)scratch.recorder.render(commands, camera, program_override.get()); // refusing (a released target) is the recorder's to report
}

void VsgBackend::collectScreenDraw(FrameScratch&                                   scratch,
                                   vn::graphics::RenderTarget*                     source,
                                   vn::raw_ptr<const vn::graphics::ShaderProgram>  program,
                                   const vn::graphics::ShaderProgramDescriptionPtr& program_description,
                                   const vn::graphics::CameraSnapshot&             camera)
{
    if (!scratch.recorder.inFrame()) {
        return; // nothing is collected outside a frame
    }
    // THE VALUE the pass carried is what the recorder reads; a hand-built description that named only the object is
    // converted here, on the content side (the same door every hand-built command uses). The tables were told about
    // the program by the frame's head (see openFrameHead), so nothing here touches the content store.
    const vn::graphics::ShaderProgramDescriptionPtr text =
        program_description != nullptr ? program_description
                                       : (program != nullptr ? program->description() : vn::graphics::ShaderProgramDescriptionPtr{});
    // The source is an IDENTITY here, exactly like a content draw's program: which images it offers was announced with
    // the pass' inputs (the value's resolved `inputs`), and the content layer resolves it among them.
    (void)scratch.recorder.drawScreenProgram(source, text.get(), camera);
}

void VsgBackend::releasePass(vn::raw_ptr<const vn::graphics::RenderPass> pass)
{
    // A CONTENT VERB (see setRenderTarget): the pass registry is device state, so it takes the content lock and
    // refuses a call from inside a frame body instead of waiting for it.
    if (d->gate.inSharedEntry()) {
        reportReentry("releasePass()");
        return;
    }
    std::lock_guard<std::mutex> guard(d->content);
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
    if (d->gate.inSharedEntry()) {
        reportReentry("releaseRenderTarget()");
        return;
    }
    // THE WINDOWS' SCRATCHES BELONG TO THEIR OPEN FRAMES while those run (the recorder's scope announcement is the
    // frame's), so the claims are taken FIRST - they wait for this window's frame to end, and nothing below holds a
    // lock while it waits. The order (claims, then the content lock) is the frame body's own (see executeFrameFor).
    std::vector<std::unique_lock<std::mutex>> claims;
    claims.reserve(d->windows.size() + 1U);
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        if (window != nullptr && window->session != nullptr) {
            claims.emplace_back(window->frame);
        }
    }
    if (d->offscreen != nullptr) {
        claims.emplace_back(d->offscreen_frame);
    }

    std::lock_guard<std::mutex> guard(d->content);
    // The SDK announces that the caller may destroy it now, so everything held for it goes: the registry
    // entry (its objects die with it, unless a borrower keeps a share of a borrowed image), the executor's
    // registration (a borrowed pointer - letting it dangle would name somebody else's memory) and a pending
    // scope announcement (the recorder drops it: a pass that would have drawn into it is SKIPPED and
    // reported, never redirected to the window - see the SDK's own note).
    (void)d->targets.release(target);
    d->executor.addTarget(target, nullptr);
    // EVERY SESSION'S SCRATCH: a pending scope announcement is the recorder's, and one lives per session (see
    // FrameScratch). A pass that would have drawn into the target is skipped and reported either way.
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        if (window->session != nullptr) {
            if (FrameScratch* scratch = window->session->frameScratch(); scratch != nullptr) {
                (void)scratch->recorder.releaseRenderTarget(target);
            }
        }
    }
    if (d->offscreen != nullptr) {
        if (FrameScratch* scratch = d->offscreen->frameScratch(); scratch != nullptr) {
            (void)scratch->recorder.releaseRenderTarget(target);
        }
    }
}

bool VsgBackend::releaseContent(vn::raw_ptr<const vn::Object> content)
{
    if (content == nullptr || !initialized() || d->store == nullptr || d->images == nullptr) {
        return false;
    }
    if (d->gate.inSharedEntry()) {
        // A RELEASE FROM INSIDE A FRAME would change the tables the running frame's plan was resolved against: refused
        // (and said once), never waited for (see reportReentry).
        reportReentry("releaseContent()");
        return false;
    }
    // THE DEVICE'S CONTENT LOCK: the store and the image cache are tables a content phase reads, so a release is one
    // cache operation away from a frame's phase - not a frame away (R6's shape: it does not wait for a frame body).
    std::lock_guard<std::mutex> guard(d->content);
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
    if (d->gate.inSharedEntry()) {
        reportReentry("releaseContent(buffer)");
        return false;
    }
    std::lock_guard<std::mutex> guard(d->content);
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
            const vn::graphics::RenderDiagnostic diagnostic = readbackRefusalDiagnostic(refusal, "readColorBuffer()");
            reportDiagnostic(diagnostic);
        }
        return false;
    };

    // A HOST VERB: the copy below waits the device's frames, so this owns the gate. From inside a frame body the
    // request answers InsideFrame at once - a readback that waited there would be the frame waiting for itself (the
    // same refusal the vk backend states for the same reason).
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        std::lock_guard<std::recursive_mutex> guard(d->episodes);
        return refuse(HostReadbackRefusal::InsideFrame, d->readback_reports[kReadbackColour]);
    }

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
    if (d->offscreen != nullptr) {
        // AND THE OFF-SCREEN SESSION, which a device created to present nothing has instead of a window: its frames
        // are submitted by its own viewer, so without this wait the copy below can be submitted while the frame
        // that wrote the pixels is still in flight (measured: a windowless device read zeros where the windowed one
        // read the picture - the parity suite is where that surfaced). One device idle, counted like the others.
        d->offscreen->waitDeviceIdle();
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
            const vn::graphics::RenderDiagnostic diagnostic = readbackRefusalDiagnostic(refusal, "readDepthBuffer()");
            reportDiagnostic(diagnostic);
        }
        return false;
    };

    // A HOST VERB, for the same reason the colour readback is one (see its note).
    const vn::graphics::backend::FrameGate::Exclusive host = d->gate.enterExclusive();
    if (!host) {
        std::lock_guard<std::recursive_mutex> guard(d->episodes);
        return refuse(HostReadbackRefusal::InsideFrame, d->readback_reports[kReadbackDepth]);
    }

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
    if (d->offscreen != nullptr) {
        d->offscreen->waitDeviceIdle(); // the windowless device's only submitter (see readColorBuffer's note)
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
    // THE WINDOW SET IS THE DEVICE'S (createSurfaceTarget mutates it under the gate). A frame body of another thread
    // may be running, so the read takes the content lock - the lock every frame head takes, and the one a caller can
    // hold without deadlocking itself (no path from here back into this function).
    std::lock_guard<std::mutex> guard(d->content);
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
    std::lock_guard<std::mutex> guard(d->content);
    std::uint64_t presented = 0;
    for (const std::unique_ptr<Data::WindowSlot>& window : d->windows) {
        presented += window->session->framesPresented();
    }
    return presented;
}

std::size_t VsgBackend::deviceWaits() const noexcept
{
    // The DEVICE's queue is where every window's destructive path is counted (see core::DeviceClock): one number,
    // because "the frame path never idles the device" is one fact about one device. The queue is written by frame
    // tails (parks, advances), so it is read under the content lock.
    std::lock_guard<std::mutex> guard(d->content);
    return d->retirement.deviceWaits();
}

std::size_t VsgBackend::releasedContentObjects() const noexcept
{
    std::lock_guard<std::mutex> guard(d->content);
    return d->released_objects;
}

std::size_t VsgBackend::releasedTextures() const noexcept
{
    std::lock_guard<std::mutex> guard(d->content);
    return d->released_textures;
}

std::size_t VsgBackend::releasedStreams() const noexcept
{
    std::lock_guard<std::mutex> guard(d->content);
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
    reportDiagnostic(backendDiagnostic(vn::graphics::DiagnosticSeverity::Info,
                                       vn::graphics::DiagnosticCategory::ContentSkipped,
                                       u8"vsg.storage.material_arena_grown",
                                       formatDiagnostic(u8"the live material set outgrew the arena, so the block storage was replaced "
                                                        u8"(a new buffer; the old one is parked): materials %u -> %u",
                                                        before.materials.capacity,
                                                        wanted.materials.capacity)));
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
    reportDiagnostic(backendDiagnostic(vn::graphics::DiagnosticSeverity::Info,
                                       vn::graphics::DiagnosticCategory::ContentSkipped,
                                       u8"vsg.storage.block_budget_grown",
                                       vn::String::fromUtf8(reason)));
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
    if (slot >= kUnservedCount) {
        return;
    }
    // THE EPISODE IS ONE PER ENTRY POINT AND SHARED BY EVERY FRAME BODY (see Data::episodes): asking "have I said
    // this?" and saying it happen under one lock, so two drivers arriving together say it once.
    std::lock_guard<std::recursive_mutex> guard(d->episodes);
    if (!d->unserved_reports[slot].shouldReport()) {
        return;
    }
    // THE SLOT IS THE CAUSE: a frame that arrived with no session and a `render()` collected before the content world
    // exists are two different repairs (initialize, versus wait for the content world), so each states its own code.
    const vn::graphics::RenderDiagnostic diagnostic = backendDiagnostic(
        vn::graphics::DiagnosticSeverity::Warning,
        vn::graphics::DiagnosticCategory::UnsupportedRequest,
        slot == kUnservedNoSession ? u8"vsg.session.not_initialized" : u8"vsg.content.no_content_world",
        vn::String::fromUtf8(std::string(kUnservedNames[slot]) + " has nowhere to go: every part of the frame protocol needs a session that is "
                                                              "up (see .ai/design/vsg-backend.md)"));
    reportDiagnostic(diagnostic);
}

void VsgBackend::reportReentry(const char* verb) noexcept
{
    // ONE EPISODE, SHARED BY EVERY FRAME BODY (see reportUnserved) - and one sentence per verb is enough: a host that
    // fixes one of these hears the next one on its own frame.
    std::lock_guard<std::recursive_mutex> guard(d->episodes);
    if (!d->host_reentry_report.shouldReport()) {
        return;
    }
    const vn::graphics::RenderDiagnostic diagnostic = backendDiagnostic(
        vn::graphics::DiagnosticSeverity::Warning,
        vn::graphics::DiagnosticCategory::PassProtocolViolation,
        u8"vsg.device.reentry",
        vn::String::fromUtf8(std::string(verb != nullptr ? verb : "a host verb") +
                             " was called from inside a frame body (a diagnostics sink or a hook): nothing was changed, because "
                             "the verb would have to wait for the frame that is calling it - call it between frames"));
    reportDiagnostic(diagnostic);
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

void BackendContentAccess::setRecordSeam(VsgBackend& backend, std::function<void()> seam)
{
    backend.d->record_seam = std::move(seam);
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
    // The window the last frame of the DEVICE'S OWN DRIVER was for (the tests that use this seam drive the aggregate,
    // whose frame state is its executor's - see VsgExecutor::FrameDrive). A frame that named NO window leaves none: the
    // off-screen form has no default framebuffer to answer with.
    const std::size_t driving = backend.d->executor.drive().driving;
    if (driving >= backend.d->windows.size()) {
        return nullptr;
    }
    return SessionContentAccess::windowTarget(*backend.d->windows[driving]->session);
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

VN_BACKEND_VSG_NS_END
