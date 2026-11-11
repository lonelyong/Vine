#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include <vine/graphics/FrameDescription.hpp>

#include "FrameWorker.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief Runs a `FrameWorker`'s frames in one of the two modes, with the same semantics.
 *
 * @details
 * - WHAT IT IS NOT: it is not the frame's work (that is `FrameWorker`) and not the backend (that is the executor).
 *   It owns exactly one thing the worker must not: **the thread and the handshake around it**. Keeping the two apart
 *   is what lets the inline mode - the calling thread, no thread object at all - and the threaded mode be literally
 *   the same code path, which is the property the equivalence tests rest on.
 * - ONE FRAME IN FLIGHT, STRICT ALTERNATION. `run` hands the frame value to the engine thread and returns when that
 *   frame is done, so a caller sees the same answer in both modes and no frame value can be touched by two threads at
 *   once. It is deliberately NOT "many frames in flight": overlapping frames change the RETURN contract (what "this
 *   frame ran" means), and that contract is a surface/device decision (see the design's P3), not a runner detail.
 * - IDLE MEANS WAITING, NOT POLLING. The engine thread blocks on a condition variable when there is no request (design
 *   2.8): an application that draws nothing costs nothing, and the caller is the one that never queues.
 * - ASKING FROM THE ENGINE THREAD RUNS INLINE. A frame requested from the engine thread itself would wait on itself;
 *   `run` detects that and executes the frame where it stands. The same rule covers a stopped runner, which is an
 *   inline runner (see `stop`), so a caller cannot deadlock by not knowing which mode is live.
 *
 * @warning `run` is the one call that may block (in the threaded mode, for the duration of that frame). Everything it
 * calls is single-threaded by contract: the worker's mirrors are touched by the engine thread and the content's row
 * reads by the caller's - never both.
 */
class FrameRunner {
  public:
    /**
     * @brief Runs @p worker's frames in @p mode.
     *
     * @param worker The worker whose frames this runner drives; it must outlive the runner.
     * @param mode   Where the work runs, and therefore what it may assume about the content (see FrameMode).
     */
    explicit FrameRunner(FrameWorker& worker, FrameMode mode = FrameMode::Threaded);

    /** @brief Stops the engine thread (if any) and joins it. */
    ~FrameRunner();

    FrameRunner(const FrameRunner&)            = delete;
    FrameRunner& operator=(const FrameRunner&) = delete;

  public:
    /**
     * @brief Runs one frame's engine half and returns when it is done.
     *
     * @details
     * - Inline (or from the engine thread, or after `stop`) it is the worker's `run`, right here.
     * - Threaded it hands @p frame to the engine thread and waits for THIS frame to finish, so the caller's next
     *   statement already sees the frame's command lists.
     *
     * @param frame The frame value to expand in place; it must stay alive until this returns.
     * @return How many deltas the frame's drains applied (see FrameWorker::run).
     */
    std::size_t run(FrameDescription& frame);

    /**
     * @brief Stops the engine thread and joins it.
     *
     * @details
     * - Idempotent, and safe while the thread is idle. After it, the runner behaves as an inline runner: a frame asked
     *   for later runs on the caller's thread instead of waiting for a thread that is gone.
     */
    void stop();

  public:
    /** @brief Gets the mode this runner was built with (it does not change: a mode is a lifetime decision).
     *
     * @return The mode. A runner with no live thread (before start, after `stop`) reports the mode it was built with,
     *         because the mode is what the frame MEANS, not which thread happens to be alive.
     */
    [[nodiscard]] FrameMode mode() const noexcept;

    /** @brief Gets how many frames the engine side has run (the threaded mode's witness). */
    [[nodiscard]] std::uint64_t frames() const;

  private:
    /// @brief The engine thread's body: wait for a request, run it, tell the caller it is done.
    void loop();

  private:
    FrameWorker&            worker_;
    FrameMode               mode_;
    std::thread             thread_;
    mutable std::mutex      mutex_;
    std::condition_variable wake_; ///< Engine thread: a request arrived, or the runner is stopping.
    std::condition_variable done_; ///< Caller: the frame it handed over is finished.
    FrameDescription*       request_{ nullptr };
    std::size_t             result_{ 0 };
    bool                    finished_{ false };
    bool                    stopping_{ false };
    std::uint64_t           frames_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
