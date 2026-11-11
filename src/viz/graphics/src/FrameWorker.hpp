#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Scene.hpp>

#include "ContentMirror.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief WHAT a frame's engine half does with the content a draw names: keep it, or answer with its mirror.
 *
 * @details
 * - The two modes are one predicate, and it follows from WHERE the work runs: on the content's own thread the frame
 *   may keep asking the content directly (bind is pointless and costs nothing), off that thread it must ask the mirror,
 *   or the expansion reads content the engine side does not own.
 * - Naming it after the FRAME (not after the thread) keeps the worker honest: it never inspects a thread, it applies
 *   the mode it was handed (see FrameRunner, which is what owns the thread and therefore chooses).
 */
enum class FrameMode : std::uint8_t
{
    Inline,   ///< The content's thread runs the frame: the draw keeps the content it names (today's path).
    Threaded, ///< Another thread runs it: every draw is re-pointed at its content's mirror (see the design's bindContent).
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
 * @warning One thread at a time. `publish` runs on the content's thread, `run` on the engine's, and they meet only in
 * the mirrors' channels.
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
     * @return true when a publish was posted (false when nothing moved, or the mirror's channel was full).
     */
    bool publish(const Scene& scene);

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
     * @param mode What this run may assume about the content (see FrameMode).
     */
    void runPass(PassDraw& pass, FrameMode mode);

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
    std::size_t run(FrameDescription& frame, FrameMode mode = FrameMode::Threaded);

  public:
    /** @brief Gets how many contents this worker mirrors. */
    [[nodiscard]] std::size_t mirrorCount() const noexcept;

    /** @brief Gets how many deltas have been applied since construction (the drain's own witness). */
    [[nodiscard]] std::uint64_t applied() const noexcept;

    /** @brief Gets how many draws named a source this worker does not mirror (see the class note). */
    [[nodiscard]] std::uint64_t unbound() const noexcept;

  private:
    /// @brief Finds the mirror a draw names, whether it names the content or the mirror itself.
    [[nodiscard]] ContentMirror* mirrorOf(const vn::graphics::DrawListSource& source) noexcept;

  private:
    std::vector<std::unique_ptr<ContentMirror>> mirrors_;
    std::uint64_t                               applied_{ 0 };
    std::uint64_t                               unbound_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
