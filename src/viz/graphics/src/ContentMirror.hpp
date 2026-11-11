#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "DeltaChannel.hpp"
#include "RenderWorld.hpp"
#include "SceneRows.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief ONE content's MIRROR: the engine side's copy of its draw list, kept in step by values.
 *
 * @details
 * - WHAT IT IS. A `RenderWorld` (the rows) plus the face a frame asks for a view (`RenderWorldSource`), plus the
 *   channel the content's thread publishes through. It is the unit a worker owns per content, and it is the whole of
 *   "the render world" for one scene: no node, no graph, no camera object.
 * - THE TWO SIDES, ONE OBJECT. `publish` runs on the CONTENT's thread (it reads the scene's rows, `SceneRows`), and
 *   `drain` runs on the ENGINE's. They meet in the channel, never in a member: the content side writes the version it
 *   published and the channel, the engine side writes the world. Nothing is shared mutable between them.
 * - WHY THE PUBLISH IS COARSE IN P1. A node edit moves the scene's epoch and a byte edit moves a box (see
 *   `SceneRows::Version`), and both are answered by publishing the whole table. That is one O(rows) copy per change
 *   instead of per frame, and it is exactly the handshake the design calls for; the finer deltas (structure, state,
 *   description, box) replace it one kind at a time, each with its applier (`../backlog/graphics.md` T31).
 * - WHAT IT DOES NOT DO. It never expands, never binds a frame and never records: those belong to the frame that asks
 *   (see the design's `FrameWorker`), and a mirror that grew a frame would be the second engine.
 *
 * @warning Single-threaded per side: `publish` on the content's thread, `drain`/`source` on the engine's. A mirror is
 * not shared between two engines - a second engine publishes its own.
 */
class ContentMirror {
  public:
    /**
     * @brief Creates an empty mirror for @p content.
     *
     * @param content The content this mirror answers for (its address is the identity a frame's binding maps).
     * @param capacity How many deltas may wait between two drains (see DeltaChannel).
     */
    explicit ContentMirror(const vn::graphics::DrawListSource& content, std::size_t capacity = DeltaChannel::kDefaultCapacity);

    ContentMirror(const ContentMirror&)            = delete;
    ContentMirror& operator=(const ContentMirror&) = delete;

  public:
    /** @brief Gets the content this mirror answers for. */
    [[nodiscard]] const vn::graphics::DrawListSource& content() const noexcept;

    /** @brief Gets the face a frame names once it has been bound (see the design's bindContent). */
    [[nodiscard]] vn::graphics::DrawListSource& source() noexcept;

    /**
     * @brief Publishes @p scene's rows to this mirror, if they moved since the last publish.
     *
     * @details
     * - Content thread. It compares the rows' version first, so a steady frame publishes nothing at all; a change
     *   posts ONE `RowsReplaced` delta carrying a copy of the table.
     * - A refused post (the channel is full) leaves the version unpublished, so the next call retries it - the same
     *   "a refusal is recoverable" property the channel is built on.
     *
     * @param scene The scene this mirror is the mirror of.
     * @return true when a publish was posted (false when nothing moved or the channel was full).
     */
    bool publish(const Scene& scene);

    /**
     * @brief Applies everything published since the last drain, in post order (engine thread).
     *
     * @return How many deltas were applied.
     */
    std::size_t drain();

  public:
    /** @brief Gets the rows this mirror holds (read-only: `drain` is what changes them). */
    [[nodiscard]] const RenderWorld& world() const noexcept;

    /** @brief Gets how many publishes the mirror accepted. */
    [[nodiscard]] std::uint64_t publishCount() const noexcept;

  private:
    const vn::graphics::DrawListSource* content_;
    RenderWorld                         world_;
    RenderWorldSource                   source_;
    DeltaChannel                        channel_;
    std::vector<OwnedDelta>             batch_;
    std::optional<SceneRows::Version>   published_;
    std::uint64_t                       publish_count_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
