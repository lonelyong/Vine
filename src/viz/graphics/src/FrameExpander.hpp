#pragma once

#include <cstdint>

#include <vine/graphics/FrameDescription.hpp>

#include "PublicationStore.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief THE ENGINE SIDE OF ONE FRAME: bind every draw to its content's publication, then expand it.
 *
 * @details
 * - WHAT IT IS. The object a frame's engine half hangs on: for each draw, the bind step re-points the draw at the
 *   publication its content last handed over, and the expansion turns the views into command lists - the one
 *   O(drawables) step of a frame (see FrameExpand).
 * - WHERE THE PUBLICATIONS COME FROM. It holds no publication of its own: it READS a `detail::PublicationStore`,
 *   whose verbs belong to the content side. That is the whole reason the two are separate objects - the content half
 *   publishes without knowing this class exists, and the engine half reads without ever publishing (`G2`).
 * - IT OWNS NO THREAD AND NO WAITING. `run` is a plain call: whoever owns it decides which thread it runs on, which is
 *   what lets the inline mode (the calling thread, no thread at all) and the threaded mode be the same object.
 *   Waiting, frame numbers and the start/stop handshake belong to the owner (see the design's 2.8 and the backlog's
 *   T31).
 * - IT READS NO CONTENT. The content arrives as the store's immutable publication and as a pointer to compare
 *   (`DrawDescription::content`); nothing here dereferences a node. A draw whose content has no publication is
 *   COUNTED (`unbound()`) instead of being quietly expanded from the content - which is correct on the calling thread
 *   and a cross-thread read anywhere else, so the count is how a host finds out it forgot to publish.
 *
 * @note
 * - A draw that already names a store-owned source (a source, not the content) binds to it again and expands as it
 *   stands, so a second run over one frame value is a no-op rather than a second collection.
 */
class FrameExpander {
  public:
    /**
     * @brief Constructs the engine half over @p publications.
     *
     * @param publications The store the bind step looks a draw's content up in; it outlives this object.
     */
    explicit FrameExpander(PublicationStore& publications) noexcept;
    ~FrameExpander();

    FrameExpander(const FrameExpander&)            = delete;
    FrameExpander& operator=(const FrameExpander&) = delete;

  public:
    /**
     * @brief Binds and expands the draws of ONE pass.
     *
     * @param pass The pass' drawing calls, expanded in place.
     */
    void runPass(PassDraw& pass);

    /**
     * @brief Runs the engine half of @p frame: bind and expand every pass.
     *
     * @details
     * - The whole-frame form of @ref runPass, and the form a runner hands to another thread: whoever runs the frame
     *   does all of it, in the order the frame requires.
     *
     * @param frame The frame value to expand in place.
     */
    void run(FrameDescription& frame);

  public:
    /** @brief Gets how many draws named a source this expander's store does not publish (see the class note). */
    [[nodiscard]] std::uint64_t unbound() const noexcept;

  private:
    PublicationStore& publications_;
    std::uint64_t     unbound_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
