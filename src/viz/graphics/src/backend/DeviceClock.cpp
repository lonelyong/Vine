#include <vine/graphics/backend/DeviceClock.hpp>

#include <algorithm>

VN_GRAPHICSBACKEND_NS_BEGIN

DeviceClock::WindowId DeviceClock::addWindow() noexcept
{
    // A window that joins proves nothing about the frames before it existed, so it starts AT the watermark: starting
    // it at zero would hold the whole device's evidence at zero for as long as that window lives.
    windows_.push_back(Window{ completed_, 0, true });
    return static_cast<WindowId>(windows_.size() - 1U);
}

void DeviceClock::removeWindow(WindowId window) noexcept
{
    if (window >= windows_.size())
    {
        return;
    }
    windows_[window].live = false;
    updateWatermark();
}

void DeviceClock::setWindowSlots(WindowId window, std::uint32_t slots) noexcept
{
    if (window >= windows_.size())
    {
        return;
    }
    windows_[window].slots = slots;
}

void DeviceClock::windowCompleted(WindowId window, std::uint64_t frame) noexcept
{
    if (window >= windows_.size())
    {
        return;
    }
    Window& entry = windows_[window];
    if (entry.slots == 0)
    {
        // It does not know how many frames may be in flight, so it cannot say which frame its evidence covers: the
        // report is dropped rather than guessed at (see setWindowSlots).
        return;
    }
    // Monotonic, like the timeline's own watermark: completion is a fact about the past.
    entry.completed = std::max(entry.completed, frame);
    updateWatermark();
}

FrameToken DeviceClock::begin() noexcept
{
    return timeline_.begin();
}

void DeviceClock::submitted(FrameToken token) noexcept
{
    timeline_.submitted(token);
}

void DeviceClock::abandoned(FrameToken token) noexcept
{
    timeline_.abandoned(token);
}

FrameTimeline& DeviceClock::timeline() noexcept
{
    return timeline_;
}

const FrameTimeline& DeviceClock::timeline() const noexcept
{
    return timeline_;
}

std::uint64_t DeviceClock::completedFrame() const noexcept
{
    return completed_;
}

std::uint32_t DeviceClock::widestSlots() const noexcept
{
    std::uint32_t widest = 0;
    for (const Window& window : windows_)
    {
        if (window.live)
        {
            widest = std::max(widest, window.slots);
        }
    }
    return widest;
}

std::size_t DeviceClock::windows() const noexcept
{
    return static_cast<std::size_t>(std::count_if(windows_.begin(), windows_.end(),
                                                  [](const Window& window) { return window.live; }));
}

void DeviceClock::updateWatermark() noexcept
{
    // THE MINIMUM, over the windows that still exist (see the file note for why it is a minimum and not a maximum).
    // With no window at all the last watermark stands: nothing new was submitted, so nothing new is proven or
    // disproven.
    bool          any = false;
    std::uint64_t least = 0;
    for (const Window& window : windows_)
    {
        if (!window.live)
        {
            continue;
        }
        if (window.slots == 0)
        {
            // A window that never learned its in-flight count proves NOTHING: its evidence would be a guess, and a
            // guess in this direction destroys an object a submitted command buffer still names.
            return;
        }
        least = any ? std::min(least, window.completed) : window.completed;
        any   = true;
    }
    if (!any)
    {
        return;
    }
    completed_ = std::max(completed_, least);
    timeline_.completeUpTo(completed_);
}

VN_GRAPHICSBACKEND_NS_END
