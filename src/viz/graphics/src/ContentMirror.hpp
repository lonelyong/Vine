#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "DeltaChannel.hpp"
#include "PublishOutcome.hpp"
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
 * - THE TWO SIDES, ONE OBJECT. `publish`/`attach`/`detach` run on the CONTENT's thread (they read the graph - the
 *   scene's rows, a subtree's own facts - and post values), and `drain`/`source` run on the ENGINE's (they read no
 *   content at all). The two sides meet in the channel, never in a member: the content side owns the version it
 *   published and the channel, the engine side owns the world. Nothing is shared mutable between them.
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
     * - Content thread. It compares the rows' version first, so an unchanged scene publishes nothing at all; a change
     *   posts ONE `RowsReplaced` delta carrying a copy of the table.
     * - A refused post (the channel is full) leaves the version unpublished, so the next call retries it - the same
     *   "a refusal is recoverable" property the channel is built on.
     * - IT IS THE HANDSHAKE AND THE COARSE FALLBACK, not a per-frame call. The version it compares moves on ANY node
     *   edit, including the ones `setState`/`attach`/`detach` already announced finely - so the version cannot tell
     *   "already told" from "not told yet", and a producer that announces edits finely must not also publish every
     *   frame (it would repeat what the deltas said). Deciding WHAT to announce is the producer's job (`2.5`); this
     *   call is for the cases it cannot express: the first handshake, an edit no fine delta covers yet (a material or
     *   program swap), and the recovery after a refusal.
     *
     * @param scene The scene this mirror is the mirror of.
     * @return What the call answered: posted / unchanged / inside an edit batch / refused by a full channel (see
     *         PublishOutcome). A caller that only needs "did it post" reads the value as a bool.
     */
    PublishOutcome publish(const Scene& scene);

    /**
     * @brief Announces @p node's PAYLOAD: a re-read of its geometry description, draw facts and handles (content thread).
     *
     * @details
     * - The one fact with a handle in it (design F5/R5): a shape edit (`Geometry::setRevision`), a primitive or instance
     *   change, a material/program swap on a geometry or on a StateNode. The content's thread re-reads it ONCE (the
     *   description is an immutable value per revision) and the engine side replaces that row and re-folds.
     * - @p node must be a LEAF: a row whose subtree gains or loses rows is a structure change (attach / detach).
     *
     * @param node The leaf whose payload changed.
     * @return true when the announcement was posted.
     */
    bool setDescription(const Node& node);

    /**
     * @brief Announces @p node's own numeric values (content thread).
     *
     * @details
     * - The interactive half of a scene's state: dim a part, isolate a layer, restack a node. The row's subtree is
     *   re-folded on the engine side; nothing is re-read from the graph there.
     * - It carries NO handle, so a StateNode's material/program change is not this call: that one fact travels as a
     *   description delta (design R5), and until it exists it goes through `publish`.
     *
     * @param node The node whose values the content now states.
     * @return true when the announcement was posted.
     */
    bool setState(const Node& node);

    /**
     * @brief Announces that @p subtree now hangs under @p parent at @p child_index (content thread).
     *
     * @details
     * - Only the subtree's OWN facts travel (see buildOwnFacts): the engine side splices them into its table and folds
     *   them there, against the parent it already has. That is what keeps a structure change O(subtree) instead of a
     *   republish of the whole table.
     * - The subtree's root must be attached in the graph already (the caller is describing what it just did): a
     *   detached node's `localTransformMatrix()` is still its own, so the block is the same either way.
     *
     * @param parent      The row the subtree hangs under (its graph node).
     * @param subtree     Root of the subtree being connected.
     * @param child_index Where among the parent's children it goes (0 = first).
     * @return true when the announcement was posted.
     */
    bool attach(const Node& parent, const Node& subtree, std::uint32_t child_index);

    /**
     * @brief Announces that @p subtree's rows are gone (content thread).
     *
     * @param subtree Root of the subtree that was disconnected.
     * @return true when the announcement was posted.
     */
    bool detach(const Node& subtree);

    /**
     * @brief Opens @p frame on this mirror's source: the frame token that lets a list outlive one frame (engine thread).
     *
     * @param frame The frame about to ask for views; 0 means none is open (nothing is memoised).
     */
    void beginFrame(std::uint64_t frame);

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

    /** @brief Gets how many lists the source has answered from its memo (the per-view reuse witness). */
    [[nodiscard]] std::uint64_t memoHits() const noexcept;

    /** @brief Gets how many lists the source has collected (the per-view walk witness). */
    [[nodiscard]] std::uint64_t memoStores() const noexcept;

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
