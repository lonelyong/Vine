#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

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
 * - WHAT STILL NEEDS THE CONTENT'S THREAD. Two steps, both named: the handshake (`buildRows`), and `refreshBoxes` - a
 *   byte write moves the box a row culls with and announces itself on the buffer, so re-reading it asks a geometry's
 *   own revision-keyed cache (a cache write). A world adopted from a description never runs the second.
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
     * @brief Applies one placement edit: the identity's own local matrix, with its subtree re-placed.
     *
     * @param identity A row this world mirrors.
     * @param local    That row's own local matrix.
     * @return false when the identity is unknown, resolves its placement per view, or carries a box only its node
     *         could answer (see Row::box_from_node); the refusal is counted.
     */
    bool place(RowId identity, const Mat4d& local);

    /** @brief Collects one view's commands, in draw order - the same list the content's collection produces. */
    void collect(const CollectView& view, std::uint32_t layer_mask, std::vector<RenderCommand>& out) const;

    /** @brief Re-reads the own boxes whose data revision moved (a content-thread step; see refreshBoxes). */
    std::uint32_t refreshBoxes();

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

} // namespace detail

VN_GRAPHICS_NS_END
