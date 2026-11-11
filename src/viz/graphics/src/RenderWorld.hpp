#pragma once

#include <cstdint>
#include <unordered_map>

#include "DrawListBake.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief The RENDER WORLD: a scene the renderer owns, filled by values instead of by reading the content's nodes.
 *
 * @details
 * - WHY IT EXISTS. A frame's GPU-facing work must not run on the thread that edits content, and the draw list it
 *   needs is built from rows, not from nodes (see `.ai/design/graphics-threading.md`). This type is the side that
 *   owns those rows: the content thread hands values in, this owns them, and its select reads rows only.
 * - WHAT OWNS WHAT. `buildFrom()` is the handshake: it walks the graph ONCE (the caller's thread) and fills the
 *   table. From then on the table moves only by value - `place()` for a placement, and (see the phases in the
 *   design) the same shape for state, structure and descriptions. The rows keep shares of the content they name
 *   (geometry, material, program), which is what lets a row outlive the caller's temporary.
 * - WHAT IT IS NOT. Not a scene, not a document and not a second graph: it has no parents walk, no names, no
 *   selection, no behaviours. It answers one question - "what would this view draw" - with the same passes the
 *   content side uses (`bakeSubtree` for the build, `resolveSubtree` for every fold, `selectFromBake` for the view,
 *   `sortCollected` for the order), so the two sides cannot drift.
 * - WHAT STILL NEEDS THE CONTENT'S THREAD. Two things, both named: the handshake itself, and `refreshBoxes` - a
 *   write to a buffer moves the box a row culls with and announces itself on the buffer, not as a delta. A caller
 *   that owns no content (an engine that only holds rows) never calls the second, so a world built from values is
 *   usable off the content's thread; a world built from a graph is not, until that poll moves too.
 *
 * @note Not thread-safe: one thread owns an instance, like every other backend-side unit. The threading design is
 *       what makes that thread the engine's, and it is the caller's job to stop reading nodes once it has crossed.
 */
class RenderWorld {
  public:
    /** @brief Builds the whole table from a graph (the handshake; runs on the content's thread). */
    void buildFrom(const Node& root, float opacity = 1.0F);

    /** @brief Forgets every row (and every share it held). */
    void clear();

    /**
     * @brief Applies one placement edit: @p node's own local matrix, with its subtree re-placed.
     *
     * @param node  A node this world knows (one the build put in the table).
     * @param local The node's own local matrix.
     * @return false when the node is unknown to this world or resolves its placement per view (see Node).
     */
    bool place(const Node& node, const Mat4d& local);

    /** @brief Collects one view's commands, in draw order - the same list the content's collection produces. */
    void collect(const CollectView& view, std::uint32_t layer_mask, std::vector<RenderCommand>& out) const;

  public:
    /** @brief Gets how many rows the table carries. */
    [[nodiscard]] std::size_t rowCount() const noexcept;

    /** @brief Gets how many rows the placements so far have re-resolved (the witness for "only the subtree"). */
    [[nodiscard]] std::uint64_t rowsRecomputed() const noexcept;

    /** @brief Gets how many edits were refused (unknown node, per-view placement, a box only a node could answer). */
    [[nodiscard]] std::uint64_t refusals() const noexcept;

    /** @brief Re-reads the own boxes of the rows whose geometry revision moved (the one content-thread step). */
    std::uint32_t refreshBoxes();

  private:
    SceneBake                                     bake_;
    std::unordered_map<const Node*, std::uint32_t> index_;
    std::uint64_t                                 rows_recomputed_{ 0 };
    std::uint64_t                                 refusals_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
