#pragma once

#include <exception>
#include <string>

#include <vsg/app/Viewer.h>
#include <vsg/core/Exception.h>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The two pieces every session's frame drive is made of, shared by the windowed and the off-screen session.
 *
 * @details
 * - WHY THEY ARE SHARED RATHER THAN WRITTEN TWICE. A windowed session (api::Session) and an off-screen one
 *   (api::OffscreenSession) submit through the SAME framework object, and the two things that must not differ are
 *   exactly the two below: that events are never pumped behind the host's back, and that a submission the driver
 *   refused is KEPT rather than dropped. A copy of either in the second session is a place for the two frame paths
 *   to drift apart - and the drift would be silent, because a dropped VkResult looks like a frame that happened.
 * - WHAT THEY ARE NOT. Neither is a session: they hold no device, no swapchain and no frame clock. The session owns
 *   those; these are the mechanics it calls.
 */
VN_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief A viewer that never pumps the platform's events: the host owns the message loop.
 *
 * @details
 * - Re-entering the toolkit from inside a frame call is how a backend recurses through someone else's event loop
 *   (the dispatched message triggers a host event, which asks for another frame, which pumps again).
 * - Input arrives from the host instead, so window polling is off and only the buffered framework events are dropped
 *   - the same shape the windowed session settled on, for the same reason.
 * - A viewer with no window is a legitimate user of this class: the off-screen session's viewer polls nothing at all,
 *   and the class states that explicitly instead of relying on an empty window list.
 */
class EmbeddedViewer : public ::vsg::Inherit<::vsg::Viewer, EmbeddedViewer> {
  public:
    /**
     * @brief Drops the buffered framework events when asked, and polls nothing.
     *
     * @param discard_previous_events Whether the events queued for the previous frame are dropped.
     * @return false always: no platform event was polled (the host dispatches input itself).
     */
    bool pollEvents(bool discard_previous_events) override
    {
        if (discard_previous_events) {
            this->getEvents().clear();
        }
        return false;
    }
};

/**
 * @brief Submits every record-and-submit task of @p viewer the way the viewer's own recordAndSubmit() does, and
 *        KEEPS the failure.
 *
 * @details
 * - WHY NOT CALL viewer->recordAndSubmit(). It returns void and DROPS each task's VkResult, so a queue submit the
 *   driver refused (device lost, out of memory) would look exactly like a frame that happened - and a session's frame
 *   path must be able to say that it did not.
 * - The viewer's own steps are taken here: reset every command graph (the exclusive state an ExecuteCommands node
 *   hands over), then one submit per task on this thread - the threading path is the viewer's own and a session never
 *   enables it.
 * - A session has ONE task (one window or none, one device, one queue family), so a failure means nothing at all was
 *   handed to the queue: there is no "some tasks submitted" half to reason about.
 *
 * @param viewer  The session's viewer.
 * @param failure Receives a human-readable reason when the submission did not happen.
 * @return true when every task reported success; false when one did not (in @p failure).
 */
inline bool submitViewerTasks(::vsg::Viewer& viewer, std::string& failure)
{
    for (const auto& task : viewer.recordAndSubmitTasks) {
        for (auto& graph : task->commandGraphs) {
            graph->reset();
        }
    }

    for (const auto& task : viewer.recordAndSubmitTasks) {
        try {
            // The frame stamp the viewer itself would have handed the task (it owns it): a submission that
            // sees no stamp is a submission of no frame.
            const ::vsg::ref_ptr<::vsg::FrameStamp> frame_stamp(viewer.getFrameStamp());
            const VkResult                          result = task->submit(frame_stamp);
            if (result != VK_SUCCESS) {
                failure = "the queue submission reported VkResult " + std::to_string(static_cast<int>(result));
                return false;
            }
        }
        catch (const ::vsg::Exception& error) {
            // vsg's own failure vocabulary, and NOT a std::exception (a plain struct with message + result):
            // a command buffer that cannot be allocated throws here, out of the record step.
            failure = "vsg::Exception: " + error.message + " (VkResult " + std::to_string(error.result) + ")";
            return false;
        }
        catch (const std::exception& error) {
            failure = std::string("std::exception: ") + error.what();
            return false;
        }
    }
    return true;
}

} // namespace detail

VN_VSG_NS_END
