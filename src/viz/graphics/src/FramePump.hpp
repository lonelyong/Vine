#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

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
 *   monotonic frame number, the PHASE (is a frame being built or answered - see `recording`), the completion counter
 *   `waitForFrame` reads, the `FrameRunner`, and the last answered frame's whole answer - its outcome, its
 *   `FrameResult`, and what each of its passes cost.
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

    /**
     * @brief ONE FRAME'S (OR ONE PREPARE'S) BUILD: the interval in which this session's shape must not change.
     *
     * @details
     * - WHY IT IS A VALUE AND NOT A FLAG (`G1` / `G10`): "a compile is running" has exactly one owner - the call that
     *   compiled - and every way out of that call ends it: the ordinary return, an early one, and a host hook that
     *   throws out of a pass' `describe`. A guard is the only spelling of that which cannot be forgotten, and its
     *   destructor is the one place the phase is cleared.
     * - WHAT IT DOES NOT COVER: the ENGINE half. Once the value is handed over, the frame is still open - the shape
     *   must stay put until it is answered - and that half belongs to the pump's own `answering` fact, not to this
     *   object (see `recording`).
     * - IT IS THE ENGINE'S LEASE ON THE SAME INTERVAL THE DEVICE GATES (`G6`). A `FrameGate` lease answers false when
     *   the entry was refused (a call from the thread that already holds it); this guard answers false the same way,
     *   for the two reasons that cannot be resolved by waiting: THIS thread already holds the interval, or ANOTHER
     *   thread does. A second frame is never QUEUED behind the first (design 2.12 / D12) and never RACES it
     *   (`graphics-threading.md` 2.17).
     */
    class Build {
      public:
        /// Why a build was not entered (`refusal()` answers it; an owning build answers #None).
        enum class Refusal : std::uint8_t
        {
            None,       ///< The build owns the interval.
            Reentrant,  ///< THIS thread holds it already (a hook or a diagnostic sink driving the same engine).
            Concurrent, ///< ANOTHER thread holds it: the interval has one owner, so the entry is refused.
        };

        Build(const Build&)            = delete;
        Build& operator=(const Build&) = delete;

        /** @brief Ends the build: the phase is cleared only when this build actually held it. */
        ~Build();

        /** @brief Answers whether this build OWNS the interval; false means the entry was refused. */
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return pump_ != nullptr;
        }

        /** @brief Gets why the entry was refused (#None when this build owns the interval). */
        [[nodiscard]] Refusal refusal() const noexcept
        {
            return pump_ != nullptr ? Refusal::None : refusal_;
        }

        /** @brief Gets the frame value this build fills.
         *
         * @pre `*this` - a refused build names no value (call it only where the guard is true).
         */
        [[nodiscard]] EngineFrame& frame() const noexcept;

      private:
        friend class FramePump;

        /** @brief Takes ownership of @p pump's interval, over @p frame (called by beginFrame / beginBuild). */
        Build(FramePump& pump, EngineFrame& frame) noexcept;

        /** @brief Answers a refused entry (called by beginFrame / beginBuild). */
        explicit Build(const Refusal refused) noexcept
          : refusal_(refused)
        {}

        FramePump*   pump_{ nullptr };
        EngineFrame* frame_{ nullptr };
        /// The pump this thread already owned when this build started: restored when it ends, so a hook that drives
        /// ANOTHER engine leaves the outer build's ownership intact.
        const FramePump* previous_{ nullptr };
        Refusal          refusal_{ Refusal::None };
    };

    /** @brief Creates an inline pump with two empty frame values (no thread, no frame built). */
    FramePump();

    /** @brief Stops the thread (if any) and joins it. */
    ~FramePump();

    FramePump(const FramePump&)            = delete;
    FramePump& operator=(const FramePump&) = delete;

  public:
    /** @brief Waits for the frame in flight, then rotates to the other value, numbers it and marks this session
     *         as RECORDING.
     *
     * @details
     * - ONE FRAME IN FLIGHT (design 2.12, D12): the previous engine half is finished before this value is handed out,
     *   so a caller never queues frames and the two halves never touch one frame at once. That wait happens BEFORE the
     *   phase is entered, so a shape change landing during it is answered at once (see `recording`) rather than held.
     * - The number is taken HERE, monotonic from 1: it names the frame in `waitForFrame`, and it is what the answer's
     *   `PresentRecord` carries. It is atomic because a host may read `frameNumber()` from a thread that does not
     *   drive frames (a HUD, a benchmark), which is also what lets `wait` tell "not answered yet" from "never built".
     *
     * @return The build this frame fills - its value, and the interval the session's shape must not change in - or an
     *         EMPTY build when another thread, or this one, already holds that interval (see `refusal`). The interval
     *         ends when the guard goes out of scope; the frame itself stays open until it is answered.
     */
    [[nodiscard]] Build beginFrame();

    /** @brief Marks this session as RECORDING without handing out a frame: a PREPARE is a build, not a frame.
     *
     * @details
     * - A PREPARE WALKS THE SAME TABLES A FRAME DOES (see RenderEngine::prepare): it compiles, resolves transients and
     *   describes every pass, so a shape change landing inside it corrupts exactly what a change during `frame()`
     *   would. The one difference is that nothing will be answered, so the interval ends with the guard.
     *
     * @return The build, over the value the last compile filled (see currentFrame), or an EMPTY build when the
     *         interval is already held (see `refusal`).
     */
    [[nodiscard]] Build beginBuild();

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

    /**
     * @brief Answers whether a frame or a prepare is being BUILT or ANSWERED: the interval the session's shape is fixed in.
     *
     * @details
     * - TWO FACTS, ONE ANSWER (`G1`): `compiling` - the content half is running on the thread that called
     *   frame/prepare - and `answering` - a frame was handed to the engine thread and nobody has answered it yet. The
     *   first is what the runner cannot see: inline there is no runner at all, and in threaded mode it ends before the
     *   job is even submitted. A gate that asked the runner alone therefore let a shape change into the compile, which
     *   is the half of the frame that reads the table a change edits (C20 used to say "a frame in flight" and only
     *   half of it was checked).
     * - READ FROM ANY THREAD, because a shape change may land from one that does not drive frames (an application pool
     *   assembling content beside a paint loop). Both facts are atomics, and neither is derived from `runner_` - that
     *   pointer is replaced by the thread that switches mode.
     *
     * @return true while a build or an answer is outstanding.
     */
    [[nodiscard]] bool recording() const;

    /**
     * @brief Takes the gate a SHAPE CHANGE holds while it decides and applies (see recording).
     *
     * @details
     * - WHY ONE LOCK AT ALL: "nothing is recording, so the shape may change" and "this session is recording now" are
     *   two statements made by two threads; with no lock around both, the first can be decided while the session is idle
     *   and the change applied while the compile walks the table it edits. The gate closes that window by making the
     *   DECISION and the CHANGE one critical section, against the one that flips the phase.
     * - NEVER HELD FOR A COMPILE: a change is short and a compile is not, so a frame start waits for the former at
     *   most. This is why it is a lock and not "wait for the frame" - a change is refused (Busy), never queued.
     *
     * @return The gate, held for as long as the guard lives.
     */
    [[nodiscard]] std::unique_lock<std::mutex> phaseGate();

    /**
     * @brief Chooses where the engine half runs (waits for the frame in flight before switching).
     *
     * @details
     * - A CALL FROM THE ENGINE THREAD CHANGES NOTHING and answers false: the mode that thread is running under cannot
     *   be ended from inside it (see FrameRunner::stop). It USED to be a silent no-op, which made "I asked for a
     *   thread and did not get one" indistinguishable from "it is on a thread already" (`G4`: a verb that can fail
     *   says so, and the caller decides whether to report it).
     *
     * @param threaded Where the engine half runs.
     * @return true when the mode was applied; false when the caller refused it (the engine thread, or a frame still
     *         being answered inline).
     */
    [[nodiscard]] bool setThreaded(bool threaded);

    /**
     * @brief Publishes ONE answered frame: the picture's fate, its whole answer, and the completion a host waits for.
     *
     * @details
     * - ONE CALL, ONE MOMENT (`G1` / `G4`): three facts travel with an answer - what became of the picture (the packed
     *   word `presentRecord` reads), what the frame consisted of and what each of its passes cost (`result` /
     *   `answeredPassCosts`), and that it is answered at all (`waitForFrame`). Publishing them from three calls let a
     *   host that polls WITHOUT waiting read one frame's picture beside the previous frame's numbers; they are
     *   published here, in that order, and the completion is the last (so a wait that returned sees all of it).
     * - The costs are copied: they live on a frame value the next frame reuses.
     *
     * @param number     The frame this answers (see beginFrame).
     * @param outcome    What became of its picture.
     * @param result     What it consisted of and reported.
     * @param pass_costs What each of its passes cost, in slot order (see detail::PassCost).
     */
    void publishAnswer(std::uint64_t number, PresentOutcome outcome, FrameResult result, std::span<const PassCost> pass_costs);

    /** @brief Gets the last answered frame's whole answer. */
    [[nodiscard]] FrameResult result() const;

    /** @brief Gets the last answered frame's per-pass cost, in slot order (empty before the first answer).
     *
     * @details
     * - A COPY, taken under the lock: the numbers belong to a frame whose value the pump reuses, so a reader never
     *   holds a handle on storage the next frame overwrites.
     *
     * @return The answered frame's costs (see detail::PassCost).
     */
    [[nodiscard]] std::vector<PassCost> answeredPassCosts() const;

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
    /// How many frames this pump has handed out (monotonic; 0 before the first one). Atomic because a host reads
    /// `frameNumber()` from threads that do not drive frames, and because `wait` compares against it.
    std::atomic<std::uint64_t> number_{ 0 };
    /// The content half of a frame or prepare is running on the thread that called it (see Build). Written only under
    /// `phase_mutex_` and only after `claimInterval` won it, so "who holds the interval" is this flag plus the
    /// thread-local below.
    std::atomic<bool> compiling_{ false };
    /// The pump THIS thread is building on (null when none): thread-local because the question "is the holder ME?"
    /// is about the caller, and the flag above says nothing about it. Same idiom as `FrameGate`'s entry stack, built
    /// without a lock and without an `atomic<std::thread::id>` (whose generic implementation may spin).
    inline static thread_local const FramePump* s_building_{ nullptr };
    /// Takes the interval for the calling thread without waiting; false when this thread or another already holds it.
    [[nodiscard]] bool claimInterval() noexcept;
    /// A frame was handed to the engine thread and has not been answered (set by submit, cleared by publishAnswer).
    std::atomic<bool> answering_{ false };
    /// The gate a shape change holds across its decision and its change, against the phase flip that starts a build.
    mutable std::mutex phase_mutex_;
    /// The id of the last frame whose engine half finished (read by wait).
    std::atomic<std::uint64_t> completed_{ 0 };
    /// Where the engine half runs; null means inline - the thread exists only when a host asked for one.
    std::unique_ptr<FrameRunner> runner_;
    /// The last answered frame's id and picture outcome, packed into one word so both halves of the answer are read
    /// without a lock and without tearing.
    std::atomic<std::uint64_t> present_{ 0 };
    /// The last answered frame's whole answer (see FrameResult), its per-pass cost, and the lock a host reads them under.
    mutable std::mutex    mutex_;
    FrameResult           result_;
    std::vector<PassCost> answered_pass_costs_;
};

} // namespace detail

VN_GRAPHICS_NS_END
