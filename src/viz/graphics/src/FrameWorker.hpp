#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Scene.hpp>

#include "SnapshotSource.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief THE ENGINE SIDE OF ONE FRAME: bind the frame to published snapshots, then expand it.
 *
 * @details
 * - WHAT IT IS. The object a frame's engine half hangs on: one `SnapshotSource` per content it draws, and the two
 *   steps that must happen in this order at frame start -
 *   1. **bind**: a draw that names a CONTENT is re-pointed at that content's PUBLISHED SNAPSHOT (see the design's
 *      `bindContent`), which is what makes the expansion stop asking the content;
 *   2. **expand**: `expandFrame`, the one O(drawables) step (see FrameExpand).
 * - IT OWNS NO THREAD AND NO WAITING. `run` is a plain call: whoever owns it decides which thread it runs on, which is
 *   what lets the inline mode (the calling thread, no thread at all) and the threaded mode be the same object. Waiting,
 *   frame numbers and the start/stop handshake belong to the owner (see the design's 2.8 and the backlog's T31).
 * - IT READS NO CONTENT. The content arrives as an immutable publication (`Scene::publish`) and as a pointer to
 *   compare (`DrawDescription::content`); nothing here dereferences a node. A draw whose content has no publication is
 *   COUNTED (`unbound()`) instead of being quietly expanded from the content - which is correct on the calling thread
 *   and a cross-thread read anywhere else, so the count is how a host finds out it forgot to publish.
 *
 * @warning `bindSnapshot` MUST NOT RUN WHILE A FRAME IS RUNNING. The snapshot handoff is safe from both sides, but the
 * LIST of publications is not: a bind may create one while `run` walks it. Today that is guaranteed by construction -
 * the runner hands a frame over and waits for it, so the content's thread is inside `run` while the engine's works -
 * and a mode that overlaps frames has to move publication creation into the handshake BEFORE it may overlap them.
 * Stating the invariant rather than relying on it silently is the point.
 */
class FrameWorker {
  public:
    FrameWorker();
    ~FrameWorker();

    FrameWorker(const FrameWorker&)            = delete;
    FrameWorker& operator=(const FrameWorker&) = delete;

  public:
    /**
     * @brief Publishes @p scene's snapshot, attaching a source on first use.
     *
     * @details
     * - Content thread, before the frame that draws @p scene is run. It calls `Scene::publish()` (the explicit
     *   publication entry) and hands the resulting publication to this content's `SnapshotSource`.
     * - It is a cheap no-op when the scene has not changed (`Unchanged`); inside an edit batch it keeps the previous
     *   snapshot, so a frame that lands mid-batch draws the state the batch started from.
     * - A scene a host BOUND (`bindSnapshot`) is not published here: the call answers `Unchanged` with the publication
     *   the source already holds, because the host owns that picture.
     *
     * @param scene The scene this frame draws.
     * @return What the scene answered, publication included (see ScenePublishResult).
     */
    ScenePublishResult publish(Scene& scene);

    /**
     * @brief Makes @p snapshot the picture this worker draws @p scene with (see RenderEngine::bindSnapshot).
     *
     * @details
     * - The EXPLICIT half of `publish`: a host that called `Scene::publish()` itself hands the value over here, and
     *   the frame draws exactly that publication. An empty handle clears it (every view then answers an empty list).
     * - It attaches the content's source on first use, exactly as `publish` does, so binding before a frame is the
     *   ordinary way to say "draw this picture".
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
     *   effect. Unbound scenes keep the migration's convenience (one publication per pass' hook, see EngineCompile),
     *   which 5b-2c deletes when every host publishes explicitly.
     *
     * @param scene The scene to ask about.
     * @return true when a host bound this scene's snapshot (see @ref bindSnapshot).
     */
    [[nodiscard]] bool hasExplicitSnapshot(const Scene& scene);

    /**
     * @brief Opens @p frame on every publication: the token their memos are keyed by (the frame's FIRST step).
     *
     * @param frame The frame about to ask for views (0 means none is open: nothing is memoised).
     */
    void beginFrame(std::uint64_t frame);

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
    /** @brief Gets how many contents this worker publishes. */
    [[nodiscard]] std::size_t publicationCount() const noexcept;

    /** @brief Gets how many draws named a source this worker does not publish (see the class note). */
    [[nodiscard]] std::uint64_t unbound() const noexcept;

    /** @brief Gets how many lists the publications' sources answered from their memos (the per-view reuse witness). */
    [[nodiscard]] std::uint64_t memoHits() const noexcept;

    /** @brief Gets how many lists the publications' sources collected (the per-view walk witness). */
    [[nodiscard]] std::uint64_t memoStores() const noexcept;

  private:
    /// @brief One content's published-snapshot face, owned by the worker.
    struct Publication {
        const vn::graphics::DrawListSource* content{ nullptr };
        std::unique_ptr<SnapshotSource>     source{};
        /// A host handed this content's publication over itself (see bindSnapshot): the frame must not publish it.
        bool explicit_snapshot{ false };
    };

    /// @brief Finds the source a draw names, whether it names the content or the source itself.
    [[nodiscard]] SnapshotSource* sourceOf(const vn::graphics::DrawListSource& source) noexcept;

    /// @brief Finds the source of the content @p scene, attaching one on first use.
    [[nodiscard]] SnapshotSource* sourceFor(Scene& scene);

    /// @brief The lookup itself, for callers that already hold `publications_mutex_`.
    [[nodiscard]] SnapshotSource* sourceOfLocked(const vn::graphics::DrawListSource& source) noexcept;

    /// @brief The attach-on-first-use half, for callers that already hold `publications_mutex_`.
    [[nodiscard]] SnapshotSource* sourceForLocked(Scene& scene);

    /// @brief The publication record of @p scene, or null when this worker never saw it.
    [[nodiscard]] Publication* publicationOfLocked(const Scene& scene) noexcept;

  private:
    std::vector<Publication> publications_;
    /// Guards the publications' LIST (not their contents): the content half creates one the first time a content is
    /// published, and the engine half walks the same list while it opens a frame and binds - two threads, one vector,
    /// and a `push_back` that can reallocate it. Held only for a lookup or an insert, never across a publish/expand.
    mutable std::mutex publications_mutex_;
    std::uint64_t      frame_{ 0 };
    std::uint64_t      unbound_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
