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
     * @brief Runs the engine half of @p frame: drain, bind, expand.
     *
     * @param frame The frame value to expand in place.
     * @return How many deltas the mirror drains applied.
     */
    std::size_t run(FrameDescription& frame);

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
