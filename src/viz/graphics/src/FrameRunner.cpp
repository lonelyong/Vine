#include "FrameRunner.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

FrameRunner::FrameRunner(FrameWorker& worker, FrameMode mode)
  : worker_(worker)
  , mode_(mode)
{
    if (mode_ == FrameMode::Threaded) {
        thread_ = std::thread([this]() { loop(); });
    }
}

FrameRunner::~FrameRunner()
{
    stop();
}

std::size_t FrameRunner::run(FrameDescription& frame)
{
    // Inline: the caller IS the engine thread. A stopped runner is one too - it must run the frame rather than wait
    // for a thread that is gone. And a request from the engine thread itself would wait on itself, so that case runs
    // where it stands as well (see the class note).
    if (mode_ == FrameMode::Inline || !thread_.joinable() || std::this_thread::get_id() == thread_.get_id()) {
        return worker_.run(frame);
    }

    std::unique_lock<std::mutex> lock(mutex_);
    request_  = &frame;
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

FrameMode FrameRunner::mode() const noexcept
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
        lock.unlock();
        const std::size_t applied = worker_.run(*frame);
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
