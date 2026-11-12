#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/Scene.hpp>

#include "SceneSnapshot.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

class DrawListMemo;

/**
 * @brief Returns the process-wide empty command list (one allocation for "nothing to draw").
 *
 * @details
 * - One list for the whole module: a scene with nothing to draw and a caller with no camera answer the SAME shared
 *   list, so "the empty answer" stays one value rather than one allocation per path.
 *
 * @return The shared empty list.
 */
[[nodiscard]] std::shared_ptr<const std::vector<vn::graphics::RenderCommand>> noCommands();

/**
 * @brief THE DERIVED SIDE OF A SCENE: what a collection and a publication compute from the authored tree.
 *
 * @details
 * - WHAT IT IS. Everything a scene DERIVES from what the host wrote: the baked graph half (`SceneBake`: placements,
 *   world boxes, folded state), the per-view list memo, the content epoch the memo is keyed by, the edit batch's
 *   depth, and the publication (`Scene::publish`'s snapshot and the version pair it compares). `Scene` owns one of
 *   these and delegates every derived operation to it, so the authored tree and its derived state are two objects
 *   with one direction between them.
 * - WHY IT IS SEPARATE. `Scene` was one class living three lives: the authored tree, the derived cache and the
 *   publication value. A change to any one of them landed in the same class, and the authoring API had to carry the
 *   lifetime rules of caches and snapshots it does not own (`G1` / `G5`). Here the derived half has one owner and
 *   one thread, and the authoring half stays an object the host holds.
 * - WHAT IT DOES NOT OWN. The content: no node, no light, no material. It holds a reference to the scene it reads
 *   (the scene owns it, both live on the content thread) and only ever reads the authored side.
 * - THREADS. Content thread, like the scene: `collectRenderCommands` walks the graph and `publish` builds from it.
 *   The immutable results (`SceneSnapshotPtr`, the shared command lists) are what may leave the thread.
 *
 * @note
 * - The door for the publication value is `snapshot()`, which `detail::SceneRows::snapshot` forwards to: the row
 *   layout stays an implementation type the public SDK does not name, and the algorithm lives next to the rows it
 *   reads (`G1`).
 */
class ScenePublisher final
{
  public:
    /**
     * @brief Constructs the derived side of @p scene.
     *
     * @param scene The scene whose authored side this reads; it owns this object and outlives it.
     */
    explicit ScenePublisher(vn::graphics::Scene& scene) noexcept;
    ~ScenePublisher();

    ScenePublisher(const ScenePublisher&)            = delete;
    ScenePublisher& operator=(const ScenePublisher&) = delete;

  public:
    /**
     * @brief Collects what @p view draws, restricted to @p layer_mask (see Scene::collectRenderCommands).
     *
     * @details
     * - NOT A FRAME SOURCE: a frame draws a publication (see SnapshotSource); this is the content thread's own query,
     *   which `Scene::collectRenderCommands` forwards to.
     *
     * @param view       The view to collect for.
     * @param layer_mask Layers this picture draws.
     * @return The shared, immutable command list.
     */
    [[nodiscard]] std::shared_ptr<const std::vector<vn::graphics::RenderCommand>>
    collectRenderCommands(const vn::graphics::CollectView& view, std::uint32_t layer_mask) const;

    /**
     * @brief Publishes the scene's current rows as an immutable snapshot, or answers why it did not.
     *
     * @return The status and the publication that is the scene's answer afterwards (see ScenePublishResult).
     */
    vn::graphics::ScenePublishResult publish();

    /** @brief Opens an edit batch: every edit until endEdit() lands as ONE publication (see Scene::beginEdit). */
    void beginEdit();

    /** @brief Closes an edit batch (see Scene::endEdit()). */
    void endEdit();

    /**
     * @brief Gets the publication `publish()` last answered `Posted` with (empty before the first one).
     *
     * @details
     * - THE PICTURE A FRAME DRAWS BY DEFAULT: a frame built after this call takes this value, which is what makes
     *   "the frame draws what was published" a property of the values rather than of an ordering rule.
     *
     * @return The published handle; empty when nothing was posted yet.
     */
    [[nodiscard]] const vn::graphics::SceneSnapshotRef& publishedSnapshot() const noexcept;

    /**
     * @brief Opens @p frame on the memo (see Scene::setContentFrame).
     *
     * @param frame Opaque frame token; the same token means the same frame.
     */
    void setContentFrame(std::uint64_t frame);

    /** @brief Drops what the scene derived (see Scene::invalidateContent). */
    void invalidateContent();

    /**
     * @brief Builds (or returns the cached) immutable snapshot at the scene's published version.
     *
     * @details
     * - The one snapshot builder: it brings the rows up to date (boxes and material values included), shares unchanged
     *   chunks with the previous publication, and returns the cached value when nothing moved (see
     *   `detail::SceneRows`, its one caller).
     *
     * @return The immutable publication; never null.
     */
    [[nodiscard]] SceneSnapshotPtr snapshot();

  public:
    /** @brief Gets how many times the tree was walked to collect commands. */
    [[nodiscard]] std::uint64_t collectCount() const noexcept;

    /** @brief Gets how many collected lists came from the memo. */
    [[nodiscard]] std::uint64_t reuseCount() const noexcept;

    /** @brief Gets how many times the graph was baked (the graph half of a collection). */
    [[nodiscard]] std::uint64_t bakeCount() const noexcept;

  private:
    /**
     * @brief Builds (once per epoch) and box-refreshes the rows a collection reads, and returns them.
     *
     * @details
     * - The one step of the derived side that needs the content thread: building reads the graph, and the box refresh
     *   reads each leaf's own data box (a cache keyed by the buffers' revisions).
     *
     * @return The rows, valid until the graph changes.
     */
    [[nodiscard]] const SceneBake& ensureRows() const;

  private:
    vn::graphics::Scene& scene_;

    // Bumped by every scene-level content change, so a memo built before the change is not reused (see
    // invalidateContent) - and by a material whose values moved (see detail::refreshValues, which runs during a
    // collection because a material edit announces nothing on the graph).
    mutable std::uint64_t content_revision_ = 1;
    // Token of the content frame currently open, 0 = none (no memoising).
    std::uint64_t content_frame_ = 0;
    // THIS SCENE'S ROOT SUBTREE content epoch when the memo was last validated: a different value means something in
    // this scene's graph changed since (Node::announceContentChange climbs to the root), so every collected list may
    // be stale and the memo is dropped before it is looked up again. Mutable: collecting is a const operation that
    // maintains the memo (see content_collect_count_).
    mutable std::uint64_t memo_epoch_ = 0;
    // The collected lists of the views in use (one per camera), reused while the graph does not change and dropped
    // when nothing asked for them in the previous frame - so the memo is bounded by the views drawn, not by frames.
    mutable std::unique_ptr<DrawListMemo> content_memo_;
    // The GRAPH half of the same collection, keyed by the same epoch: placements, world boxes and folds, in walk
    // order. A view reads it instead of recomputing it, which is why a moving camera costs the prune and the sort
    // rather than a full walk.
    mutable std::unique_ptr<SceneBake> content_bake_;
    mutable std::uint64_t              bake_epoch_            = 0;
    mutable std::uint64_t              content_bake_count_    = 0;
    mutable std::uint64_t              content_collect_count_ = 0;
    mutable std::uint64_t              content_reuse_count_   = 0;
    // How many times a re-read box actually moved (see ensureRows): a byte edit announces itself on a buffer, not on
    // the graph, so it does not move `bake_epoch_` - and a reader that published these rows needs a version that does.
    mutable std::uint64_t box_generation_ = 0;
    // How many times a VALUE moved under the rows (see detail::refreshValues): a material's values and a program's
    // text are edited through live objects, so no node mutator announces it and `bake_epoch_` cannot see it - while
    // the values a command carries do change (RenderCommand::material_description / program_description).
    mutable std::uint64_t value_generation_ = 0;
    // HOW DEEP THE EDIT BATCH IS (see Scene::beginEdit), and the version a publication compares (`publish`). Inside a
    // batch the published pair stays where it was, so a frame that lands mid-batch publishes nothing; the next
    // collection after the outermost endEdit() catches it up in one step - which is also the only moment the whole
    // batch is read.
    int                   edit_depth_        = 0;
    mutable std::uint64_t published_epoch_   = 0;
    mutable std::uint64_t published_boxes_   = 0;
    mutable std::uint64_t published_values_  = 0;
    // The publication `publish()` last answered `Posted` with: the handle is also the answer `Unchanged` and
    // `InsideBatch` hand back, so a host always receives the picture a frame would draw.
    vn::graphics::SceneSnapshotRef published_snapshot_;
};

} // namespace detail

VN_GRAPHICS_NS_END
