#include <vine/graphics/FrameTimeline.hpp>

#include <algorithm>

VN_GRAPHICS_NS_BEGIN

FrameToken FrameTimeline::begin() noexcept
{
    // MINTED AT OPENING: two drivers may open a frame before either submits, and they must not share a number (two
    // frames that share one collapse into one in every comparison a token is used for).
    ++minted_;
    open_.push_back(minted_);
    return FrameToken{ minted_ };
}

void FrameTimeline::submitted(FrameToken token) noexcept
{
    const auto found = std::find(open_.begin(), open_.end(), token.frame);
    if (found == open_.end()) {
        // A stale or foreign token: the timeline only spends a frame it actually opened.
        return;
    }
    open_.erase(found);
}

void FrameTimeline::abandoned(FrameToken token) noexcept
{
    const auto found = std::find(open_.begin(), open_.end(), token.frame);
    if (found == open_.end()) {
        return; // the same rule as submitted(): a stale token never closes a frame it does not name
    }
    // The token is spent (the frame cannot be retried) but nothing was handed to the queue, so no submission is minted
    // (see the declaration).
    open_.erase(found);
}

FrameToken FrameTimeline::current() const noexcept
{
    return open_.empty() ? FrameToken{} : FrameToken{ open_.back() };
}

bool FrameTimeline::hasOpenFrame() const noexcept
{
    return !open_.empty();
}

bool FrameTimeline::isOpenFrame(FrameToken token) const noexcept
{
    return std::find(open_.begin(), open_.end(), token.frame) != open_.end();
}

VN_GRAPHICS_NS_END
