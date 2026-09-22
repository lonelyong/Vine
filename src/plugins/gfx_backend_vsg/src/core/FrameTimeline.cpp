#include <vine/vsg/core/FrameTimeline.hpp>

V_VSG_NS_BEGIN

namespace core
{

FrameToken FrameTimeline::begin() noexcept
{
    if (open_)
    {
        // Already inside a frame: hand the same token back rather than inventing a second frame that
        // nothing would ever commit. The protocol layer reports the mismatch.
        return open_;
    }
    open_ = FrameToken{submitted_ + 1};
    return open_;
}

void FrameTimeline::submitted(FrameToken token) noexcept
{
    if (!open_ || token.frame != open_.frame)
    {
        // A stale or foreign token: the timeline only moves for the frame it actually opened.
        return;
    }
    submitted_ = open_.frame;
    open_      = FrameToken{};
}

void FrameTimeline::abandoned(FrameToken token) noexcept
{
    if (!open_ || token.frame != open_.frame)
    {
        return;  // the same rule as submitted(): a stale token never closes a frame it does not name
    }
    // The token is spent (the frame cannot be retried), but nothing was handed to the queue: the frame is
    // over and the SUBMITTED watermark deliberately does not move (see the declaration).
    open_ = FrameToken{};
}

void FrameTimeline::completeUpTo(std::uint64_t frame) noexcept
{
    if (frame > completed_)
    {
        completed_ = frame;
    }
}

FrameToken FrameTimeline::current() const noexcept
{
    return open_;
}

std::uint64_t FrameTimeline::submittedFrame() const noexcept
{
    return submitted_;
}

std::uint64_t FrameTimeline::completedFrame() const noexcept
{
    return completed_;
}

bool FrameTimeline::hasOpenFrame() const noexcept
{
    return static_cast<bool>(open_);
}

bool FrameTimeline::isOpenFrame(FrameToken token) const noexcept
{
    return open_ && token.frame == open_.frame;
}

std::uint64_t FrameTimeline::retirePoint(std::uint64_t submitted_frame, std::uint32_t slots) noexcept
{
    return submitted_frame + static_cast<std::uint64_t>(slots) + 1;
}

}  // namespace core

V_VSG_NS_END
