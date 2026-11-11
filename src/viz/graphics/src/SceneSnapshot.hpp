#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/RenderCommand.hpp>

#include "DrawListBake.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief An immutable, chunked view of one Scene publication.
 *
 * @details
 * - WHY IT EXISTS. `Scene` owns a mutable `SceneBake` that it refreshes in place. A select must not read that
 *   mutable table while the content thread edits it, so a publication hands out THIS value: rows are copied into
 *   fixed-size chunks, the `nodes` bookkeeping is dropped, and the result is only read afterwards.
 * - STRUCTURE SHARING. A new snapshot reuses the previous snapshot's chunk when none of its rows' identities,
 *   content epochs or structural fields moved. An edit therefore copies the chunks it touches, not the table.
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
    std::uint64_t                                        epoch{ 0 };
    std::uint64_t                                        boxes{ 0 };
    std::uint64_t                                        materials{ 0 };

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
 * @param epoch     The scene's published content epoch.
 * @param boxes     The scene's box generation.
 * @param materials The scene's material generation.
 * @param previous  The snapshot this one may share chunks with, or null for a full copy.
 * @return The new snapshot; never null.
 */
[[nodiscard]] SceneSnapshotPtr makeSceneSnapshot(const SceneBake&        rows,
                                                 std::uint64_t           epoch,
                                                 std::uint64_t           boxes,
                                                 std::uint64_t           materials,
                                                 const SceneSnapshotPtr& previous);

/**
 * @brief The pure view half of a collection, over a snapshot.
 *
 * @details
 * - It reads only @p snapshot and @p view; it never asks a Scene, a Node or a material for anything.
 * - The output is cleared first, then filled in walk order and sorted by the same `sortCollected` the Scene uses,
 *   so the result is directly comparable with `Scene::collectRenderCommands` for the same view.
 * - `viewport_used` reports whether a viewport-dependent placement was resolved, exactly as the Scene's collector
 *   reports it to its memo.
 *
 * @param snapshot      The published rows to select from.
 * @param view          The view value to select for.
 * @param layer_mask    Layers this selection draws.
 * @param out           Receives the commands (replaced).
 * @param viewport_used Receives whether the view's viewport affected a placement.
 */
void selectSnapshot(const SceneSnapshot&        snapshot,
                    const CollectView&          view,
                    std::uint32_t               layer_mask,
                    std::vector<RenderCommand>& out,
                    bool&                       viewport_used);

} // namespace detail

VN_GRAPHICS_NS_END
