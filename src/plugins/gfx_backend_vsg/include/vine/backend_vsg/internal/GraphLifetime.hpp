#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/app/CommandGraph.h>

#include <vine/backend_vsg/backend_vsg_global.hpp>

VN_BACKEND_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief The command graphs a session has stopped recording but a submission may still READ.
 *
 * @details
 * - WHY A GRAPH OUTLIVES ITS FRAME. A session hands the viewer the frame's `vsg::CommandGraph` and the viewer records
 *   and submits it; the graph's nodes own the Vulkan objects that submission names (buffers above all, whose
 *   destructors release driver memory). Replacing `RecordAndSubmitTask::commandGraphs` on the next frame drops the
 *   last reference to the previous graph WHILE ITS SUBMISSION MAY STILL BE RUNNING - the sessions pipeline frames
 *   (`core::FrameTimeline` plus the slot rings), so "the last commit returned" does not mean "the queue is empty".
 *   Measured with a memory checker on the reference backend: the driver's worker thread read a buffer 96 bytes into a
 *   block `vsg::Buffer::~Buffer` had already freed, i.e. a use-after-free the validation layer cannot see.
 * - THE EVIDENCE IS THE SESSION'S OWN. `core::FrameTimeline::completedFrame()` is what this backend already trusts for
 *   content retirement and slot recycling (the recycled slot is the proof); a retired graph is dropped on exactly that
 *   proof, one frame after it could still be named - the same rule, one home.
 * - IT IS NOT A DEVICE IDLE. Waiting per frame would serialize the pipeline the sessions are built around; keeping the
 *   graph alive costs one `ref_ptr` and nothing else. `VsgBackend::shutdown` still idles the device before dropping
 *   everything (see there), because a session can end with frames it never got to complete.
 *
 * @warning Not thread-safe: it is touched from the thread that drives the session, like the rest of the frame path.
 */
class GraphLifetime
{
  public:
    /**
     * @brief Keeps @p graphs alive until the session proves the frame that submits them complete.
     *
     * @details
     * - CALLED WITH WHAT THE TASKS ARE ABOUT TO FORGET, before they are handed the new frame's graphs: an empty list
     *   (the viewer's own bootstrap graph) adds nothing.
     *
     * @param graphs The graphs being replaced (borrowed for this call; a reference is kept).
     * @param frame  The frame number the outgoing graphs were submitted with (see FrameTimeline).
     */
    void retire(const ::vsg::CommandGraphs& graphs, std::uint64_t frame)
    {
        for (const ::vsg::ref_ptr<::vsg::CommandGraph>& graph : graphs) {
            if (graph != nullptr) {
                retired_.emplace_back(frame, graph);
            }
        }
    }

    /**
     * @brief Drops every graph whose submitting frame the session has proven complete.
     *
     * @param completed The session's completion watermark (`core::FrameTimeline::completedFrame()`).
     */
    void dropCompleted(const std::uint64_t completed) noexcept
    {
        std::erase_if(retired_, [completed](const auto& entry) { return entry.first <= completed; });
    }

    /** @brief Drops everything; the caller states that the device is idle and nothing can still read them. */
    void clear() noexcept
    {
        retired_.clear();
    }

    /** @brief How many graphs are still being kept alive (a witness a test can read). */
    [[nodiscard]] std::size_t holding() const noexcept
    {
        return retired_.size();
    }

  private:
    /// The graph and the frame that submitted it; kept in submission order.
    std::vector<std::pair<std::uint64_t, ::vsg::ref_ptr<::vsg::CommandGraph>>> retired_{};
};

} // namespace detail

VN_BACKEND_VSG_NS_END
