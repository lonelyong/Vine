#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <vine/graphics/Scene.hpp>

#include "SnapshotSource.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief THE CONTENT SIDE OF THE PUBLICATION CHANNEL: one `SnapshotSource` per content, and the one lock over the table.
 *
 * @details
 * - WHAT IT IS. The table a frame's draws are bound through: for every content that has ever been published or bound,
 *   the `SnapshotSource` that hands the frame's engine half the publication to draw. The table's verbs are the
 *   CONTENT side's - `publish` (the frame's implicit publication) and `bindSnapshot` (the host's explicit handoff) -
 *   because a publication is produced where the content is written.
 * - WHY IT IS NOT PART OF THE EXPANDER. The content half (see `RenderEngine::buildFrameValue`) has to publish, and
 *   the engine half (see `detail::FrameExpander`) has to bind a draw to the source. Keeping both halves on one object
 *   made the content half know the engine half's object just to publish (an object-level edge pointing the wrong way,
 *   `G2`); here the dependency is one-directional: the expander reads this table, this table knows nothing about it.
 * - THE LOCK GUARDS THE LIST, NOT THE VALUES. `publish`/`bindSnapshot` run on the content thread and may create an
 *   entry; `sourceOf` runs on whichever thread answers a frame. The mutex is held for a lookup or an insert, never
 *   across a publish or an expansion.
 *
 * @warning `bindSnapshot` MUST NOT RUN WHILE A FRAME IS RUNNING. A bind may create an entry while the expander walks
 * the same list. Today the runner hands a frame over and waits for it, so the content's thread is never inside a bind
 * while the frame is answered; a mode that overlaps frames has to move publication creation into the handshake BEFORE
 * it may overlap them.
 *
 * @note
 * - `beginFrame` opens the frame token on every source, which is what makes the per-view memos share one walk across
 *   a frame's passes. It is a table-wide operation, so it lives here rather than on the object that reads a single
 *   draw.
 */
class PublicationStore {
  public:
    PublicationStore();
    ~PublicationStore();

    PublicationStore(const PublicationStore&)            = delete;
    PublicationStore& operator=(const PublicationStore&) = delete;

  public:
    /**
     * @brief Publishes @p scene's snapshot, attaching a source on first use.
     *
     * @details
     * - Content thread, once per frame and scene (see `RenderEngine::buildFrameValue`: every hook has run before the
     *   frame publishes, so the one publication carries all of their writes).
     * - It calls `Scene::publish()` (the explicit publication entry) and hands the resulting publication to this
     *   content's `SnapshotSource`. It is a cheap no-op when the scene has not changed (`Unchanged`); inside an edit
     *   batch it keeps the previous snapshot, so a frame that lands mid-batch draws the state the batch started from.
     * - A scene a host BOUND (@ref bindSnapshot) is not published here: the call answers `Unchanged` with the
     *   publication the source already holds, because the host owns that picture.
     *
     * @param scene The scene this frame draws.
     * @return What the scene answered, publication included (see ScenePublishResult).
     */
    ScenePublishResult publish(Scene& scene);

    /**
     * @brief Makes @p snapshot the picture frames draw @p scene with (see RenderEngine::bindSnapshot).
     *
     * @details
     * - The EXPLICIT half of `publish`: a host that called `Scene::publish()` itself hands the value over here, and
     *   the frame draws exactly that publication. An empty handle clears it (every view then answers an empty list).
     * - It attaches the content's source on first use, exactly as `publish` does, so binding before a frame is the
     *   ordinary way to say "draw this picture".
     *
     * @warning See the class note: not while a frame is running.
     *
     * @param scene    The scene whose publication is being handed over.
     * @param snapshot The publication to draw, or an empty handle to draw nothing.
     */
    void bindSnapshot(Scene& scene, const SceneSnapshotRef& snapshot);

    /**
     * @brief Gets whether @p scene's publication was taken over by a host (@ref bindSnapshot).
     *
     * @details
     * - THE FRAME'S IMPLICIT PUBLICATION ASKS THIS FIRST: a host that bound a publication owns it, so the frame must
     *   not overwrite it with whatever the content says at frame start - that would make `bindSnapshot` a call with no
     *   effect. Unbound scenes keep the migration's convenience (one publication per frame, see `buildFrameValue`),
     *   which 5b-2c deletes when every host publishes explicitly.
     *
     * @param scene The scene to ask about.
     * @return true when a host bound this scene's snapshot (see @ref bindSnapshot).
     */
    [[nodiscard]] bool hasExplicitSnapshot(const Scene& scene);

    /**
     * @brief Opens @p frame on every source: the token their memos are keyed by (the frame's FIRST step).
     *
     * @param frame The frame about to ask for views (0 means none is open: nothing is memoised).
     */
    void beginFrame(std::uint64_t frame);

    /**
     * @brief Finds the source @p source names, whether it names the content or a source itself.
     *
     * @details
     * - The engine half's bind step reads this: a draw naming a CONTENT is re-pointed at that content's published
     *   snapshot, and a draw that already names the source is left alone (a second run over one frame value).
     *
     * @param source The draw's `content` (a content or one of this store's sources).
     * @return The source to bind to, or null when this store never published that content.
     */
    [[nodiscard]] SnapshotSource* sourceOf(const vn::graphics::DrawListSource& source) noexcept;

  public:
    /** @brief Gets how many contents this store publishes. */
    [[nodiscard]] std::size_t publicationCount() const noexcept;

    /** @brief Gets how many lists the publications' sources answered from their memos (the per-view reuse witness). */
    [[nodiscard]] std::uint64_t memoHits() const noexcept;

    /** @brief Gets how many lists the publications' sources collected (the per-view walk witness). */
    [[nodiscard]] std::uint64_t memoStores() const noexcept;

  private:
    /// @brief One content's published-snapshot face, owned by the store.
    struct Publication {
        const vn::graphics::DrawListSource* content{ nullptr };
        std::unique_ptr<SnapshotSource>     source{};
        /// A host handed this content's publication over itself (see bindSnapshot): the frame must not publish it.
        bool explicit_snapshot{ false };
    };

    /// @brief Finds the source of the content @p scene, attaching one on first use.
    [[nodiscard]] SnapshotSource* sourceFor(Scene& scene);

    /// @brief The attach-on-first-use half, for callers that already hold `publications_mutex_`.
    [[nodiscard]] SnapshotSource* sourceForLocked(Scene& scene);

    /// @brief The lookup itself, for callers that already hold `publications_mutex_`.
    [[nodiscard]] SnapshotSource* sourceOfLocked(const vn::graphics::DrawListSource& source) noexcept;

    /// @brief The publication record of @p scene, or null when this store never saw it.
    [[nodiscard]] Publication* publicationOfLocked(const Scene& scene) noexcept;

  private:
    std::vector<Publication> publications_;
    /// Guards the publications' LIST (not their contents): a content-side call may create an entry the engine side
    /// walks while it binds. Held only for a lookup or an insert, never across a publish or an expansion.
    mutable std::mutex publications_mutex_;
    std::uint64_t      frame_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
