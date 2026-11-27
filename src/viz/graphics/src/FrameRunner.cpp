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
        // PUBLISHED LAST: a reader that sees `State::Running` sees the id it is compared against (see state_).
        state_.store(State::Running, std::memory_order_release);
    }
}

bool FrameRunner::onRunnerThread() const noexcept
{
    // THE THREAD THAT IS STILL INSIDE ITS LAST JOB IS STILL THE FRAME THREAD (see Stopping): its wait must not turn into a
    // self-wait just because its teardown has been asked for.
    const State state = state_.load(std::memory_order_acquire);
    return (state == State::Running || state == State::Stopping) && std::this_thread::get_id() == thread_id_;
}

bool FrameRunner::accepting() const noexcept
{
    return state_.load(std::memory_order_acquire) == State::Running;
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
    if (mode_ == RunnerMode::Inline || onRunnerThread()) {
        // Inline - and a job asked for FROM the engine thread runs where it stands (waiting on itself would deadlock).
        // Neither changes WHAT the job does; only where it runs.
        runHere(job);
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    // ONE IN FLIGHT: the previous job is finished before this one is handed over - which is also what makes a job that
    // runs HERE (below) obey the same rule: a runner a stop has left is still not a place for two jobs at once.
    done_.wait(lock, [this]() { return !has_request_; });
    if (state_.load(std::memory_order_acquire) != State::Running) {
        // A RUNNER A STOP HAS LEFT IS AN INLINE ONE (see stop): the loop takes no new job, so this one runs here.
        lock.unlock();
        runHere(job);
        return;
    }
    request_     = std::move(job);
    has_request_ = true;
    wake_.notify_one();
}

bool FrameRunner::wait(std::chrono::milliseconds timeout)
{
    if (mode_ == RunnerMode::Inline) {
        return true; // nothing was ever handed over: an inline runner has nothing that could still be running
    }
    if (onRunnerThread()) {
        // THE FRAME THIS THREAD IS INSIDE cannot be finished by this thread: the honest answer is "not answered yet",
        // never a wait for itself (see onRunnerThread).
        return false;
    }
    // WHETHER THE THREAD IS RUNNING, LEAVING OR JOINED, THE ANSWER IS THE SAME QUESTION: is a handed-over job still in
    // flight? (A joined runner has none, so it answers true at once - not because the thread is gone, but because
    // nothing was left on it.)
    std::unique_lock<std::mutex> lock(mutex_);
    return done_.wait_for(lock, timeout, [this]() { return !has_request_; });
}

void FrameRunner::waitIdle()
{
    if (mode_ == RunnerMode::Inline || onRunnerThread()) {
        return; // inline: nothing was ever handed over; the frame thread: no self-wait
    }
    // NOT GATED ON "IS THERE A THREAD": a stop asked for from the frame thread leaves the last job running while the
    // thread itself still has to be joined, and this is the wait that must not answer "idle" under it (see state_).
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this]() { return !has_request_; });
}

void FrameRunner::run(Job job)
{
    submit(std::move(job));
    // WAIT FOR WHAT WAS HANDED OVER, whatever the state is by now: a stop landing between the two must not make the
    // caller return before its own job ran (the loop drains the request it was woken for).
    if (mode_ == RunnerMode::Threaded && !onRunnerThread()) {
        (void)wait();
    }
}

void FrameRunner::stop()
{
    if (mode_ == RunnerMode::Inline) {
        return;
    }
    if (state_.load(std::memory_order_acquire) == State::Stopped) {
        return; // already joined: idempotent
    }
    if (state_.load(std::memory_order_acquire) == State::Running) {
        {
            // THE INLINE ANSWER LANDS UNDER THE SAME LOCK `submit` DECIDES UNDER (see `accepting` / `submit`): a job
            // submitted from here on runs on the caller's thread, and one already handed over is still run by the loop
            // (it drains the request it was woken for before leaving) - so a job is never accepted and then dropped.
            const std::lock_guard<std::mutex> lock(mutex_);
            state_.store(State::Stopping, std::memory_order_release);
        }
        wake_.notify_all();
    }
    if (onRunnerThread()) {
        // A THREAD CANNOT JOIN ITSELF. The loop leaves after the job it is inside, and the next stop() from another
        // thread - the pump's destructor, shutdown - performs the join. Destroying the runner here would leave a
        // joinable thread for its own destructor, which is a terminate; that is a defect of the caller, not a state to
        // paper over (the same answer FrameGate's destructor gives).
        return;
    }
    thread_.join();
    // A JOINED RUNNER IS AN INLINE ONE (see state_): published after the join, so a waiter that sees it knows the thread
    // is gone - and `accepting()` already answered false since the stop was asked for.
    state_.store(State::Stopped, std::memory_order_release);
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
        // LEAVING IS AN ASKED-FOR STOP, NOT "SOMETHING OTHER THAN RUNNING": the loop may already be waiting before its
        // constructor publishes `Running`, and it must not read that as a stop (the runner is not visible to anybody yet,
        // so no job can have been handed over either).
        wake_.wait(lock, [this]() {
            const State state = state_.load(std::memory_order_acquire);
            return has_request_ || state == State::Stopping || state == State::Stopped;
        });
        if (!has_request_) {
            return; // asked to leave, and nothing was in flight
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
