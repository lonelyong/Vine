#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/** @brief WHERE a frame's engine half runs (the source question is the publication channel's, see PublicationStore). */
enum class RunnerMode : std::uint8_t
{
    Inline,   ///< On the calling thread: no thread, no waiting ("the engine thread" IS the caller).
    Threaded, ///< On an engine thread of its own.
};

/**
 * @brief Runs ONE unit of engine-side work at a time, here or on a thread of its own.
 *
 * @details
 * - WHAT IT IS NOT: it is not the frame's work (that is the engine's two halves, see detail::expandFrame and the
 *   design's 2.12) and not the backend (that is the device). It owns exactly one thing the work must not: **the thread
 *   and the handshake around it**. Keeping the two apart is what lets the inline mode - the calling thread, no thread
 *   object at all - and the threaded mode be the same code path, which is the property the equivalence tests rest on.
 * - ONE JOB IN FLIGHT, STRICT ALTERNATION. `submit` waits for the previous job before handing the next one over, and
 *   `wait` says when the one in flight is done - so no two jobs are ever inside the publications or the device at
 *   once, and a caller cannot build up a queue of frames nobody asked for. It is deliberately NOT a pool: what a
 *   second frame in flight would mean for the RETURN contract (and for the swapchain) is a design decision (see
 *   `.ai/design/graphics-threading.md` 2.12 and D12), not a runner detail.
 * - IDLE MEANS WAITING, NOT POLLING. The engine thread blocks on a condition variable when nothing was submitted
 *   (design 2.8): an application that draws nothing costs nothing.
 * - ASKING FROM THE ENGINE THREAD RUNS INLINE. A job submitted from the thread that would run it would wait on
 *   itself; `submit` detects that and runs it where it stands. The same rule covers a stopped runner, which is an
 *   inline runner (see `stop`), so a caller cannot deadlock by not knowing which mode is live.
 * - ONE FACT SAYS WHERE A JOB WILL RUN (`G1` / `G4`): `accepting()` - true only while the thread exists AND has not
 *   been asked to leave. `submit` decides under the runner's own lock from that same fact, so there is no state in
 *   which the answer says "the thread" while the job runs here, and a `stop()` asked from the frame thread itself
 *   makes the runner inline BEFORE it returns (the loop can no longer take a job, even though the thread still has
 *   to be joined by somebody).
 *
 * @warning Every call may block: `submit` until the previous job finished, `wait` until this one does. Nothing here is
 * reentrant, and the job itself is single-threaded by contract: what it touches is touched by the engine thread and
 * never by the caller.
 */
class FrameRunner {
  public:
    /// The unit of work: one frame's engine half, as far as this class is concerned.
    using Job = std::function<void()>;

    /** @brief Builds a runner (and starts its thread when @p mode is Threaded).
     *
     * @param mode Where jobs run (see RunnerMode).
     */
    explicit FrameRunner(RunnerMode mode = RunnerMode::Threaded);

    /** @brief Stops the engine thread (if any) and joins it. */
    ~FrameRunner();

    FrameRunner(const FrameRunner&)            = delete;
    FrameRunner& operator=(const FrameRunner&) = delete;

  public:
    /** @brief Hands @p job over and returns; it runs after the previous one finished.
     *
     * @details
     * - Inline (or from the engine thread, or after `stop`) the job runs HERE, before this returns - an inline runner
     *   is the caller's thread, and "hand it over" has nowhere else to go.
     * - Threaded it waits for the job in flight (there is at most one) and then hands this one over: the call returns
     *   while the job is still to run, which is what `wait` is for.
     *
     * @param job What to run; it must be safe to run on the engine thread.
     */
    void submit(Job job);

    /** @brief Waits for the job in flight to finish.
     *
     * @param timeout How long to wait.
     * @return true when nothing is in flight any more (including "there never was"), false on timeout.
     */
    bool wait(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /** @brief Waits until nothing is in flight - no timeout, because the caller cannot proceed otherwise.
     *
     * @details
     * - For the two places that must not race a job: building the next frame (the one-in-flight rule) and shutting
     *   down. A bounded wait there would turn "the frame is slow" into "two jobs touch the mirrors".
     */
    void waitIdle();

    /** @brief Runs @p job and returns when it is done - the synchronous spelling of submit + wait.
     *
     * @param job What to run.
     */
    void run(Job job);

    /** @brief Stops the engine thread and joins it.
     *
     * @details
     * - Idempotent, and safe while the thread is idle. A job in flight is finished first (the thread is joined, not
     *   detached). After it, the runner behaves as an inline runner: a job submitted later runs on the caller's thread
     *   instead of waiting for a thread that is gone.
     * - THE INLINE ANSWER LANDS BEFORE THIS RETURNS, on every path (`accepting()`, and `submit` with it): a stop asked
     *   from the frame thread cannot join itself, but the loop takes no new job from that moment, so the runner must
     *   not keep advertising a thread that will never run one (review 19 `Z21`: a job handed to a loop that had left
     *   was accepted, never run, and left the one-in-flight handshake set for ever). The JOIN is what a later call
     *   from another thread completes.
     */
    void stop();

  public:
    /** @brief Gets where this runner was built to run jobs (it does not change: a mode is a lifetime decision).
     *
     * @return The mode. A runner with no live thread (before start, after `stop`) reports the mode it was built with,
     *         because the mode is what the work MEANS, not which thread happens to be alive.
     */
    [[nodiscard]] RunnerMode mode() const noexcept;

    /**
     * @brief Answers whether the CALLING thread is this runner's frame thread.
     *
     * @details
     * - THE QUESTION EVERY SELF-WAIT ASKS FIRST (see waitIdle / wait / stop): a job running on the frame thread that
     *   waited for "the frame in flight" would wait for itself, and no amount of timeout makes that answer.
     * - TRUE WHILE THE THREAD IS STILL AROUND, INCLUDING AFTER A STOP WAS ASKED FOR: the loop is inside its last job,
     *   and that job must not wait for itself (see `accepting` for the "will it run my job" question).
     *
     * @return true when the frame thread is the caller.
     */
    [[nodiscard]] bool onRunnerThread() const noexcept;

    /** @brief Answers whether a job submitted NOW is handed to the engine thread.
     *
     * @details
     * - THE ONE FACT THAT SAYS WHERE A JOB WILL RUN (`G1`): true while the thread exists and no stop was asked for;
     *   false for an inline runner, and false the moment `stop()` is asked for - INCLUDING from the frame thread, where
     *   the join still has to wait for somebody else but the loop can no longer take a job.
     * - `submit` reads it under its own lock, so the answer and the hand-over cannot disagree; `FramePump::threaded`
     *   publishes the same answer, which is how "a frame still runs on a thread" stops being true the moment it is.
     *
     * @return true when `submit` hands the job over, false when it runs it on the calling thread.
     */
    [[nodiscard]] bool accepting() const noexcept;

    /** @brief Gets how many jobs have finished (run on the engine thread or inline). */
    [[nodiscard]] std::uint64_t frames() const;

    /** @brief Gets how many jobs threw (counted in `frames()` too: a job that threw still ran).
     *
     * @details
     * - A job is not supposed to throw - the engine answers its own frame (see RenderEngine::answerEngineHalf) - so a
     *   number here is a caller's bug that the runner survived rather than a state a host acts on.
     */
    [[nodiscard]] std::uint64_t failures() const;

    /** @brief Whether a job is in flight right now. */
    [[nodiscard]] bool busy() const;

  private:
    /// The runner's one life fact (see accepting / onRunnerThread / stop).
    enum class State : std::uint8_t
    {
        Inline,   ///< No thread at all: jobs run on the caller's thread.
        Running,  ///< A thread exists and takes a job handed over.
        Stopping, ///< A stop was asked for: the loop finishes the job in flight and exits; nobody hands it a new one.
        Stopped,  ///< The thread was joined: an inline runner from here on.
    };

    /// @brief The engine thread's body: wait for a job, run it, tell the waiter it is done.
    void loop();

    /// @brief Runs a job on the calling thread and counts it (the inline path).
    void runHere(const Job& job);

  private:
    RunnerMode              mode_;
    std::thread             thread_;
    /// The frame thread's id, taken once at start: what onRunnerThread compares against.
    std::thread::id         thread_id_{};
    /// WHERE THE THREAD IS IN ITS LIFE (`G1`: one fact, not `live_` plus `stopping_`). ATOMIC, because its readers are
    /// not its writer: a caller that only WAITS (`wait` / `waitIdle`, and the pump's `waitForFrame` from any thread)
    /// asks while another thread may be inside `stop()`, and reading `thread_.joinable()` there would race the join.
    std::atomic<State>      state_{ State::Inline };
    mutable std::mutex      mutex_;
    std::condition_variable wake_; ///< Engine thread: a job arrived, or the runner is stopping.
    std::condition_variable done_; ///< Caller: the job in flight finished.
    Job                     request_;
    bool                    has_request_{ false }; ///< A job was handed over and has not finished.
    std::uint64_t           frames_{ 0 };
    std::uint64_t           failures_{ 0 }; ///< Jobs that threw (see failures).
};

} // namespace detail

VN_GRAPHICS_NS_END
