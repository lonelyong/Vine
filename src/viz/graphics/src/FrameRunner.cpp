#include "FrameRunner.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

FrameRunner::FrameRunner(RunnerMode mode)
  : mode_(mode)
{
    if (mode_ == RunnerMode::Threaded) {
        thread_ = std::thread([this]() { loop(); });
        thread_id_ = thread_.get_id();
    }
}

bool FrameRunner::onRunnerThread() const noexcept
{
    return thread_.joinable() && std::this_thread::get_id() == thread_id_;
}

FrameRunner::~FrameRunner()
{
    stop();
}

void FrameRunner::runHere(const Job& job)
{
    // A JOB THAT THROWS STILL OWES THE COUNT. Inline the caller sees the exception (it is their own thread), but the
    // job did run, and the frame counter must not depend on how it ended.
    try {
        job();
    }
    catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++frames_;
        ++failures_;
        throw;
    }
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
    if (onRunnerThread()) {
        // THE FRAME THIS THREAD IS INSIDE cannot be finished by this thread: the honest answer is "not answered yet",
        // never a wait for itself (see onRunnerThread).
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    return done_.wait_for(lock, timeout, [this]() { return !has_request_; });
}

void FrameRunner::waitIdle()
{
    if (!thread_.joinable() || onRunnerThread()) {
        return; // inline, stopped, or the frame thread asking about its own frame: no self-wait
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
    if (onRunnerThread()) {
        // A THREAD CANNOT JOIN ITSELF. The loop leaves after the job it is inside (stopping_ is set), and the next
        // stop() from another thread - the pump's destructor, shutdown - performs the join. Destroying the runner here
        // would leave a joinable thread for its own destructor, which is a terminate; that is a defect of the caller,
        // not a state to paper over (the same answer FrameGate's destructor gives).
        return;
    }
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

std::uint64_t FrameRunner::failures() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return failures_;
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
        // THE ONE-IN-FLIGHT HAND-SHAKE IS THE RUNNER'S, so it survives a job that throws: an exception leaving this
        // thread function calls `std::terminate`, and one leaving `has_request_` set would block every later submit
        // forever. Who the failure belongs to is the job's business (the engine answers its own frame - see
        // RenderEngine::answerEngineHalf); this is only the guarantee that no job can take the runner down with it.
        bool failed = false;
        try {
            job();
        }
        catch (...) {
            failed = true;
        }
        lock.lock();
        ++frames_;
        if (failed) {
            ++failures_;
        }
        has_request_ = false;
        done_.notify_all();
    }
}

} // namespace detail

VN_GRAPHICS_NS_END
