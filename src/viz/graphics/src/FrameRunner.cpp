#include "FrameRunner.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

FrameRunner::FrameRunner(RunnerMode mode)
  : mode_(mode)
{
    if (mode_ == RunnerMode::Threaded) {
        thread_ = std::thread([this]() { loop(); });
    }
}

FrameRunner::~FrameRunner()
{
    stop();
}

void FrameRunner::runHere(const Job& job)
{
    job();
    std::lock_guard<std::mutex> lock(mutex_);
    ++frames_;
}

void FrameRunner::submit(Job job)
{
    if (mode_ == RunnerMode::Inline || !thread_.joinable() || std::this_thread::get_id() == thread_.get_id()) {
        // Inline - and a stopped runner IS one, and a job asked for FROM the engine thread runs where it stands (waiting
        // on itself would deadlock). None of these is a reason to change WHAT the job does; only where it runs.
        runHere(job);
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    // ONE IN FLIGHT: the previous job is finished before this one is handed over, so a caller cannot queue frames the
    // engine side never asked for and no two of them ever touch the mirrors at once.
    done_.wait(lock, [this]() { return !has_request_; });
    request_     = std::move(job);
    has_request_ = true;
    wake_.notify_one();
}

bool FrameRunner::wait(std::chrono::milliseconds timeout)
{
    if (!thread_.joinable()) {
        return true; // an inline (or stopped) runner has nothing that could still be running
    }
    std::unique_lock<std::mutex> lock(mutex_);
    return done_.wait_for(lock, timeout, [this]() { return !has_request_; });
}

void FrameRunner::waitIdle()
{
    if (!thread_.joinable()) {
        return; // an inline (or stopped) runner has nothing in flight, ever
    }
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this]() { return !has_request_; });
}

void FrameRunner::run(Job job)
{
    submit(std::move(job));
    if (mode_ == RunnerMode::Threaded && thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) {
        (void)wait();
    }
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

bool FrameRunner::busy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return has_request_;
}

void FrameRunner::loop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [this]() { return has_request_ || stopping_; });
        if (!has_request_) {
            return; // stopping, and nothing was in flight
        }
        // The work runs OUTSIDE the lock: the caller only needs to know when it is done, not to watch it happen.
        Job job = std::move(request_);
        request_ = Job();
        lock.unlock();
        job();
        lock.lock();
        ++frames_;
        has_request_ = false;
        done_.notify_all();
    }
}

} // namespace detail

VN_GRAPHICS_NS_END
