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
 * @brief THE PUBLICATION CHANNEL: one `SnapshotSource` per (content, publication), and the one lock over the table.
 *
 * @details
 * - WHAT IT IS. The table a frame's draws are bound through: for every publication a frame has drawn, the
 *   `SnapshotSource` that answers its views. Its verbs are the CONTENT side's - `sourceFor` (the source of one
 *   publication, attached on first use), `publicationFor` (which publication a frame built now takes) and
 *   `bindSnapshot` (a host saying "draw this one instead") - because a publication is produced where the content is
 *   written.
 * - KEYED BY PUBLICATION, NOT BY CONTENT. A source is the `DrawListSource` face of ONE immutable value, so a new
 *   publication gets its own source: a frame that was already built keeps drawing the value it took, and a publish
 *   landing while it is answered cannot change its picture (`G4`: the guarantee is the value, not an ordering rule).
 *   A scene that publishes nothing new re-uses its previous source, so a static scene's per-view memo still survives.
 * - WHY IT IS NOT PART OF THE EXPANSION. The content half (see `RenderEngine::buildFrameValue`) takes the publication
 *   and binds each draw; the engine half (see `detail::expandFrame`) only reads the values it is handed. The
 *   dependency is one-directional: the content half reads this table, the engine half's single call is `beginFrame`
 *   (the token the memos share).
 * - THE LOCK GUARDS THE LISTS, NOT THE VALUES. `sourceFor` / `bindSnapshot` / `publicationFor` run on the content
 *   thread; `beginFrame` runs on whichever thread answers a frame. The mutex is held for a lookup, an insert or an
 *   eviction, never across a collection.
 *
 * @note
 * - `beginFrame` opens the frame token on every source, which is what makes the per-view memos share one walk across
 *   a frame's passes, and drops the sources no frame has named for one frame. With one frame in flight that is at
 *   most two publications per content, which is exactly "the one being answered + the one being built".
 */
class PublicationStore {
  public:
    PublicationStore();
    ~PublicationStore();

    PublicationStore(const PublicationStore&)            = delete;
    PublicationStore& operator=(const PublicationStore&) = delete;

  public:
    /**
     * @brief States which publication frames draw @p scene with (see RenderEngine::bindSnapshot).
     *
     * @details
     * - THE EXPLICIT OVERRIDE: a host that wants an older or a different picture than the scene's latest publication
     *   hands the value over here, and every frame built afterwards draws it - until another bind replaces it.
     * - An empty handle is a legitimate bound value: every view answers an empty list, the same answer as a scene
     *   whose content draws nothing.
     * - WITHOUT A BIND, a frame takes the scene's latest publication (`Scene::publishedSnapshot`), so publishing is
     *   the only verb a host needs.
     *
     * @param scene    The scene whose publication is being handed over.
     * @param snapshot The publication to draw, or an empty handle to draw nothing.
     */
    void bindSnapshot(Scene& scene, const SceneSnapshotRef& snapshot);

    /**
     * @brief Gets the publication a frame built NOW takes for @p scene: the bound one, else the scene's latest.
     *
     * @details
     * - Content thread, at the frame's build step. A scene that never published and was never bound answers an empty
     *   handle, which is how the compile tells "there is no picture" from "the picture is empty" - the draw then
     *   names no source and nothing is collected.
     *
     * @param scene The content the frame is about to draw.
     * @return The publication to draw, or an empty handle when there is none.
     */
    [[nodiscard]] SceneSnapshotRef publicationFor(const Scene& scene);

    /**
     * @brief Gets the source that answers @p snapshot for @p scene, attaching one on first use.
     *
     * @details
     * - THE FRAME'S BIND STEP READS THIS (see `detail::bindSceneDraws`): a scene drawing call is pointed at the source
     *   of the publication the frame took, so the expansion asks a publication and never the live tree.
     * - One source per (content, publication): two frames that took two publications do not share a collector, and
     *   the sources no frame names any more are dropped by `beginFrame` (see the class note).
     *
     * @param scene    The content the publication belongs to.
     * @param snapshot The publication to answer with; an empty handle answers null (nothing to draw).
     * @return The source to bind to, or null when @p snapshot is empty.
     */
    [[nodiscard]] SnapshotSource* sourceFor(Scene& scene, const SceneSnapshotRef& snapshot);

    /**
     * @brief Opens @p frame on every source and drops the publications no frame has named for one frame.
     *
     * @param frame The frame about to ask for views (0 means none is open: nothing is memoised).
     */
    void beginFrame(std::uint64_t frame);

  public:
    /** @brief Gets how many publications this store holds a source for. */
    [[nodiscard]] std::size_t publicationCount() const noexcept;

    /** @brief Gets how many lists the publications' sources answered from their memos (the per-view reuse witness). */
    [[nodiscard]] std::uint64_t memoHits() const noexcept;

    /** @brief Gets how many lists the publications' sources collected (the per-view walk witness). */
    [[nodiscard]] std::uint64_t memoStores() const noexcept;

  private:
    /// @brief One publication's source, owned by the store.
    struct Publication {
        const Scene*                    content{ nullptr };
        /// The publication this source answers (`SceneSnapshotRef::revision`, unique per posted publication).
        std::uint64_t                   revision{ 0 };
        std::unique_ptr<SnapshotSource> source{};
        /// The frame that last named it: a source no frame has named for one frame is dropped (see the class note).
        std::uint64_t                   last_used_frame{ 0 };
    };

    /// @brief One content's explicit override (see bindSnapshot).
    struct Binding {
        const Scene*     content{ nullptr };
        SceneSnapshotRef snapshot{};
    };

    /// @brief The source of @p scene's @p snapshot, attaching one when this store has not seen it yet.
    [[nodiscard]] SnapshotSource* sourceForLocked(Scene& scene, const SceneSnapshotRef& snapshot);

    /// @brief The publication record of @p scene's @p revision, or null.
    [[nodiscard]] Publication* publicationOfLocked(const Scene& scene, std::uint64_t revision) noexcept;

    /// @brief The override bound for @p scene, or null.
    [[nodiscard]] const Binding* bindingOfLocked(const Scene& scene) const noexcept;

  private:
    std::vector<Publication> publications_;
    std::vector<Binding>     bindings_;
    /// Guards the two lists (not their contents): a content-side call may create an entry the engine side walks while
    /// it opens a frame. Held only for a lookup, an insert or an eviction, never across a collection.
    mutable std::mutex publications_mutex_;
    std::uint64_t      frame_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
