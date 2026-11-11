#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vine/graphics/DrawListSource.hpp>

#include "DrawListBake.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief The RENDER WORLD: the rows a draw list is built from, owned by the side that draws them.
 *
 * @details
 * - WHY IT EXISTS. A frame's GPU-facing work must not run on the thread that edits content, and the draw list is
 *   built from rows, not from nodes (see `.ai/design/graphics-threading.md`). This type owns those rows: it takes
 *   them as VALUES (`adopt`), moves them by values (`place`), and answers "what would this view draw" from rows only
 *   (`selectFromBake` + `sortCollected`) - so nothing here reads a node's mutable state.
 * - WHO BUILDS THE ROWS. The content's thread does, with `buildRows` (the only code that reads a graph), or any
 *   producer that can fill own facts from a description. That single conversion is the handshake; after it this side
 *   needs no content object at all, apart from the identity it was told to key by.
 * - IDENTITY. A row is addressed by `RowId`, the opaque address it mirrors (ADR-2026-11-09 keys caches by address and
 *   keeps the object alive). A `const Node*` converts to it implicitly, so a content-side caller writes
 *   `world.place(&node, matrix)` without this header naming a content type in any signature.
 * - WHAT IT IS NOT. Not a scene, not a document and not a second graph: no parent walks, no names, no selection, no
 *   behaviours. It answers one question, with the passes the content side uses too, so the two cannot drift.
 * - WHAT STILL NEEDS THE CONTENT'S THREAD. Two steps, both named: the handshake (`buildRows`), and the box refresh a
 *   byte write causes - a write moves the box a row culls with, and re-reading it asks a geometry's own
 *   revision-keyed cache. `Scene` runs the second; a world adopted from values holds no node to re-read, which is why
 *   `adopt` drops that record (see `SceneBake::nodes`).
 *
 * @note Not thread-safe: one thread owns an instance, like every other backend-side unit.
 */
class RenderWorld {
  public:
    /// The identity a row mirrors: opaque on this side, and the same address the content object has.
    using RowId = const void*;

  public:
    /** @brief Takes ownership of @p rows (the handshake; the values came from the content's thread). */
    void adopt(SceneBake rows);

    /** @brief Forgets every row (and every share it held). */
    void clear();

    /** @brief Returns whether this world mirrors @p identity. */
    [[nodiscard]] bool contains(RowId identity) const noexcept;

    /**
     * @brief Applies one placement edit: the identity's placement as the content's thread now states it.
     *
     * @details
     * - Both halves travel, because they are what a row is placed by: the view-independent local matrix, and the
     *   per-view placement value (null when the node's placement is that local matrix alone). Passing the new value
     *   is what lets a billboard move - its own numbers are IN the value, so a delta that leaves it out would leave
     *   the mirror drawing the old orientation.
     *
     * @param identity  A row this world mirrors.
     * @param local     That row's own local matrix.
     * @param placement That row's per-view placement, or null when it has none.
     * @return false when the identity is unknown (a row this world did not adopt); the refusal is counted.
     */
    bool place(RowId identity, const Mat4d& local, ViewPlacementPtr placement = {});

    /** @brief Collects one view's commands, in draw order - the same list the content's collection produces. */
    void collect(const CollectView& view, std::uint32_t layer_mask, std::vector<RenderCommand>& out) const;

  public:
    /** @brief Gets how many rows the world carries. */
    [[nodiscard]] std::size_t rowCount() const noexcept;

    /** @brief Gets how many rows the edits so far have re-resolved (the witness for "only the subtree"). */
    [[nodiscard]] std::uint64_t rowsRecomputed() const noexcept;

    /** @brief Gets how many edits were refused. */
    [[nodiscard]] std::uint64_t refusals() const noexcept;

  private:
    SceneBake                             bake_;
    std::unordered_map<RowId, std::uint32_t> index_;
    std::uint64_t                         rows_recomputed_{ 0 };
    std::uint64_t                         refusals_{ 0 };
};

/**
 * @brief The RENDER WORLD as the thing a frame asks for a view's commands (see `vn::graphics::DrawListSource`).
 *
 * @details
 * - WHY IT EXISTS. A frame states a VIEW and names who answers it (see `DrawDescription::content`); a `Scene` and a
 *   mirror must both be able to be that answer, or the threaded mode would need a second frame path. This is the
 *   mirror's face on that interface, and it is how the equivalence test proves the two sides produce one picture.
 * - IT OWNS NOTHING. The world belongs to whoever drains the deltas into it (the engine or its worker); this adapter
 *   only borrows it for the duration of a collection.
 *
 * @warning The world is single-threaded (see RenderWorld): the source answers on the thread that owns it.
 */
class RenderWorldSource final : public vn::graphics::DrawListSource {
  public:
    /** @brief Presents @p world as a draw list source.
     *
     * @param world The mirror to borrow; it must outlive this adapter.
     */
    explicit RenderWorldSource(RenderWorld& world) noexcept;

    /** @brief Collects what @p view draws from the mirror (see DrawListSource).
     *
     * @param view       The view to collect for.
     * @param layer_mask Layers this picture draws.
     * @return The commands, in draw order, shared the way every source shares one collection.
     */
    [[nodiscard]] std::shared_ptr<const std::vector<RenderCommand>> collectRenderCommands(const vn::graphics::CollectView& view,
                                                                                         std::uint32_t                    layer_mask) const override;

  private:
    RenderWorld* const world_;
};

} // namespace detail

VN_GRAPHICS_NS_END
