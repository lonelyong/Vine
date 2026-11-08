#pragma once

#include <vine/geom/geom_global.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/geom/spatial/detail/BvhBuildJob.hpp>
#include <vine/geom/spatial/detail/VolumeTraits.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

/// Height limit the traversal stacks are sized for; a builder may go shallower, never deeper.
///
/// It sits in this header rather than in `Bvh.hpp` because this one is BELOW it: the tree and the restore algorithm both
/// read it, and a constant both need belongs to the lower of the two headers.
inline constexpr unsigned kBvhMaxDepth = 64;

/**
 * @brief One NODE's place in a tree: the part of it a stored or restored tree keeps.
 *
 * @details
 * - A node's BOX is a function of the triangles, so it is never stored: a tree that travels - through a file, into another
 *   process - keeps only the part a build cannot recompute cheaply, which is the grouping. Every box is recomputed from
 *   the primitives on the way back in, so a stale topology can only produce a worse tree, never a wrong answer.
 * - The meaning of the two numbers is `detail::BvhNode`'s: `count == 0` is an internal node whose LEFT child is the next
 *   slot and whose `offset` is the right child; `count > 0` is a leaf whose `offset` is its first slot in the primitive
 *   order. That is one convention, stated once; a writer that assumed "two child indices" would restore a tree whose
 *   traversal visits the wrong subtrees.
 * - It is a plain value on purpose (no accessors, no methods beyond equality): it is exactly what a file holds per node,
 *   so a reader, a writer and a test all compare and copy the same thing.
 */
struct BvhNodeTopology
{
    /// Right-child index for an internal node; first primitive-order slot for a leaf.
    std::uint32_t offset{ 0 };

    /// Primitive count for a leaf; zero for an internal node.
    std::uint32_t count{ 0 };

    /// Value equality, so two node arrays can be compared as values (a restored topology against the stored one).
    constexpr bool operator==(const BvhNodeTopology&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<BvhNodeTopology>, "a cache file holds these bytes, so a node must be plain data");

/**
 * @brief A tree's shape as somebody else's arrays: the form a reader, a writer and a restore pass pass around.
 *
 * @details
 * - The BORROWED form of @ref BvhTopology: the same three facts - nodes, primitive order and height - read from storage
 *   this object does not own (a decoded file, a caller's vector, a `BvhTopology` itself).
 * - It exists so "I borrow the shape" is a type rather than a comment. A restore pass that took the owning value by
 *   const reference would leave the caller to remember not to move it - an unwritten convention (`G4`); a view is
 *   obviously borrowed, and passing one costs three words.
 * - The three facts still travel together for the reason they do in @ref BvhTopology: node offsets index into the order
 *   and `depth` states the node array's height, so a view assembled from two shapes describes neither.
 */
struct BvhTopologyView
{
    /// One entry per node, in depth-first order (see @ref BvhNodeTopology for what the two numbers mean).
    std::span<const BvhNodeTopology> nodes;

    /// The primitive permutation the leaves' `offset`/`count` index into.
    std::span<const std::uint32_t> order;

    /// The tree's height, the root being depth 0.
    unsigned depth{ 0 };

    /// How many primitives this shape covers: one entry per primitive in @ref order.
    [[nodiscard]] std::size_t primitiveCount() const noexcept
    {
        return order.size();
    }

    /// Whether the shape states nothing at all; an unbuilt tree's shape reports this.
    [[nodiscard]] bool empty() const noexcept
    {
        return nodes.empty() || order.empty();
    }
};

/**
 * @brief A tree's SHAPE as one value: its nodes, the primitive permutation its leaves index into, and its height.
 *
 * @details
 * - This is the half of a tree a build cannot recompute cheaply, and therefore the half worth keeping and storing. A
 *   BOX is a function of the primitives, so boxes are deliberately absent: `RestoreTopology` re-derives them from the
 *   current primitives, which is what makes a stale or foreign shape produce a worse tree instead of a wrong answer.
 * - One value, not three loose arrays: `nodes`, `order` and `depth` only mean anything together (the node offsets index
 *   into `order`, and `depth` states the height the node array has). A caller that assembles them from three places can
 *   pair one tree's nodes with another tree's order; a caller that holds this cannot.
 * - It is what `Bvh::topology()` returns and what `RestoreTopology` consumes, so "what a tree's shape is" is stated once.
 */
struct BvhTopology
{
    /// One entry per node, in depth-first order (see @ref BvhNodeTopology for what the two numbers mean).
    std::vector<BvhNodeTopology> nodes;

    /// The primitive permutation the leaves' `offset`/`count` index into.
    std::vector<std::uint32_t> order;

    /// The tree's height, the root being depth 0.
    unsigned depth{ 0 };

    /// How many primitives this shape covers: one entry per primitive in @ref order.
    [[nodiscard]] std::size_t primitiveCount() const noexcept
    {
        return order.size();
    }

    /// Borrows this shape: the form a reader, a writer and a restore pass all pass around.
    [[nodiscard]] BvhTopologyView view() const noexcept
    {
        return BvhTopologyView{ nodes, order, depth };
    }

    /**
     * @brief Borrows this shape, so a function that takes a `BvhTopologyView` accepts a holder of the value directly.
     *
     * @details
     * - A view is a strictly WEAKER promise about the same value, so the conversion cannot lose a fact: it only drops
     *   the ownership. Qt's `QByteArray` to `QByteArrayView` conversion is the same relationship, and the reason is the
     *   same - requiring `.view()` at every call site would be ceremony about a fact the type already states.
     *
     * @warning The returned view borrows THIS object: it has to outlive the call it is passed to (a temporary is fine -
     *          it lives to the end of the full expression that contains the call).
     *
     * @return A view over this shape's arrays.
     */
    operator BvhTopologyView() const noexcept
    {
        return view();
    }

    /// Value equality: a restored shape can be compared with the one it came from.
    bool operator==(const BvhTopology&) const = default;
};

namespace detail
{

/**
 * @brief Recomputes every node's volume from the primitives, keeping the topology.
 *
 * @details
 * - ONE reverse pass: children always sit after their parent, which makes the sweep a valid post-order, so a parent is
 *   merged only after both of its children are complete.
 * - This is the only implementation of "derive the boxes from the current primitives": `Bvh::refit()` (after moving
 *   primitives) and `RestoreTopology` (after loading a grouping) both call it, so a restored tree cannot end up with a
 *   different notion of what a box is.
 *
 * @tparam T      Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 * @param nodes   The nodes to re-box, in depth-first order.
 * @param order   The primitive permutation the leaves index into.
 * @param bounds  The primitives' volumes, indexed by primitive id.
 */
template <typename T, template <typename> class Volume>
void refitNodes(std::vector<BvhNode<T, Volume>>& nodes, std::span<const std::uint32_t> order, std::span<const Volume<T>> bounds)
{
    using Traits = VolumeTraits<Volume<T>>;
    for (std::size_t index = nodes.size(); index-- > 0;) {
        BvhNode<T, Volume>& node = nodes[index];
        if (node.count != 0) {
            Volume<T> merged = bounds[order[node.offset]];
            for (std::uint32_t slot = node.offset + 1u; slot < node.offset + node.count; ++slot) {
                merged = Traits::merged(merged, bounds[order[slot]]);
            }
            node.bounds = merged;
            continue;
        }
        node.bounds = Traits::merged(nodes[index + 1u].bounds, nodes[node.offset].bounds);
    }
}

} // namespace detail

/**
 * @brief A construction algorithm that puts a STORED grouping into a tree instead of computing a split.
 *
 * @details
 * - It is a build algorithm like the median and SAH ones - same `BvhBuilder::buildWith()` door, same `finishBuild` tail -
 *   so "how a tree came to have this shape" stays one concept: every way of building one is content written into a
 *   `BuildJob`, and none of them can leave the tree in a state the build path would not produce (see
 *   `.ai/design/geometry-bvh.md` 2.13).
 * - It copies the primitive order, appends the nodes with their stored `offset`/`count`, and then re-derives every box
 *   with `detail::refitNodes` - the same pass `refit()` uses. The primitives' volumes must already be registered on the
 *   tree (one per triangle, in triangle order), which is what `restoreTriangleBvh()` does before calling this.
 * - Nothing is trusted: a node count outside `[1, 2n]`, a leaf outside the primitive order, an internal node whose right
 *   child is not strictly after its left one, an order that is not a permutation, an unreachable node or a height that
 *   is not the tree's own all make the call return 0, which leaves the tree unbuilt. A caller treats that as "no
 *   cache" rather than as damage.
 *
 * @warning
 * - It BORROWS the shape it is given (a `BvhTopologyView` says so): the arrays have to outlive the `buildWith()` call that
 *   runs it - a temporary is fine, it lives to the end of the full expression that contains the call. Everything the tree
 *   keeps is written into the build job, so nothing refers to the shape afterwards.
 */
class RestoreTopology
{
  public:
    /**
     * @brief Prepares a restore of @p shape.
     *
     * @param shape The shape to install: nodes, primitive order and height, all from one tree.
     */
    explicit RestoreTopology(BvhTopologyView shape)
      : shape_(shape)
    {}

    /**
     * @brief Fills @p job with the stored grouping.
     *
     * @details
     * - Reads as a checklist: the arrays have to be the right sizes, the order a permutation, every node's numbers inside
     *   the primitive order, and the node array one tree whose real height is the stored one. Each check is its own named
     *   function, so "what makes a shape installable" is readable without reading the copying code next to it.
     *
     * @tparam T      Floating-point scalar type.
     * @tparam Volume Bounding-volume template.
     * @param job     The build job to fill; `order` and `nodes` are written, `depth` and `node_count` are set.
     * @return The number of node slots used, or 0 when the stored shape does not describe this tree.
     */
    template <typename T, template <typename> class Volume>
    std::uint32_t operator()(detail::BuildJob<T, Volume>& job) const
    {
        const std::span<const BvhNodeTopology> nodes = shape_.nodes;
        const std::span<const std::uint32_t>   order = shape_.order;
        const auto prim_count = static_cast<std::uint32_t>(job.bounds.size());
        if (prim_count == 0 || nodes.empty() || nodes.size() > 2u * job.bounds.size()) {
            return 0; // one slot per leaf at most, and an internal node per split on top of that
        }
        if (order.size() != prim_count || job.order.size() != prim_count) {
            return 0;
        }
        if (!orderIsPermutation(order, prim_count) || !nodesFitPrimitives(nodes, prim_count)) {
            return 0;
        }

        // The stored height has to be the array's own: a shape that claims another one was truncated, or written by
        // something that is not this format - and the traversal stacks are sized by that contract.
        const std::optional<unsigned> height = measuredHeight(nodes);
        if (!height.has_value() || *height != shape_.depth || *height > job.max_depth) {
            return 0;
        }

        writeInto(job, *height);
        return static_cast<std::uint32_t>(nodes.size());
    }

  private:
    /**
     * @brief Whether @p order is a permutation of `[0, count)`.
     *
     * @param order The primitive order to check.
     * @param count The primitive count it must cover exactly once each.
     * @return true when every id in `[0, count)` appears exactly once.
     */
    [[nodiscard]] static bool orderIsPermutation(std::span<const std::uint32_t> order, std::uint32_t count)
    {
        std::vector<bool> seen(count, false);
        for (const std::uint32_t id : order) {
            if (id >= count || seen[id]) {
                return false;
            }
            seen[id] = true;
        }
        return true;
    }

    /**
     * @brief Whether every node's numbers land inside the arrays they refer to.
     *
     * @details
     * - Internal (`count == 0`): the left child is the next slot, so the right child has to follow it and stay in the
     *   node array.
     * - Leaf (`count > 0`): its run has to lie inside `[0, primitive_count)`.
     *
     * @param nodes           Nodes in depth-first order.
     * @param primitive_count Primitives the leaf runs may cover.
     * @return true when no node points outside the node array or the primitive order.
     */
    [[nodiscard]] static bool nodesFitPrimitives(std::span<const BvhNodeTopology> nodes, std::uint32_t primitive_count) noexcept
    {
        const auto node_count = static_cast<std::uint32_t>(nodes.size());
        for (std::uint32_t index = 0; index < node_count; ++index) {
            const BvhNodeTopology& node = nodes[index];
            if (node.count == 0) {
                if (index + 1u >= node_count || node.offset <= index + 1u || node.offset >= node_count) {
                    return false;
                }
                continue;
            }
            if (node.offset > primitive_count || node.count > primitive_count - node.offset) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief The height the node array actually describes, or nothing when it is not one tree.
     *
     * @details
     * - One walk from the root on the traversal's own stack shape, measuring the deepest leaf and counting the slots it
     *   reaches: an array with a stray slot is not a tree, however well-formed each entry looks on its own.
     * - Requires nodesFitPrimitives() to have accepted @p nodes - every index it follows is then in range.
     *
     * @param nodes Nodes in depth-first order.
     * @return The deepest leaf's depth, or nullopt when a slot is unreachable or the walk is deeper than any traversal.
     */
    [[nodiscard]] static std::optional<unsigned> measuredHeight(std::span<const BvhNodeTopology> nodes) noexcept
    {
        unsigned      height  = 0;
        std::uint32_t visited = 0;
        std::array<std::pair<std::uint32_t, unsigned>, kBvhMaxDepth + 2u> stack{};
        unsigned                                                          top = 0;
        stack[top++]                                                          = { 0u, 0u };
        while (top > 0) {
            const auto [index, level] = stack[--top];
            ++visited;
            height = level > height ? level : height;
            const BvhNodeTopology& node = nodes[index];
            if (node.count != 0) {
                continue;
            }
            if (top + 2u > stack.size()) {
                return std::nullopt; // deeper than any traversal stack: refuse rather than write a tree nobody can walk
            }
            stack[top++] = { index + 1u, level + 1u };
            stack[top++] = { node.offset, level + 1u };
        }
        return visited == nodes.size() ? std::optional<unsigned>(height) : std::nullopt;
    }

    /**
     * @brief Copies the validated shape into @p job and re-derives every box from the primitives.
     *
     * @tparam T      Floating-point scalar type.
     * @tparam Volume Bounding-volume template.
     * @param job     The build job to fill.
     * @param height  The tree's height, already verified against the node array and the job's limit.
     */
    template <typename T, template <typename> class Volume>
    void writeInto(detail::BuildJob<T, Volume>& job, unsigned height) const
    {
        std::copy(shape_.order.begin(), shape_.order.end(), job.order.begin());
        job.nodes.clear();
        job.nodes.reserve(shape_.nodes.size());
        for (const BvhNodeTopology& node : shape_.nodes) {
            const auto index        = job.appendNode();
            job.nodes[index].offset = node.offset;
            job.nodes[index].count  = node.count;
        }
        detail::refitNodes<T, Volume>(job.nodes, job.order, job.bounds);
        job.depth = height;
    }

    BvhTopologyView shape_;
};

VN_GEOM_SPATIAL_NS_END
