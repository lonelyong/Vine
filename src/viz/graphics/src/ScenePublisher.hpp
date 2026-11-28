#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vine/graphics/View.hpp>
#include <vine/graphics/DrawItem.hpp>
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
[[nodiscard]] std::shared_ptr<const std::vector<vn::graphics::DrawItem>> noCommands();

/**
 * @brief THE DERIVED SIDE OF A SCENE: what a collection and a publication compute from the authored tree.
 *
 * @details
 * - WHAT IT IS. Everything a scene DERIVES from what the host wrote: the baked graph half (`SceneBake`: placements,
 *   world boxes, folded state), the per-view list memo, the rows' version (which table, and the clocks that move under
 *   it - see `RowsVersion`) the memo is keyed through, the edit batch's depth, and the publication (`Scene::publish`'s
 *   snapshot). `Scene` owns one of these and delegates every derived operation to it, so the authored tree and its
 *   derived state are two objects with one direction between them.
 * - WHY IT IS SEPARATE. `Scene` was one class living three lives: the authored tree, the derived cache and the
 *   publication value. A change to any one of them landed in the same class, and the authoring API had to carry the
 *   lifetime rules of caches and snapshots it does not own (`G1` / `G5`). Here the derived half has one owner and
 *   one thread, and the authoring half stays an object the host holds.
 * - WHAT IT DOES NOT OWN. The content: no node, no light, no material. It holds a reference to the scene it reads
 *   (the scene owns it, both live on the content thread) and only ever reads the authored side.
 * - THREADS. Content thread, like the scene: `collectDrawItems` selects from the publication and `publish`
 *   builds it. The immutable results (`SceneSnapshotPtr`, the shared command lists) are what may leave the thread.
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
     * @brief Collects the commands @p view draws (see Scene::collectDrawItems).
     *
     * @details
     * - ONE SELECTION RULE: the query answers from the scene's current publication (the value a frame draws) through
     *   the same `selectSnapshot` the frame's source runs. There is no second walk over the live rows.
     * - NOT A FRAME SOURCE: a frame draws a publication (see SnapshotSource); this is the content thread's own query,
     *   which `Scene::collectDrawItems` forwards to - and inside an edit batch it answers the picture the batch
     *   started from, exactly as `publish()` reports it.
     *
     * @param view The view to collect for: camera value, device-pixel area and layer mask.
     * @return The shared, immutable command list.
     */
    [[nodiscard]] std::shared_ptr<const std::vector<vn::graphics::DrawItem>>
    collectDrawItems(const vn::graphics::View& view) const;

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
     *   chunks with the previous publication, and returns the cached value when its `RowsVersion` still equals the
     *   published one (see `detail::SceneRows`, its one caller).
     * - INSIDE AN EDIT BATCH it answers the PREVIOUS PUBLICATION - the very value `publish()` answers `InsideBatch`
     *   with - so a query that lands mid-batch draws what a frame mid-batch draws, whatever the batch has edited so
     *   far (review 22 `Z31`; building from the live rows there made the answer depend on whether a publication had
     *   ever been cached).
     * - Const because a query (see collectDrawItems) reads it too: everything it maintains is a mutable cache.
     *
     * @return The immutable publication; never null (an empty one before anything moved).
     */
    [[nodiscard]] SceneSnapshotPtr snapshot() const;

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
     * - It is a DISPATCHER over four steps that each answer one question (see the helpers below), so the contract above
     *   can be read once and the paths can be tested apart.
     *
     * @return The rows, valid until the graph changes.
     */
    [[nodiscard]] const SceneBake& ensureRows() const;

    /** @brief What comparing the row table with the graph found: whether the SHAPE still matches, and which rows moved. */
    struct GraphScan {
        /// True when the table's rows, the node each was read from and its recorded epoch still line up with the graph.
        bool                       matches{ false };
        /// The rows whose OWN content epoch moved (their ancestors' may have moved with them).
        std::vector<std::uint32_t> changed;
    };

    /**
     * @brief Compares the table's shape with the graph's, and collects the rows whose own epoch moved.
     *
     * @details
     * - The structure half checks what no child count can see: the row walk order IS the child order, so a reorder or
     *   a same-count child swap is a mismatch (the fast path must not accept it).
     *
     * @return Whether the shape still matches, and which rows announced a change.
     */
    [[nodiscard]] GraphScan rowsStillMatchTheGraph() const;

    /**
     * @brief Re-reads the rows the graph announced, and re-folds only their topmost subtrees.
     *
     * @details
     * - THE INCREMENTAL PATH, and it never guesses: a structure change, a body change, or "nothing worth re-folding"
     *   answers false, and the caller rebuilds the table instead (see rebuildRows).
     *
     * @return true when the rows were brought up to date in place.
     */
    [[nodiscard]] bool refreshChangedRows() const;

    /** @brief Replaces the table with one fresh build, carrying the serial counter across (see SceneBake::next_serial). */
    void rebuildRows() const;

    /** @brief Empties the table for a scene with no rooted root, and drops the publication built from it. */
    [[nodiscard]] const SceneBake& resetToEmpty() const;

  private:
    vn::graphics::Scene& scene_;

    // Token of the content frame currently open, 0 = none (no memoising).
    std::uint64_t content_frame_ = 0;
    // The collected lists of the views in use (one per view), reused while the publication's revision does not change
    // and dropped when nothing asked for them in the previous frame. Keyed by the publication, not by a second epoch:
    // the snapshot's own versions are what say a list is stale (see snapshot()).
    mutable std::unique_ptr<DrawListMemo> content_memo_;
    // The GRAPH half of the same collection, keyed by the same epoch: placements, world boxes and folds, in walk
    // order. A view reads it instead of recomputing it, which is why a moving camera costs the prune and the sort
    // rather than a full walk.
    mutable std::unique_ptr<SceneBake> content_bake_;
    // THE LAST PUBLICATION built from the rows (and the value a new one shares chunks with). It belongs HERE, not to
    // the table: a rebuild replaces the table and must not orphan the publication, and a query that reads the table
    // has no business owning the picture a frame draws (see `SceneBake`).
    mutable SceneSnapshotPtr publication_cache_;
    // WHICH ROOT THE TABLE DESCRIBES (review 22 `Z30`): the one fact that says "these rows are this scene's". A
    // content epoch is a counter the described tree owns, and a tree built by the same builder reaches the same
    // number - so an epoch alone would let a replaced or re-rooted scene be served from the previous tree's rows,
    // through raw node pointers that tree no longer owns. Invalid until a table is built; cleared by resetToEmpty.
    mutable NodeId        baked_root_{};
    // The TABLE'S GENERATION: moves on every rebuild and every reset. The epoch above is the cheap "did a node
    // announce" clock for the incremental path; this one is the identity a publication compares, so "a new table"
    // and "the same table, edited" stay distinguishable even when the epoch repeats (see RowsVersion).
    mutable std::uint64_t table_generation_ = 0;
    mutable std::uint64_t bake_epoch_            = 0;
    mutable std::uint64_t content_bake_count_    = 0;
    mutable std::uint64_t content_collect_count_ = 0;
    mutable std::uint64_t content_reuse_count_   = 0;
    // How many times a re-read box actually moved (see ensureRows): a byte edit announces itself on a buffer, not on
    // the graph, so it does not move `bake_epoch_` - and a reader that published these rows needs a version that does.
    mutable std::uint64_t box_generation_ = 0;
    // How many times a VALUE moved under the rows (see detail::refreshValues): a material's values and a program's
    // text are edited through live objects, so no node mutator announces it and `bake_epoch_` cannot see it - while
    // the values a command carries do change (DrawItem::material_description / program_description).
    mutable std::uint64_t value_generation_ = 0;
    // HOW DEEP THE EDIT BATCH IS (see Scene::beginEdit), and the version a publication compares (`publish`). Inside a
    // batch the published version stays where it was, so a frame that lands mid-batch publishes nothing; the next
    // collection after the outermost endEdit() catches it up in one step - which is also the only moment the whole
    // batch is read.
    int                          edit_depth_       = 0;
    mutable RowsVersion          published_version_{};
    // The publication `publish()` last answered `Posted` with: the handle is also the answer `Unchanged` and
    // `InsideBatch` hand back, so a host always receives the picture a frame would draw - and inside a batch it is
    // what the content query answers too (see snapshot()).
    vn::graphics::SceneSnapshotRef published_snapshot_;
};

} // namespace detail

VN_GRAPHICS_NS_END
