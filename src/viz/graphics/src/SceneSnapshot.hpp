#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vine/graphics/View.hpp>
#include <vine/graphics/DrawItem.hpp>

#include "DrawListBake.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief THE ROWS' VERSION: which table they came from, and the three clocks that move under it.
 *
 * @details
 * - ONE VALUE, ONE COMPARISON (`G1`): a cached publication is still the answer iff this value equals the one it was
 *   built with, and the memo's key is the publication it belongs to - so the question "did the rows move" is asked
 *   once, of one value, instead of field by field at every reader.
 * - `table` makes the version a TABLE IDENTITY rather than a clock. `epoch` and the two generations are counters the
 *   described tree (or the publisher's own refresh steps) owns, and two different trees reach the same number -
 *   a tree built by the same builder counts the same announcements. `table` moves on every rebuild and every reset
 *   instead, so a replaced or cleared root is a different table even when its epoch happens to agree (review 22
 *   `Z30`).
 * - `epoch` still carries what a rebuild cannot see for free: the INCREMENTAL edit path re-folds rows in place, and
 *   that path bumps the root's epoch without replacing the table.
 */
struct RowsVersion {
    std::uint64_t table{ 0 };  ///< The table generation: moves on every rebuild and every reset.
    std::uint64_t epoch{ 0 };  ///< The root's content epoch when the rows were last brought up to date.
    std::uint64_t boxes{ 0 };  ///< How many box refreshes moved under the rows (see refreshBoxes).
    std::uint64_t values{ 0 }; ///< How many material / program value refreshes moved under the rows (see refreshValues).

    /**
     * @brief Whether @p other is the same rows state.
     *
     * @details
     * - Defaulted on purpose, like every version value here: a field added to it joins the comparison by
     *   construction, and a hand-written list is a list somebody forgets.
     *
     * @param other The value to compare against.
     * @return true when every part is equal.
     */
    [[nodiscard]] bool operator==(const RowsVersion& other) const = default;
};

/**
 * @brief An immutable, chunked view of one Scene publication.
 *
 * @details
 * - WHY IT EXISTS. `Scene` owns a mutable `SceneBake` that it refreshes in place. A select must not read that
 *   mutable table while the content thread edits it, so a publication hands out THIS value: rows are copied into
 *   fixed-size chunks, the `nodes` bookkeeping is dropped, and the result is only read afterwards.
 * - STRUCTURE SHARING. A new snapshot reuses the previous snapshot's chunk when no row in it moved: its identity and
 *   structure are the same, its node's own facts did not announce (the content epoch), its own FOLD input did not move
 *   (`RowOwn::Fold`, which carries the per-view placement's identity - a fact the fold's answer cannot state), and the
 *   FOLD's answer for it did not change (`Row::resolved_serial`). An edit therefore copies the chunks it touches -
 *   including those below the edited node, whose epochs do not move but whose resolved facts do - and shares the rest.
 * - IT IS STILL A BRIDGE. The row layout is today's `Row`; the hot/cold `ContentRef` layout is the next step. This
 *   value exists to make the snapshot boundary and chunk sharing measurable without changing the row fields.
 * - CONTENT VALUES TRAVEL IN THE ROWS. `Row::description` / `material_description` are immutable values carrying
 *   their revision, so a snapshot keeps the values it was copied with when a material or geometry is edited later.
 *
 * @warning Content-thread publication. `SceneRows::snapshot` reads the live graph; the returned snapshot is
 * immutable and may be read from any thread afterwards.
 */
struct SceneSnapshot {
    static constexpr std::size_t kRowsPerChunk = 32U;

    std::vector<std::shared_ptr<const std::vector<Row>>> chunks{};
    std::vector<NodeId>                                  identities{};  ///< One per row, for the next share decision.
    std::vector<std::uint64_t>                           node_epochs{}; ///< One per row, for the next share decision.
    std::size_t                                          row_count{ 0 };
    /// A publication id, unique for every newly built snapshot (see `makeSceneSnapshot`). It is the one state key a
    /// collected-list memo needs: two snapshots are different even when their row counts happen to agree.
    std::uint64_t                                        revision{ 0 };
    /// Which rows state this publication IS (see RowsVersion): what a next publication compares, and what says a
    /// cached one is still the answer.
    RowsVersion                                          version{};

    /** @brief Returns the row at @p index; the index must be below row_count. */
    [[nodiscard]] const Row& row(std::size_t index) const
    {
        return chunks[index / kRowsPerChunk]->at(index % kRowsPerChunk);
    }
};

using SceneSnapshotPtr = std::shared_ptr<const SceneSnapshot>;

/**
 * @brief Builds a snapshot from the scene's rows, sharing unchanged chunks with @p previous.
 *
 * @param rows      The current rows (read only).
 * @param version   The rows state this snapshot IS (see RowsVersion); the caller compares it to decide "cached".
 * @param previous  The snapshot this one may share chunks with, or null for a full copy.
 * @return The new snapshot; never null.
 */
[[nodiscard]] SceneSnapshotPtr makeSceneSnapshot(const SceneBake&        rows,
                                                 const RowsVersion&      version,
                                                 const SceneSnapshotPtr& previous);

/**
 * @brief The pure view half of a collection, over a snapshot.
 *
 * @details
 * - It reads only @p snapshot and @p view; it never asks a Scene, a Node or a material for anything.
 * - It is the same `selectRows` the Scene's own collector runs, so its list is directly comparable with
 *   `Scene::collectDrawItems` for the same view - one rule, one implementation (`G1`).
 * - `Selection::viewport_used` reports whether a viewport-dependent placement was resolved, exactly as the Scene's
 *   collector reports it to its memo.
 *
 * @param snapshot The published rows to select from.
 * @param view     The view value to select for (camera value, device-pixel area and layer mask).
 * @return The commands, in draw order, and whether the viewport took part (see Selection).
 */
[[nodiscard]] Selection selectSnapshot(const SceneSnapshot& snapshot, const View& view);

} // namespace detail

VN_GRAPHICS_NS_END
