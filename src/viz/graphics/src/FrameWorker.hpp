#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Scene.hpp>

#include "ContentMirror.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief WHAT a frame's engine half asks for a draw: the CONTENT it names, or that content's MIRROR.
 *
 * @details
 * - This is a statement about the SOURCE, not about the thread - and the two are independent: a mirror-backed frame may
 *   run on the content's own thread (an engine that publishes and then expands in place), and only the reverse is
 *   illegal - `Direct` reads content, so it may ONLY run where that content lives.
 * - `Mirrored` is what makes a frame portable across threads at all: every draw is re-pointed at the mirror that was
 *   published for it, and after that the frame names nothing the content owns.
 * - Naming it after the SOURCE keeps the worker honest (it never inspects a thread) and the owner honest (the engine
 *   decides how its frames are answered; the runner decides where they run).
 */
enum class FrameMode : std::uint8_t
{
    Direct,  ///< Ask the content a draw names. Legal only on the content's thread (the inline path, and the fallback).
    Mirrored, ///< Ask that content's mirror (see the design's bindContent). Legal anywhere it was published from.
};

/**
 * @brief THE ENGINE SIDE OF ONE FRAME: drain what the contents published, bind the frame to their mirrors, expand it.
 *
 * @details
 * - WHAT IT IS. The object a frame's engine half hangs on: one mirror per content it draws (see ContentMirror), and
 *   the three steps that must happen in this order at frame start -
 *   1. **drain**: every mirror applies what its content published since the last frame, in post order;
 *   2. **bind**: a draw that names a CONTENT is re-pointed at that content's MIRROR (see the design's `bindContent`),
 *      which is what makes the expansion stop asking the content;
 *   3. **expand**: `expandFrame`, the one O(drawables) step (see FrameExpand).
 * - WHY THE ORDER IS NOT NEGOTIABLE. A frame expanded before the drain draws last frame's values; a frame expanded
 *   before the bind asks the content from the wrong thread. Both are silent, so the order lives in ONE function
 *   instead of in a caller's habits.
 * - IT OWNS NO THREAD AND NO WAITING. `run` is a plain call: whoever owns it decides which thread it runs on, which is
 *   what lets the inline mode (the calling thread, no thread at all) and the threaded mode be the same object. Waiting,
 *   frame numbers and the start/stop handshake belong to the owner (see the design's 2.8 and the backlog's T31).
 * - IT READS NO CONTENT. The content arrives as rows on a channel (`publish`) and as a pointer to compare
 *   (`DrawDescription::content`); nothing here dereferences a node. A draw whose content has no mirror is COUNTED
 *   (`unbound()`) instead of being quietly expanded from the content - which is correct on the calling thread and a
 *   cross-thread read anywhere else, so the count is how a host finds out it forgot to publish.
 *
 * @warning `publish` MUST NOT RUN WHILE A FRAME IS RUNNING. The value channel is safe from both sides, but the LIST of
 * mirrors is not: `publish` may create one while `run` walks it. Today that is guaranteed by construction - the runner
 * hands a frame over and waits for it, so the content's thread is inside `run` while the engine's works - and a mode
 * that overlaps frames has to move mirror creation into the handshake (or publish through the channel) BEFORE it may
 * overlap them. Stating the invariant rather than relying on it silently is the point.
 */
class FrameWorker {
  public:
    FrameWorker();
    ~FrameWorker();

    FrameWorker(const FrameWorker&)            = delete;
    FrameWorker& operator=(const FrameWorker&) = delete;

  public:
    /**
     * @brief Publishes @p scene's rows to its mirror, attaching a mirror on first use.
     *
     * @details
     * - Content thread, before the frame that draws @p scene is run. It is a cheap no-op when the scene has not
     *   changed (see ContentMirror::publish), so the caller can (and should) call it every frame.
     *
     * @param scene The scene this frame draws.
     * @return What the call answered: posted / unchanged / inside an edit batch / refused by a full channel (see
     *         ContentMirror::publish). A caller that only needs "did it post" reads the value as a bool.
     */
    PublishOutcome publish(const Scene& scene);

    /**
     * @brief Opens @p frame on every mirror: the token their memos are keyed by (the frame's FIRST step).
     *
     * @param frame The frame about to ask for views (0 means none is open: nothing is memoised).
     */
    void beginFrame(std::uint64_t frame);

    /**
     * @brief Applies what every mirror's content published (the frame's FIRST step).
     *
     * @details
     * - It is separate from the expansion because a caller that measures its passes one by one still has to drain
     *   exactly ONCE per frame: a drain per pass would apply nothing the second time but would walk every mirror again,
     *   and - worse - would suggest that a pass can see values another pass did not.
     *
     * @return How many deltas were applied.
     */
    std::size_t drain();

    /**
     * @brief Binds (see @p mode) and expands the draws of ONE pass.
     *
     * @param pass The pass' drawing calls, expanded in place.
     * @param mode What a draw may ask (see FrameMode).
     */
    void runPass(PassDraw& pass, FrameMode mode = FrameMode::Mirrored);

    /**
     * @brief Runs the engine half of @p frame: drain, then every pass.
     *
     * @details
     * - The whole-frame form of @ref drain + @ref runPass, and the form a runner hands to another thread: whoever runs
     *   the frame does all of it, in the order the frame requires.
     *
     * @param frame The frame value to expand in place.
     * @param mode  What the run may assume about the content (see FrameMode).
     * @return How many deltas the mirror drains applied.
     */
    std::size_t run(FrameDescription& frame, FrameMode mode = FrameMode::Mirrored);

  public:
    /** @brief Gets how many contents this worker mirrors. */
    [[nodiscard]] std::size_t mirrorCount() const noexcept;

    /** @brief Gets how many deltas have been applied since construction (the drain's own witness). */
    [[nodiscard]] std::uint64_t applied() const noexcept;

    /** @brief Gets how many draws named a source this worker does not mirror (see the class note). */
    [[nodiscard]] std::uint64_t unbound() const noexcept;

    /** @brief Gets how many lists the mirrors' sources answered from their memos (the per-view reuse witness). */
    [[nodiscard]] std::uint64_t memoHits() const noexcept;

    /** @brief Gets how many lists the mirrors' sources collected (the per-view walk witness). */
    [[nodiscard]] std::uint64_t memoStores() const noexcept;

  private:
    /// @brief Finds the mirror a draw names, whether it names the content or the mirror itself.
    [[nodiscard]] ContentMirror* mirrorOf(const vn::graphics::DrawListSource& source) noexcept;

    /// @brief The lookup itself, for callers that already hold `mirrors_mutex_`.
    [[nodiscard]] ContentMirror* mirrorOfLocked(const vn::graphics::DrawListSource& source) noexcept;

  private:
    std::vector<std::unique_ptr<ContentMirror>> mirrors_;
    /// Guards the mirrors' LIST (not their contents): the content half creates one the first time a content is
    /// published, and the engine half walks the same list while it drains, opens a frame and binds - two threads, one
    /// vector, and a `push_back` that can reallocate it. Held only for a lookup or an insert, never across a
    /// publish/drain/expand.
    mutable std::mutex mirrors_mutex_;
    std::uint64_t                               frame_{ 0 };
    std::uint64_t                               applied_{ 0 };
    std::uint64_t                               unbound_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
