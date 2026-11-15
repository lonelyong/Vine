#include <vine/graphics/backend/FrameTimeline.hpp>

#include <algorithm>

VN_GRAPHICSBACKEND_NS_BEGIN

FrameToken FrameTimeline::begin() noexcept
{
    // MINTED AT OPENING, not derived from the submission count: two drivers may open a frame before either submits,
    // and they must not share a number (two frames that share one collapse into one in every watermark comparison).
    minted_ = std::max(minted_, submitted_) + 1U;
    open_.push_back(minted_);
    return FrameToken{ minted_ };
}

void FrameTimeline::submitted(FrameToken token) noexcept
{
    const auto found = std::find(open_.begin(), open_.end(), token.frame);
    if (found == open_.end()) {
        // A stale or foreign token: the timeline only moves for a frame it actually opened.
        return;
    }
    open_.erase(found);
    submitted_ = std::max(submitted_, token.frame);
}

void FrameTimeline::abandoned(FrameToken token) noexcept
{
    const auto found = std::find(open_.begin(), open_.end(), token.frame);
    if (found == open_.end()) {
        return; // the same rule as submitted(): a stale token never closes a frame it does not name
    }
    // The token is spent (the frame cannot be retried), but nothing was handed to the queue: the frame is
    // over and the SUBMITTED watermark deliberately does not move (see the declaration).
    open_.erase(found);
}

void FrameTimeline::completeUpTo(std::uint64_t frame) noexcept
{
    if (frame > completed_) {
        completed_ = frame;
    }
}

FrameToken FrameTimeline::current() const noexcept
{
    return open_.empty() ? FrameToken{} : FrameToken{ open_.back() };
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
    return !open_.empty();
}

bool FrameTimeline::isOpenFrame(FrameToken token) const noexcept
{
    return std::find(open_.begin(), open_.end(), token.frame) != open_.end();
}

std::uint64_t FrameTimeline::retirePoint(std::uint64_t submitted_frame, std::uint32_t slots) noexcept
{
    return submitted_frame + static_cast<std::uint64_t>(slots) + 1;
}

VN_GRAPHICSBACKEND_NS_END
