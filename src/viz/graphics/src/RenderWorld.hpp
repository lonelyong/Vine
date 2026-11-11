#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vine/graphics/DrawListSource.hpp>

#include "DeltaChannel.hpp"
#include "DrawListBake.hpp"
#include "DrawListMemo.hpp"

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

    /**
     * @brief Applies one row's own numeric values: opacity, layer declaration, draw order (see StateValues).
     *
     * @details
     * - The row's OWN facts change, so the SUBTREE is re-folded (a child inherits all three) and the ancestors' boxes are
     *   re-derived - the same one fold a placement edit runs, which is why this costs the subtree and not the table.
     * - A handle-carrying state change (a StateNode's material or program) is NOT this: it travels as a description
     *   delta (design R5), so the two halves of that one fact never arrive separately.
     *
     * @param identity A row this world mirrors.
     * @param values   The values the content now states for it.
     * @return false when the identity is unknown; the refusal is counted.
     */
    bool setState(RowId identity, const StateValues& values);

    /**
     * @brief Replaces one row's OWN facts with the block's single row: its payload and whatever a re-read states.
     *
     * @details
     * - This is how a DESCRIPTION travels (design F5/R5): a geometry whose shape, primitives or handles changed, and a
     *   StateNode whose material or program was swapped - one fact with a handle in it, re-read once by the content's
     *   thread and applied here as values.
     * - STRUCTURE IS KEPT: identity, parent, subtree end and child count come from the live row; everything else is
     *   taken from @p block and then re-folded (the resolved values are the fold's). A change that alters the SHAPE of
     *   the subtree (a node becoming invisible, gaining or losing children) is not this - that is attach/detach.
     *
     * @param identity The row the block describes.
     * @param block    A block of exactly one row (see buildOwnFacts on a leaf).
     * @return false when the identity is unknown or the block is not one row; the refusal is counted.
     */
    bool setDescription(RowId identity, const SceneBake& block);

    /**
     * @brief Connects a subtree: @p block's rows (its OWN facts, see buildOwnFacts) become the @p child_index'th child
     *        of @p parent_identity.
     *
     * @details
     * - The rows are SPLICED into the table's walk order, so every range a select jumps by (`Row::end`) still means
     *   what it meant: the rows after the insertion point move, and every row whose subtree contains it grows by the
     *   block's size. That costs the table O(rows) index fixups - once per structure change, which is the price of the
     *   select being pure index arithmetic (see DrawListBake's note on why the table is an array).
     * - The block arrives UNFOLDED because only the table knows the parent's placement; the fold runs here, against
     *   the real parent, and then re-derives the ancestors' boxes (see resolveSubtree).
     * - A block with no rows (an invisible subtree, see buildOwnFacts) changes nothing and is not a refusal.
     *
     * @param parent_identity The row the subtree hangs under.
     * @param child_index     Where among that row's children it goes (0 = first, past the end = last).
     * @param block           The block to splice; its identity keys are what the table will answer by.
     * @return false when the parent is not in this world; the refusal is counted.
     */
    bool attach(RowId parent_identity, std::uint32_t child_index, SceneBake block);

    /**
     * @brief Disconnects the subtree rooted at @p identity: its rows leave the table.
     *
     * @details
     * - Nothing is parked: a collected command names what it draws (a geometry share, a matrix, a state), not a row, and
     *   the residency window for a retired object is the backend's own rule (RetirementQueue) - unchanged here.
     * - Only boxes change above it, so the ancestors are re-derived with the box-only fold (see refoldChain): the
     *   subtree's own values are gone, not recomputed.
     * - The ROOT cannot be detached (it has no parent row to fix up): replacing a whole table is `adopt`'s job.
     *
     * @param identity The subtree's root.
     * @return false when the identity is unknown, or is this world's root; the refusal is counted.
     */
    bool detach(RowId identity);

    /**
     * @brief Collects one view's commands, in draw order - the same list the content's collection produces.
     *
     * @param view       The view to collect for.
     * @param layer_mask Layers this picture draws.
     * @param out        Receives the commands, in draw order.
     * @return true when a per-view row needed the DEVICE-PIXEL viewport (the memo's key half: such a list is only
     *         reusable for the same rectangle).
     */
    bool collect(const CollectView& view, std::uint32_t layer_mask, std::vector<RenderCommand>& out) const;

  public:
    /** @brief Gets how many rows the world carries. */
    [[nodiscard]] std::size_t rowCount() const noexcept;

    /** @brief Gets the version of the rows: moves on every edit, and is what a collected list is keyed by. */
    [[nodiscard]] std::uint64_t revision() const noexcept;

    /** @brief Gets how many rows the edits so far have re-resolved (the witness for "only the subtree"). */
    [[nodiscard]] std::uint64_t rowsRecomputed() const noexcept;

    /** @brief Gets how many edits were refused. */
    [[nodiscard]] std::uint64_t refusals() const noexcept;

  private:
    /// @brief Rebuilds the identity → row index after the table's indices moved.
    void rebuildIndex();

  private:
    SceneBake                             bake_;
    std::unordered_map<RowId, std::uint32_t> index_;
    std::uint64_t                         revision_{ 0 }; ///< Bumped by every edit (see revision()).
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

    /**
     * @brief Drops the lists nothing asked for in the previous frame (see DrawListMemo::beginFrame).
     *
     * @param frame The frame about to be asked; must be called where frames run.
     */
    void beginFrame(std::uint64_t frame);

    /** @brief Gets how many lists this source has handed out of its memo (the per-view reuse witness). */
    [[nodiscard]] std::uint64_t memoHits() const noexcept;

    /** @brief Gets how many lists this source has collected (the per-view walk witness). */
    [[nodiscard]] std::uint64_t memoStores() const noexcept;

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
    // THE SAME MEMO THE CONTENT HAS (see DrawListMemo): a list is a pure function of (rows, view, mask), and a camera
    // drawn by two passes must be culled and selected once - which the mirror could not do before this existed.
    mutable DrawListMemo memo_;
    std::uint64_t        frame_{ 0 }; ///< The frame currently asking (0: no frame open, nothing is memoised).
};

} // namespace detail

VN_GRAPHICS_NS_END
