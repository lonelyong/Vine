#pragma once

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

/** @brief WHERE a frame's engine half runs (the source question is FrameMode's, see FrameWorker). */
enum class RunnerMode : std::uint8_t
{
    Inline,   ///< On the calling thread: no thread, no waiting ("the engine thread" IS the caller).
    Threaded, ///< On an engine thread of its own.
};

/**
 * @brief Runs ONE unit of engine-side work at a time, here or on a thread of its own.
 *
 * @details
 * - WHAT IT IS NOT: it is not the frame's work (that is the engine's two halves, see FrameWorker and the design's
 *   2.12) and not the backend (that is the device). It owns exactly one thing the work must not: **the thread and the
 *   handshake around it**. Keeping the two apart is what lets the inline mode - the calling thread, no thread object
 *   at all - and the threaded mode be the same code path, which is the property the equivalence tests rest on.
 * - ONE JOB IN FLIGHT, STRICT ALTERNATION. `submit` waits for the previous job before handing the next one over, and
 *   `wait` says when the one in flight is done - so no two jobs are ever inside the mirrors or the device at once, and
 *   a caller cannot build up a queue of frames nobody asked for. It is deliberately NOT a pool: what a second frame
 *   in flight would mean for the RETURN contract (and for the swapchain) is a design decision (see
 *   `.ai/design/graphics-threading.md` 2.12 and D12), not a runner detail.
 * - IDLE MEANS WAITING, NOT POLLING. The engine thread blocks on a condition variable when nothing was submitted
 *   (design 2.8): an application that draws nothing costs nothing.
 * - ASKING FROM THE ENGINE THREAD RUNS INLINE. A job submitted from the thread that would run it would wait on
 *   itself; `submit` detects that and runs it where it stands. The same rule covers a stopped runner, which is an
 *   inline runner (see `stop`), so a caller cannot deadlock by not knowing which mode is live.
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
     */
    void stop();

  public:
    /** @brief Gets where this runner was built to run jobs (it does not change: a mode is a lifetime decision).
     *
     * @return The mode. A runner with no live thread (before start, after `stop`) reports the mode it was built with,
     *         because the mode is what the work MEANS, not which thread happens to be alive.
     */
    [[nodiscard]] RunnerMode mode() const noexcept;

    /** @brief Gets how many jobs have finished (run on the engine thread or inline). */
    [[nodiscard]] std::uint64_t frames() const;

    /** @brief Whether a job is in flight right now. */
    [[nodiscard]] bool busy() const;

  private:
    /// @brief The engine thread's body: wait for a job, run it, tell the waiter it is done.
    void loop();

    /// @brief Runs a job on the calling thread and counts it (the inline path).
    void runHere(const Job& job);

  private:
    RunnerMode              mode_;
    std::thread             thread_;
    mutable std::mutex      mutex_;
    std::condition_variable wake_; ///< Engine thread: a job arrived, or the runner is stopping.
    std::condition_variable done_; ///< Caller: the job in flight finished.
    Job                     request_;
    bool                    has_request_{ false }; ///< A job was handed over and has not finished.
    bool                    stopping_{ false };
    std::uint64_t           frames_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
