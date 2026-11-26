#include "FramePump.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

FramePump::FramePump()
  : slots_{ std::make_unique<EngineFrame>(), std::make_unique<EngineFrame>() }
{
    // THE CURRENT VALUE EXISTS FROM THE START (see frameDescription): before the first frame it is the empty value a
    // host reads, not a null pointer.
    current_ = slots_[0].get();
}

FramePump::~FramePump()
{
    stop();
}

EngineFrame& FramePump::beginFrame()
{
    // ONE FRAME IN FLIGHT: the previous engine half is finished before this value moves, so a caller cannot queue
    // frames and no two jobs ever touch the publications or the device at once (design 2.12, D12).
    waitIdle();
    EngineFrame& next = *slots_[slot_];
    slot_             = (slot_ + 1) % slots_.size();
    next.number       = ++number_;
    current_          = &next;
    return next;
}

EngineFrame& FramePump::currentFrame() noexcept
{
    return *current_;
}

const EngineFrame& FramePump::currentFrame() const noexcept
{
    return *current_;
}

EngineFrame* FramePump::takePendingMicros() noexcept
{
    return pending_micros_.exchange(nullptr, std::memory_order_acquire);
}

void FramePump::submit(Job job)
{
    if (runner_ == nullptr) {
        // Inline: the calling thread IS the engine thread, so "hand it over" has nowhere else to go. The answer is
        // complete when this returns - which is what `frame()` reports as Ran.
        job();
        return;
    }
    runner_->submit(std::move(job));
}

void FramePump::waitIdle()
{
    if (runner_ != nullptr) {
        runner_->waitIdle();
    }
}

bool FramePump::wait(std::uint64_t number, std::chrono::milliseconds timeout)
{
    if (number == 0U) {
        return false; // no frame was ever handed out, so nothing can be "answered"
    }
    if (completed_.load(std::memory_order_acquire) >= number) {
        return true;
    }
    if (runner_ == nullptr) {
        // Inline: every frame this pump handed out was answered before submit() returned, so a number it does not know
        // is simply not built yet.
        return completed_.load(std::memory_order_acquire) >= number;
    }
    (void)runner_->wait(timeout);
    return completed_.load(std::memory_order_acquire) >= number;
}

std::uint64_t FramePump::lastNumber() const noexcept
{
    return number_;
}

bool FramePump::threaded() const noexcept
{
    return runner_ != nullptr;
}

bool FramePump::onEngineThread() const noexcept
{
    return runner_ != nullptr && runner_->onRunnerThread();
}

bool FramePump::busy() const
{
    return runner_ != nullptr && runner_->busy();
}

void FramePump::setThreaded(bool threaded)
{
    if (threaded == (runner_ != nullptr)) {
        return; // already there
    }
    if (onEngineThread()) {
        // A MODE CHANGE FROM INSIDE THE FRAME IS REFUSED, not half applied: stopping the runner from its own thread
        // cannot join it (see FrameRunner::stop), and dropping it here would leave a joinable thread for its destructor.
        // The caller may switch between frames, never from a frame body.
        return;
    }
    if (threaded) {
        // The thread exists only when a host asked for one: an engine that never does allocates nothing and starts
        // nothing, and the inline path stays literally the same code.
        runner_ = std::make_unique<FrameRunner>(RunnerMode::Threaded);
        return;
    }
    // Back to inline: a frame in flight is finished first (the thread is joined, not detached), so nothing is being
    // answered while the host goes on to describe frames here.
    runner_->waitIdle();
    runner_->stop();
    runner_.reset();
}

void FramePump::notePendingMicros(EngineFrame& frame) noexcept
{
    pending_micros_.store(&frame, std::memory_order_release);
}

void FramePump::publishPresent(std::uint64_t number, PresentOutcome outcome) noexcept
{
    // The picture's fate and the frame's id travel as ONE word: a host reading the record while the next frame is
    // being answered must never see this frame's outcome beside that frame's id.
    present_.store((number << 8U) | static_cast<std::uint64_t>(outcome), std::memory_order_release);
}

void FramePump::markCompleted(std::uint64_t number) noexcept
{
    completed_.store(number, std::memory_order_release);
}

void FramePump::publishResult(FrameResult result)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    result_ = std::move(result);
}

FrameResult FramePump::result() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

std::uint64_t FramePump::packedPresent() const noexcept
{
    return present_.load(std::memory_order_acquire);
}

void FramePump::stop()
{
    if (runner_ != nullptr) {
        runner_->waitIdle();
        runner_->stop(); // a call from the frame thread only asks it to leave (see FrameRunner::stop)
        if (onEngineThread()) {
            // THE JOIN BELONGS TO ANOTHER THREAD: the loop exits after the job it is inside, and the next stop() - from
            // the destructor or from shutdown() on the host's thread - performs it.
            return;
        }
        runner_.reset();
    }
}

} // namespace detail

VN_GRAPHICS_NS_END
