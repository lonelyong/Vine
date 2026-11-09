#pragma once

#include <vine/geom/geom_global.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

VN_GEOM_SPATIAL_NS_BEGIN

/// Height limit the traversal stacks are sized for; a builder may go shallower, never deeper.
///
/// It sits in this header because this one is the vocabulary BOTH sides read: the tree sizes its stacks by it, the
/// restore check rejects a shape deeper than it, and the builder defaults to it. A constant three of them need belongs
/// with the vocabulary, not with any one of them.
inline constexpr unsigned kBvhMaxDepth = 64;

/**
 * @brief One NODE's place in a tree: the combinatorics of a node, and nothing else.
 *
 * @details
 * - A node's BOX is a function of the primitives, so it is never stored beside the grouping: the tree keeps the boxes in
 *   a PARALLEL array (`Bvh`'s node bounds) and a stored shape keeps only what a build cannot recompute cheaply. Every box
 *   is recomputed from the primitives on the way back in, so a stale topology can only produce a worse tree, never a
 *   wrong answer.
 * - This IS the node type - the in-memory tree and the file both hold this layout, so there is one statement of what the
 *   two numbers mean rather than a stored copy of an internal one: `count == 0` is an internal node whose LEFT child is
 *   the next slot and whose `offset` is the right child; `count > 0` is a leaf whose `offset` is its first slot in the
 *   primitive order. A writer that assumed "two child indices" would restore a tree whose traversal visits the wrong
 *   subtrees.
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

VN_GEOM_SPATIAL_NS_END
