#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameResult.hpp>
#include <vine/graphics/PresentTarget.hpp>
#include <vine/graphics/graphics_global.hpp>

#include "EngineFrame.hpp"
#include "FrameRunner.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief THE FRAME PUMP: frame numbers, the one frame in flight, and the answer a host waits for.
 *
 * @details
 * - WHY IT IS NOT IN `RenderEngine` (`G5` / `G12`): a frame's engine half may run on a thread of its own, and everything
 *   that policy needs - the thread, the strict one-in-flight handshake, the completion number, the last answer - is
 *   MECHANISM. It lived in the composition root, so changing the threading policy meant editing the class that also
 *   composes passes, surfaces and diagnostics; here it is one unit whose contract is "where does the second half run,
 *   and how does a host learn it finished".
 * - WHAT IT OWNS: the two `EngineFrame` values (the content half fills one while the other is being answered), the
 *   monotonic frame number, the completion counter `waitForFrame` reads, the `FrameRunner`, the pending per-pass cost,
 *   and the last answered frame's outcome and `FrameResult`.
 * - WHAT IT DOES NOT OWN: the content half, the engine half, the passes and the device. It hands out a frame value and
 *   accepts a job; building and answering stay in the engine (`G7`: the pump absorbs the threading, it does not take
 *   over the frame).
 * - INLINE IS A REAL MODE, not a fallback: with no thread the runner object does not exist (`setThreaded(false)` stops
 *   and joins it), `submit` runs the job here, and `wait` answers from the completion counter - so an engine that never
 *   asks for a thread allocates nothing and starts nothing.
 *
 * @warning The job handed to `submit` runs on the pump's thread, not the caller's. It must touch only what the engine
 *          half contract names (the frame value it was given, the publications, the device) - see `graphics-threading.md`
 *          2.12.
 */
class FramePump {
  public:
    /// One frame's engine half, as far as this class is concerned.
    using Job = FrameRunner::Job;

    /** @brief Creates an inline pump with two empty frame values (no thread, no frame built). */
    FramePump();

    /** @brief Stops the thread (if any) and joins it. */
    ~FramePump();

    FramePump(const FramePump&)            = delete;
    FramePump& operator=(const FramePump&) = delete;

  public:
    /** @brief Waits for the frame in flight, then rotates to the other value and numbers it.
     *
     * @details
     * - ONE FRAME IN FLIGHT (design 2.12, D12): the previous engine half is finished before this value is handed out,
     *   so a caller never queues frames and the two halves never touch one frame at once.
     * - The number is taken HERE, monotonic from 1: it names the frame in `waitForFrame`, and it is what the answer's
     *   `PresentRecord` carries.
     *
     * @return The frame value this frame fills (reused across frames; its storage survives).
     */
    EngineFrame& beginFrame();

    /** @brief Gets the frame value the next `prepare()` would fill (no rotation, no number taken).
     *
     * @return The current frame value.
     */
    [[nodiscard]] EngineFrame& currentFrame() noexcept;

    /** @brief Gets the frame value the last compile filled (see currentFrame).
     *
     * @return The current frame value.
     */
    [[nodiscard]] const EngineFrame& currentFrame() const noexcept;

    /** @brief Takes the frame whose engine half just finished, so its per-pass cost can reach the plan.
     *
     * @details
     * - The plan belongs to the content half, so the engine publishes the cost at the one moment nobody else is in it:
     *   right after `beginFrame` waited. Null means nothing is pending (inline publishes immediately).
     *
     * @return The finished frame, or null.
     */
    [[nodiscard]] EngineFrame* takePendingMicros() noexcept;

    /** @brief Hands the engine half over (threaded), or runs it here (inline).
     *
     * @param job What to run; it must be safe on the pump's thread.
     */
    void submit(Job job);

    /** @brief Waits until the frame in flight finished (no-op inline). */
    void waitIdle();

    /** @brief Waits until frame @p number has been answered.
     *
     * @param number  Frame id (see beginFrame).
     * @param timeout How long to wait.
     * @return true when that frame has been answered, false on timeout (or when it was never built).
     */
    bool wait(std::uint64_t number, std::chrono::milliseconds timeout);

    /** @brief Gets the id of the last frame this pump handed out (0 before the first one). */
    [[nodiscard]] std::uint64_t lastNumber() const noexcept;

    /** @brief Gets whether the engine half runs on a thread of its own. */
    [[nodiscard]] bool threaded() const noexcept;

    /**
     * @brief Answers whether the CALLING thread is the engine thread (false inline, and false on every other thread).
     *
     * @details
     * - WHAT THE ENGINE ASKS BEFORE IT BUILDS A FRAME (`RenderEngine::frame` / `prepare`): a frame asked from inside a
     *   frame would wait for the frame it is inside (see `beginFrame`), so the question has one owner - this pump,
     *   which is the class that knows where the engine half runs.
     *
     * @return true when the frame thread is the caller.
     */
    [[nodiscard]] bool onEngineThread() const noexcept;

    /** @brief Gets whether a job is in flight (always false inline). */
    [[nodiscard]] bool busy() const;

    /** @brief Chooses where the engine half runs (waits for the frame in flight before switching). */
    void setThreaded(bool threaded);

    /** @brief Remembers the frame whose cost still has to reach the plan (threaded mode). */
    void notePendingMicros(EngineFrame& frame) noexcept;

    /** @brief Publishes what became of one frame's picture (the packed word a host reads). */
    void publishPresent(std::uint64_t number, PresentOutcome outcome) noexcept;

    /** @brief Marks frame @p number as answered (what waitForFrame observes). */
    void markCompleted(std::uint64_t number) noexcept;

    /** @brief Publishes the last answered frame's whole answer. */
    void publishResult(FrameResult result);

    /** @brief Gets the last answered frame's whole answer. */
    [[nodiscard]] FrameResult result() const;

    /** @brief Gets the last answered frame's outcome word (`frame << 8 | outcome`). */
    [[nodiscard]] std::uint64_t packedPresent() const noexcept;

    /** @brief Stops the thread and joins it (idempotent; the pump becomes inline).
     *
     * @details
     * - CALLED FROM THE ENGINE THREAD it only asks the loop to leave: a thread cannot join itself, so the join happens
     *   in the next stop() from another thread (the destructor, `shutdown()`), which is also the only safe place for
     *   it.
     */
    void stop();

  private:
    /// Two frame values: one is being built while the other is being answered.
    std::array<std::unique_ptr<EngineFrame>, 2> slots_;
    /// Which slot `beginFrame` fills next.
    std::size_t slot_{ 0 };
    /// The value the last compile filled (what `frameDescription` answers).
    EngineFrame* current_{ nullptr };
    /// How many frames this pump has handed out (monotonic; 0 before the first one).
    std::uint64_t number_{ 0 };
    /// The id of the last frame whose engine half finished (read by wait).
    std::atomic<std::uint64_t> completed_{ 0 };
    /// Where the engine half runs; null means inline - the thread exists only when a host asked for one.
    std::unique_ptr<FrameRunner> runner_;
    /// A frame whose engine half finished on the thread and whose per-pass cost still has to reach the plan.
    std::atomic<EngineFrame*> pending_micros_{ nullptr };
    /// The last answered frame's id and picture outcome, packed into one word so both halves of the answer are read
    /// without a lock and without tearing.
    std::atomic<std::uint64_t> present_{ 0 };
    /// The last answered frame's whole answer (see FrameResult) and the lock a host reads it under.
    mutable std::mutex mutex_;
    FrameResult        result_;
};

} // namespace detail

VN_GRAPHICS_NS_END
