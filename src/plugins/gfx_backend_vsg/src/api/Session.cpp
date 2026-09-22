#include <vine/vsg/api/Session.hpp>

#include <chrono>
#include <cstdlib>

#include <vine/vsg/api/DeviceFeatures.hpp>
#include <vine/vsg/api/SessionContent.hpp>
#include <vine/vsg/api/WindowTarget.hpp>

#include <string>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/app/Window.h>
#include <vsg/app/WindowTraits.h>
#include <vsg/utils/Profiler.h>
#include <vsg/vk/DeviceFeatures.h>
#include <vsg/vk/PhysicalDevice.h>

#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgHostWindow.hpp>
#include <vine/vsg/core/DeviceRequirements.hpp>
#include <vine/vsg/core/SessionMove.hpp>
#include <vine/vsg/core/SlotProbe.hpp>

V_VSG_NS_BEGIN

namespace api
{

namespace
{

using vine::graphics::DiagnosticCategory;
using vine::graphics::DiagnosticSeverity;

/// @brief Builds a `vine::String` from an ASCII sentence (the repo's spelling for UTF-8 bytes).
vine::String asString(const std::string& text)
{
    return vine::String(reinterpret_cast<const char8_t*>(text.c_str()));
}

/// @brief Reads an environment variable as a non-negative number, or @p fallback when unset or unparsable.
unsigned int envLevel(const char* name, unsigned int fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr)
    {
        return fallback;
    }
    char*       end     = nullptr;
    const long  parsed  = std::strtol(value, &end, 10);
    return (end != nullptr && end != value && parsed > 0) ? static_cast<unsigned int>(parsed) : fallback;
}

/**
 * @brief A viewer that never pumps the platform's events: the host owns the message loop.
 *
 * Re-entering the toolkit from inside a frame call is how a backend recurses through someone else's event
 * loop (the dispatched message triggers a host event, which asks for another frame, which pumps again).
 * Input arrives from the host instead, so window polling is off and only the buffered framework events
 * are dropped - the same shape the existing implementation settled on, for the same reason.
 */
class EmbeddedViewer : public ::vsg::Inherit<::vsg::Viewer, EmbeddedViewer>
{
  public:
    bool pollEvents(bool discard_previous_events) override
    {
        if (discard_previous_events)
        {
            this->getEvents().clear();
        }
        return false;
    }
};

/**
 * @brief Submits every record-and-submit task of @p viewer the way the viewer's own recordAndSubmit() does,
 *        and KEEPS the failure.
 *
 * WHY NOT CALL viewer->recordAndSubmit(). It returns void and DROPS each task's VkResult, so a queue submit
 * the driver refused (device lost, out of memory) would look exactly like a frame that happened - and this
 * backend's frame path must be able to say that it did not. So the viewer's own steps are taken here: reset
 * every command graph (the exclusive state an ExecuteCommands node hands over), then one submit per task on
 * this thread - the threading path is the viewer's own and a session never enables it.
 *
 * A session has ONE task (one window, one device, one queue family), so a failure means nothing at all was
 * handed to the queue: there is no "some tasks submitted" half to reason about.
 *
 * @param viewer  The session's viewer.
 * @param failure Receives a human-readable reason when the submission did not happen.
 * @return true when every task reported success; false when one did not (in @p failure).
 */
bool submitViewerTasks(::vsg::Viewer& viewer, std::string& failure)
{
    for (const auto& task : viewer.recordAndSubmitTasks)
    {
        for (auto& graph : task->commandGraphs)
        {
            graph->reset();
        }
    }

    for (const auto& task : viewer.recordAndSubmitTasks)
    {
        try
        {
            // The frame stamp the viewer itself would have handed the task (it owns it): a submission that
            // sees no stamp is a submission of no frame.
            const ::vsg::ref_ptr<::vsg::FrameStamp> frame_stamp(viewer.getFrameStamp());
            const VkResult                          result = task->submit(frame_stamp);
            if (result != VK_SUCCESS)
            {
                failure = "the queue submission reported VkResult " + std::to_string(static_cast<int>(result));
                return false;
            }
        }
        catch (const ::vsg::Exception& error)
        {
            // vsg's own failure vocabulary, and NOT a std::exception (a plain struct with message + result):
            // a command buffer that cannot be allocated throws here, out of the record step.
            failure = "vsg::Exception: " + error.message + " (VkResult " + std::to_string(error.result) + ")";
            return false;
        }
        catch (const std::exception& error)
        {
            failure = std::string("std::exception: ") + error.what();
            return false;
        }
    }
    return true;
}

}  // namespace

struct Session::Impl
{
    core::FrameTimeline    timeline;             ///< One clock for the session (see the class note).
    core::RetirementQueue  retirement{0};        ///< Constructed for real once the slots are learned.
    core::Diagnostics*     diagnostics{nullptr}; ///< Where the session's reasons go (borrowed).
    /// The device profiler, when the switch asked for one (see Session::profiling): the executor wraps the
    /// passes, this owns the query pool and the log they are read from.
    ::vsg::ref_ptr<::vsg::Profiler> profiler;
    ::vsg::ref_ptr<::vsg::Window> window;
    ::vsg::ref_ptr<::vsg::Viewer> viewer;
    ::vsg::ref_ptr<::vsg::Group>  content;  ///< What the frame renders (see SessionContentAccess).
    std::unique_ptr<WindowTarget> window_target;  ///< The default framebuffer as a target the executor serves.
    void*                  host_handle{nullptr}; ///< The host window this session is on, or nullptr.
    bool                   on_host_window{false};///< Whether the session adopted a host window.
    std::uint32_t          slots{0};             ///< In-flight slots the tracker learned (0 = not learned yet).
    core::SlotTracker      slot_tracker;         ///< Folds per-frame probes until the count stops growing.
    std::uint64_t          frames_presented{0};
    std::uint64_t          lost_frames{0};  ///< Commits whose submission did not happen (see commitFrame).
    // The frame clock the view block reads (see Session::frameSeconds): the session's own start is the zero
    // of the time line, and the value is re-sampled once per opened frame.
    std::chrono::steady_clock::time_point started_at{};
    float                                 frame_seconds{0.0F};
    bool                   ready{false};
    // Statistics about how this Session object brought sessions up. NOT reset by shutdown(): they answer
    // "what did this host's announcements cost", which a teardown does not change.
    std::uint64_t moves{0};
    std::uint64_t rebuilds{0};
    std::uint64_t keeps{0};
    std::uint64_t generation{0};
};

Session::Session()
  : impl(std::make_unique<Impl>())
{
}

Session::~Session()
{
    shutdown();
}

bool Session::initialize(const SessionOptions& options, core::Diagnostics& diagnostics)
{
    impl->diagnostics = &diagnostics;

    // The decision comes FIRST, and before anything is torn down: keeping a session is the cheap answer and
    // must not travel through a teardown to be reached (see core::planSessionMove).
    core::MoveFacts facts;
    facts.has_live_session = impl->ready;
    facts.on_host_window   = impl->on_host_window;
    facts.handle_announced = options.native_handle != nullptr;
    facts.handle_differs   = options.native_handle != impl->host_handle;

    const core::MoveDecision decision = core::planSessionMove(facts);
    if (decision.action == core::MoveAction::Keep)
    {
        ++impl->keeps;
        return true;
    }
    if (decision.action == core::MoveAction::Move && moveTo(options.native_handle))
    {
        ++impl->moves;
        ++impl->generation;
        return true;
    }
    if (facts.has_live_session)
    {
        // The expensive path, said out loud: the instance, the device and every compiled pipeline are about
        // to be rebuilt, and the host may be able to avoid it (a swapchain format it keeps, a window it
        // does not recreate).
        ++impl->rebuilds;
        diagnostics.report(DiagnosticSeverity::Warning, DiagnosticCategory::UnsupportedRequest,
                           asString(std::string("the session is rebuilt from scratch: ") + decision.reason));
    }

    shutdown();

    try
    {
        auto traits          = ::vsg::WindowTraits::create();
        traits->windowTitle  = "Vine session";
        traits->width        = options.width;
        traits->height       = options.height;
        traits->debugLayer   = options.validation;
        traits->deviceFeatures = ::vsg::DeviceFeatures::create();
        applyRequiredFeatures(traits->deviceFeatures);
        // The features and the extensions are ONE policy in two halves (see requiredDeviceExtensions): the
        // dynamic states whose bits are requested above live in EDS3, so a session that asked for the bits and
        // not for the names would compile pipelines the driver may reject. The session creates its device
        // through a window rather than through api::Device, which is exactly why the policy is a function both
        // call instead of a list written twice.
        for (const auto& name : requiredDeviceExtensions())
        {
            traits->deviceExtensionNames.push_back(name);
        }

        if (options.native_handle != nullptr)
        {
            // The host's window is ADOPTED, not created: this is the facility that knows how (the platform
            // handle's exact type, and the rule that the host's window outlives us) - see VsgHostWindow.
            traits->nativeWindow = detail::hostHandleFromVoid(options.native_handle);
            impl->window         = ::vsg::ref_ptr<::vsg::Window>(detail::VsgHostWindow::create(traits));
        }
        else
        {
            impl->window = ::vsg::Window::create(traits);
        }
        impl->host_handle    = options.native_handle;
        impl->on_host_window = options.native_handle != nullptr;

        if (!impl->window)
        {
            diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed,
                               asString("the session could not create a window"));
            shutdown();
            return false;
        }

        impl->viewer = ::vsg::ref_ptr<::vsg::Viewer>(new EmbeddedViewer());
        impl->viewer->addWindow(impl->window);

        // The window as the frame's default-framebuffer target: it owns the one render graph every window pass
        // records into (see WindowTarget) - and one stable view under it, which is what gives the window's
        // content its own compiled pipelines (vsg compiles per view id, and the swapchain's render pass is not
        // compatible with an off-screen one of the same engine shape).
        impl->window_target = WindowTarget::create(impl->window);
        if (impl->window_target == nullptr)
        {
            diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed,
                               asString("the session could not wrap its window as a render target"));
            shutdown();
            return false;
        }
        // The content root, attached BEFORE the compile pass: content a caller adds later is compiled by
        // asking for one more pass (see SessionContentAccess::recompile). The session draws whatever is in
        // here, every frame, and nothing else.
        impl->content = ::vsg::Group::create();
        impl->window_target->addContent(impl->content);

        auto command_graph = ::vsg::CommandGraph::create(impl->window);
        command_graph->addChild(impl->window_target->graph());
        impl->viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });

        // The device profiler, when the environment asked for one: installed AFTER the record-and-submit
        // task, because the task is what the viewer propagates instrumentation through (the legacy
        // backend's own order, for the same reason).
        if (std::getenv("VINE_VSG_PROFILE") != nullptr)
        {
            auto settings                        = ::vsg::Profiler::Settings::create();
            settings->cpu_instrumentation_level  = envLevel("VINE_VSG_PROFILE_CPU", 0U);
            // Level 1 is the level the executor's per-pass wrappers carry; a higher level timestamps every
            // recorded node and the profiler's fixed pool then drops what does not fit, silently.
            settings->gpu_instrumentation_level  = envLevel("VINE_VSG_PROFILE_GPU", 1U);
            impl->profiler                       = ::vsg::Profiler::create(settings);
            impl->viewer->assignInstrumentation(impl->profiler);
        }

        // The slot count is NOT probed here, and that is not an oversight: the framework's per-slot index
        // table is filled by advance(), which the first frame's open runs - before that, every fence query
        // answers "no slot" (the index table still holds its unset sentinel). The session therefore learns
        // its count when the first frame is opened (see probeSlots).

        const auto compiled = impl->viewer->compile();
        if (!compiled)
        {
            diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed,
                               asString("the session failed to compile: " + compiled.message));
            shutdown();
            return false;
        }

        impl->ready = true;
        impl->started_at = std::chrono::steady_clock::now();
        impl->frame_seconds = 0.0F;
        ++impl->generation;
        return true;
    }
    catch (const std::exception& error)
    {
        diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed,
                           asString(std::string("the session failed to initialize: ") + error.what()));
        shutdown();
        return false;
    }
}

void Session::shutdown() noexcept
{
    if (!impl)
    {
        return;
    }

    // A whole-session replacement, in one order that cannot half-apply: the viewer (which owns the device
    // and the swapchain) goes first, the window after it, and the bookkeeping is reset last - a session
    // that is not ready owns no evidence, and saying otherwise is what makes a re-initialize read the
    // previous session's numbers.
    impl->ready            = false;
    impl->frames_presented = 0;
    impl->slots            = 0;
    impl->slot_tracker     = core::SlotTracker();
    impl->retirement       = core::RetirementQueue(0);
    impl->timeline         = core::FrameTimeline();
    impl->frame_seconds    = 0.0F;
    impl->started_at       = {};
    impl->host_handle      = nullptr;
    impl->on_host_window   = false;
    impl->viewer           = {};
    impl->window_target    = nullptr;
    impl->window           = {};
    impl->content          = {};
}

bool Session::initialized() const noexcept
{
    return impl && impl->ready;
}

core::FrameToken Session::beginFrame()
{
    if (!impl || !impl->ready)
    {
        return {};
    }

    // The frame's time stamp is sampled HERE and does not move while the frame is open: every pass of this
    // frame binds the same "now" (see frameSeconds).
    const std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - impl->started_at;
    impl->frame_seconds                        = elapsed.count();

    impl->viewer->advanceToNextFrame();
    impl->viewer->handleEvents();

    // The in-flight slot count is neither readable nor answerable in one probe (see SlotProbe): one entry of
    // the framework's slot table is filled per frame, so the session keeps folding answers in until the
    // number stops growing - and only then may anything be parked.
    if (!impl->slot_tracker.known())
    {
        probeSlots();
    }

    return impl->timeline.begin();
}

void Session::probeSlots()
{
    const auto outcome = core::probeSlotCount([this](std::uint32_t index) {
        return !impl->viewer->recordAndSubmitTasks.empty() &&
               impl->viewer->recordAndSubmitTasks.front()->fence(index) != nullptr;
    });

    if (!impl->slot_tracker.observe(outcome))
    {
        // Still filling: there is no window yet, and nothing to report - parking is simply unavailable
        // (RetirementQueue::retire() refuses rather than guessing).
        return;
    }

    impl->slots = impl->slot_tracker.slots();
    // In place: the queue's own counters (the counted device idles) must survive the moment the count is
    // learned - replacing the queue here is how a session silently loses them.
    impl->retirement.setSlots(impl->slots);

    if (impl->diagnostics == nullptr || impl->slot_tracker.agreesWithAssumption())
    {
        return;
    }
    impl->diagnostics->report(DiagnosticSeverity::Info, DiagnosticCategory::UnsupportedRequest,
                              asString("the in-flight slot count is " + std::to_string(impl->slots) +
                                       ", not the " + std::to_string(core::kAssumedInFlightSlots) +
                                       " this code was written against (learned over " +
                                       std::to_string(impl->slot_tracker.framesObserved()) + " frames)"));
}

bool Session::commitFrame()
{
    if (!impl || !impl->ready)
    {
        return false;
    }
    if (!impl->timeline.hasOpenFrame())
    {
        // Committing what was never opened would advance the deferral clocks for a frame nothing
        // accounted for; the protocol layer reports it, and here it simply does not happen.
        if (impl->diagnostics != nullptr)
        {
            impl->diagnostics->report(DiagnosticSeverity::Warning, DiagnosticCategory::PassProtocolViolation,
                                      asString("commitFrame with no open frame"));
        }
        return false;
    }

    const core::FrameToken token        = impl->timeline.current();
    const std::uint64_t    frame_number = token.frame;

    impl->viewer->update();

    // This is where the completion evidence is produced: recording frame F waits the fence of the slot
    // that recorded frame F - slots, so every frame up to that point is provably finished.
    std::string failure;
    if (!submitViewerTasks(*impl->viewer, failure))
    {
        // The submission did NOT happen: nothing was handed to the queue, so nothing may claim it did -
        // no present, no presented-frame count, no completion evidence (no slot was recycled). The frame
        // itself is over (its swapchain image was acquired and its graph was recorded, so it cannot be
        // retried): the timeline abandons the token, which keeps the submitted watermark a count of
        // SUBMISSIONS - the fact a frame drive reads to mark what this frame wrote (a lost submission).
        impl->timeline.abandoned(token);
        impl->retirement.advance(impl->timeline);
        ++impl->lost_frames;
        // THE SWAPCHAIN IS REBUILT, and this is the whole reason a lost frame is survivable here: the frame's
        // image was acquired and never presented, so the WSI is short one image and the next acquire cannot
        // be trusted (measured: a forward-progress warning on the acquire and a present of an image that was
        // not acquired). It is the one repair this layer can make without a new device, and it costs a device
        // idle - COUNTED, because vsg's buildSwapchain() waits the device before it destroys the old
        // swapchain (WindowAdapter::resize): a lost frame is the exception, and the counter is how that stays
        // visible instead of becoming a habit.
        if (impl->window != nullptr)
        {
            impl->window->resize();
            impl->retirement.noteDeviceWait();
        }
        if (impl->diagnostics != nullptr)
        {
            impl->diagnostics->report(DiagnosticSeverity::Error, DiagnosticCategory::SubmissionFailed,
                                      asString("the frame's submission did not happen, so nothing it recorded "
                                               "was performed and nothing was presented - the failure was: " +
                                               failure));
        }
        return false;
    }

    if (impl->slots != 0 && frame_number > impl->slots)
    {
        impl->timeline.completeUpTo(frame_number - impl->slots);
    }

    // The one present, and it happens even when the frame queued nothing - see the class note.
    impl->viewer->present();
    ++impl->frames_presented;

    impl->timeline.submitted(token);
    // The frame's LAST step: a park made after this would be dated against the next frame and released
    // one frame early (the failure this ordering exists to prevent).
    impl->retirement.advance(impl->timeline);
    return true;
}

std::uint32_t Session::slots() const noexcept
{
    return impl ? impl->slots : 0;
}

std::uint64_t Session::framesPresented() const noexcept
{
    return impl ? impl->frames_presented : 0;
}

std::uint64_t Session::lostFrames() const noexcept
{
    return impl ? impl->lost_frames : 0;
}

float Session::frameSeconds() const noexcept
{
    return impl ? impl->frame_seconds : 0.0F;
}

bool Session::profiling() const noexcept
{
    return impl != nullptr && impl->profiler != nullptr;
}

Session::GpuProfile Session::gpuProfile() const
{
    GpuProfile profile;
    profile.enabled = profiling();
    if (!profile.enabled)
    {
        return profile;
    }
    // Whether this device can write timestamps at all is a device fact, not a software one: without it the
    // answer stays "enabled, nothing available" instead of looking like a frame that cost nothing.
    const auto device   = impl->window != nullptr ? impl->window->getOrCreateDevice() : nullptr;
    const auto physical = device != nullptr ? device->getPhysicalDevice() : nullptr;
    profile.timestamps_available = physical != nullptr &&
                                   physical->getProperties().limits.timestampComputeAndGraphics != VK_FALSE;
    if (!profile.timestamps_available)
    {
        return profile;
    }

    const auto& log = impl->profiler->log;
    if (log == nullptr)
    {
        return profile;
    }

    // The newest frame that HAS results, walked backwards: the profiler reads without waiting (see the
    // header), so the last frames in the log have no timestamps yet - looking further back is what turns
    // "not readable yet" into "this is the newest one".
    const auto& frames = log->frameIndices;
    for (std::size_t index = frames.size(); index-- > 0;)
    {
        const auto& frame_entry = log->entry(frames[index]);
        if (frame_entry.type != ::vsg::ProfileLog::FRAME || !frame_entry.enter)
        {
            continue;  // the ring has overwritten this frame's entry
        }
        const std::uint64_t frame_end = frame_entry.reference;
        if (frame_end <= frames[index] || (frame_end - frames[index]) >= log->entries.size())
        {
            continue;  // not a frame span this log still holds
        }

        GpuProfile candidate;
        candidate.enabled              = true;
        candidate.timestamps_available = true;
        bool        saw_frame_interval = false;
        const double ticks_to_ms       = log->timestampScaleToMilliseconds;
        for (std::uint64_t reference = frames[index]; reference <= frame_end; ++reference)
        {
            const auto& interval = log->entry(reference);
            if (!interval.enter || interval.gpuTime == 0 || interval.reference <= reference ||
                interval.reference > frame_end)
            {
                continue;
            }
            const auto& end = log->entry(interval.reference);
            if (end.gpuTime <= interval.gpuTime)
            {
                continue;  // this pair has not been read back yet
            }
            const double milliseconds = static_cast<double>(end.gpuTime - interval.gpuTime) * ticks_to_ms;
            if (interval.type == ::vsg::ProfileLog::COMMAND_BUFFER)
            {
                candidate.frame_gpu_ms = milliseconds;
                saw_frame_interval     = true;
                continue;
            }
            if (interval.type != ::vsg::ProfileLog::GPU || interval.object == nullptr)
            {
                continue;  // an interval upstream's own hook wrote, with no object to attribute it to
            }
            GpuSample sample;
            // The ADDRESS is the key, and it is only ever compared (see GpuSample::key): the frame it names
            // may have been released since, and reading through it is the crash the legacy backend measured.
            sample.key    = static_cast<const void*>(interval.object);
            sample.gpu_ms = milliseconds;
            candidate.passes.push_back(sample);
        }
        if (saw_frame_interval || !candidate.passes.empty())
        {
            candidate.age_frames = frames.size() - 1U - index;
            candidate.readable   = true;
            profile              = std::move(candidate);
            return profile;
        }
    }
    return profile;
}

std::size_t Session::deviceWaits() const noexcept
{
    return impl ? impl->retirement.deviceWaits() : 0;
}

core::FrameTimeline& Session::timeline() noexcept
{
    return impl->timeline;
}

core::RetirementQueue& Session::retirement() noexcept
{
    return impl->retirement;
}

std::uint64_t Session::moves() const noexcept
{
    return impl->moves;
}

std::uint64_t Session::rebuilds() const noexcept
{
    return impl->rebuilds;
}

std::uint64_t Session::keeps() const noexcept
{
    return impl->keeps;
}

std::uint64_t Session::generation() const noexcept
{
    return impl->generation;
}

void* Session::hostHandle() const noexcept
{
    return impl->host_handle;
}

bool Session::moveTo(void* handle)
{
    if (!impl->ready || !impl->viewer || !impl->window || handle == nullptr)
    {
        return false;
    }
    // Only a session that ADOPTED a host window has a host surface to follow; one on a window of this
    // backend's own owns its swapchain and cannot serve the host's window.
    if (!detail::onHostWindow(impl->window))
    {
        return false;
    }
    auto host_window = impl->window.cast<detail::VsgHostWindow>();
    if (!host_window)
    {
        return false;
    }

    // A COUNTED device stop, because the surface, the swapchain and the images being replaced may still be
    // named by work in flight. This is the one place a move needs the device quiet, and it is counted so
    // "the frame path never idles the device" stays checkable.
    impl->viewer->deviceWaitIdle();
    impl->retirement.noteDeviceWait();

    if (!host_window->moveToHostSurface(handle))
    {
        if (impl->diagnostics != nullptr)
        {
            impl->diagnostics->report(
                DiagnosticSeverity::Warning, DiagnosticCategory::UnsupportedRequest,
                asString("the host's new window could not serve this session (a different swapchain format, "
                         "or a window that never went as far as a device): it is rebuilt from scratch"));
        }
        return false;
    }

    impl->host_handle = handle;
    return true;
}

}  // namespace api

namespace detail
{

::vsg::ref_ptr<::vsg::Group> SessionContentAccess::root(const api::Session& session) noexcept
{
    return session.impl != nullptr ? session.impl->content : ::vsg::ref_ptr<::vsg::Group>{};
}

::vsg::ref_ptr<::vsg::Device> SessionContentAccess::device(const api::Session& session) noexcept
{
    if (session.impl == nullptr || session.impl->window == nullptr)
    {
        return {};
    }
    return session.impl->window->getOrCreateDevice();
}

WindowTarget* SessionContentAccess::windowTarget(const api::Session& session) noexcept
{
    if (session.impl == nullptr)
    {
        return nullptr;
    }
    return session.impl->window_target.get();
}

::vsg::ref_ptr<::vsg::CommandGraph> SessionContentAccess::makeFrameGraph(const api::Session& session) noexcept
{
    if (session.impl == nullptr || session.impl->window == nullptr)
    {
        return {};
    }
    // Bound to the window: the graph's device and queue family come from the surface it presents, which is
    // exactly the session's, and the caller does not have to be told either of them.
    return ::vsg::CommandGraph::create(session.impl->window);
}

bool SessionContentAccess::assignFrameGraphs(api::Session& session, const ::vsg::CommandGraphs& graphs)
{
    if (session.impl == nullptr || session.impl->viewer == nullptr)
    {
        return false;
    }
    // THE TASKS SURVIVE THE HANDOVER, and that is the whole point: a task owns the fences and semaphores a
    // submission in flight still names, so the viewer's own assignRecordAndSubmitTaskAndPresentation() -
    // which CLEARS the task list and builds new ones - destroys objects the queue is using the moment it is
    // called while the previous frame is still running (measured: 12 VUIDs about fences, semaphores and
    // command buffers in use, and a crash inside the validation layer that a run without it hides). One
    // frame's graphs become the session's by handing them to the tasks it already has; only a viewer with no
    // task at all (not this session: it comes up with its own frame graph) is built by the viewer.
    if (session.impl->viewer->recordAndSubmitTasks.empty())
    {
        session.impl->viewer->assignRecordAndSubmitTaskAndPresentation(graphs);
    }
    else
    {
        // One session has one task (one window, one device, one queue family), so this is a formality - and
        // it keeps every task's fences, its WSI semaphores and the window they present on untouched.
        for (const auto& task : session.impl->viewer->recordAndSubmitTasks)
        {
            task->commandGraphs = graphs;
        }
    }

    // The compile pass follows immediately: a graph the viewer has never seen has nodes whose implementations
    // (descriptor sets above all) are created by exactly that pass, and the viewer's record pass would
    // otherwise find them missing.
    return SessionContentAccess::recompile(session);
}

bool SessionContentAccess::recompile(api::Session& session)
{
    if (session.impl == nullptr || session.impl->viewer == nullptr)
    {
        return false;
    }
    // A compile pass over the same graphs: already-compiled objects keep their implementations (a pipeline is
    // only created while its implementation is missing), and the newly attached content gets its own.
    const ::vsg::CompileResult compiled = session.impl->viewer->compile();
    if (!compiled)
    {
        // The session never degrades silently: a pass that did not run is reported where the session's other
        // reasons go, and the caller learns about it through the return value.
        if (session.impl->diagnostics != nullptr)
        {
            session.impl->diagnostics->report(api::DiagnosticSeverity::Warning, api::DiagnosticCategory::InitFailed,
                                              api::asString(std::string("the content attached to the session did not compile: ") +
                                                            compiled.message));
        }
        return false;
    }
    return true;
}

}  // namespace detail

V_VSG_NS_END
