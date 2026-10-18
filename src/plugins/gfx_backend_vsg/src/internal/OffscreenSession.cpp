#include <vine/vsg/internal/OffscreenSession.hpp>

#include <chrono>
#include <exception>
#include <string>

#include <vsg/app/Viewer.h>
#include <vsg/core/Exception.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/internal/SessionDrive.hpp>
#include <vine/graphics/backend/SlotProbe.hpp>

VN_VSG_NS_BEGIN

namespace api
{

namespace
{

using vn::graphics::DiagnosticCategory;
using vn::graphics::DiagnosticSeverity;

}  // namespace

struct OffscreenSession::Impl
{
    core::FrameTimeline   timeline;        ///< One clock for the session, when the device's is not borrowed.
    core::RetirementQueue retirement{0};   ///< Constructed for real once the slots are learned.
    /// The DEVICE's clock and park queue, when this session is one of several frame sources on one device (see
    /// core::DeviceClock): the borrowed pair then REPLACES the two above, which stay unused.
    core::DeviceClock*     clock{nullptr};
    core::RetirementQueue* shared_retirement{nullptr};
    core::DeviceClock::WindowId clock_window{0};
    bool                        clock_window_live{false};
    vn::graphics::Diagnostics*  diagnostics{nullptr};  ///< Where the session's reasons go (borrowed).
    /// The device is BORROWED (the session takes its own reference to the caller's object): an off-screen session
    /// submits onto a device somebody else created and keeps alive.
    ::vsg::ref_ptr<::vsg::Device> device;
    ::vsg::ref_ptr<::vsg::Viewer> viewer;
    int                    queue_family{-1};
    std::uint32_t          slots{0};         ///< In-flight slots the tracker learned (0 = not learned yet).
    core::SlotTracker      slot_tracker;     ///< Folds per-frame probes until the count stops growing.
    std::uint64_t          lost_frames{0};   ///< Commits whose submission did not happen (see commitFrame).
    /// The frame clock the view block reads: this session's own start is the zero of its time line.
    std::chrono::steady_clock::time_point started_at{};
    float                                 frame_seconds{0.0F};
    bool                                  ready{false};
};

OffscreenSession::OffscreenSession()
  : impl(std::make_unique<Impl>())
{
}

OffscreenSession::~OffscreenSession()
{
    shutdown();
}

bool OffscreenSession::initialize(const OffscreenSessionOptions& options, vn::graphics::Diagnostics& diagnostics)
{
    impl->diagnostics = &diagnostics;

    // IDEMPOTENT, and that is the whole of this session's move table: it has no surface to keep or move, so a
    // second announcement of the same device changes nothing (see the header).
    if (impl->ready)
    {
        return true;
    }

    shutdown();

    impl->device = ::vsg::ref_ptr<::vsg::Device>(static_cast<::vsg::Device*>(options.device));
    if (impl->device == nullptr || options.queue_family < 0)
    {
        diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed,
                           vn::String::fromUtf8("the off-screen session needs a device and its graphics queue family"));
        shutdown();
        return false;
    }
    impl->queue_family = options.queue_family;

    // The DEVICE's clock and park queue, when this session shares them: the completion evidence is the device's,
    // because what it decides (whether a shared buffer or pipeline may be destroyed) is a device question.
    impl->clock             = options.clock;
    impl->shared_retirement = options.retirement;
    if (impl->clock != nullptr)
    {
        impl->clock_window      = impl->clock->addWindow();
        impl->clock_window_live = true;
    }

    try
    {
        impl->viewer = ::vsg::ref_ptr<::vsg::Viewer>(new detail::EmbeddedViewer());

        // A WINDOWLESS COMMAND GRAPH is the framework's own spelling of "a submission that neither acquires an image
        // nor presents one": the device and its queue family are all a graph needs to allocate and submit command
        // buffers. A graph with no children is a valid frame (it submits nothing), and it is what gives the viewer
        // the ONE task whose fences the slot probe reads.
        ::vsg::ref_ptr<::vsg::CommandGraph> graph = ::vsg::CommandGraph::create(impl->device, impl->queue_family);
        impl->viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ graph });

        const auto compiled = impl->viewer->compile();
        if (!compiled)
        {
            diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed,
                               vn::String::fromUtf8("the off-screen session failed to compile: " + compiled.message));
            shutdown();
            return false;
        }

        impl->ready       = true;
        impl->started_at  = std::chrono::steady_clock::now();
        impl->frame_seconds = 0.0F;
        return true;
    }
    catch (const ::vsg::Exception& error)
    {
        // vsg's own failure vocabulary: `vsg::Exception` is NOT a `std::exception`, so both are caught (see
        // deviceFailureMessage for the one a host actually meets).
        diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed,
                           vn::String::fromUtf8(std::string("the off-screen session failed to initialize: ") +
                                                error.message));
        shutdown();
        return false;
    }
    catch (const std::exception& error)
    {
        diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed,
                           vn::String::fromUtf8(std::string("the off-screen session failed to initialize: ") +
                                                error.what()));
        shutdown();
        return false;
    }
}

void OffscreenSession::shutdown() noexcept
{
    if (!impl)
    {
        return;
    }

    // The viewer (which owns the task, its fences and the graph) goes first; the device reference after it, and the
    // bookkeeping last - a session that is not ready owns no evidence.
    impl->ready         = false;
    impl->lost_frames   = 0;
    impl->slots         = 0;
    impl->slot_tracker  = core::SlotTracker();
    impl->retirement    = core::RetirementQueue(0);
    impl->timeline      = core::FrameTimeline();
    impl->frame_seconds = 0.0F;
    impl->started_at    = {};
    impl->viewer        = {};
    impl->device        = {};
    impl->queue_family  = -1;
    // The DEVICE's bookkeeping goes last, and only if it was this session's to leave: a session that goes away stops
    // constraining the device's watermark, which is what lets the remaining ones advance it.
    if (impl->clock != nullptr && impl->clock_window_live)
    {
        impl->clock->removeWindow(impl->clock_window);
    }
    impl->clock_window_live = false;
    impl->clock             = nullptr;
    impl->shared_retirement = nullptr;
}

bool OffscreenSession::initialized() const noexcept
{
    return impl && impl->ready;
}

core::FrameToken OffscreenSession::beginFrame()
{
    if (!impl || !impl->ready)
    {
        return {};
    }

    // The frame's time stamp is sampled HERE and does not move while the frame is open, exactly as the windowed
    // session samples it: every pass of this frame binds the same "now".
    const std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - impl->started_at;
    impl->frame_seconds                        = elapsed.count();

    // The viewer's frame step, with NO WINDOW to acquire from: this creates the frame stamp the record step needs,
    // advances the task's slot index (which is what makes the slot probe answer one more slot per frame) and
    // acquires nothing, because there is no window in the viewer.
    impl->viewer->advanceToNextFrame();
    impl->viewer->handleEvents();

    if (!impl->slot_tracker.known())
    {
        probeSlots();
    }

    return timeline().begin();
}

void OffscreenSession::probeSlots()
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
    if (impl->shared_retirement != nullptr && impl->clock != nullptr)
    {
        // A device's park window is the WIDEST any of its frame sources learned: a park made during this session's
        // frame may name something a window with more slots still has in flight (see core::DeviceClock::widestSlots).
        impl->clock->setWindowSlots(impl->clock_window, impl->slots);
        impl->shared_retirement->setSlots(impl->clock->widestSlots());
    }
    else
    {
        impl->retirement.setSlots(impl->slots);
    }

    if (impl->diagnostics == nullptr || impl->slot_tracker.agreesWithAssumption())
    {
        return;
    }
    impl->diagnostics->report(DiagnosticSeverity::Info, DiagnosticCategory::UnsupportedRequest,
                              vn::String::fromUtf8("the off-screen session's in-flight slot count is " +
                                                   std::to_string(impl->slots) + ", not the " +
                                                   std::to_string(core::kAssumedInFlightSlots) +
                                                   " this code was written against (learned over " +
                                                   std::to_string(impl->slot_tracker.framesObserved()) + " frames)"));
}

bool OffscreenSession::commitFrame()
{
    if (!impl || !impl->ready)
    {
        return false;
    }
    if (!timeline().hasOpenFrame())
    {
        if (impl->diagnostics != nullptr)
        {
            impl->diagnostics->report(DiagnosticSeverity::Warning, DiagnosticCategory::PassProtocolViolation,
                                      vn::String::fromUtf8("the off-screen commitFrame() had no open frame"));
        }
        return false;
    }

    const core::FrameToken token        = timeline().current();
    const std::uint64_t    frame_number = token.frame;

    impl->viewer->update();

    std::string failure;
    if (!detail::submitViewerTasks(*impl->viewer, failure))
    {
        // The submission did NOT happen: nothing was handed to the queue, so nothing may claim it did - no
        // completion evidence and no present. The frame itself is over (its graph was recorded, so it cannot be
        // retried): the timeline abandons the token, which keeps the submitted watermark a count of SUBMISSIONS.
        timeline().abandoned(token);
        retirement().advance(timeline());
        ++impl->lost_frames;
        if (impl->diagnostics != nullptr)
        {
            impl->diagnostics->report(DiagnosticSeverity::Error, DiagnosticCategory::SubmissionFailed,
                                      vn::String::fromUtf8("the off-screen frame's submission did not happen, so "
                                                           "nothing it recorded was performed - the failure was: " +
                                                           failure));
        }
        return false;
    }

    if (impl->slots != 0 && frame_number > impl->slots)
    {
        // THIS SESSION'S evidence, not the device's: the recycled slot proves the work IT submitted up to this frame
        // is finished. The device folds every source's report by minimum (see core::DeviceClock) - which is the
        // whole reason an off-screen frame has to report at all: without it the watermark would stay pinned where
        // this session joined.
        const std::uint64_t proven = frame_number - impl->slots;
        if (impl->clock != nullptr && impl->clock_window_live)
        {
            impl->clock->windowCompleted(impl->clock_window, proven);
        }
        else
        {
            timeline().completeUpTo(proven);
        }
    }

    // NO PRESENT. The frame's work draws into targets that are not the presentation's, so there is no image to hand
    // back and nothing to show (see the header).
    timeline().submitted(token);
    // The frame's LAST step: a park made after this would be dated against the next frame and released one frame
    // early (the ordering rule core::RetirementQueue documents).
    retirement().advance(timeline());
    return true;
}

std::uint32_t OffscreenSession::slots() const noexcept
{
    return impl ? impl->slots : 0;
}

std::uint64_t OffscreenSession::framesPresented() const noexcept
{
    return 0;
}

std::uint64_t OffscreenSession::lostFrames() const noexcept
{
    return impl ? impl->lost_frames : 0;
}

float OffscreenSession::frameSeconds() const noexcept
{
    return impl ? impl->frame_seconds : 0.0F;
}

std::size_t OffscreenSession::deviceWaits() const noexcept
{
    if (impl == nullptr)
    {
        return 0;
    }
    return impl->shared_retirement != nullptr ? impl->shared_retirement->deviceWaits() : impl->retirement.deviceWaits();
}

int OffscreenSession::queueFamily() const noexcept
{
    return impl ? impl->queue_family : -1;
}

core::FrameTimeline& OffscreenSession::timeline() noexcept
{
    return impl->clock != nullptr ? impl->clock->timeline() : impl->timeline;
}

core::RetirementQueue& OffscreenSession::retirement() noexcept
{
    return impl->shared_retirement != nullptr ? *impl->shared_retirement : impl->retirement;
}

::vsg::ref_ptr<::vsg::CommandGraph> OffscreenSession::makeFrameGraph() const
{
    if (!impl || !impl->ready)
    {
        return {};
    }
    // Bound to the DEVICE and its queue family - the framework's spelling of "a submission with no window": no
    // surface, no acquire, no present, only the queue the work is handed to.
    return ::vsg::CommandGraph::create(impl->device, impl->queue_family);
}

bool OffscreenSession::assignFrameGraphs(const ::vsg::CommandGraphs& graphs)
{
    if (!impl || !impl->ready || impl->viewer == nullptr)
    {
        return false;
    }
    // THE TASK SURVIVES THE HANDOVER: it owns the fences and semaphores a submission in flight still names, so the
    // graphs are given to it instead of the task list being rebuilt (the windowed session's rule, for the same
    // measured reason).
    if (impl->viewer->recordAndSubmitTasks.empty())
    {
        impl->viewer->assignRecordAndSubmitTaskAndPresentation(graphs);
    }
    else
    {
        for (const auto& task : impl->viewer->recordAndSubmitTasks)
        {
            task->commandGraphs = graphs;
        }
    }

    // The compile pass follows immediately: a graph the viewer has never seen has nodes whose implementations
    // (descriptor sets above all) are created by exactly that pass, and the record step would otherwise find them
    // missing.
    const ::vsg::CompileResult compiled = impl->viewer->compile();
    if (!compiled)
    {
        if (impl->diagnostics != nullptr)
        {
            impl->diagnostics->report(DiagnosticSeverity::Warning, DiagnosticCategory::InitFailed,
                                      vn::String::fromUtf8(std::string("the content of an off-screen frame did not "
                                                                       "compile: ") +
                                                           compiled.message));
        }
        return false;
    }
    return true;
}

}  // namespace api

VN_VSG_NS_END
