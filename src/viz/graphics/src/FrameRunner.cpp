#include "FrameRunner.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

FrameRunner::FrameRunner(FrameWorker& worker, RunnerMode mode)
  : worker_(worker)
  , mode_(mode)
{
    if (mode_ == RunnerMode::Threaded) {
        thread_ = std::thread([this]() { loop(); });
    }
}

FrameRunner::~FrameRunner()
{
    stop();
}

std::size_t FrameRunner::run(FrameDescription& frame, FrameMode mode)
{
    if (mode_ == RunnerMode::Inline || !thread_.joinable()) {
        // Inline - and a stopped runner IS one: the frame runs here, exactly as the caller asked it to be answered
        // (a stopped runner is not a reason to change WHAT a frame asks; it is only a different thread).
        return worker_.run(frame, mode);
    }
    if (std::this_thread::get_id() == thread_.get_id()) {
        // Asked FROM the engine thread: waiting on itself would deadlock, so the frame runs where it stands.
        return worker_.run(frame, mode);
    }

    std::unique_lock<std::mutex> lock(mutex_);
    request_  = &frame;
    request_mode_ = mode;
    finished_ = false;
    wake_.notify_one();
    done_.wait(lock, [this]() { return finished_; });
    finished_ = false;
    const std::size_t applied = result_;
    return applied;
}

void FrameRunner::stop()
{
    if (!thread_.joinable()) {
        return; // already stopped: idempotent
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

RunnerMode FrameRunner::mode() const noexcept
{
    return mode_;
}

std::uint64_t FrameRunner::frames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
}

void FrameRunner::loop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [this]() { return request_ != nullptr || stopping_; });
        if (request_ == nullptr) {
            return; // stopping, and nothing was in flight
        }
        // The work runs OUTSIDE the lock: the caller only needs to know when it is done, not to watch it happen.
        FrameDescription* frame = request_;
        const FrameMode   mode  = request_mode_;
        lock.unlock();
        const std::size_t applied = worker_.run(*frame, mode);
        lock.lock();
        ++frames_;
        result_   = applied;
        request_  = nullptr;
        finished_ = true;
        done_.notify_one();
    }
}

} // namespace detail

VN_GRAPHICS_NS_END
